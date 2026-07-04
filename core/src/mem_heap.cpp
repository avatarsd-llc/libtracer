/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/mem_heap.hpp"

namespace tr::mem {

// heap_backend_t is defined in the header (mem_heap.hpp) so the module-set
// destroy dispatch (backend_set.cpp, ADR-0047 §2) can see the concrete type.
mem_backend_t& heap_backend() noexcept {
    static heap_backend_t backend;
    return backend;
}

}  // namespace tr::mem

namespace tr::view {

segment_ptr_t heap_alloc(std::size_t size) {
    return segment_ptr_t::adopt(mem::heap_backend().alloc(size, mem::alloc_hint_t::NONE));
}

}  // namespace tr::view
