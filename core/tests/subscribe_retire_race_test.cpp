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
 * unsubscribes `/v` against one that retires and re-registers it. In an ordinary build it checks
 * that both sides made progress; under TSan (the tsan CI lanes run this suite, under
 * `setarch -R`) the run itself is the judgement.
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
    std::atomic<bool> go{false};
    long subscribed = 0;
    long retired = 0;
    std::thread subscriber([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (long i = 0; i < iterations; ++i) {
            if (auto s = g.subscribe(path_t("/v"), &sink, nullptr)) {
                ++subscribed;
                (void)g.unsubscribe(*s);
            }
        }
    });
    std::thread retirer([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (long i = 0; i < iterations; ++i) {
            if (auto v = g.find(path_t("/v").key()); v && g.retire(*v)) ++retired;
            (void)g.try_register_vertex(path_t("/v"), role_t::STORED_VALUE);
        }
    });
    go.store(true, std::memory_order_release);
    subscriber.join();
    retirer.join();
    g.collect();
    std::printf("    %ld subscribes landed, %ld retires\n", subscribed, retired);
    check(subscribed > 0 && retired > 0, "both racers made progress (the race was live)");
}

}  // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : kIterations;
    test_subscribe_against_retire(iterations);
    return tr::testing::summary("subscribe_retire_race");
}
