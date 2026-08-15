/**
 * @file
 * @brief #1223 step 2 — an ACCEPTED `slot_server_t` session holds an identity anchor: a
 *        vertex-map slot with a saturating generation, revived in place on slot reuse.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The client plane is a REAL `tr::net::transport_tcp_server` on an ephemeral port with
 * genuinely dialled sockets, because slot allocation, naming and teardown are exactly the
 * machinery under test — a fake would assume away the first-free recycling that #1223's
 * confirmed disclosure rests on.
 *
 * Two claims, and the second one is a pair of NEGATIVE assertions, so every one of them is
 * stated next to a positive control that fails if the mechanism is simply absent (#1223's
 * recorded false-negative: a harness whose sink had been cleared reported the mechanism
 * missing while it was fully intact):
 *
 *  1. **Revive in place.** A recycled slot's anchor is the SAME vertex-map slot with a
 *     BUMPED generation, and the anchor population stays bounded by `max_peers` across many
 *     connect/disconnect cycles rather than growing with churn (ADR-0044 §Amendment).
 *  2. **Anchored, not addressed.** The anchor is unreachable by path descent (RFC-0020 §3
 *     hazard) and the mount's `:children[]` bytes are unchanged by its existence, so
 *     `enumerate_peers` stays the single source of truth for that listing (ADR-0044
 *     §Decision 1 hazard).
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/path.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_tcp.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::testing::check;

using bytes_t = std::vector<std::byte>;

/** @brief The qualified mount NAME the router registers this test's listener under. */
constexpr std::string_view kMount = "net/tcp-server/srv";

/** @brief Poll @p f until it holds or the deadline expires; the final answer is returned. */
template <class Fn>
bool wait_until(Fn f, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return f();
}

/** @brief A real TCP client session against the node's listener — dial, then hang up. */
struct client_t {
    explicit client_t(std::uint16_t port)
        : link(std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", port,
                                                          &tr::mem::heap_backend(), 0, 0,
                                                          /*defer_recv=*/true)) {}
    [[nodiscard]] bool ok() const { return link && link->ok(); }
    std::unique_ptr<tr::net::tcp_transport_t> link;
};

/** @brief The listener's currently audible peer names, in enumeration order. */
std::vector<std::string> peers_of(const tr::net::transport_tcp_server& s) {
    std::vector<std::string> out;
    s.enumerate_peers([&out](std::string_view n) { out.emplace_back(n); });
    return out;
}

/**
 * @brief The mount vertex, wired exactly as `transport_vertex_t` wires a BUS link's
 *        synthesized listing: `on_children` answers from `enumerate_peers` and nothing else.
 *
 * This is the surface hazard 2 is about — if an anchor were a real child of the mount, this
 * handler and the graph's member listing would be two sources of truth for one answer.
 */
vertex_handle_t register_mount(graph_t& g, tr::net::bus_link_t& bus) {
    tr::graph::handlers_t handlers;
    handlers.on_children = [&bus]() -> tr::graph::result_t<tr::view::view_t> {
        bytes_t members;
        bus.enumerate_peers([&members](std::string_view peer) {
            bytes_t body;
            tr::wire::emit_name(body, peer);
            tr::wire::emit_tlv(members, tr::wire::type_t::POINT, tr::wire::opt_t{.pl = true}, body);
        });
        bytes_t out;
        tr::wire::emit_tlv(out, tr::wire::type_t::POINT, tr::wire::opt_t{.pl = true}, members);
        const auto res = tr::view::over_bytes(out);
        if (!res) return std::unexpected(tr::graph::status_t::BACKPRESSURE);
        return *res;
    };
    bytes_t key;
    for (std::string_view seg : {"net", "tcp-server", "srv"}) tr::wire::emit_name(key, seg);
    const auto v = g.register_vertex_key(std::move(key), role_t::STORED_VALUE, std::move(handlers));
    check(v.has_value(), "the mount vertex registers at /net/tcp-server/srv");
    return *v;
}

/** @brief The mount's `:children[]` bytes, flattened for byte comparison. */
bytes_t children_bytes(const graph_t& g, vertex_handle_t mount) {
    const auto r = g.read_children_materialized(mount);
    check(r.has_value(), "the mount answers :children[]");
    const tr::view::view_t flat = r->flatten();
    const std::span<const std::byte> b = flat.bytes();
    return bytes_t(b.begin(), b.end());
}

/**
 * @brief The whole test: one node, one real listener, one client slot recycled many times.
 */
void run() {
    graph_t node;
    fwd_router_t router(node);
    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/2,
                                         /*peer_named=*/true);
    check(server.ok(), "the listener bound an ephemeral port");
    const vertex_handle_t mount = register_mount(node, *server.bus());
    check(router.add_child(std::string(kMount), server), "the listener mounts on the router");
    const std::uint16_t port = server.local_port();

    const std::string id0 = fwd_router_t::session_anchor_id(kMount, "p0");
    const std::size_t slots_before = node.vertex_slot_count();
    const bytes_t children_idle = children_bytes(node, mount);
    check(!node.find_session_anchor(id0).has_value(), "no anchor exists before any session");

    // ---- S1 arrives: the anchor is minted ------------------------------------------------
    auto s1 = std::make_unique<client_t>(port);
    check(s1->ok(), "S1 dialled the listener");
    check(wait_until([&] { return peers_of(server).size() == 1; }), "the listener shows one peer");
    check(peers_of(server) == std::vector<std::string>{"p0"}, "S1 was named p0 (first-free slot)");
    check(wait_until([&] { return node.find_session_anchor(id0).has_value(); }),
          "S1's arrival registered its identity anchor");

    // POSITIVE CONTROL for every negative assertion below: the anchor is live and MINTABLE
    // right now. Each "not reachable" claim that follows is made in this same instant, so a
    // silently absent mechanism reddens here first rather than passing as a clean negative.
    const vertex_handle_t a1 = *node.find_session_anchor(id0);
    const auto slot1 = node.vertex_slot(a1);
    check(slot1.has_value(), "positive control: the anchor has a vertex-map slot to be named by");
    std::printf("  S1 anchor: index=%u generation=%u\n", slot1->index, slot1->generation);
    check(node.deref_vertex_slot(slot1->index, slot1->generation).has_value(),
          "positive control: an element naming the live anchor dereferences");

    // ---- Hazard 1: anchored, never addressed ---------------------------------------------
    check(!path_t::parse(id0).has_value(),
          "the anchor's id is not a spellable path (`:` and `/` are reserved in a segment)");
    bytes_t anchor_key;
    tr::wire::emit_name(anchor_key, id0);
    check(!node.find(anchor_key).has_value(),
          "the anchor's own key does NOT resolve by descent from the addressable root");
    bytes_t below_mount;
    for (std::string_view seg : {"net", "tcp-server", "srv", "p0"})
        tr::wire::emit_name(below_mount, seg);
    check(!node.find(below_mount).has_value(),
          "nothing named /net/tcp-server/srv/p0 exists in the local graph (RFC-0020 §3)");

    // ---- Hazard 2: `:children[]` is byte-identical ----------------------------------------
    const bytes_t children_live = children_bytes(node, mount);
    check(children_live != children_idle,
          "positive control: :children[] DOES change when a peer is audible");
    check(wait_until([&] { return children_bytes(node, mount) == children_live; }),
          "…and it is exactly what enumerate_peers synthesized — the anchor adds no member");
    {
        // The listing with the anchor present must equal the listing enumerate_peers alone
        // produces, byte for byte. Rebuilt here from the transport directly, so the two
        // sources of truth the anchor could have created would disagree if one existed.
        bytes_t members;
        server.enumerate_peers([&members](std::string_view peer) {
            bytes_t body;
            tr::wire::emit_name(body, peer);
            tr::wire::emit_tlv(members, tr::wire::type_t::POINT, tr::wire::opt_t{.pl = true}, body);
        });
        bytes_t expect;
        tr::wire::emit_tlv(expect, tr::wire::type_t::POINT, tr::wire::opt_t{.pl = true}, members);
        check(children_live == expect, "the mount's :children[] IS enumerate_peers' output");
    }

    // ---- S1 departs: retire bumps the generation ------------------------------------------
    s1.reset();
    check(wait_until([&] { return peers_of(server).empty(); }), "the listener shows zero peers");
    check(wait_until([&] { return !node.find_session_anchor(id0).has_value(); }),
          "S1's departure RETIRED its anchor");
    const std::uint32_t gen_after_retire = node.retire_generation(a1);
    check(gen_after_retire == slot1->generation + 1,
          "the retire bumped the saturating generation by exactly one");
    check(!node.deref_vertex_slot(slot1->index, slot1->generation).has_value(),
          "an element minted against the DEAD session no longer dereferences");

    // ---- S2 lands in the recycled slot: same vertex, same slot, new generation -------------
    auto s2 = std::make_unique<client_t>(port);
    check(s2->ok(), "S2 dialled the listener");
    check(wait_until([&] { return peers_of(server).size() == 1; }), "the listener shows one peer");
    check(peers_of(server) == std::vector<std::string>{"p0"},
          "S2 landed in the RECYCLED slot and got the SAME name p0");
    check(wait_until([&] { return node.find_session_anchor(id0).has_value(); }),
          "S2's arrival revived the anchor");
    const vertex_handle_t a2 = *node.find_session_anchor(id0);
    check(a2 == a1, "the revived anchor is the SAME vertex_t (revive in place, not a new one)");
    const auto slot2 = node.vertex_slot(a2);
    check(slot2.has_value() && slot2->index == slot1->index,
          "…in the SAME vertex-map slot — the insert-only map did not grow");
    check(slot2->generation == gen_after_retire && slot2->generation > slot1->generation,
          "…with the BUMPED generation, so S1's element and S2's element are distinguishable");
    check(!node.deref_vertex_slot(slot1->index, slot1->generation).has_value(),
          "S1's element STILL fails against the revived slot (the whole point of #1223)");
    check(node.deref_vertex_slot(slot2->index, slot2->generation).has_value(),
          "positive control: S2's own element dereferences");

    // ---- Bounded across churn --------------------------------------------------------------
    s2.reset();
    check(wait_until([&] { return peers_of(server).empty(); }), "the listener drains again");
    const std::size_t slots_with_one_anchor = node.vertex_slot_count();
    std::uint32_t last_gen = node.retire_generation(a1);
    int cycles = 0;
    bool monotonic = true;
    for (int i = 0; i < 20; ++i) {
        auto c = std::make_unique<client_t>(port);
        if (!c->ok()) continue;
        if (!wait_until([&] { return node.find_session_anchor(id0).has_value(); })) continue;
        ++cycles;
        // Accumulated rather than asserted per iteration: one line of verdict, not twenty.
        monotonic =
            monotonic && node.vertex_slot(*node.find_session_anchor(id0))->generation >= last_gen;
        c.reset();
        if (!wait_until([&] { return !node.find_session_anchor(id0).has_value(); })) continue;
        last_gen = node.retire_generation(a1);
    }
    std::printf("  %d connect/disconnect cycles; final generation=%u\n", cycles, last_gen);
    check(cycles >= 10, "enough cycles ran for the bound to mean something");
    check(monotonic, "the generation never moved backwards across any recycle");
    check(last_gen > gen_after_retire, "churn advanced the generation");
    check(node.vertex_slot_count() == slots_with_one_anchor,
          "the vertex map did NOT grow across the churn — anchors are bounded by max_peers");
    check(node.session_anchor_slots() == 1,
          "exactly ONE anchor allocation exists after 20 sessions through one slot");
    check(node.vertex_slot_count() == slots_before + 1,
          "the whole run cost exactly one vertex-map slot");

    // Two peers at once must anchor separately — the bound is `max_peers`, not one.
    auto ca = std::make_unique<client_t>(port);
    auto cb = std::make_unique<client_t>(port);
    check(wait_until([&] { return peers_of(server).size() == 2; }), "two peers are audible");
    const std::string id1 = fwd_router_t::session_anchor_id(kMount, "p1");
    check(wait_until([&] { return node.find_session_anchor(id1).has_value(); }),
          "the second slot anchored under its own id");
    check(node.session_anchor_slots() == 2, "two slots ⇒ two anchors, and no more");
    ca.reset();
    cb.reset();
    check(wait_until([&] { return peers_of(server).empty(); }), "both peers departed");
    check(node.session_anchor_slots() == 2, "the anchor count is the SLOT count, not the churn");
}

}  // namespace

int main() {
    std::printf("#1223 step 2 — session identity anchors on an accepted slot_server_t peer:\n");
    run();
    return tr::testing::summary("session_anchor");
}
