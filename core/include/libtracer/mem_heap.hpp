/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_heap — the host allocator backend. Owns malloc'd bytes; frees them and
 * the segment_t control block on destroy. The week-1 MVP backend for hosted
 * targets (docs/reference/09-memory-substrate.md §mem_heap).
 */
#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief The `mem_heap` L0 backend (`tr::mem`) and its L1 alloc helper (`tr::view`),
 *        plus the nothrow `std::vector` growth primitives (`tr::detail`).
 */

namespace tr::detail {

/**
 * @brief Nothrow `std::vector::reserve`: grow @p v to hold at least @p n elements
 *        WITHOUT ever aborting.
 *
 * A throwing `std::vector::reserve` calls `operator new` and, on failure, throws
 * `std::bad_alloc` — which under the MCU profile's `-fno-exceptions` is an
 * `abort()` (core/STYLE.md §language profile). This probes the exact target
 * allocation with a nothrow `operator new` first; if it fails (or @p n exceeds
 * `max_size()`) the call soft-fails, and only on success is the throwing `reserve`
 * run — which reclaims the just-freed probe block on the single-threaded reply
 * build, so it cannot throw.
 * @retval false Allocation would fail / @p n is impossible — nothing changed.
 * @retval true  @p v now has capacity for at least @p n elements.
 */
template <class T>
[[nodiscard]] bool try_reserve(std::vector<T>& v, std::size_t n) noexcept {
    if (n <= v.capacity()) return true;
    if (n > v.max_size()) return false;  // impossible count — the reserve would throw length_error
    void* probe = ::operator new(n * sizeof(T), std::nothrow);
    if (probe == nullptr) return false;
    ::operator delete(probe);
    v.reserve(n);  // nothrow now: the just-freed probe block satisfies it (single-threaded)
    return true;
}

/**
 * @brief Nothrow `std::vector::push_back`: append @p x, growing (capacity-doubling)
 *        through @ref try_reserve so no reallocation can abort under `-fno-exceptions`.
 *
 * For a vector whose element count is not known up front (the composed-read pre-order
 * collection stacks) — where @ref try_reserve cannot be called once with the final
 * count. @p x is appended only when the growth (if any) succeeds; the just-reserved
 * capacity guarantees the `push_back` itself never reallocates.
 * @retval false The growth allocation failed — @p x was NOT appended.
 */
template <class T>
[[nodiscard]] bool try_push_back(std::vector<T>& v, T&& x) noexcept {
    if (v.size() == v.capacity()) {
        const std::size_t grow = v.capacity() == 0 ? 1u : v.capacity() * 2u;
        if (!try_reserve(v, grow)) return false;
    }
    v.push_back(std::move(x));  // guaranteed no reallocation now
    return true;
}

}  // namespace tr::detail

namespace tr::mem {

/**
 * @brief The host allocator backend: owns `operator new`'d bytes, frees them and
 *        the `segment_t` control block on destroy.
 *
 * Exposed here (rather than TU-local) so the module-set destroy dispatch
 * (backend_set.cpp, ADR-0047 §2) can devirtualize its release; a `final` class,
 * so the qualified call in that switch is a direct call.
 */
class heap_backend_t final : public mem_backend_t {
   public:
    heap_backend_t() noexcept : mem_backend_t("mem_heap") {}

    view::segment_t* alloc(std::size_t size, alloc_hint_t /*hint*/) override {
        const std::align_val_t al{alignof(std::max_align_t)};
        void* raw = size ? ::operator new(size, al, std::nothrow) : nullptr;
        if (size && raw == nullptr) return nullptr;
        auto* seg = new (std::nothrow)
            view::segment_t(this, std::span<std::byte>(static_cast<std::byte*>(raw), size));
        if (seg == nullptr) {
            if (raw) ::operator delete(raw, al);
            return nullptr;
        }
        return seg;
    }

    void destroy(view::segment_t* seg) noexcept override {
        if (!seg->bytes.empty()) {
            ::operator delete(seg->bytes.data(), std::align_val_t{alignof(std::max_align_t)});
        }
        delete seg;
    }

    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::HEAP; }

    // Module-set traits (ADR-0047 §2). `needs_cache_ops` is read by `mem::transfer`.
    static constexpr bool needs_cache_ops =
        false; /**< @brief No DMA cache maintenance (host RAM). */
    static constexpr bool is_isr_safe =
        false; /**< @brief `alloc`/`destroy` call `operator new`/`delete` — not ISR-safe. */
    static constexpr bool owns_bytes =
        true; /**< @brief Owns the `operator new`'d bytes — durably storable. */
};

/** @brief The process-wide heap backend (function-local static — no init-order trap). */
[[nodiscard]] mem_backend_t& heap_backend() noexcept;

}  // namespace tr::mem

namespace tr::view {

/**
 * @brief Allocate a fresh, owned heap segment of @p size bytes, wrapped in an
 *        adopting `segment_ptr_t`.
 *
 * An L1 helper (it produces an owning handle), so it lives in `tr::view`, not
 * `tr::mem` (docs/adr/0016 §2).
 * @retval {} An empty handle on allocation failure.
 */
[[nodiscard]] segment_ptr_t heap_alloc(std::size_t size);

/**
 * @brief Allocate a fresh heap segment, copy @p bytes into it, and return a view
 *        over it — the canonical "own a copy of these bytes as a view_t" idiom
 *        (heap_alloc + memcpy + view_t::over) in one place.
 *
 * Collapses the repeated alloc/copy/over triplet across the codec and runtime
 * (graph read_schema/read_acl, the FWD resolver's WRITE-payload and reply head,
 * fwd_router's local delivery) into one audited locus.
 *
 * The return type disambiguates the two outcomes an unowned `view_t` used to
 * conflate (docs/reference/08 §L1 contracts): `std::nullopt` is an allocation
 * failure the caller maps to BACKPRESSURE; an **engaged, empty** view is a
 * legitimately-empty @p bytes span (`heap_alloc(0)` is not called).
 *
 * @retval std::nullopt Allocation failure / backpressure.
 * @retval {engaged}    An owned copy of @p bytes (empty-and-unowned iff @p bytes
 *                      is empty).
 */
[[nodiscard]] inline std::optional<view_t> over_bytes(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) return view_t{};  // engaged-empty: a legitimately-empty input
    segment_ptr_t seg = heap_alloc(bytes.size());
    if (!seg) return std::nullopt;  // allocation failure => BACKPRESSURE
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

}  // namespace tr::view
