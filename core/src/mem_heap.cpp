/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/mem_heap.hpp"

namespace tr::mem {

/**
 * @brief heap_backend_t is defined in the header (mem_heap.hpp) so the module-set destroy dispatch
 *        (backend_set.cpp, ADR-0047 §2) can see the concrete type.
 */
mem_backend_t& heap_backend() noexcept {
    static heap_backend_t backend;
    return backend;
}

}  // namespace tr::mem

namespace tr::view {

/**
 * @brief The one locus of "adopt a fresh owned segment from a backend" (#793) — @ref
 *        heap_alloc is this over @ref mem::heap_backend.
 */
segment_ptr_t segment_alloc(mem::mem_backend_t& backend, std::size_t size) {
    return segment_ptr_t::adopt(backend.alloc(size, mem::alloc_hint_t::NONE));
}

// NOT written as `segment_alloc(mem::heap_backend(), size)`: this is the arm every
// pre-#793 call site takes, and the whole latency claim for #793 is that those call
// sites are BYTE-identical. Delegating would put one extra call frame on it. One
// duplicated line is the price of an object-file `cmp` being the proof.
segment_ptr_t heap_alloc(std::size_t size) {
    return segment_ptr_t::adopt(mem::heap_backend().alloc(size, mem::alloc_hint_t::NONE));
}

}  // namespace tr::view
