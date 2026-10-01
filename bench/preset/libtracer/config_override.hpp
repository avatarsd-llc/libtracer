/**
 * @file
 * @brief The bench preset — the lean defaults with the two link modules opted in (#1670).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `default_config_t::kBusLinks` and `kSelfHealLinks` are `false` since v0.17.0. Several benches
 * measure exactly those modules — peer-named tcp/ws listeners (`bench_tcp_fanin`,
 * `bench_tcp_peer_scaling`, `bench_conn_ram`), a CAN link (`bench_conn_ram`) and the
 * engine-managed DIAL (`bench_ram_census_tcp`) — so `bench/CMakeLists.txt` puts this directory
 * first on `libtracer`'s include path and compiles the matching TUs
 * (`LIBTRACER_TRANSPORT_CAN`, `LIBTRACER_SELF_HEAL_LINKS`). Everything else stays at the
 * default, so the perf gate and the symbol ratchet still measure the library a node ships.
 *
 * `-DLIBTRACER_INSTRUMENT_COUNTERS=ON` binds the core test build's instrumented preset
 * instead, which carries the same two opt-ins plus the counters.
 */
#pragma once

namespace tr::graph {

/** @brief The defaults, with the peer-named tier and the S5 liveness engine compiled in. */
struct bench_config_t : default_config_t {
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
};

using config_t = bench_config_t;

}  // namespace tr::graph
