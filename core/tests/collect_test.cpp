/**
 * @file
 * @brief graph_t::collect() / parked_seam_count() — the explicit end of retirement's
 *        value-seam park (#576, the direction-3 ruling that supersedes ADR-0072).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `retire()` detaches a HANDLER-role vertex's value seam and parks it, because the seam is
 * read lock-free and the retiring thread cannot free a block a reader may still hold. Until
 * #576 the park had ONE append site and ZERO release sites, so every connection teardown
 * (`transport_vertex_t::remove_connection` retires the `/net/<name>` identity vertex) leaked
 * ~96 B of `std::function` permanently. Two properties are asserted here:
 *
 *   (a) the park is BOUNDED and OBSERVABLE — N retired handler-bearing vertices show as N
 *       parked seams, and `collect()` takes that to 0;
 *   (b) the free happens OUTSIDE every graph lock — the load-bearing one. Three earlier
 *       design rounds each died exactly here: a free performed while the graph's map lock is
 *       held puts arbitrary user destructor code inside the widest lock in the runtime, and
 *       a seam callback that owns anything graph-shaped deadlocks on the way out. The probe
 *       installs a seam whose DESTRUCTOR re-enters the graph (`find()`), and a watchdog
 *       turns the resulting hang into a FAIL line rather than a stuck CI job.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;

int g_failures = 0;

/** @brief Report one assertion, tallying failures for main's exit status. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief An inert `on_read` seam — enough to make a vertex allocate a `value_handlers_t`. */
tr::graph::result_t<tr::view::rope_t> inert_read() { return std::unexpected(status_t::NOT_FOUND); }

/**
 * @brief A value-seam callback whose DESTRUCTOR re-enters the graph — the probe for (b).
 *
 * Stands in for the real thing a seam captures: a connection object, a handle, a callback
 * bundle whose release path touches the graph. If `collect()` freed the parked block under
 * `map_mutex_`, this `find()` would block forever on a non-recursive `std::shared_mutex`
 * already held by the very thread running the destructor.
 *
 * Copyable because `std::function` demands it; the move constructor disarms the source so a
 * moved-from husk re-enters nothing.
 */
struct reentrant_seam_t {
    graph_t* g{nullptr};                 /**< @brief Disarmed (null) in a moved-from husk. */
    std::atomic<int>* fired{nullptr};    /**< @brief Bumped once per armed destructor run. */
    std::atomic<int>* resolved{nullptr}; /**< @brief Bumped when the re-entrant find succeeded. */

    reentrant_seam_t(graph_t* graph, std::atomic<int>* f, std::atomic<int>* r)
        : g(graph), fired(f), resolved(r) {}
    reentrant_seam_t(const reentrant_seam_t&) = default;
    reentrant_seam_t& operator=(const reentrant_seam_t&) = default;
    reentrant_seam_t(reentrant_seam_t&& other) noexcept
        : g(std::exchange(other.g, nullptr)), fired(other.fired), resolved(other.resolved) {}
    reentrant_seam_t& operator=(reentrant_seam_t&& other) noexcept {
        g = std::exchange(other.g, nullptr);
        fired = other.fired;
        resolved = other.resolved;
        return *this;
    }

    ~reentrant_seam_t() {
        if (g == nullptr) return;
        // THE re-entry: a graph operation from inside the free of a parked seam.
        const bool ok = g->find(path_t("/probe").key()).has_value();
        if (ok && resolved != nullptr) resolved->fetch_add(1, std::memory_order_relaxed);
        if (fired != nullptr) fired->fetch_add(1, std::memory_order_release);
    }

    tr::graph::result_t<tr::view::rope_t> operator()() const { return inert_read(); }
};

/** @brief Register `/dev/h<i>` as a HANDLER bearing an inert value seam. */
vertex_handle_t make_handler_vertex(graph_t& g, const std::string& path) {
    handlers_t h;
    h.on_read = [] { return inert_read(); };
    return g.register_vertex(path_t(path), role_t::HANDLER, std::move(h));
}

// ---------------------------------------------------------------------------
// (a) The park is bounded and observable: N retirements => N parked => collect => 0.
void test_parked_count_and_collect() {
    std::printf("#576(a): the parked-seam count tracks retirement, and collect() drains it:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    check(g.parked_seam_count() == 0, "a fresh graph parks nothing");

    constexpr int kN = 5;
    std::vector<vertex_handle_t> handlers;
    for (int i = 0; i < kN; ++i)
        handlers.push_back(make_handler_vertex(g, "/dev/h" + std::to_string(i)));
    check(g.parked_seam_count() == 0, "registering handler vertices parks nothing");

    // A vertex with NO value seam parks nothing when retired — the park counts seams, not
    // retirements (a leaf/app-field vertex never allocated a value_handlers_t).
    const vertex_handle_t plain = g.register_vertex(path_t("/dev/plain"), role_t::STORED_VALUE);
    check(g.retire(plain).has_value(), "retire a seamless STORED_VALUE vertex");
    check(g.parked_seam_count() == 0, "retiring a seamless vertex parks nothing");

    for (int i = 0; i < kN; ++i)
        check(g.retire(handlers[static_cast<std::size_t>(i)]).has_value(),
              "retire /dev/h" + std::to_string(i));
    check(g.parked_seam_count() == static_cast<std::size_t>(kN),
          "N retired handler-bearing vertices == N parked seams (the leak, made observable)");

    g.collect();
    check(g.parked_seam_count() == 0, "collect() drained the park to 0");

    g.collect();
    check(g.parked_seam_count() == 0, "collect() on an empty park is a no-op (idempotent)");

    // The cycle repeats — collect() does not disable parking, it empties it.
    const vertex_handle_t again = make_handler_vertex(g, "/dev/again");
    check(g.retire(again).has_value(), "retire a freshly registered handler vertex");
    check(g.parked_seam_count() == 1, "the park refills after a collect");
    g.collect();
    check(g.parked_seam_count() == 0, "and drains again");

    // The live graph is untouched by a collect.
    (void)g.register_vertex(path_t("/dev/live"), role_t::STORED_VALUE);
    g.collect();
    check(g.find(path_t("/dev/live").key()).has_value(), "collect() leaves live vertices alone");
}

// ---------------------------------------------------------------------------
// (b) THE load-bearing one: the free runs outside every graph lock. A seam destructor that
// re-enters the graph must neither deadlock nor throw.
void test_free_runs_outside_graph_locks() {
    std::printf("#576(b): the parked seam is freed OUTSIDE every graph lock:\n");
    std::atomic<int> fired{0};
    std::atomic<int> resolved{0};

    graph_t g;
    // The vertex the seam destructor resolves on its way out — proof the re-entry actually
    // reached the graph rather than bailing early.
    (void)g.register_vertex(path_t("/probe"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    {
        handlers_t h;
        h.on_read = reentrant_seam_t{&g, &fired, &resolved};
        const vertex_handle_t v =
            g.register_vertex(path_t("/dev/reentrant"), role_t::HANDLER, std::move(h));
        check(g.retire(v).has_value(), "retire the vertex bearing the re-entrant seam");
    }
    check(g.parked_seam_count() == 1, "its seam is parked, not yet freed");

    const int before = fired.load(std::memory_order_acquire);

    // Run collect() on a worker so a deadlocked free is a reported FAIL, not a hung job. A
    // free performed under map_mutex_ (the shape rounds 2 and 3 died on) hangs right here:
    // the destructor's find() waits for a shared hold on a mutex this same thread owns.
    std::atomic<bool> done{false};
    std::thread worker([&] {
        g.collect();
        done.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    if (!done.load(std::memory_order_acquire)) {
        check(false, "collect() returned — the seam free is outside the graph locks");
        std::printf(
            "  !! DEADLOCK: collect() did not return within 10 s. The parked seam is being "
            "freed while a graph lock is held, and its destructor's re-entry cannot get in.\n");
        std::fflush(stdout);
        // The worker owns a lock it will never release; unwinding is not an option.
        std::_Exit(1);
    }
    worker.join();
    check(true, "collect() returned — the seam free is outside the graph locks");
    check(fired.load(std::memory_order_acquire) > before,
          "the parked seam's destructor RAN during collect()");
    check(resolved.load(std::memory_order_acquire) > 0,
          "and it re-entered the graph from inside that free (find() resolved /probe)");
    check(g.parked_seam_count() == 0, "the park is empty afterwards");

    // Nothing re-entrant may be left parked: the graph's own teardown frees the remainder
    // AFTER the vertex tree and the map lock are gone (the contract collect()'s @note states).
    check(g.parked_seam_count() == 0, "no re-entrant seam is left for teardown to free");
}

// ---------------------------------------------------------------------------
// The reported symptom: a retire/revive churn cycle no longer grows the park without bound.
void test_churn_is_bounded_by_collect() {
    std::printf("#576: peer churn no longer grows the park without bound:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/net"), role_t::STORED_VALUE);

    std::size_t high_water = 0;
    bool every_retire_ok = true;
    for (int i = 0; i < 64; ++i) {
        // The transport_vertex_t::remove_connection shape: register the identity vertex of a
        // connection, then retire it on teardown.
        const vertex_handle_t conn = make_handler_vertex(g, "/net/peer");
        every_retire_ok = every_retire_ok && g.retire(conn).has_value();
        const std::size_t parked = g.parked_seam_count();
        if (parked > high_water) high_water = parked;
        g.collect();
    }
    check(every_retire_ok, "64 connect/teardown cycles each retired cleanly");
    check(high_water <= 1, "with collect() in the teardown loop the park never exceeds 1");
    check(g.parked_seam_count() == 0, "and ends empty after 64 connect/teardown cycles");
}

}  // namespace

int main() {
    test_parked_count_and_collect();
    test_free_runs_outside_graph_locks();
    test_churn_is_bounded_by_collect();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
