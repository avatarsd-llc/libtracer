/**
 * @file
 * @brief CI preset: the WIDE combined leg, the full ACL host policy and the hazard-pointer LKV
 *        slot bound together (#2449).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The landing run's `build-test (acl_full=ON, slot=hazard_slot_t)` leg and the
 * `slot=hazard_slot_t acl_full=ON` legs of `sanitizers` and `tsan` in `core-ci.yml` put this
 * directory on the include path (`-DCMAKE_CXX_FLAGS=-I<this dir>`). One build has one
 * `config_t`, so the two single-setting presets (`acl-full`, `hazard-slot`) cannot be stacked;
 * this fragment is their union, member for member. Each of them still runs alone nightly and on
 * every release tag.
 */
#pragma once

namespace tr::graph {

/** @brief The instrumented test configuration with `full_acl_policy_t` and `hazard_slot_t`. */
struct ci_wide_config_t : default_config_t {
    using acl_policy_t = full_acl_policy_t;
    using lkv_slot_t = hazard_slot_t;
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kFaultInjection = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
    /** @brief A POSIX host receives a borrowed UDP datagram on the stack (#2142). */
    static constexpr std::size_t kUdpStackScratchBytes = 16384;
};

using config_t = ci_wide_config_t;

}  // namespace tr::graph
