/**
 * @file
 * @brief CI preset — the instrumented test build with `reclaim_qsbr_t` bound AND the
 *        asymmetric fence opted in (`kQsbrAsymmetricFence`, #2100).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The `tsan qsbr (slot=single_writer_slot_t, fence=membarrier)` leg of `core-ci.yml` puts this
 * directory on the include path (`-DCMAKE_CXX_FLAGS=-I<this dir>`) and sets
 * `LT_EXPECT_MEMBARRIER=1`, so the leg FAILS when the runner refuses `membarrier(2)` instead of
 * silently testing the fenced entry again. It runs the plain bracket entry and single-flight drain
 * passes; its siblings `reclaim-qsbr` and `reclaim-qsbr-hazard-slot` run the default fenced entry.
 * One build has one `config_t`, so this ONE fragment binds the policy and the fence; the test
 * build's instrumented preset yields to it, so it restates that preset's members as well.
 */
#pragma once

namespace tr::graph {

/** @brief The instrumented test configuration, with a grace point spanning every thread and the
 *         reclaim side's `membarrier` in place of the bracket entry's fence. */
struct ci_reclaim_qsbr_membarrier_config_t : default_config_t {
    using reclaim_policy_t = reclaim_qsbr_t;
    static constexpr bool kQsbrAsymmetricFence = true;
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kFaultInjection = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
};

using config_t = ci_reclaim_qsbr_membarrier_config_t;

}  // namespace tr::graph
