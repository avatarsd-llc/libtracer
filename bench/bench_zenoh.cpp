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

/**
 * @brief How a point spells its destination — the #1485 addendum-C axis.
 *
 * The two spellings are the SAME semantic operation reaching the SAME subscribers; what differs
 * is whether the destination was resolved once at setup or is resolved inside every iteration.
 * Keeping them apart is the whole point: the pre-existing comparison put libtracer's
 * resolve-per-write row against Zenoh's declared-publisher row, so a resolution term sat inside
 * one arm and nowhere in the other.
 */
enum class addr_t {
    BOUND, /**< @brief A declared `Publisher` — Zenoh's pre-bound handle. */
    ADDR   /**< @brief `Session::put` against a pre-built `KeyExpr`, resolved on every put. */
};

void run(Session& session, std::size_t S, std::size_t F, std::size_t E, const char* mode,
         std::uint64_t budget = kDeliveryBudget, std::uint64_t latbudget = kLatencyDeliveryBudget,
         addr_t addr = addr_t::BOUND) {
    std::atomic<std::uint64_t> recv{0};
    std::vector<Subscriber<void>> subs;
    std::vector<Publisher> pubs;
    std::vector<KeyExpr> kes;
    subs.reserve(F * E);
    pubs.reserve(E);
    kes.reserve(E);
    for (std::size_t e = 0; e < E; ++e) {
        const std::string ke = "bench/zenoh/" + std::to_string(e);
        for (std::size_t f = 0; f < F; ++f) {
            subs.push_back(session.declare_subscriber(
                KeyExpr(ke), [&](const Sample&) { recv.fetch_add(1, std::memory_order_relaxed); },
                closures::none));
        }
        if (addr == addr_t::BOUND)
            pubs.push_back(session.declare_publisher(KeyExpr(ke)));
        else
            kes.push_back(KeyExpr(ke));
    }
    const std::vector<std::uint8_t> payload(S, 0xAB);
    // The ONE line the two spellings differ by. The `KeyExpr` objects are pre-built, exactly as
    // libtracer's `topics-addr` arm pre-parses its `path_t`s: what is being compared is
    // per-operation RESOLUTION, not per-operation string parsing, and charging one engine for a
    // parse the other hoisted is how the previous comparison went wrong in the first place.
    const auto publish = [&](std::size_t i) {
        if (addr == addr_t::BOUND)
            pubs[i % E].put(Bytes(payload));
        else
            session.put(kes[i % E], Bytes(payload));
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let pub<->sub match

    const std::size_t MSGS = publishes_for(F, budget);
    const std::uint64_t want = static_cast<std::uint64_t>(MSGS) * F;

    // Equal warmup with bench_libtracer (1000 puts), drained before the counter reset
    // so a leftover warmup delivery never counts toward the timed phase's `want`.
    for (std::size_t i = 0; i < 1000; ++i) publish(i);
    const auto wd = Clock::now() + std::chrono::seconds(5);
    while (recv.load(std::memory_order_relaxed) < 1000ull * F && Clock::now() < wd)
        std::this_thread::yield();

    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) publish(i);
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
        publish(i);
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

/**
 * @brief #1485 addendum C — the TOPIC-COUNT arm, in both address spellings.
 *
 * @p bound_first flips which spelling runs first at each topic count. Arm order is not free on a
 * shared host: an always-same-first ordering manufactured an apparent 12.55-vs-8.07 M/s win here
 * that vanished on the flip. `run_topics.sh` executes both orders and reduces with
 * `best_of_rounds.py`, so a point whose two orders disagree on the sign of a trend is visible as
 * unresolved rather than published as a verdict.
 */
void run_topics(Session& session, bool bound_first) {
    for (const std::size_t E : kTopicLadder) {
        const auto bound = [&] {
            run(session, kRefSize, kRefFanout, E, "topics-bound", kDeliveryBudget,
                kLatencyDeliveryBudget, addr_t::BOUND);
        };
        const auto by_addr = [&] {
            run(session, kRefSize, kRefFanout, E, "topics-addr", kDeliveryBudget,
                kLatencyDeliveryBudget, addr_t::ADDR);
        };
        if (bound_first) {
            bound();
            by_addr();
        } else {
            by_addr();
            bound();
        }
    }
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
    // Multicast scouting OFF, as `bench_zenoh_net` already does. This is an
    // IN-PROCESS comparison: libtracer runs no discovery subsystem at all, so
    // leaving Zenoh's on puts a background thread and real multicast traffic
    // inside the timed window on one side only. Measured on the default config:
    // 23 sendto + 35 receives across one `grid` run. Small, but it is a
    // fairness asymmetry in a chart whose whole premise is like-for-like.
    Config cfg = Config::create_default();
    cfg.insert_json5("scouting/multicast/enabled", "false");
    auto session = Session::open(std::move(cfg));
    if (argc > 1 && std::string_view(argv[1]) == "grid") {
        run_grid(session);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "topics") {
        run_topics(session, true);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "topics-rev") {
        run_topics(session, false);
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
