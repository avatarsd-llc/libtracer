/**
 * @file
 * @brief Zenoh side of the comparison: in-process (peer) pub/sub via zenoh-cpp over zenoh-c.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Sweeps the same matrix as bench_libtracer — fan-out (F subscribers on
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
         std::uint64_t budget = kDeliveryBudget, std::uint64_t latbudget = kLatencyDeliveryBudget) {
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

    // Equal warmup with bench_libtracer (1000 puts), drained before the counter reset
    // so a leftover warmup delivery never counts toward the timed phase's `want`.
    for (std::size_t i = 0; i < 1000; ++i) pubs[i % E].put(Bytes(payload));
    const auto wd = Clock::now() + std::chrono::seconds(5);
    while (recv.load(std::memory_order_relaxed) < 1000ull * F && Clock::now() < wd)
        std::this_thread::yield();

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
    // Symmetric bracketing with bench_libtracer: the timed window is put + spin-on-recv
    // ONLY. The escape hatch for a dropped sample is an iteration cap fixed OUTSIDE the
    // bracket — the old per-sample `Clock::now() + 500ms` deadline put a time_point
    // construction and per-spin clock reads inside the start..stop window, inflating
    // every Zenoh sample by ~tens of ns.
    constexpr std::uint64_t kSpinCap = 5'000'000;  // yields (~1s) before giving up a sample
    for (std::size_t i = 0; i < LATN; ++i) {
        const std::uint64_t want_i = recv.load(std::memory_order_relaxed) + F;
        const auto start = now_ns();
        pubs[i % E].put(Bytes(payload));
        for (std::uint64_t spins = 0;
             recv.load(std::memory_order_relaxed) < want_i && spins < kSpinCap; ++spins)
            std::this_thread::yield();
        lat.add(now_ns() - start);
    }
    const double pub_s = MSGS / secs;
    const double deliv_s = got / secs;
    emit("zenoh", mode, S, F, E, pub_s, deliv_s, deliv_s * static_cast<double>(S) / 1e6,
         lat.summarize());
}

/**
 * @brief Response-surface grid matching bench_libtracer's grid: size x fanout (mode `inproc`) and
 *        size x endpoints (mode `inproc-path`).
 *
 * Emits the same mode-tagged
 * RESULT line as the default run so one parser feeds the docs comparison charts.
 */
void run_grid(Session& session) {
    for (std::size_t S : kGridSizes)
        for (std::size_t F : kGridFanouts)
            run(session, S, F, 1, "inproc", kGridBudget, kGridLatBudget);
    for (std::size_t S : kGridSizes)
        for (std::size_t E : kGridEndpoints)
            run(session, S, 1, E, "inproc-path", kGridBudget, kGridLatBudget);
}

/*
 * `run_scatter` was DELETED, not fixed. It declared a publisher with no subscriber and no
 * peer, so `put()` never reached the wire — measured with strace, 5 `sendto` for 520 000
 * puts, and those five were multicast scouting beacons. It then emitted ONE K-independent
 * put rate for every K in {1,8,64,256}, so its "curve" was arithmetic. Charted against a
 * libtracer side that published `sendmsg_rate * K` (egress-only, no receiver), it produced
 * a published multi-x win on a page that described the scenario as "loopback UDP, two
 * processes" — one process, and no network on the Zenoh side at all.
 *
 * A valid composition comparison needs a real subscriber in a SECOND PROCESS on both sides
 * with deliveries counted at the receiver. That is a new benchmark; it is tracked as an
 * issue rather than left here as something to "fix".
 */

}  // namespace

int main(int argc, char** argv) {
    init_log_from_env_or("error");
    auto session = Session::open(Config::create_default());
    if (argc > 1 && std::string_view(argv[1]) == "grid") {
        run_grid(session);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "scatter") {
        std::fprintf(stderr,
                     "bench_zenoh: the `scatter` mode was removed — it measured no network "
                     "I/O (see the note above run_grid). Emitting nothing is deliberate.\n");
        return 0;
    }
    for (std::size_t F : kFanouts) run(session, kRefSize, F, kRefEndpoints, "inproc");
    for (std::size_t S : kSizes) run(session, S, kRefFanout, kRefEndpoints, "inproc");
    for (std::size_t E : kEndpoints) run(session, kRefSize, kRefFanout, E, "inproc-path");
    return 0;
}
