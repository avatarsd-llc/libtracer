/**
 * @file
 * @brief `delivery_mode` against a concurrent assign: the sweep-set exclusion and the
 *        plain-byte data race (#895, RFC-0008 §B/§C).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * "What does the next covering propagate deliver" is kept in two graph-global byte-keyed
 * sets: `pending_` (IF_NEWER vertices assigned since the last covering sweep, DRAINED by
 * it) and `unconditional_` (permanent members, ITERATED). Their intended invariant is that
 * one vertex is in AT MOST ONE of them, so a sweep collects each descendant once.
 *
 * `graph_t::mark_pending` decided which set a vertex belonged in by reading
 * `vertex_t::delivery_mode()` with NO lock, then rendering the key (an O(depth) parent walk
 * plus an allocation), and only THEN taking `sweep_mutex_` to insert. `set_delivery_mode`
 * holds that same lock across the mode store and both set edits. A flip to UNCONDITIONAL
 * landing inside that window therefore ran to completion — insert into `unconditional_`,
 * erase from `pending_` (empty) — before the marker's insert put the key into `pending_`
 * as well. The key was then in BOTH sets, and the next covering propagate collected it from
 * each: ONE vertex delivered TWICE in one sweep. Two threads and one unlocked read; the
 * same window also admitted an EXPLICIT vertex into `pending_`, which an ancestor sweep must
 * never include at all.
 *
 * The unlocked read was ALSO a data race in the C++ sense — a plain `delivery_mode_t` byte
 * read while another thread stored to it — i.e. UB, not a benign stale read, and the one
 * member of its four-byte field group that was not atomic.
 *
 * So this file is a genuine racer, and it carries the two claims separately:
 *
 * - **Double delivery** (visible in an ordinary build, no sanitizer): the main thread is the
 *   ONLY deliverer — `assign` is state-plane-only and `set_delivery_mode` delivers nothing —
 *   so the per-sweep counter around one `propagate(root)` counts exactly how many times the
 *   sweep delivered the racing leaf. It is 0 (clean IF_NEWER), or 1 (marked, or
 *   UNCONDITIONAL), and 2 ONLY under double membership. `>= 2` is therefore the defect
 *   itself, directly observed, with no timing interpretation attached.
 * - **The data race**: the same run under `-fsanitize=thread` (the tsan CI lane builds and
 *   runs this suite) puts the marker's lock-free `delivery_mode()` read and the flipper's
 *   store in the report. Making the member atomic is what removes it; re-reading the mode
 *   under the lock does not, which is why both halves of the fix exist.
 *
 * Two shape choices decide whether the window is reachable at all. @ref kChainDepth makes
 * the marker's post-read work (the parent walk + key allocation) LONG, so a flip has room to
 * complete inside it, and @ref kAssignThreads puts several markers in that window at once
 * against a single flipper — the exposure is per marker-in-flight, not per sweep.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::delivery_mode_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;

/** @brief Depth of the chain the racing leaf hangs off, i.e. the length of the parent walk
 *         `mark_pending` performs AFTER its unlocked mode read and BEFORE it takes the
 *         sweep lock. This is the window; shortening it hides the defect rather than
 *         fixing it. */
constexpr int kChainDepth = 24;
/** @brief Markers racing one flipper. The window is entered per assign, so concurrent
 *         assigners multiply the chance that one of them is inside it when the flip lands. */
constexpr int kAssignThreads = 3;
/** @brief Covering sweeps the observer runs. Each one is a complete claim; the run stops
 *         early the moment a sweep double-delivers. */
constexpr int kSweeps = 20000;
/** @brief Wall-clock ceiling. Expiry is not a failure — the sweeps are the budget — but it
 *         keeps a loaded CI runner from stalling the suite. */
constexpr int kRunCeilingMs = 60000;

using tr::testing::check;
using tr::testing::make_value;

/** @brief Deliveries of the racing leaf inside the sweep currently in flight. Atomic only
 *         so the sanitizer legs judge the graph rather than this counter — every store and
 *         every load below happens on the observer thread. */
std::atomic<int> g_hits{0};

/**
 * @brief One vertex must never be delivered twice by one covering sweep, however a
 *        concurrent `set_delivery_mode` interleaves with the assigns marking it.
 */
void test_no_double_delivery_across_a_mode_flip() {
    std::printf("delivery_mode flip vs assign — sweep-set exclusion (#895):\n");
    graph_t g;
    // /r/n0/n1/.../n{kChainDepth-1}/leaf — the depth is the marker's unlocked window.
    std::string spelling = "/r";
    auto root = g.register_vertex(path_t(spelling), role_t::STORED_VALUE);
    for (int i = 0; i < kChainDepth; ++i) {
        spelling += "/n";
        spelling += std::to_string(i);
    }
    spelling += "/leaf";
    auto leaf = g.register_vertex(path_t(spelling), role_t::STORED_VALUE);
    // The leaf carries its OWN subscriber, so the counter sees the leaf's deliveries and
    // nothing else — and it is what lifts mark_pending off its unobserved-write fast path.
    auto on_leaf = [](const tr::view::rope_t&) { g_hits.fetch_add(1, std::memory_order_relaxed); };
    (void)g.subscribe(path_t(spelling), on_leaf);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> flips{0};
    std::vector<std::thread> markers;
    markers.reserve(kAssignThreads);
    for (int t = 0; t < kAssignThreads; ++t) {
        markers.emplace_back([&g, leaf, &stop] {
            const std::byte payload[] = {std::byte{0x5a}};
            while (!stop.load(std::memory_order_relaxed)) (void)g.assign(leaf, make_value(payload));
        });
    }
    std::thread flipper([&g, leaf, &stop, &flips] {
        while (!stop.load(std::memory_order_relaxed)) {
            g.set_delivery_mode(leaf, delivery_mode_t::UNCONDITIONAL);
            g.set_delivery_mode(leaf, delivery_mode_t::IF_NEWER);
            flips.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kRunCeilingMs);
    int worst = 0;
    int sweeps = 0;
    for (; sweeps < kSweeps; ++sweeps) {
        g_hits.store(0, std::memory_order_relaxed);
        g.propagate(root);  // the only deliverer in this test
        const int hits = g_hits.load(std::memory_order_relaxed);
        if (hits > worst) worst = hits;
        if (worst > 1) break;  // the defect, observed — no reason to keep racing
        if ((sweeps & 0x3ff) == 0 && std::chrono::steady_clock::now() > deadline) break;
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& m : markers) m.join();
    flipper.join();

    std::printf("    %d sweeps, %llu mode flips, worst deliveries in one sweep = %d\n", sweeps,
                static_cast<unsigned long long>(flips.load(std::memory_order_relaxed)), worst);
    check(worst <= 1, "one covering propagate delivers the racing leaf at most once");
    // Liveness of the instrument: a run where the leaf was never delivered, or the flipper
    // never ran, proves nothing about the window and must not be read as a pass.
    check(worst >= 1, "the sweep did deliver the leaf (the racer was live)");
    check(flips.load(std::memory_order_relaxed) > 0, "the flipper completed at least one flip");
}

}  // namespace

int main() {
    test_no_double_delivery_across_a_mode_flip();
    return tr::testing::summary("delivery_mode_race");
}
