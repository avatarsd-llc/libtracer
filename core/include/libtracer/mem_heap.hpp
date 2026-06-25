// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// mem_heap — the host allocator backend. Owns malloc'd bytes; frees them and
// the Segment control block on destroy. The week-1 MVP backend for hosted
// targets (docs/reference/09-memory-substrate.md §mem_heap).
#pragma once

#include <cstddef>

#include "libtracer/backend.hpp"
#include "libtracer/segment.hpp"

namespace tracer::mem {

// The process-wide heap backend (a function-local static — no init-order trap).
[[nodiscard]] MemBackend& heap_backend() noexcept;

// Allocate a fresh, owned heap segment of `size` bytes, already wrapped in an
// adopting SegmentPtr. Returns an empty handle on allocation failure.
[[nodiscard]] SegmentPtr heap_alloc(std::size_t size);

}  // namespace tracer::mem
