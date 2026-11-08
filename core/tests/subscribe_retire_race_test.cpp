/**
 * @file
 * @brief `subscribe` against `retire` on the same vertex: the subscriber's scan of parked edge
 *        arrays and the retire's slot-table swap must not race (#1919).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every edge mutation ends with `scan_retired_edges`, run outside the stripe lock, which reads
 * the slot table's source to free the arrays no reader pins. `retire` moves the slot table out
 * under the stripe lock. Before #1919 it then move-assigned a fresh empty table into the slot,
 * rewriting the source field the scan was reading: the same value, but a data race under the
 * C++ memory model, which ThreadSanitizer reports. This test runs one thread that subscribes and
 * unsubscribes `/v` against one that retires and re-registers it. Under TSan (the tsan CI lanes
 * run this suite, under `setarch -R`) the run itself is the judgement.
 *
 * The race has to be live, and a loaded host can starve one side: a retirer that re-registers
 * `/v` and retires it again straight away leaves a window the subscriber can miss every time, and
 * a run then lands no subscribe at all (#2355, #2645). So the race is live by construction, not
 * by scheduling. Each retire cycle opens an epoch once `/v` is registered again, and does not
 * retire until the subscriber has announced an attempt in that epoch. The subscriber announces
 * its epoch just before it calls `subscribe`, so every retire overlaps at least one attempt that
 * started with `/v` present: the retire runs while that subscribe, its unsubscribe and their
 * scans of the parked edge arrays are in flight, and the subscribe can land or lose the race. It
 * is never strict alternation: the subscriber never waits for the retirer and keeps attempting
 * through the retire. A run that has landed nothing after its count of cycles gets one more
 * count, bounded by the retirer's own progress, never by a clock; a run that still lands nothing
 * is a race that is live and failing, and the check reports it.
 *
 * The iteration count defaults to @ref kIterations; a first argument overrides it.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::value_t;
using tr::testing::check;

/** @brief Iterations of each racer in a default run. A first argument overrides it. */
constexpr long kIterations = 20000;

/** @brief A subscriber that does nothing: the race is on the edge table, not on delivery. */
void sink(void*, const value_t&) {}

void test_subscribe_against_retire(long iterations) {
    std::printf("subscribe and unsubscribe against retire of the same vertex (#1919):\n");
    graph_t g;
    (void)g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    // A start barrier: neither racer begins until both are running.
    std::atomic<int> ready{0};
    const auto arrive = [&] {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    };
    // The handshake: the retirer opens an epoch once `/v` is registered, the subscriber announces
    // the epoch it saw before each attempt, and the retirer blocks on `attempted` (a futex wait,
    // no clock) until that epoch is announced, so each retire overlaps an attempt that started
    // with `/v` present. Blocking rather than yielding hands a single CPU to the subscriber.
    std::atomic<long> opened{0};
    std::atomic<long> attempted{0};
    std::atomic<bool> retirer_done{false};
    std::atomic<long> subscribed{0};
    long retired = 0;
    std::thread subscriber([&] {
        arrive();
        long announced = 0;
        while (!retirer_done.load(std::memory_order_acquire)) {
            const long e = opened.load(std::memory_order_acquire);
            const bool fresh = e != announced;
            if (fresh) {
                announced = e;
                attempted.store(e, std::memory_order_release);
                attempted.notify_one();
            }
            if (auto s = g.subscribe(path_t("/v"), &sink, nullptr)) {
                subscribed.fetch_add(1, std::memory_order_relaxed);
                (void)g.unsubscribe(*s);
            }
            // On one CPU the woken retirer would otherwise wait out this thread's time slice.
            if (fresh) std::this_thread::yield();
        }
    });
    std::thread retirer([&] {
        arrive();
        // Past its count, a run that landed nothing gets one more count of cycles.
        for (long i = 1; i <= iterations ||
                         (subscribed.load(std::memory_order_relaxed) == 0 && i <= 2 * iterations);
             ++i) {
            opened.store(i, std::memory_order_release);
            for (long seen = attempted.load(std::memory_order_acquire); seen < i;
                 seen = attempted.load(std::memory_order_acquire))
                attempted.wait(seen, std::memory_order_acquire);
            if (auto v = g.find(path_t("/v").key()); v && g.retire(*v)) ++retired;
            (void)g.try_register_vertex(path_t("/v"), role_t::STORED_VALUE);
        }
        retirer_done.store(true, std::memory_order_release);
    });
    subscriber.join();
    retirer.join();
    g.collect();
    const long landed = subscribed.load(std::memory_order_relaxed);
    std::printf("    %ld subscribes landed, %ld retires\n", landed, retired);
    check(landed > 0 && retired > 0, "both racers made progress (the race was live)");
}

}  // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : kIterations;
    test_subscribe_against_retire(iterations);
    return tr::testing::summary("subscribe_retire_race");
}
