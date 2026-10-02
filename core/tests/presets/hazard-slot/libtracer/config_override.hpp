/**
 * @file
 * @brief CI preset — the instrumented test build with the hazard-pointer LKV slot bound (#1722).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The `slot=hazard_slot_t` legs of `core-ci.yml` (build-test, sanitizers, tsan) put this
 * directory on the include path (`-DCMAKE_CXX_FLAGS=-I<this dir>`). It replaced the removed
 * `-DLIBTRACER_LKV_SLOT=hazard_slot_t` configure option. The test build's instrumented preset
 * yields to it, so it restates that preset's three members as well.
 */
#pragma once

namespace tr::graph {

/** @brief The instrumented test configuration, with the lock-free hazard slot under graph_t. */
struct ci_hazard_slot_config_t : default_config_t {
    using lkv_slot_t = hazard_slot_t;
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
};

using config_t = ci_hazard_slot_config_t;

}  // namespace tr::graph
