// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// mem_pool — a fixed, caller-owned slab carved into equal slots. The bounded
// "custom allocator" reference backend: alloc returns nullptr when the pool is
// exhausted (the BACKPRESSURE signal, docs/reference/09 §pressure), making it
// the deterministic choice for MCUs and bounded-memory targets (= mem_pool_static
// in reference/09). The free list is threaded through the slab itself — there is
// NO auxiliary heap allocation, so total memory use is exactly the caller's slab.
//
// Not internally synchronized: alloc/destroy on a shared pool are the
// application's to serialize. Per ADR-0012 each backend declares its own
// concurrency contract; this one's is "single-threaded reclamation."
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "libtracer/backend.hpp"
#include "libtracer/segment.hpp"

namespace tracer::mem {

class Pool final : public MemBackend {
   public:
    // Carve `slab` (caller-owned; must outlive the pool) into slots each able to
    // hold a Segment control block plus `slot_payload` usable bytes, with the
    // payload aligned to `align` (a power of two). The slot count is whatever
    // fits after aligning the slab base.
    Pool(std::span<std::byte> slab, std::size_t slot_payload,
         std::size_t align = alignof(std::max_align_t)) noexcept;

    Segment* alloc(std::size_t size, std::uint32_t hint) override;
    void destroy(Segment* seg) noexcept override;
    [[nodiscard]] std::size_t alignment() const noexcept override { return align_; }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override { return slot_payload_; }

    [[nodiscard]] std::size_t capacity() const noexcept { return slot_count_; }   // total slots
    [[nodiscard]] std::size_t available() const noexcept { return free_count_; }  // free slots

   private:
    static constexpr std::size_t kNil = ~std::size_t{0};
    [[nodiscard]] std::byte* slot_at(std::size_t i) const noexcept {
        return slab_.data() + i * stride_;
    }
    void store_next(std::size_t slot, std::size_t next) noexcept;
    [[nodiscard]] std::size_t load_next(std::size_t slot) const noexcept;

    std::span<std::byte> slab_;     // aligned, usable region of the caller's slab
    std::size_t slot_payload_ = 0;  // usable bytes per slot
    std::size_t align_ = 0;         // payload alignment reported via alignment()
    std::size_t header_ = 0;        // aligned sizeof(Segment) prefixing each slot
    std::size_t stride_ = 0;        // header_ + aligned slot_payload_
    std::size_t slot_count_ = 0;
    std::size_t free_count_ = 0;
    std::size_t free_head_ = kNil;  // index of first free slot (intrusive free list)
};

}  // namespace tracer::mem
