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
 * vertex) leaked its seam block (then ~96 B of `std::function`) permanently. Four properties are
 * asserted here:
 *
 *   (a) the park is BOUNDED and OBSERVABLE — N retired seam-bearing vertices show as N
 *       parked seams; one `collect()` ages them (a seam parked just before a call
 *       survives it), and the second takes that to 0;
 *   (b) the free runs NO user code. Three earlier design rounds each died on a free that put
 *       arbitrary user destructor code (a `std::function`'s captures) inside the graph's map
 *       lock; since RFC-0028 slice 7 a seam is a `{fn, ctx}` hook that owns nothing, so the
 *       parked block is trivially destructible and the caller's context is never touched.
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
#include <type_traits>
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
tr::graph::result_t<tr::graph::value_ref_t> inert_read() {
    return std::unexpected(status_t::NOT_FOUND);
}

/** @brief Register `/dev/h<i>` as a HANDLER bearing an inert value seam. */
vertex_handle_t make_handler_vertex(graph_t& g, const std::string& path) {
    handlers_t h;
    auto h_on_read = [] { return inert_read(); };
    h.on_read = tr::graph::thunk(h_on_read);
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
    auto h_on_children = []() -> tr::graph::result_t<tr::view::view_t> {
        return std::unexpected(status_t::NOT_FOUND);
    };
    h.on_children = tr::graph::thunk(h_on_children);
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
    check(g.parked_seam_count() == static_cast<std::size_t>(kN),
          "one collect() only ages the park: a seam parked since the previous call survives");
    g.collect();
    check(g.parked_seam_count() == 0, "the second collect() drained the park to 0");

    g.collect();
    check(g.parked_seam_count() == 0, "collect() on an empty park is a no-op");

    // The cycle repeats — collect() does not disable parking, it empties it.
    const vertex_handle_t again = make_handler_vertex(g, "/dev/again");
    check(g.retire(again).has_value(), "retire a freshly registered handler vertex");
    check(g.parked_seam_count() == 1, "the park refills after a collect");
    g.collect();
    check(g.parked_seam_count() == 1, "it waits out one full interval");
    g.collect();
    check(g.parked_seam_count() == 0, "and drains again");

    // The live graph is untouched by a collect.
    (void)g.register_vertex(path_t("/dev/live"), role_t::STORED_VALUE);
    g.collect();
    check(g.find(path_t("/dev/live").key()).has_value(), "collect() leaves live vertices alone");
}

// ---------------------------------------------------------------------------
// (b) The free runs no user code at all, so no lock placement can make it deadlock.
void test_free_runs_no_user_code() {
    std::printf("#576(b): freeing a parked seam runs NO user code (RFC-0028 D10):\n");
    // The hazard (b) used to guard — a parked `std::function` whose captured state's DESTRUCTOR
    // re-enters the graph from inside `collect()` — is gone by construction: a seam is a
    // `{fn, ctx}` hook that owns nothing, so the block `collect()` frees has a trivial
    // destructor and the caller's context is never touched by the free.
    static_assert(std::is_trivially_destructible_v<tr::graph::value_handlers_t>,
                  "a parked seam block must free without running user code");
    struct ctx_t {
        int reads = 0;
    } ctx;

    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    handlers_t h;
    h.on_read = {[](void* c) -> tr::graph::result_t<tr::graph::value_ref_t> {
                     ++static_cast<ctx_t*>(c)->reads;
                     return inert_read();
                 },
                 &ctx};
    const vertex_handle_t v = g.register_vertex(path_t("/dev/seam"), role_t::HANDLER, h);
    check(g.retire(v).has_value(), "retire the vertex bearing the seam");
    check(g.parked_seam_count() == 1, "its seam block is parked, not yet freed");
    g.collect();
    g.collect();
    check(g.parked_seam_count() == 0, "two collect() calls freed it");
    check(ctx.reads == 0, "and the caller's context was never called or touched by the free");
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
    auto sv_read_on_read = [] { return inert_read(); };
    sv_read.on_read = tr::graph::thunk(sv_read_on_read);
    const vertex_handle_t sv =
        g.register_vertex(path_t("/dev/sv_read"), role_t::STORED_VALUE, std::move(sv_read));
    check(g.retire(sv).has_value(), "retire a STORED_VALUE vertex carrying {on_read}");
    check(g.parked_seam_count() == 1, "STORED_VALUE + {on_read} PARKS ONE (role is not consulted)");
    g.collect();
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
    g.collect();
    check(g.parked_seam_count() == 0, "collect() drains the production shape too");

    // And the third seam, for completeness: presence of ANY of the three allocates.
    handlers_t sv_write;
    auto sv_write_on_write = [](const tr::graph::value_t&,
                                const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        return {};
    };
    sv_write.on_write = tr::graph::thunk(sv_write_on_write);
    const vertex_handle_t w =
        g.register_vertex(path_t("/dev/sv_write"), role_t::STORED_VALUE, std::move(sv_write));
    check(g.retire(w).has_value(), "retire a STORED_VALUE vertex carrying {on_write}");
    check(g.parked_seam_count() == 1, "STORED_VALUE + {on_write} parks one as well");
    g.collect();
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
 * @brief A bus-capable link: `bus()` returns its own facet, exactly as `can_transport_t`
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
    /** @brief One census peer, so every valid handle resolves to its name (#1294). */
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t peer,
                                             std::span<char>) const override {
        return peer.valid() ? "p0" : std::string_view{};
    }
};

/** @brief SPEC{ name } with no config — the provide_link-staged connection form. */
tr::view::view_t conn_spec(std::string_view name) { return tr::net::conn_spec_t(name).view(); }

/** @brief The module staged connections mount under here. */
constexpr std::string_view kModule = "ws-client";

/** @brief The qualified key of connection @p name (RFC-0014 §1 / ADR-0061, #605). */
std::string mount_of(std::string_view name) {
    return "net/" + std::string(kModule) + "/" + std::string(name);
}

/**
 * @brief Create connection @p name over the staged @p link through the module's CREATOR
 *        ENDPOINT — `/net/<module>/conn`, the one creation door since RFC-0014 S7.
 *
 * The module is declared with an EMPTY kind: a staged link bypasses the transport factory, so
 * there is no kind to name, and the kind-less SPEC below resolves to this single declaration.
 * `register_module` deduplicates on (kind, role), so repeating it per create declares once.
 */
bool create_conn(graph_t& g, transport_vertex_t& net, tr::net::transport_t& link,
                 std::string_view name) {
    if (!net.register_module(std::string(kModule), "", tr::net::conn_role_t::DIAL)) return false;
    net.provide_link(std::string(kModule), std::string(name), link);
    const auto p = path_t::parse("/net/" + std::string(kModule) + "/conn");
    if (!p) return false;
    return g.write(*p, conn_spec(name)).has_value();
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
        check(g.parked_seam_count() == 1, "one collect() ages it");
        g.collect();
        check(g.parked_seam_count() == 0, "the second collect() drains it");

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
        check(high_water == 2,
              "and never exceed 2 parked seams (two generations) with collect() in the loop");
        g.collect();
        check(g.parked_seam_count() == 0, "and end at 0 one collect() after the last");
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
    check(high_water <= 2, "with collect() in the teardown loop the park never exceeds 2");
    g.collect();
    check(g.parked_seam_count() == 0, "and ends empty one collect() after 64 cycles");
}

}  // namespace

int main() {
    test_parked_count_and_collect();
    test_free_runs_no_user_code();
    test_park_is_keyed_on_handler_presence_not_role();
    test_remove_connection_parks_only_over_a_bus_link();
    test_churn_is_bounded_by_collect();

    return tr::testing::summary("collect");
}
