// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// mem_heap — the host allocator backend. Owns malloc'd bytes; frees them and
// the segment_t control block on destroy. The week-1 MVP backend for hosted
// targets (docs/reference/09-memory-substrate.md §mem_heap).
#pragma once

#include <cstddef>

#include "libtracer/backend.hpp"
#include "libtracer/segment.hpp"

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

}  // namespace tr::view
