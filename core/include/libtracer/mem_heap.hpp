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
#include <span>
#include <utility>

#include "libtracer/backend.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief The `mem_heap` L0 backend (`tr::mem`) and its L1 alloc helper (`tr::view`).
 */

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
 * @retval A view with a null @ref view_t::owner on allocation failure (the caller
 *         maps that to BACKPRESSURE); an empty @p bytes span yields an unowned
 *         empty view (heap_alloc(0) is not called).
 */
[[nodiscard]] inline view_t over_bytes(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) return view_t{};
    segment_ptr_t seg = heap_alloc(bytes.size());
    if (!seg) return view_t{};
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

}  // namespace tr::view
