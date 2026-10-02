/**
 * @file
 * @brief The guarded-sequence preset — the host's mutex guard behind a section counter, and
 *        the write sequence forced onto its guarded `rmw_counter_t` binding (#1715).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A host's 32-bit atomics are native, so a stock host build never runs the guarded bump a core
 * without atomic RMW (ESP32-C3, Cortex-M0) compiles to, nor the publish that fuses it into the
 * LKV slot's section. `core/tests/CMakeLists.txt` builds a separate copy of the required core,
 * `libtracer_guarded_seq`, with this directory FIRST on its include path, and links the
 * #1715 tests against it. Nothing else links that library: one configuration per library, so
 * the two never mix in one binary.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "libtracer/guard_mutex.hpp"

namespace tr::graph {

/**
 * @brief `tr::mutex_guard_t`, counting the sections it opens on the calling thread.
 *
 * The lock itself is the host guard's: each stripe IS a `mutex_guard_t`, striped by the same
 * address hash. Only `lock()` adds a thread-local count, which is how a test sees how many
 * guard sections one publish opens.
 */
struct counting_guard_t : ::tr::mutex_guard_t {
    static constexpr const char* name = "counting_guard"; /**< @brief Census name. */

    /** @brief Sections opened on this thread since it started (or since the test reset it). */
    static inline thread_local std::size_t sections = 0;

    /** @brief Count one section, then take the host guard's lock. */
    void lock() noexcept {
        ++sections;
        ::tr::mutex_guard_t::lock();
    }

    /** @brief The stripe that serializes sections over @p at — `mutex_guard_t`'s own hash. */
    static counting_guard_t& for_address(const void* at) noexcept {
        static counting_guard_t table[kStripes];
        auto a = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(at));
        a ^= a >> 17;
        a *= 0x9E3779B97F4A7C15ull;
        return table[(a >> 58) % kStripes];
    }
};

/** @brief The defaults, with the counting guard bound and the guarded counter forced. */
struct guarded_seq_config_t : default_config_t {
    using guard_t = counting_guard_t;
    static constexpr bool kForceGuardedRmw = true;
};

using config_t = guarded_seq_config_t;

}  // namespace tr::graph
