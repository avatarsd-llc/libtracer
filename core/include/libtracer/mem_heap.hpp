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
