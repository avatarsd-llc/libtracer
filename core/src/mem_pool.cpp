/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/mem_pool.hpp"

#include <cstring>

#include "libtracer/placement.hpp"

namespace tr::mem {

/**
 * @brief The sanctioned L0↔L1 boundary type (docs/adr/0016 §2): a backend constructs and reclaims
 *        segments, so it names this one `tr::view` symbol.
 */
using view::segment_t;

namespace {

std::byte* align_ptr_up(std::byte* p, std::size_t a) noexcept {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    const auto aligned = (v + a - 1) & ~(static_cast<std::uintptr_t>(a) - 1);
    return reinterpret_cast<std::byte*>(aligned);
}

}  // namespace

pool_t::pool_t(std::span<std::byte> slab, std::size_t slot_payload, std::size_t align) noexcept
    : mem_backend_t("mem_pool") {
    slot_payload_ = slot_payload;
    align_ = align;

    // The slot start must satisfy both the payload alignment and segment_t's own
    // alignment (a segment_t is placement-constructed at each slot start). The header and the
    // stride are the placement module's (`%placement.hpp`), like every backend's.
    std::byte* base = align_ptr_up(slab.data(), segment_block_align(align));
    const std::size_t lost = static_cast<std::size_t>(base - slab.data());
    slab_ = (lost < slab.size()) ? std::span<std::byte>(base, slab.size() - lost)
                                 : std::span<std::byte>{};

    header_ = segment_header_bytes(align);
    stride_ = segment_slot_bytes(slot_payload, align);
    slot_count_ = stride_ ? slab_.size() / stride_ : 0;
    free_count_ = slot_count_;
    free_head_ = slot_count_ ? 0 : kNil;

    // Thread the embedded free list: slot i -> i+1, last -> kNil.
    for (std::size_t i = 0; i < slot_count_; ++i) {
        store_next(i, i + 1 < slot_count_ ? i + 1 : kNil);
    }
}

void pool_t::store_next(std::size_t slot, std::size_t next) noexcept {
    std::memcpy(slot_at(slot), &next, sizeof(next));
}

std::size_t pool_t::load_next(std::size_t slot) const noexcept {
    std::size_t next = kNil;
    std::memcpy(&next, slot_at(slot), sizeof(next));
    return next;
}

void* pool_t::try_alloc(std::size_t bytes, std::size_t align) noexcept {
    if (bytes > header_ + slot_payload_ || align > segment_block_align(align_) ||
        free_head_ == kNil)
        return nullptr;
    const std::size_t idx = free_head_;
    free_head_ = load_next(idx);
    --free_count_;
    return slot_at(idx);
}

void pool_t::release(void* p, std::size_t /*bytes*/, std::size_t /*align*/) noexcept {
    const std::size_t idx =
        static_cast<std::size_t>(static_cast<std::byte*>(p) - slab_.data()) / stride_;
    store_next(idx, free_head_);
    free_head_ = idx;
    ++free_count_;
}

segment_t* pool_t::alloc(std::size_t size, alloc_hint_t /*hint*/) {
    if (size > slot_payload_) return nullptr;
    void* const block = pool_t::try_alloc(header_ + size, align_);
    return block != nullptr ? place_segment(this, block, size, align_) : nullptr;
}

void pool_t::destroy(segment_t* seg) noexcept {
    seg->~segment_t();
    pool_t::release(seg, 0, 0);
}

}  // namespace tr::mem
