/**
 * @file
 * @brief CI preset — the instrumented test build with `reclaim_qsbr_t` AND the hazard-pointer
 *        LKV slot bound (#1810).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The `tsan qsbr (slot=hazard_slot_t)` leg of `core-ci.yml` puts this directory on the include
 * path (`-DCMAKE_CXX_FLAGS=-I<this dir>`). That leg exercises the hazard slot's self-drain
 * under the cross-thread grace period. One build has one `config_t`, so this ONE fragment binds
 * both the slot and the policy; a second fragment on the include path would shadow it rather
 * than compose with it. The test build's instrumented preset yields to it, so it restates that
 * preset's members as well.
 */
#pragma once

namespace tr::graph {

/** @brief The instrumented test configuration, with the hazard slot and a grace point spanning
 *         every thread. */
struct ci_reclaim_qsbr_hazard_slot_config_t : default_config_t {
    using lkv_slot_t = hazard_slot_t;
    using reclaim_policy_t = reclaim_qsbr_t;
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kFaultInjection = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
};

using config_t = ci_reclaim_qsbr_hazard_slot_config_t;

}  // namespace tr::graph
