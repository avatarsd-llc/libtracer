/**
 * @file
 * @brief `tr::esp::critical_guard_t` — the interrupt-masked reader guard the single-writer LKV
 *        slot opens on an ESP-IDF chip (#1618, RFC 0028 §5.5).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `tr::graph::single_writer_slot_t` swaps its `value_t*` on publish and retains it on read
 * inside `config_t::reader_guard_t`, and `tr::mem::synchronized_pool_t` edits its free list
 * inside the same type. This component's generated override fragment binds that
 * guard to this type on every chip target. A critical section cannot be preempted, so a
 * high-priority reader can never catch a low-priority writer half-way through the swap and wait
 * on it — the wait that, with the spin-locked `std::atomic<std::shared_ptr>` slot it replaces,
 * spun until the task watchdog fired.
 *
 * It includes only FreeRTOS, never a libtracer header: the override fragment is included FROM
 * `libtracer/config.hpp`, so anything this header pulled in from libtracer would be a cycle.
 *
 * ISR USE. `portENTER_CRITICAL_SAFE` picks the task- or ISR-context primitive, so a read from an
 * ISR is safe. The sections are a pointer swap and a refcount increment; the displaced value is
 * released after the guard closes, so no destructor or allocator runs with interrupts masked.
 */
#pragma once

#include "freertos/FreeRTOS.h"

namespace tr::esp {

/**
 * @brief The interrupt-masked critical section — the ONE guard an ESP-IDF chip binds for both
 *        the LKV slot and the fixed-slot pool (RFC 0028 §5.5, slice 10).
 *
 * A lock object over its own portMUX, modelling `tr::graph::reader_guard`. Every LKV slot
 * shares the one instance @ref for_address returns; a `tr::mem::synchronized_pool_t` owns its
 * own (`tr::esp::critical_pool_t`). On a single-core chip the mux is only the critical-section
 * token; on a dual-core chip it is also the cross-core spinlock, taken with interrupts off on
 * both sides, so the holder cannot be preempted and the section stays bounded — which is why
 * @ref may_spin is `false` even there.
 *
 * Before slice 10 the pool's spelling of this type was a separate `portmux_sync_t`; the two are
 * one type now.
 */
struct critical_guard_t {
    static constexpr bool is_isr_safe = true; /**< @brief Interrupts off — ISR-callable. */
    /** @brief Interrupt-disable — no heap, no syscall, no OS wait (#928). */
    static constexpr bool is_nonblocking = true;
    /** @brief The holder cannot be preempted, so any cross-core wait is bounded. */
    static constexpr bool may_spin = false;
    static constexpr const char* name = "critical_guard"; /**< @brief Census name. */

    critical_guard_t() noexcept = default;
    critical_guard_t(const critical_guard_t&) = delete;
    critical_guard_t& operator=(const critical_guard_t&) = delete;

    /** @brief Enter the critical section (interrupts off). */
    void lock() noexcept { portENTER_CRITICAL_SAFE(&mux_); }
    /** @brief Leave the critical section. */
    void unlock() noexcept { portEXIT_CRITICAL_SAFE(&mux_); }

    /** @brief The one instance every LKV slot in the process shares. */
    static critical_guard_t& for_address(const void*) noexcept {
        static critical_guard_t g;
        return g;
    }

   private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED; /**< @brief This section's token. */
};

}  // namespace tr::esp
