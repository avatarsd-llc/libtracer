/**
 * @file
 * @brief CI preset — the instrumented test build with the asymmetric fence opted in
 *        (`kQsbrAsymmetricFence`, #2100) under the default `reclaim_qsbr_t`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The `tsan (fence=membarrier)` leg of `core-ci.yml` puts this
 * directory on the include path (`-DCMAKE_CXX_FLAGS=-I<this dir>`) and sets
 * `LT_EXPECT_MEMBARRIER=1`, so the leg FAILS when the runner refuses `membarrier(2)` instead of
 * silently testing the fenced entry again. It runs the plain bracket entry and single-flight drain
 * passes; every other leg runs the default fenced entry. One build has one `config_t`, so the
 * test build's instrumented preset yields to this fragment, which restates its members.
 */
#pragma once

namespace tr::graph {

/** @brief The instrumented test configuration, with the reclaim side's `membarrier` in place
 *         of the bracket entry's fence. */
struct ci_reclaim_qsbr_membarrier_config_t : default_config_t {
    static constexpr bool kQsbrAsymmetricFence = true;
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kFaultInjection = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
    /** @brief A POSIX host receives a borrowed UDP datagram on the stack (#2142). */
    static constexpr std::size_t kUdpStackScratchBytes = 16384;
};

using config_t = ci_reclaim_qsbr_membarrier_config_t;

}  // namespace tr::graph
