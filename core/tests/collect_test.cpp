/**
 * @file
 * @brief graph_t::collect() / parked_seam_count() — the explicit end of retirement's
 *        value-seam park (#576, the direction-3 ruling that supersedes ADR-0072).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `retire()` detaches a vertex's value seam and parks it, because the seam is read lock-free
 * and the retiring thread cannot free a block a reader may still hold. Until #576 the park
 * had ONE append site and ZERO release sites, so a BUS node's connection teardown
 * (`transport_vertex_t::remove_connection` retires the `/net/<module>/<name>` identity
 * vertex) leaked ~96 B of `std::function` permanently. Four properties are asserted here:
 *
 *   (a) the park is BOUNDED and OBSERVABLE — N retired seam-bearing vertices show as N
 *       parked seams, and `collect()` takes that to 0;
 *   (b) the free happens OUTSIDE every graph lock — the load-bearing one. Three earlier
 *       design rounds each died exactly here: a free performed while the graph's map lock is
 *       held puts arbitrary user destructor code inside the widest lock in the runtime, and
 *       a seam callback that owns anything graph-shaped deadlocks on the way out. The probe
 *       installs a seam whose DESTRUCTOR re-enters the graph (`find()`), and a watchdog
 *       turns the resulting hang into a FAIL line rather than a stuck CI job.
 *   (c) the population is keyed on handler PRESENCE, never on `role_t`: `adopt_identity`
 *       allocates the seam iff `on_read || on_write || on_children`. `STORED_VALUE` +
 *       `{on_read}` parks 1; `HANDLER` + an empty `handlers_t` parks 0 — the exact inverse
 *       of a role-keyed reading. `STORED_VALUE` + `{on_children}` is the PRODUCTION shape
 *       (the `/net/<module>/<name>` identity vertex of a bus link);
 *   (d) `transport_vertex_t::remove_connection` end to end, over both link shapes: a
 *       bus-capable link (the `peer_named = true` / CAN shape, `bus() != nullptr`) parks one
 *       seam per teardown and `collect()` drains it, while the point-to-point control
 *       (`bus() == nullptr` — every dial link, UDP, loopback, a default-wired server) parks
 *       ZERO. Both halves matter: a default-transport embedder needs no quiescent point, and
 *       a bus node is the one that leaks without one.
 */

#include <algorithm>
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

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::net::bus_link_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;

using tr::testing::check;

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

/**
 * @brief Register @p path in the PRODUCTION shape: `role_t::STORED_VALUE` bearing an
 *        `on_children` seam.
 *
 * This is exactly what `transport_vertex_t::add_connection` builds for a bus link's
 * `/net/<module>/<name>` identity vertex — a synthesized `:children[]` listing on a
 * STORED_VALUE vertex (ADR-0044). It is the shape a role-keyed reading of the park
 * excludes, and the only one #576 exists for.
 */
vertex_handle_t make_stored_value_children_vertex(graph_t& g, const std::string& path) {
    handlers_t h;
    h.on_children = []() -> tr::graph::result_t<tr::view::view_t> {
        return std::unexpected(status_t::NOT_FOUND);
    };
    return g.register_vertex(path_t(path), role_t::STORED_VALUE, std::move(h));
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

    // Nothing re-entrant may be left parked. The graph's own teardown is a growth backstop
    // only: retired_seams_ is declared before map_mutex_ and root_, so it destructs LAST —
    // after the vertex tree and the map lock are already gone — and a seam destructor that
    // re-enters the graph then re-enters a half-destroyed object. Hence the @note's "safe
    // HERE and only here": such an owner must be collected explicitly.
    check(g.parked_seam_count() == 0, "no re-entrant seam is left for teardown to free");
}

// ---------------------------------------------------------------------------
// (c) The park is keyed on handler PRESENCE, not on role_t. The published contract used to
// say "HANDLER-role vertex", which is the exact inverse of what adopt_identity does — and
// excluded the one production site the collector exists for.
void test_park_is_keyed_on_handler_presence_not_role() {
    std::printf("#576(c): the park is keyed on handler PRESENCE, never on role_t:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    // STORED_VALUE + {on_read}: the role says "not a handler", the seam exists anyway.
    handlers_t sv_read;
    sv_read.on_read = [] { return inert_read(); };
    const vertex_handle_t sv =
        g.register_vertex(path_t("/dev/sv_read"), role_t::STORED_VALUE, std::move(sv_read));
    check(g.retire(sv).has_value(), "retire a STORED_VALUE vertex carrying {on_read}");
    check(g.parked_seam_count() == 1, "STORED_VALUE + {on_read} PARKS ONE (role is not consulted)");
    g.collect();

    // HANDLER + an empty handlers_t: the role says "handler", no seam was ever allocated.
    const vertex_handle_t bare =
        g.register_vertex(path_t("/dev/bare_handler"), role_t::HANDLER, handlers_t{});
    check(g.retire(bare).has_value(), "retire a HANDLER-role vertex with an EMPTY handlers_t");
    check(g.parked_seam_count() == 0, "HANDLER + {} parks NOTHING (the inverse of the old text)");

    // The production shape: STORED_VALUE + {on_children} — the /net/<module>/<name> identity
    // vertex of a bus link. A role-keyed quiescent point excludes exactly this.
    const vertex_handle_t prod = make_stored_value_children_vertex(g, "/dev/identity");
    check(g.retire(prod).has_value(), "retire a STORED_VALUE vertex carrying {on_children}");
    check(g.parked_seam_count() == 1,
          "STORED_VALUE + {on_children} PARKS ONE — the production identity-vertex shape");
    g.collect();
    check(g.parked_seam_count() == 0, "collect() drains the production shape too");

    // And the third seam, for completeness: presence of ANY of the three allocates.
    handlers_t sv_write;
    sv_write.on_write = [](const tr::view::rope_t&) -> tr::graph::result_t<void> { return {}; };
    const vertex_handle_t w =
        g.register_vertex(path_t("/dev/sv_write"), role_t::STORED_VALUE, std::move(sv_write));
    check(g.retire(w).has_value(), "retire a STORED_VALUE vertex carrying {on_write}");
    check(g.parked_seam_count() == 1, "STORED_VALUE + {on_write} parks one as well");
    g.collect();
}

// ---------------------------------------------------------------------------
// (d) transport_vertex_t::remove_connection END TO END, over both link shapes.
//
// A transport that records nothing and opens nothing — the point-to-point control. Its
// bus() keeps transport_t's default nullptr, which is every dial link, UDP, loopback and a
// default-wired tcp/ws server.
class p2p_link_t : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte> frame) override { (void)frame; }
};

/**
 * @brief A bus-capable link: `bus()` returns its own facet, exactly as `transport_can`
 *        does unconditionally and as tcp/ws do when wired `peer_named = true`.
 *
 * That single property is what makes `add_connection` install an `on_children` on the
 * identity vertex — and therefore what makes the teardown park a seam.
 */
class bus_capable_link_t : public tr::net::transport_t, public bus_link_t {
   public:
    void send(std::span<const std::byte> frame) override { (void)frame; }
    [[nodiscard]] bus_link_t* bus() override { return this; }

    void enumerate_peers(const peer_visitor_t& visit) const override { visit("p0"); }
    [[nodiscard]] tr::net::transport_t* peer_link(std::string_view peer) override {
        return peer == "p0" ? this : nullptr;
    }
};

/** @brief SPEC{ type, name } with no config — the provide_link-staged connection form. */
tr::view::view_t conn_spec(std::string_view type, std::string_view name) {
    return tr::net::conn_spec_t(type, name).view();
}

/** @brief The module staged connections mount under here. */
constexpr std::string_view kModule = "ws-client";

/** @brief The qualified key of connection @p name (RFC-0014 §1 / ADR-0061, #605). */
std::string mount_of(std::string_view name) {
    return "net/" + std::string(kModule) + "/" + std::string(name);
}

/** @brief Create connection @p name over the staged @p link via the `:children[]` SPEC. */
bool create_conn(graph_t& g, transport_vertex_t& net, tr::net::transport_t& link,
                 std::string_view name) {
    net.provide_link(std::string(kModule), std::string(name), link);
    const auto p = path_t::parse("/net:children[]");
    if (!p) return false;
    return g.write(*p, conn_spec("client", name)).has_value();
}

void test_remove_connection_parks_only_over_a_bus_link() {
    std::printf("#576(d): remove_connection parks over a BUS link and NOT point-to-point:\n");

    // --- the control: a point-to-point link. bus() == nullptr, so add_connection installs
    // no on_children, so the identity vertex bears no seam, so teardown parks nothing.
    {
        graph_t g;
        fwd_router_t router(g);
        transport_vertex_t net(g, router);
        p2p_link_t link;
        check(create_conn(g, net, link, "p2p"), "created /net/ws-client/p2p over a p2p link");
        check(g.parked_seam_count() == 0, "creating it parks nothing");
        check(net.remove_connection(mount_of("p2p")).has_value(), "remove_connection succeeds");
        check(g.parked_seam_count() == 0,
              "a POINT-TO-POINT teardown parks ZERO — no quiescent point is owed");
        const auto after = g.read(*path_t::parse("/net/ws-client/p2p"));
        check(!after && after.error() == status_t::NOT_FOUND, "and the identity vertex is retired");
    }

    // --- the leaking case: a bus-capable link (the CAN / peer_named=true shape).
    {
        graph_t g;
        fwd_router_t router(g);
        transport_vertex_t net(g, router);
        bus_capable_link_t link;
        check(create_conn(g, net, link, "bus"), "created /net/ws-client/bus over a BUS link");
        check(g.parked_seam_count() == 0, "creating it parks nothing");
        check(net.remove_connection(mount_of("bus")).has_value(), "remove_connection succeeds");
        check(g.parked_seam_count() == 1,
              "a BUS-link teardown parks ONE seam — the ~96 B that leaked before #576");
        g.collect();
        check(g.parked_seam_count() == 0, "collect() drains it");

        // Churn: the leak was unbounded growth, so prove the cycle is now bounded.
        std::size_t high_water = 0;
        bool every_cycle_ok = true;
        for (int i = 0; i < 16; ++i) {
            every_cycle_ok = every_cycle_ok && create_conn(g, net, link, "bus") &&
                             net.remove_connection(mount_of("bus")).has_value();
            high_water = std::max(high_water, g.parked_seam_count());
            g.collect();
        }
        check(every_cycle_ok, "16 further bus connect/teardown cycles each completed");
        check(high_water == 1, "and never exceed 1 parked seam with collect() in the loop");
        check(g.parked_seam_count() == 0, "and end at 0");
    }
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
        // The graph-level churn shape (the end-to-end transport drive lives in (d)): register
        // an identity vertex in the production shape — STORED_VALUE bearing an on_children,
        // as a bus link's /net/<module>/<name> is — then retire it on teardown.
        const vertex_handle_t conn = make_stored_value_children_vertex(g, "/net/peer");
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
    test_park_is_keyed_on_handler_presence_not_role();
    test_remove_connection_parks_only_over_a_bus_link();
    test_churn_is_bounded_by_collect();

    return tr::testing::summary("collect");
}
