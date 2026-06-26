// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/mem_pool.hpp"

#include <cstring>
#include <new>

namespace tr::mem {

// The sanctioned L0↔L1 boundary type (docs/adr/0016 §2): a backend constructs
// and reclaims segments, so it names this one `tr::view` symbol.
using view::segment_t;

namespace {

constexpr std::size_t align_up(std::size_t n, std::size_t a) noexcept {
    return (n + a - 1) & ~(a - 1);
}

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
    // alignment (a segment_t is placement-constructed at each slot start).
    const std::size_t a = align < alignof(segment_t) ? alignof(segment_t) : align;
    std::byte* base = align_ptr_up(slab.data(), a);
    const std::size_t lost = static_cast<std::size_t>(base - slab.data());
    slab_ = (lost < slab.size()) ? std::span<std::byte>(base, slab.size() - lost)
                                 : std::span<std::byte>{};

    header_ = align_up(sizeof(segment_t), a);
    stride_ = align_up(header_ + slot_payload, a);
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

segment_t* pool_t::alloc(std::size_t size, alloc_hint_t /*hint*/) {
    if (size > slot_payload_ || free_head_ == kNil) return nullptr;
    const std::size_t idx = free_head_;
    free_head_ = load_next(idx);
    --free_count_;
    std::byte* payload = slot_at(idx) + header_;
    return new (slot_at(idx)) segment_t(this, std::span<std::byte>(payload, size));
}

void pool_t::destroy(segment_t* seg) noexcept {
    const std::size_t idx =
        static_cast<std::size_t>(reinterpret_cast<std::byte*>(seg) - slab_.data()) / stride_;
    seg->~segment_t();
    store_next(idx, free_head_);
    free_head_ = idx;
    ++free_count_;
}

}  // namespace tr::mem
