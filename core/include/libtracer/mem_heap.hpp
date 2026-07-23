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
#include <string>
#include <string_view>
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
 * @brief Test-only OOM-injection seam over @ref probe_bytes: when set, a probe of @p bytes
 *        that the hook rejects soft-fails as if the heap were exhausted.
 *
 * The nothrow soft-fail paths cannot be exercised by really exhausting the host heap, so
 * this is their failure-injection tool — the global-heap twin of the failing `mem_backend_t`
 * the `graph_value_backend_test` precedent injects (ADR-0060 §3). Production never sets it;
 * the cost is one predictable null-check on the (cold) growth/probe paths.
 */
inline bool (*probe_fail_hook)(std::size_t bytes) noexcept = nullptr;

/**
 * @brief The @ref probe_fail_hook gate alone (no real probe): true when no hook is set or
 *        the hook admits @p bytes.
 *
 * For soft-fail sites whose failure leg is not the probe itself (e.g. a host-profile
 * `catch (bad_alloc)`) but that must still honor the test seam.
 */
[[nodiscard]] inline bool probe_hook_ok(std::size_t bytes) noexcept {
    return probe_fail_hook == nullptr || probe_fail_hook(bytes);
}

/**
 * @brief Probe whether a @p bytes-sized heap allocation would succeed, nothrow — the ONE
 *        locus of the nothrow `operator new` probe.
 *
 * Factored out of the `try_*` growth templates so the `new`/`delete` pair is emitted ONCE,
 * not duplicated into every `T` instantiation (the esp32c6 footprint sentinel). A throwing
 * `std::vector::reserve` would `abort()` under the MCU profile's `-fno-exceptions`; the
 * caller probes here first, then runs the throwing grow only once it is known to succeed —
 * on the single-threaded reply build the just-freed probe block is what the grow reclaims.
 * @retval false The allocation would fail (OOM) — nothing was allocated.
 */
[[nodiscard]] inline bool probe_bytes(std::size_t bytes) noexcept {
    if (!probe_hook_ok(bytes)) return false;  // test-only OOM injection
    void* p = ::operator new(bytes, std::nothrow);
    if (p == nullptr) return false;
    ::operator delete(p);
    return true;
}

/**
 * @brief The capacity-doubling grow target for a full vector (min 1) — a non-template
 *        helper so the size math is not duplicated per `try_push_back<T>`.
 */
[[nodiscard]] inline std::size_t grow_capacity(std::size_t cap) noexcept {
    return cap == 0 ? 1u : cap * 2u;
}

/**
 * @brief Nothrow `std::vector::reserve`: grow @p v to hold at least @p n elements
 *        WITHOUT ever aborting.
 *
 * Probes the exact target allocation via @ref probe_bytes first; if it fails (or @p n
 * exceeds `max_size()`) the call soft-fails, and only on success is the throwing `reserve`
 * run (which cannot throw — the just-freed probe block satisfies it).
 * @retval false Allocation would fail / @p n is impossible — nothing changed.
 * @retval true  @p v now has capacity for at least @p n elements.
 */
template <class T>
[[nodiscard]] bool try_reserve(std::vector<T>& v, std::size_t n) noexcept {
    if (n <= v.capacity()) return true;
    if (n > v.max_size()) return false;  // impossible count — the reserve would throw length_error
    if (!probe_bytes(n * sizeof(T))) return false;
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
    if (v.size() == v.capacity() && !try_reserve(v, grow_capacity(v.capacity()))) return false;
    v.push_back(std::move(x));  // guaranteed no reallocation now
    return true;
}

/**
 * @brief Nothrow byte-vector copy-assign: replace @p dst's contents with @p src WITHOUT
 *        ever aborting — @ref try_reserve then an in-capacity `assign`.
 *
 * Non-template (the one element type the store/delivery path copies is `std::byte` —
 * vertex keys, route records), per the footprint-sentinel discipline.
 * @retval false The growth allocation failed — @p dst is unchanged.
 */
[[nodiscard]] inline bool try_assign(std::vector<std::byte>& dst,
                                     std::span<const std::byte> src) noexcept {
    if (!try_reserve(dst, src.size())) return false;
    dst.assign(src.begin(), src.end());  // within capacity — no reallocation
    return true;
}

/**
 * @brief Nothrow string copy-assign: replace @p dst's contents with @p src WITHOUT ever
 *        aborting.
 *
 * A fitting copy (SSO or existing capacity) assigns directly; a growing one probes the
 * `size + 1` (NUL) buffer via @ref probe_bytes first and soft-fails instead of throwing.
 * @retval false The growth allocation failed — @p dst is unchanged.
 */
[[nodiscard]] inline bool try_assign(std::string& dst, std::string_view src) noexcept {
    if (src.size() > dst.capacity() && !probe_bytes(src.size() + 1)) return false;
    dst.assign(src);  // fits capacity, or the just-freed probe block satisfies the grow
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
