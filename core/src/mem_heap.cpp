/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/mem_heap.hpp"

#include <cstddef>
#include <new>
#include <span>

namespace tr::mem {

/**
 * @brief heap_backend_t is defined in the header (mem_heap.hpp) so the module-set destroy dispatch
 *        (backend_set.cpp, ADR-0047 §2) can see the concrete type.
 */
mem_backend_t& heap_backend() noexcept {
    static heap_backend_t backend;
    return backend;
}

namespace detail {

/**
 * @brief Draw a large heap segment as two blocks, payload first (#1768), in the split layout
 *        the placement module gives (@ref kSplitHeaderBytes at @ref kSplitHeaderAlign).
 */
view::segment_t* heap_alloc_split(heap_backend_t* owner, std::size_t size,
                                  std::size_t align) noexcept {
    void* const payload = heap_source_t::acquire(size, align);
    if (payload == nullptr) return nullptr;
    void* const header = heap_source_t::acquire(kSplitHeaderBytes, kSplitHeaderAlign);
    if (header == nullptr) {
        heap_source_t::reclaim(payload, size, align);
        return nullptr;
    }
    return place_split(owner, payload, header, size);
}

/**
 * @brief Return both blocks of a split segment, sized as `heap_alloc_split` drew them.
 *        `bytes` is read before the header is destroyed, since it is the only record of where
 *        the payload block is.
 */
void heap_destroy_split(view::segment_t* seg, std::size_t align) noexcept {
    std::byte* const payload = seg->bytes.data();
    const std::size_t size = seg->bytes.size();
    seg->~segment_t();
    heap_source_t::reclaim(payload, size, align);
    heap_source_t::reclaim(seg, kSplitHeaderBytes, kSplitHeaderAlign);
}

}  // namespace detail

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
    const bool loan = len >= loan_min && backend.space() == mem::mem_space_t::HOST &&
                      mem::rx_loan_fits(len, backend.max_segment_size(), backend.alignment());
    if (!loan) return {segment_ptr_t::adopt(backend.alloc(len)), 0};
    // ONE request either way: a backend that refuses the reserved block is exhausted, and a
    // second, smaller ask would spend its refusal twice on one frame. A backend whose blocks
    // simply cannot hold the reserve said so through `max_segment_size` above.
    segment_ptr_t seg = segment_ptr_t::adopt(backend.alloc(len + mem::kRxLoanBytes));
    if (!seg) return {};
    new (seg->bytes.data()) rx_loan_word_t(0);  // unclaimed
    seg->rx_loan = 1;
    return {std::move(seg), mem::kRxLoanBytes};
}

// NOT written as `segment_alloc(mem::heap_backend(), size)`: this is the arm every
// pre-#793 call site takes, and the whole latency claim for #793 is that those call
// sites are BYTE-identical. Delegating would put one extra call frame on it. One
// duplicated line is the price of an object-file `cmp` being the proof.
segment_ptr_t heap_alloc(std::size_t size) {
    return segment_ptr_t::adopt(mem::heap_backend().alloc(size, mem::alloc_hint_t::NONE));
}

}  // namespace tr::view
