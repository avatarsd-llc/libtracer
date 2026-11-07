/**
 * @file
 * @brief CI preset: the NARROW combined leg, the heap-rooted MCU default and the zero-cost
 *        `reclaim_strict_t` bound together (#2449).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The landing run's `build-test (... root=heap reclaim=strict)` leg of `core-ci.yml` puts this
 * directory on the include path (`-DCMAKE_CXX_FLAGS=-I<this dir>`). One build has one
 * `config_t`, so the two single-setting presets (`heap-root`, `reclaim-strict`) cannot be
 * stacked; this fragment is their union, member for member. Each of them still runs alone
 * nightly and on every release tag.
 */
#pragma once

#include <cstddef>

namespace tr::graph {

/** @brief The instrumented test configuration with the heap as the root and `reclaim_strict_t`. */
struct ci_narrow_config_t : default_config_t {
    using reclaim_policy_t = reclaim_strict_t;
    static constexpr bool kSlabPool = false;
    static constexpr std::size_t kArenaBytes = 0;
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
    /** @brief A POSIX host receives a borrowed UDP datagram on the stack (#2142). */
    static constexpr std::size_t kUdpStackScratchBytes = 16384;
    static constexpr bool kFaultInjection = true;
    static constexpr bool kCreationHooks = true;
};

using config_t = ci_narrow_config_t;

}  // namespace tr::graph
