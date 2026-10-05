/**
 * @file
 * @brief CI preset — the instrumented test build with the ADR-0080 cross-thread reclamation
 *        policy, `reclaim_qsbr_t`, bound (#1810).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The `build-test-reclaim-qsbr` leg and the `tsan qsbr (slot=single_writer_slot_t)` leg of
 * `core-ci.yml` put this directory on the include path (`-DCMAKE_CXX_FLAGS=-I<this dir>`). It
 * replaced the fragments those legs used to generate inline. The test build's instrumented
 * preset yields to it, so it restates that preset's members as well. Its `slot=hazard_slot_t`
 * sibling is `core/tests/presets/reclaim-qsbr-hazard-slot`: one build has one `config_t`, so
 * the slot and the policy are bound by ONE fragment, never by two that shadow each other.
 */
#pragma once

namespace tr::graph {

/** @brief The instrumented test configuration, with a grace point spanning every thread. */
struct ci_reclaim_qsbr_config_t : default_config_t {
    using reclaim_policy_t = reclaim_qsbr_t;
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kFaultInjection = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
};

using config_t = ci_reclaim_qsbr_config_t;

}  // namespace tr::graph
