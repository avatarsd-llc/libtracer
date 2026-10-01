/**
 * @file
 * @brief Test fixture — the `guard_rename` STALE arm: a fragment written before #1703 that still
 *        binds the build's guard as `reader_guard_t`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This must NOT compile, and must be refused by the rename tripwire in `config.hpp`, whose
 * message names `guard_t`. Were it accepted, the library would read `guard_t` — inherited, so the
 * host `tr::mutex_guard_t` — and the guard this fragment meant to bind would be ignored without a
 * word. The guard is a well-formed `tr::guard` that never spins, so no other assertion objects.
 */
#pragma once

namespace tr::graph {

/** @brief A stand-in for an interrupt-masked critical section that never waits. */
struct stale_fixture_guard_t {
    static constexpr bool is_isr_safe = true;            /**< @brief As a masked section is. */
    static constexpr bool is_nonblocking = true;         /**< @brief As a masked section is. */
    static constexpr bool may_spin = false;              /**< @brief As a masked section is. */
    static constexpr const char* name = "stale_fixture"; /**< @brief Census name. */
    /** @brief Stand-in: nothing to open. */
    void lock() noexcept {}
    /** @brief Stand-in: nothing to close. */
    void unlock() noexcept {}
    /** @brief The one instance every address shares. */
    static stale_fixture_guard_t& for_address(const void*) noexcept {
        static stale_fixture_guard_t g;
        return g;
    }
};

/** @brief An RTOS-shaped fragment that binds its guard under the pre-#1703 member name. */
struct guard_rename_stale_config_t : default_config_t {
    static constexpr bool kSpinWaitSafe = false;
    using reader_guard_t = stale_fixture_guard_t;
    using lkv_slot_t = single_writer_slot_t;
};

using config_t = guard_rename_stale_config_t;

}  // namespace tr::graph
