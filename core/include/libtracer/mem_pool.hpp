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

#include <atomic>
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

/**
 * @brief A thread-safe `pool_t` for the graph's `value_backend_` on a MULTI-CORE host
 *        (ADR-0060 §2), guarding the O(1) free-list with a spinlock.
 *
 * `value_backend_` MUST be thread-safe: a `segment` self-routes its reclaim on whatever
 * thread drops the last ref — typically a *reader/subscriber* thread, concurrent with a
 * writer's `alloc` (ADR-0060 §2). A single thread-safe pool (never per-stripe sharding,
 * which removes no race and adds partition imbalance) is the answer. The sync mechanism
 * is an ADR-0047 §2 module-set trait chosen by the target: this is the **spinlock**
 * variant for the multi-core host (`is_isr_safe == false`) — negligible contention on an
 * O(1) section, and it avoids the ~2 µs OS-mutex round-trip that would dominate the
 * ~120 ns free-list op. The single-core interrupt-disable critical-section variant and the
 * many-core lock-free index+tag CAS upgrade (the free list is already index-based) are the
 * recorded ADR-0060 §2 follow-ups.
 *
 * Composition over `pool_t`: a freshly-`alloc`'d segment is re-pointed to `this` with a
 * `UNKNOWN` tag, so `destroy_dispatch` routes reclaim through the virtual (locked)
 * `destroy` here instead of the devirtualized POOL fast path (which would bypass the
 * lock). `pool_t::destroy` recovers the slot from the segment's slab offset, so the
 * re-point is invisible to the inner pool. The re-point touches only the just-allocated
 * segment, which no other thread can observe until the caller publishes it.
 */
class sync_pool_t final : public mem_backend_t {
   public:
    /** @brief Carve @p slab into @p slot_payload-byte slots (see @ref pool_t), thread-safe. */
    sync_pool_t(std::span<std::byte> slab, std::size_t slot_payload,
                std::size_t align = alignof(std::max_align_t)) noexcept
        : mem_backend_t("mem_sync_pool"), inner_(slab, slot_payload, align) {}

    view::segment_t* alloc(std::size_t size, alloc_hint_t hint = alloc_hint_t::NONE) override {
        lock();
        view::segment_t* seg = inner_.alloc(size, hint);
        if (seg != nullptr) {
            seg->backend = this;               // reclaim routes back through this (locked)
            seg->btag = backend_tag::UNKNOWN;  // -> destroy_dispatch virtual fallback
        }
        unlock();
        return seg;
    }
    void destroy(view::segment_t* seg) noexcept override {
        lock();
        inner_.destroy(seg);
        unlock();
    }
    [[nodiscard]] std::size_t alignment() const noexcept override { return inner_.alignment(); }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override {
        return inner_.max_segment_size();
    }
    /** @brief UNKNOWN so `destroy_dispatch` takes the virtual (locked) `destroy`, not the
     *         devirtualized POOL fast path that would bypass the spinlock. */
    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::UNKNOWN; }

    // Module-set traits (ADR-0047 §2). Spinlock => NOT ISR-safe (the single-core
    // crit-section variant is the ISR-safe one); plain-RAM slab => no cache ops.
    static constexpr bool needs_cache_ops = false; /**< @brief Plain RAM slab. */
    static constexpr bool is_isr_safe = false;     /**< @brief Spinlock (multi-core host). */
    static constexpr bool owns_bytes = true;       /**< @brief Backend-managed, durably storable. */

    /** @brief Total slots (delegated to the inner @ref pool_t). */
    [[nodiscard]] std::size_t capacity() const noexcept { return inner_.capacity(); }

   private:
    pool_t inner_;
    std::atomic_flag lk_{};
    void lock() noexcept {
        while (lk_.test_and_set(std::memory_order_acquire)) { /* spin: O(1) section */
        }
    }
    void unlock() noexcept { lk_.clear(std::memory_order_release); }
};

}  // namespace tr::mem
