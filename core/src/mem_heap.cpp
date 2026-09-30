/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/mem_heap.hpp"

#include <new>

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

rx_block_t alloc_rx(mem::mem_backend_t& backend, std::size_t len, std::size_t loan_min) noexcept {
    // The loan pays only where the value would be SHARED, and only on memory the graph can
    // lay a record in: host bytes, aligned for the record's words, with room for both.
    const std::size_t cap = backend.max_segment_size();
    const bool loan = len >= loan_min && cap >= kRxLoanBytes && len <= cap - kRxLoanBytes &&
                      backend.space() == mem::mem_space_t::HOST &&
                      backend.alignment() >= alignof(void*);
    if (!loan) return {segment_ptr_t::adopt(backend.alloc(len)), 0};
    // ONE request either way: a backend that refuses the reserved block is exhausted, and a
    // second, smaller ask would spend its refusal twice on one frame. A backend whose blocks
    // simply cannot hold the reserve said so through `max_segment_size` above.
    segment_ptr_t seg = segment_ptr_t::adopt(backend.alloc(len + kRxLoanBytes));
    if (!seg) return {};
    new (seg->bytes.data()) rx_loan_word_t(0);  // unclaimed
    seg->rx_loan = 1;
    return {std::move(seg), kRxLoanBytes};
}

// NOT written as `segment_alloc(mem::heap_backend(), size)`: this is the arm every
// pre-#793 call site takes, and the whole latency claim for #793 is that those call
// sites are BYTE-identical. Delegating would put one extra call frame on it. One
// duplicated line is the price of an object-file `cmp` being the proof.
segment_ptr_t heap_alloc(std::size_t size) {
    return segment_ptr_t::adopt(mem::heap_backend().alloc(size, mem::alloc_hint_t::NONE));
}

}  // namespace tr::view
