/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Zenoh side of the comparison: in-process (peer) pub/sub via zenoh-cpp over
 * zenoh-c. Sweeps the same matrix as bench_libtracer — fan-out (F subscribers on
 * one key expression), payload size, and endpoint count (E key expressions) —
 * and emits the same RESULT line. Intra-session local delivery is the closest
 * Zenoh analogue to libtracer's in-process path. See bench/README.md.
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "zenoh.hxx"

using namespace zenoh;
using namespace bench;

namespace {

void run(Session& session, std::size_t S, std::size_t F, std::size_t E, const char* mode,
         bool csv = false, std::uint64_t budget = kDeliveryBudget,
         std::uint64_t latbudget = kLatencyDeliveryBudget) {
    std::atomic<std::uint64_t> recv{0};
    std::vector<Subscriber<void>> subs;
    std::vector<Publisher> pubs;
    subs.reserve(F * E);
    pubs.reserve(E);
    for (std::size_t e = 0; e < E; ++e) {
        const std::string ke = "bench/zenoh/" + std::to_string(e);
        for (std::size_t f = 0; f < F; ++f) {
            subs.push_back(session.declare_subscriber(
                KeyExpr(ke), [&](const Sample&) { recv.fetch_add(1, std::memory_order_relaxed); },
                closures::none));
        }
        pubs.push_back(session.declare_publisher(KeyExpr(ke)));
    }
    const std::vector<std::uint8_t> payload(S, 0xAB);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let pub<->sub match

    const std::size_t MSGS = publishes_for(F, budget);
    const std::uint64_t want = static_cast<std::uint64_t>(MSGS) * F;

    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) pubs[i % E].put(Bytes(payload));
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (recv.load(std::memory_order_relaxed) < want && Clock::now() < deadline)
        std::this_thread::yield();
    const double secs = (now_ns() - t0) / 1e9;
    const std::uint64_t got = recv.load(std::memory_order_relaxed);
    if (got < want)
        std::fprintf(stderr, "[zenoh] S=%zu F=%zu E=%zu delivered %llu/%llu (best-effort drops)\n",
                     S, F, E, static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));

    Latency lat;
    const std::size_t LATN = publishes_for(F, latbudget);
    for (std::size_t i = 0; i < LATN; ++i) {
        const std::uint64_t before = recv.load(std::memory_order_relaxed);
        const auto start = now_ns();
        pubs[i % E].put(Bytes(payload));
        const auto ld = Clock::now() + std::chrono::milliseconds(500);
        while (recv.load(std::memory_order_relaxed) < before + F && Clock::now() < ld)
            std::this_thread::yield();
        lat.add(now_ns() - start);
    }
    const double pub_s = MSGS / secs;
    const double deliv_s = got / secs;
    if (csv)
        emit_csv("zenoh", S, F, E, pub_s, deliv_s, lat.summarize());
    else
        emit("zenoh", mode, S, F, E, pub_s, deliv_s, deliv_s * static_cast<double>(S) / 1e6,
             lat.summarize());
}

// Response-surface grid matching bench_libtracer's grid mode (CSV for plot.py).
void run_grid(Session& session) {
    emit_csv_header();
    for (std::size_t S : kGridSizes)
        for (std::size_t F : kGridFanouts)
            run(session, S, F, 1, "grid", true, kGridBudget, kGridLatBudget);
    for (std::size_t S : kGridSizes)
        for (std::size_t E : kGridEndpoints)
            run(session, S, 1, E, "grid", true, kGridBudget, kGridLatBudget);
}

}  // namespace

int main(int argc, char** argv) {
    init_log_from_env_or("error");
    auto session = Session::open(Config::create_default());
    if (argc > 1 && std::string_view(argv[1]) == "grid") {
        run_grid(session);
        return 0;
    }
    for (std::size_t F : kFanouts) run(session, kRefSize, F, kRefEndpoints, "inproc");
    for (std::size_t S : kSizes) run(session, S, kRefFanout, kRefEndpoints, "inproc");
    for (std::size_t E : kEndpoints) run(session, kRefSize, kRefFanout, E, "inproc-path");
    return 0;
}
