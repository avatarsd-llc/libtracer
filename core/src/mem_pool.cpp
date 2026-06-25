// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/mem_pool.hpp"

#include <cstring>
#include <new>

namespace tracer::mem {
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

Pool::Pool(std::span<std::byte> slab, std::size_t slot_payload, std::size_t align) noexcept
    : MemBackend("mem_pool") {
    slot_payload_ = slot_payload;
    align_ = align;

    // The slot start must satisfy both the payload alignment and Segment's own
    // alignment (a Segment is placement-constructed at each slot start).
    const std::size_t a = align < alignof(Segment) ? alignof(Segment) : align;
    std::byte* base = align_ptr_up(slab.data(), a);
    const std::size_t lost = static_cast<std::size_t>(base - slab.data());
    slab_ = (lost < slab.size()) ? std::span<std::byte>(base, slab.size() - lost)
                                 : std::span<std::byte>{};

    header_ = align_up(sizeof(Segment), a);
    stride_ = align_up(header_ + slot_payload, a);
    slot_count_ = stride_ ? slab_.size() / stride_ : 0;
    free_count_ = slot_count_;
    free_head_ = slot_count_ ? 0 : kNil;

    // Thread the embedded free list: slot i -> i+1, last -> kNil.
    for (std::size_t i = 0; i < slot_count_; ++i) {
        store_next(i, i + 1 < slot_count_ ? i + 1 : kNil);
    }
}

void Pool::store_next(std::size_t slot, std::size_t next) noexcept {
    std::memcpy(slot_at(slot), &next, sizeof(next));
}

std::size_t Pool::load_next(std::size_t slot) const noexcept {
    std::size_t next = kNil;
    std::memcpy(&next, slot_at(slot), sizeof(next));
    return next;
}

Segment* Pool::alloc(std::size_t size, std::uint32_t /*hint*/) {
    if (size > slot_payload_ || free_head_ == kNil) return nullptr;
    const std::size_t idx = free_head_;
    free_head_ = load_next(idx);
    --free_count_;
    std::byte* payload = slot_at(idx) + header_;
    return new (slot_at(idx)) Segment(this, std::span<std::byte>(payload, size));
}

void Pool::destroy(Segment* seg) noexcept {
    const std::size_t idx =
        static_cast<std::size_t>(reinterpret_cast<std::byte*>(seg) - slab_.data()) / stride_;
    seg->~Segment();
    store_next(idx, free_head_);
    free_head_ = idx;
    ++free_count_;
}

}  // namespace tracer::mem
