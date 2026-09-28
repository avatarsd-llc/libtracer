/**
 * @file
 * @brief `tr::esp::critical_guard_t` — the interrupt-masked reader guard the single-writer LKV
 *        slot opens on an ESP-IDF chip (#1618, RFC 0028 §5.5).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `tr::graph::single_writer_slot_t` swaps its `shared_ptr` on publish and copies it on read
 * inside `config_t::reader_guard_t`. This component's generated override fragment binds that
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
 * @brief One interrupt-masked section for the lifetime of the object.
 *
 * Every slot shares one portMUX. On a single-core chip the mux is only the critical-section
 * token; on a dual-core chip it is also the cross-core spinlock, taken with interrupts off on
 * both sides, so the holder cannot be preempted and the section stays bounded.
 */
struct critical_guard_t {
    critical_guard_t() noexcept { portENTER_CRITICAL_SAFE(&mux()); }
    ~critical_guard_t() { portEXIT_CRITICAL_SAFE(&mux()); }
    critical_guard_t(const critical_guard_t&) = delete;
    critical_guard_t& operator=(const critical_guard_t&) = delete;

    /** @brief The one mux every LKV slot in the process shares. */
    static portMUX_TYPE& mux() noexcept {
        static portMUX_TYPE m = portMUX_INITIALIZER_UNLOCKED;
        return m;
    }
};

}  // namespace tr::esp
