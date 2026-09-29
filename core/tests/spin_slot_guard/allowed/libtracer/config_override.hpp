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

/** @brief A stand-in for an interrupt-masked critical section; the guard never opens it. */
struct fixture_guard_t {};

/** @brief The defaults as a single-core RTOS target overrides them. */
struct spin_slot_allowed_config_t : default_config_t {
    static constexpr bool kSpinWaitSafe = false;
    static constexpr bool kSingleWriter = true;
    using reader_guard_t = fixture_guard_t;
    using lkv_slot_t = single_writer_slot_t;
};

using config_t = spin_slot_allowed_config_t;

}  // namespace tr::graph
