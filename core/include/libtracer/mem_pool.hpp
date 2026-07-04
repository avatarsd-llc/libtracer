/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_pool — a fixed, caller-owned slab carved into equal slots. The bounded
 * "custom allocator" reference backend: alloc returns nullptr when the pool is
 * exhausted (the BACKPRESSURE signal, docs/reference/09 §pressure), making it
 * the deterministic choice for MCUs and bounded-memory targets (= mem_pool_static
 * in reference/09). The free list is threaded through the slab itself — there is
 * NO auxiliary heap allocation, so total memory use is exactly the caller's slab.
 *
 * Not internally synchronized: alloc/destroy on a shared pool are the
 * application's to serialize. Per ADR-0012 each backend declares its own
 * concurrency contract; this one's is "single-threaded reclamation."
 */
#pragma once

#include <cstddef>
#include <span>

#include "libtracer/backend.hpp"
#include "libtracer/segment.hpp"

/**
 * @file
 * @brief The bounded `mem_pool` L0 backend (`tr::mem`) over a caller-owned slab.
 */

namespace tr::mem {

/**
 * @brief A fixed-slot allocator over a caller-owned slab; `alloc`-or-`nullptr`.
 *
 * Carves the slab into equal slots with the free list threaded through the slab
 * (no auxiliary heap), so memory use is exactly the caller's slab and
 * exhaustion is a return value, not an OOM. The deterministic MCU choice.
 */
class pool_t final : public mem_backend_t {
   public:
    /**
     * @brief Carve @p slab (caller-owned; must outlive the pool) into slots.
     *
     * Each slot holds a `segment_t` control block plus @p slot_payload usable
     * bytes, payload aligned to @p align (a power of two). The slot count is
     * whatever fits after aligning the slab base.
     */
    pool_t(std::span<std::byte> slab, std::size_t slot_payload,
           std::size_t align = alignof(std::max_align_t)) noexcept;

    /**
     * @brief Hand out the next free slot as a `segment_t` of @p size bytes.
     * @retval nullptr `size` exceeds the slot payload, or the pool is exhausted.
     */
    view::segment_t* alloc(std::size_t size, alloc_hint_t hint = alloc_hint_t::NONE) override;
    /** @brief Return @p seg's slot to the free list (placement-destroying it). */
    void destroy(view::segment_t* seg) noexcept override;
    [[nodiscard]] std::size_t alignment() const noexcept override { return align_; }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override { return slot_payload_; }
    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::POOL; }

    // Module-set traits (ADR-0047 §2): compile-time backend contracts the seam
    // consumes in place of prose. `needs_cache_ops` is read by `mem::transfer`.
    static constexpr bool needs_cache_ops =
        false; /**< @brief No DMA cache maintenance (plain RAM slab). */
    static constexpr bool is_isr_safe =
        true; /**< @brief `alloc`/`destroy` are O(1) free-list ops — no heap, no syscall. */
    static constexpr bool owns_bytes =
        true; /**< @brief Bytes are backend-managed (freed only on `destroy`) — durably storable. */

    [[nodiscard]] std::size_t capacity() const noexcept {
        return slot_count_;
    } /**< @brief Total slots. */
    [[nodiscard]] std::size_t available() const noexcept {
        return free_count_;
    } /**< @brief Free slots. */

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
    std::size_t header_ = 0;        // aligned sizeof(segment_t) prefixing each slot
    std::size_t stride_ = 0;        // header_ + aligned slot_payload_
    std::size_t slot_count_ = 0;
    std::size_t free_count_ = 0;
    std::size_t free_head_ = kNil;  // index of first free slot (intrusive free list)
};

}  // namespace tr::mem
