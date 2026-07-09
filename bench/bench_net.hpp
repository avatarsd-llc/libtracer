/**
 * @file
 * @brief Shared scaffolding for the TWO-PROCESS network benchmark (libtracer-UDP vs Zenoh-UDP).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The publisher and subscriber are separate processes on localhost;
 * each message's payload self-identifies its phase and carries a send timestamp,
 * so the subscriber needs no out-of-band coordination and tolerates UDP drops.
 *
 * payload = [ send_ts : 8 bytes LE CLOCK_MONOTONIC ns ][ phase : 1 byte ][ filler ]
 *   phase 0 = latency  (publisher paces these → no queueing; one-way latency)
 *   phase 1 = throughput (publisher blasts these; rate = count / span)
 *   phase 2 = EOF      (end of this size; subscriber prints the RESULT)
 *
 * CLOCK_MONOTONIC is system-wide on Linux, so one-way latency (recv_ts - send_ts)
 * is valid across the two processes on one machine.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "bench_common.hpp"

namespace bench::net {

inline constexpr std::size_t kSizes[] = {16, 256, 1024, 8192};  // ≥ 9 (ts+phase)
inline constexpr std::size_t kLatencyMsgs = 4000;
inline constexpr std::size_t kThroughputMsgs = 20000;
inline constexpr std::uint64_t kPaceNs = 150000;  // 150 µs between latency sends
/**
 * @brief Pause after each size's throughput blast so its backlog fully drains before the next
 *        size's PACED latency probes — otherwise, on an ordered transport (TCP/WS), the latency
 *        samples queue behind the still-draining blast and read as milliseconds, not µs.
 */
inline constexpr std::uint64_t kDrainMs = 600;

enum Phase : std::uint8_t { kLatency = 0, kThroughput = 1, kEof = 2 };

/** @brief Fill `out` (size S) with a fresh message of the given phase. */
inline void make_payload(std::vector<std::uint8_t>& out, std::size_t S, Phase phase) {
    out.assign(S, 0xAB);
    const std::uint64_t ts = now_ns();
    for (int i = 0; i < 8; ++i) out[static_cast<std::size_t>(i)] = (ts >> (8 * i)) & 0xFF;
    out[8] = static_cast<std::uint8_t>(phase);
}

inline std::uint64_t read_ts(std::span<const std::byte> p) {
    std::uint64_t ts = 0;
    for (std::size_t i = 0; i < 8 && i < p.size(); ++i)
        ts |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
    return ts;
}

/**
 * @brief Subscriber-side accumulator.
 *
 * Mutated only on the single receive thread. `mode`
 * tags the transport (e.g. `net-udp`) so the two-process transport benches share the
 * in-process RESULT parser; it defaults to `net` for the legacy single-transport use.
 */
struct SubState {
    explicit SubState(std::string sys, std::string mode = "net")
        : system(std::move(sys)), mode(std::move(mode)) {}
    std::string system;
    std::string mode;
    std::size_t cur_size = 0;
    Latency lat;
    std::uint64_t thru_first = 0, thru_last = 0, thru_count = 0;

    /**
     * @brief Feed one received payload (raw application bytes).
     *
     * Prints a RESULT on EOF.
     */
    void on_payload(std::span<const std::byte> p) {
        if (p.size() < 9) return;
        const std::uint64_t now = now_ns();
        const std::uint64_t ts = read_ts(p);
        const auto phase = static_cast<Phase>(std::to_integer<std::uint8_t>(p[8]));
        if (phase == kEof) {
            if (cur_size && thru_count) {
                const double secs = (thru_last - thru_first) / 1e9;
                const double mps = secs > 0 ? thru_count / secs : 0;
                emit(system.c_str(), mode.c_str(), cur_size, 1, 1, mps, mps,
                     thru_count * static_cast<double>(cur_size) / (secs > 0 ? secs : 1) / 1e6,
                     lat.summarize());
            }
            cur_size = 0;
            thru_first = thru_last = thru_count = 0;
            lat = Latency{};
            return;
        }
        cur_size = p.size();
        if (phase == kLatency) {
            lat.add(now - ts);  // one-way
        } else {                // throughput
            if (thru_count == 0) thru_first = now;
            thru_last = now;
            ++thru_count;
        }
    }
};

}  // namespace bench::net
