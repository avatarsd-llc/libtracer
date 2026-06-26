// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Zenoh side of the comparison: in-process (peer) pub/sub via zenoh-cpp over
// zenoh-c. A single Session with a publisher and subscriber on the same key
// expression (intra-session local delivery) — the closest Zenoh analogue to
// libtracer's in-process path. Same payload sizes and message counts as
// bench_libtracer.cpp, same RESULT line format. See bench/README.md.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "zenoh.hxx"

using namespace zenoh;
using namespace bench;

namespace {

void run_size(Session& session, std::size_t S) {
    std::atomic<std::uint64_t> recv{0};
    auto sub = session.declare_subscriber(
        KeyExpr("bench/zenoh"),
        [&](const Sample&) { recv.fetch_add(1, std::memory_order_relaxed); }, closures::none);
    auto pub = session.declare_publisher(KeyExpr("bench/zenoh"));
    const std::vector<std::uint8_t> payload(S, 0xAB);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let pub<->sub match

    // throughput
    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < kThroughputMsgs; ++i) pub.put(Bytes(payload));
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (recv.load(std::memory_order_relaxed) < kThroughputMsgs && Clock::now() < deadline)
        std::this_thread::yield();
    const double secs = (now_ns() - t0) / 1e9;
    const std::uint64_t got = recv.load(std::memory_order_relaxed);
    if (got < kThroughputMsgs)
        std::fprintf(stderr, "[zenoh] size=%zu delivered %llu/%zu (best-effort drops)\n", S,
                     static_cast<unsigned long long>(got), kThroughputMsgs);

    // latency, one message at a time (no pipelining)
    Latency lat;
    for (std::size_t i = 0; i < kLatencyMsgs; ++i) {
        const std::uint64_t before = recv.load(std::memory_order_relaxed);
        const auto start = now_ns();
        pub.put(Bytes(payload));
        const auto ld = Clock::now() + std::chrono::milliseconds(200);
        while (recv.load(std::memory_order_relaxed) == before && Clock::now() < ld)
            std::this_thread::yield();
        lat.add(now_ns() - start);
    }
    emit("zenoh", "inproc", S, got / secs, got * static_cast<double>(S) / secs / 1e6,
         lat.summarize());
}

}  // namespace

int main() {
    init_log_from_env_or("error");
    auto session = Session::open(Config::create_default());
    for (std::size_t S : kSizes) run_size(session, S);
    return 0;
}
