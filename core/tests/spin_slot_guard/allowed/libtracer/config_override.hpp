/**
 * @file
 * @brief Test fixture — the `spin_slot_guard` ALLOWED arm: an RTOS-shaped single-writer build.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Spin-waiting refused, the single-writer contract stated, and the slot bound to the policy an
 * RTOS target binds. This must compile: the guard may not reject the configuration it exists
 * to steer builds towards.
 */
#pragma once

namespace tr::graph {

/** @brief A stand-in for an interrupt-masked critical section: a @ref reader_guard that opens
 *         nothing and never waits. */
struct fixture_guard_t {
    static constexpr bool is_isr_safe = true;      /**< @brief As a masked section is. */
    static constexpr bool is_nonblocking = true;   /**< @brief As a masked section is. */
    static constexpr bool may_spin = false;        /**< @brief As a masked section is. */
    static constexpr const char* name = "fixture"; /**< @brief Census name. */
    /** @brief Stand-in: nothing to open. */
    void lock() noexcept {}
    /** @brief Stand-in: nothing to close. */
    void unlock() noexcept {}
    /** @brief The one instance every address shares. */
    static fixture_guard_t& for_address(const void*) noexcept {
        static fixture_guard_t g;
        return g;
    }
};

/** @brief The defaults as a single-core RTOS target overrides them. */
struct spin_slot_allowed_config_t : default_config_t {
    static constexpr bool kSpinWaitSafe = false;
    static constexpr bool kSingleWriter = true;
    using reader_guard_t = fixture_guard_t;
    using lkv_slot_t = single_writer_slot_t;
};

using config_t = spin_slot_allowed_config_t;

}  // namespace tr::graph
