/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * can_tx_pool_t (#383) — owned in-flight TX frame storage for ASYNCHRONOUS
 * can_link_t backends. A synchronous link (socketcan_link_t: the kernel copies
 * the frame inside ::write) needs none of this; a link whose driver queues the
 * frame POINTER and formats the buffer later — possibly from a tx-done ISR,
 * like ESP-IDF's esp_driver_twai node API — must keep the descriptor and
 * payload alive until the driver signals completion. Handing the driver
 * write_raw's stack storage is a use-after-free with ISR-context corruption
 * (the #383 finding); this pool is the storage the link owns instead.
 *
 * The pool is deliberately mechanism-only: fixed capacity decided at
 * construction (heap-backed, per the no-internal-static-buffers ruling,
 * ADR-0041 §5), non-blocking acquire, lock-free release. The FULL policy —
 * bounded backpressure and/or a counted drop — belongs to the owning link,
 * which pairs the pool with its platform's blocking primitive (the TWAI link
 * uses a FreeRTOS counting semaphore given back from the tx-done ISR).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

/**
 * @file
 * @brief `tr::net::can_tx_pool_t` — the owned in-flight TX slot pool behind
 *        asynchronous `can_link_t` write paths (#383).
 */

namespace tr::net {

/**
 * @brief Fixed-capacity pool of TX frame slots owned by a link until tx-done.
 *
 * Ownership protocol: @ref try_acquire hands out a free slot the caller fills
 * and submits to the driver; the slot stays IN FLIGHT — the driver may read it
 * at any point, including from ISR context — until the driver's completion
 * callback hands it back through @ref release. Release is a single
 * store-release on the slot's flag: no locks, no allocation, no system calls,
 * so it is safe to call from a tx-done ISR.
 *
 * Concurrency contract (matches the `can_link_t` seam, where writes are
 * serialized under the link's write lock):
 *  - @ref try_acquire callers are serialized EXTERNALLY (one acquirer at a
 *    time); the internal scan hint is deliberately unsynchronized.
 *  - @ref release may run concurrently with @ref try_acquire from any context
 *    (thread or ISR). The release/acquire flag pairing orders the completed
 *    transmit before the slot's next reuse.
 *
 * @tparam slot_t The link-defined slot record (driver frame descriptor +
 *                payload bytes). Must be default-constructible; the pool never
 *                reads or writes slot contents.
 */
template <typename slot_t>
class can_tx_pool_t {
   public:
    /**
     * @brief Allocate @p capacity slots, all initially free.
     *
     * Size it to the driver's maximum in-flight frame count (for the ESP TWAI
     * node: `tx_queue_depth` + the hardware TX slot), so a successful acquire
     * implies the driver can accept the frame without waiting.
     * @param capacity Number of slots; at least 1 is enforced.
     */
    explicit can_tx_pool_t(std::size_t capacity)
        : slots_(std::make_unique<slot_t[]>(capacity == 0 ? 1 : capacity)),
          in_flight_(std::make_unique<std::atomic<bool>[]>(capacity == 0 ? 1 : capacity)),
          capacity_(capacity == 0 ? 1 : capacity) {}

    can_tx_pool_t(const can_tx_pool_t&) = delete;
    can_tx_pool_t& operator=(const can_tx_pool_t&) = delete;

    /**
     * @brief Claim a free slot, or `nullptr` when every slot is in flight.
     *
     * Never blocks — exhaustion is the caller's FULL-policy decision point
     * (bounded backpressure, counted drop, …). Callers are serialized
     * externally per the class contract.
     */
    [[nodiscard]] slot_t* try_acquire() noexcept {
        for (std::size_t probe = 0; probe < capacity_; ++probe) {
            const std::size_t i = next_;
            next_ = (next_ + 1) % capacity_;
            bool expected = false;
            // acquire on success: the slot's next writes happen-after the
            // completed transmit that release()d it (store-release below).
            if (in_flight_[i].compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                      std::memory_order_relaxed)) {
                count_.fetch_add(1, std::memory_order_relaxed);
                return &slots_[i];
            }
        }
        return nullptr;
    }

    /**
     * @brief Return @p slot to the pool once the driver is done with it.
     *
     * ISR-safe: one store-release plus a relaxed counter update — no locks, no
     * allocation. @p slot must be a pointer previously handed out by
     * @ref try_acquire on this pool and not yet released.
     */
    void release(slot_t* slot) noexcept {
        const auto i = static_cast<std::size_t>(slot - slots_.get());
        count_.fetch_sub(1, std::memory_order_relaxed);
        in_flight_[i].store(false, std::memory_order_release);
    }

    /** @brief The fixed slot count chosen at construction. */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /** @brief Slots currently in flight (approximate under concurrent release). */
    [[nodiscard]] std::size_t in_flight() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

   private:
    std::unique_ptr<slot_t[]> slots_; /**< @brief The slot storage (index-parallel). */
    std::unique_ptr<std::atomic<bool>[]>
        in_flight_;                     /**< @brief Per-slot ownership flag (index-parallel). */
    std::size_t capacity_;              /**< @brief Number of slots. */
    std::size_t next_ = 0;              /**< @brief Scan hint; touched only by the (serialized)
                                             acquirer, so plain non-atomic is correct. */
    std::atomic<std::size_t> count_{0}; /**< @brief In-flight count for stats/tests. */
};

}  // namespace tr::net
