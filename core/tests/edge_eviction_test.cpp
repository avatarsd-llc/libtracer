/**
 * @file
 * @brief RFC-0009 §D extended to peer departure — subscriber-edge eviction on link
 *        teardown, and edge-slot reuse.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What ships without this: nothing evicts a departed link's subscriber edges, so every
 * browser session leaves its remote edges ACTIVE in every write fan-out forever
 * (~27 KB/session measured on the C6), and `add_edge` grows `subs_` without bound
 * because a freed slot is never reused. Assertions:
 *
 *   - `graph_t::evict_link_edges(link)` deactivates every subscriber edge stored
 *     against that link — deliveries to it STOP — while local edges, other links'
 *     edges, and the RFC-0005 subtree-listener bookkeeping survive intact;
 *   - slot indices of surviving edges never renumber (§D.2), a re-subscribe REUSES
 *     the freed slots before `subs_` grows, and the reused slot delivers;
 *   - `fwd_router_t::link_down` evicts AND drops the link's route-handle label
 *     state (reusing clear_link), so a compact flow's egress binding dies with it;
 *   - the `add_child`-installed departure notifiers (point-to-point down, bus
 *     peer-down) reach the same hook — the seam every transport teardown fires;
 *   - eviction racing a writer thread is crash/TSan-clean (the concurrency gate);
 *   - plain subscribe/deliver still works after eviction (regression).
 */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::net::bus_link_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

/** @brief One PASS/FAIL line; failures accumulate into the process exit code. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A view_t over a fresh, owned heap segment holding @p bytes. */
view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief Byte-list sugar over @ref make_value. */
view_t make_value(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> v;
    v.reserve(bytes.size());
    for (const std::uint8_t b : bytes) v.push_back(std::byte{b});
    return make_value(v);
}

/** @brief Concatenate pre-encoded TLV byte runs. */
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

/** @brief A NAME TLV. */
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}

/** @brief A PATH TLV over the given segments. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) append(body, b_name(s));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief A VALUE TLV over one byte (index_mode / op / marker payloads). */
std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/**
 * @brief A SUBSCRIBER TLV whose PATH child carries @p marker — byte-distinguishable
 *        per edge, so an indexed `:subscribers[N]` read identifies WHICH edge occupies
 *        a slot (the §D.2 stability / reuse oracle).
 */
std::vector<std::byte> b_subscriber(std::string_view marker) {
    std::vector<std::byte> body;
    append(body, b_path({marker}));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}

/** @brief FIELD{ NAME "subscribers", VALUE u8 ELEMENT } — the ":subscribers[]" append. */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    append(body, b_name("subscribers"));
    append(body, b_value_u8(1));  // index_mode = ELEMENT (append)
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/** @brief A FWD frame: op, dst, optional field, src, optional payload. */
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& field = {},
                             const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    append(body, b_value_u8(static_cast<std::uint8_t>(op)));
    append(body, dst);
    if (!field.empty()) append(body, field);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief Bind one wire subscriber at @p v arriving over @p link, tagged @p marker. */
bool wire_sub(graph_t& g, vertex_handle_t v, std::string_view link, std::string_view marker) {
    return g
        .subscribe_wire(v, make_value(b_subscriber(marker)),
                        make_value(b_path({std::string(link)})), std::string(link))
        .has_value();
}

/** @brief The flat bytes of a rope (test-side compare; frames here are small). */
std::vector<std::byte> rope_bytes(const rope_t& r) {
    std::vector<std::byte> out;
    for (const view_t& l : r.links()) out.insert(out.end(), l.bytes().begin(), l.bytes().end());
    return out;
}

/**
 * @brief An in-memory point-to-point transport recording every send; `die()` fires the
 *        `add_child`-installed down notifier exactly as a real transport's teardown does.
 */
class fake_link_t : public transport_t {
   public:
    /** @brief Record one outbound frame. */
    void send(std::span<const std::byte> frame) override {
        const std::lock_guard lock(m_);
        sent_.emplace_back(frame.begin(), frame.end());
    }
    /** @brief Poke one inbound frame into the router-installed receiver. */
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    /** @brief Simulate the transport observing its one connection dead. */
    void die() { notify_down(); }
    /** @brief Drain the recorded sends. */
    std::vector<std::vector<std::byte>> drain() {
        const std::lock_guard lock(m_);
        return std::exchange(sent_, {});
    }
    /** @brief The recorded send count. */
    std::size_t count() {
        const std::lock_guard lock(m_);
        return sent_.size();
    }

   private:
    std::mutex m_;                             /**< @brief Guards @ref sent_. */
    std::vector<std::vector<std::byte>> sent_; /**< @brief Recorded outbound frames. */
};

/**
 * @brief An in-memory BUS transport (ADR-0044 facet): frames arrive tagged with a peer
 *        name, replies/deliveries route to a per-peer recording endpoint, and
 *        `peer_die()` fires the peer-departure notifier as a bus adapter's session
 *        teardown does.
 */
class fake_bus_t : public transport_t, public bus_link_t {
   public:
    /** @brief One peer's directed recording endpoint. */
    class peer_ep_t : public transport_t {
       public:
        /** @brief Record one outbound frame to this peer. */
        void send(std::span<const std::byte> frame) override {
            const std::lock_guard lock(m_);
            sent_.emplace_back(frame.begin(), frame.end());
        }
        /** @brief The recorded send count. */
        std::size_t count() {
            const std::lock_guard lock(m_);
            return sent_.size();
        }
        /** @brief Drain the recorded sends. */
        std::vector<std::vector<std::byte>> drain() {
            const std::lock_guard lock(m_);
            return std::exchange(sent_, {});
        }

       private:
        std::mutex m_;                             /**< @brief Guards @ref sent_. */
        std::vector<std::vector<std::byte>> sent_; /**< @brief Recorded frames. */
    };

    /** @brief Undirected sends are not part of this facet's contract; drop. */
    void send(std::span<const std::byte>) override {}
    /** @brief Expose the bus facet, like every multi-peer link. */
    bus_link_t* bus() override { return this; }
    /** @brief Visit the peers that ever appeared (test-static census). */
    void enumerate_peers(const peer_visitor_t& visit) const override {
        for (const auto& [name, ep] : peers_) visit(name);
    }
    /** @brief Resolve a peer NAME to its directed recording endpoint. */
    transport_t* peer_link(std::string_view peer) override {
        const auto it = peers_.find(std::string(peer));
        return it == peers_.end() ? nullptr : it->second.get();
    }

    /** @brief The (created-on-first-use) endpoint for @p peer. */
    peer_ep_t& peer(std::string_view name) {
        std::unique_ptr<peer_ep_t>& ep = peers_[std::string(name)];
        if (!ep) ep = std::make_unique<peer_ep_t>();
        return *ep;
    }
    /** @brief Poke one inbound frame tagged with the sending peer's name. */
    void inject_peer(std::string_view peer_name, std::span<const std::byte> frame) {
        (void)peer(peer_name);  // a peer that speaks exists
        peer_rx_.deliver_borrowed(peer_name, frame);
    }
    /** @brief Simulate the bus observing @p peer_name's session dead. */
    void peer_die(std::string_view peer_name) { notify_peer_down(peer_name); }

   private:
    std::map<std::string, std::unique_ptr<peer_ep_t>> peers_; /**< @brief name → endpoint. */
};

// ---------------------------------------------------------------------------

/** @brief Eviction stops deliveries to the dead link only; RFC-0005 bookkeeping unwinds. */
void test_evict_scoped_to_link() {
    std::printf("evict_link_edges is scoped to the departed link:\n");
    graph_t g;
    vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    vertex_handle_t ab = g.register_vertex(path_t("/a/b"), role_t::STORED_VALUE);
    vertex_handle_t x = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);

    std::size_t cli = 0, other = 0;
    g.set_remote_delivery_sink([&](const tr::graph::remote_delivery_t& d, const rope_t&) {
        (d.link == "cli" ? cli : other) += 1;
    });
    std::size_t local = 0;
    auto on_local = [&](const rope_t&) { ++local; };

    check(wire_sub(g, a, "cli", "c0") && wire_sub(g, a, "cli", "c1") && wire_sub(g, x, "cli", "c2"),
          "three edges over 'cli' (/a x2, /x)");
    check(wire_sub(g, a, "other", "o0"), "one edge over 'other' at /a");
    check(g.subscribe(path_t("/a"), on_local).has_value(), "one LOCAL edge at /a");

    check(g.write(a, make_value({0x01})).has_value(), "write /a pre-evict");
    check(cli == 2 && other == 1 && local == 1, "pre-evict fan-out reaches every edge");

    cli = other = local = 0;
    check(g.evict_link_edges("cli") == 3, "evict('cli') reports exactly its 3 edges");
    check(g.evict_link_edges("cli") == 0, "second evict is a no-op (idempotent)");
    check(g.evict_link_edges("ghost") == 0, "evicting an unknown link is a no-op");

    check(g.write(a, make_value({0x02})).has_value(), "write /a post-evict");
    check(cli == 0, "the dead link gets NO delivery");
    check(other == 1 && local == 1, "the other link's and the local edge still deliver");

    // The RFC-0005 subtree counters unwound by exactly k: a descendant write still
    // bubbles to /a's SURVIVING edges (and only once each).
    cli = other = local = 0;
    check(g.write(ab, make_value({0x03})).has_value(), "descendant write /a/b post-evict");
    check(cli == 0 && other == 1 && local == 1, "bubbling intact for survivors only");

    cli = other = local = 0;
    check(g.write(x, make_value({0x04})).has_value(), "write /x post-evict");
    check(cli == 0 && other == 0, "/x's only (evicted) edge is silent");

    // Regression: a fresh subscribe on a NEW link session delivers again.
    check(wire_sub(g, x, "cli:2", "c3"), "re-subscribe /x from the redialed session");
    other = 0;
    check(g.write(x, make_value({0x05})).has_value(), "write /x after re-subscribe");
    check(other == 1, "the redialed session's edge delivers");
}

/** @brief §D.2: surviving indices stable; freed slots reused before growth; reuse delivers. */
void test_slot_reuse_and_index_stability() {
    std::printf("slot reuse and index stability:\n");
    graph_t g;
    vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);

    check(wire_sub(g, v, "cli", "A") && wire_sub(g, v, "keep", "B") && wire_sub(g, v, "cli", "C"),
          "slots 0/1/2 = A(cli), B(keep), C(cli)");

    /** @brief The stored SUBSCRIBER bytes at :subscribers[idx] (empty on error). */
    const auto slot = [&](std::size_t idx) -> std::vector<std::byte> {
        const auto r = g.read(path_t(("/v:subscribers[" + std::to_string(idx) + "]").c_str()));
        return r ? rope_bytes(*r) : std::vector<std::byte>{};
    };
    check(g.evict_link_edges("cli") == 2, "evict('cli') frees slots 0 and 2");
    check(slot(1) == b_subscriber("B"), "survivor B still reads at index 1 (no renumber)");
    check(slot(0).empty() && slot(2).empty(), "freed slots read as cleared");

    check(wire_sub(g, v, "cli:2", "D"), "re-subscribe D");
    check(slot(0) == b_subscriber("D"), "D REUSED freed slot 0 (no growth)");
    check(wire_sub(g, v, "cli:2", "E"), "re-subscribe E");
    check(slot(2) == b_subscriber("E"), "E reused freed slot 2");
    check(wire_sub(g, v, "cli:2", "F"), "subscribe F with no free slot left");
    check(slot(3) == b_subscriber("F"), "F appended at slot 3 — growth only past reuse");
    check(slot(1) == b_subscriber("B"), "B undisturbed throughout");

    const auto subs = g.read_subscribers(v);
    check(subs.has_value() && subs->size() == 4, ":subscribers[] lists exactly the 4 active");

    // The reused slots DELIVER (the reclaimed shell became a real edge again).
    std::size_t hits = 0;
    g.set_remote_delivery_sink(
        [&](const tr::graph::remote_delivery_t& d, const rope_t&) { hits += d.link == "cli:2"; });
    check(g.write(v, make_value({0x11})).has_value(), "write /v");
    check(hits == 3, "D, E and F (two reused slots + one appended) all deliver");
}

/** @brief fwd_router_t::link_down = graph eviction + route-handle label drop, per link. */
void test_router_link_down() {
    std::printf("fwd_router_t::link_down (evict + clear_link):\n");
    graph_t g;
    fwd_router_t router(g);
    fake_link_t cli, other;
    router.add_child("cli", cli);
    router.add_child("other", other);

    (void)g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    // A compact-flagged subscribe over 'cli' (so label state forms), a plain one over
    // 'other'. SUBSCRIBER{ PATH, SETTINGS qos{delivery_compact=1} } mirrors
    // fwd_fanout_test's builder inline.
    std::vector<std::byte> sub_body = b_path({"cli"});
    {
        std::vector<std::byte> qos;
        append(qos, b_name("delivery_compact"));
        append(qos, b_value_u8(1));
        std::vector<std::byte> settings;
        tr::wire::emit_tlv(settings, type_t::SETTINGS, opt_t{.pl = true}, qos);
        append(sub_body, settings);
    }
    std::vector<std::byte> sub_compact;
    tr::wire::emit_tlv(sub_compact, type_t::SUBSCRIBER, opt_t{.pl = true}, sub_body);
    cli.inject(b_fwd(fwd_op_t::WRITE, b_path({"s"}), b_path({"cli"}), b_field_subscribers_append(),
                     sub_compact));
    other.inject(b_fwd(fwd_op_t::WRITE, b_path({"s"}), b_path({"other"}),
                       b_field_subscribers_append(), b_subscriber("o")));
    cli.drain();
    other.drain();

    check(g.write(path_t("/s"), rope_t{make_value(b_value_u8(0x21))}).has_value(),
          "write /s pre-departure");
    check(cli.count() >= 2, "compact flow established (ADVERTISE + COMPACT to cli)");
    check(router.handles().egress_route("cli", 1).has_value(),
          "route-handle egress binding exists for cli");
    cli.drain();
    other.drain();

    router.link_down("cli");
    check(!router.handles().egress_route("cli", 1).has_value(),
          "link_down dropped cli's label state");
    check(g.write(path_t("/s"), rope_t{make_value(b_value_u8(0x22))}).has_value(),
          "write /s post-departure");
    check(cli.count() == 0, "no delivery to the departed link");
    check(other.count() == 1, "the other link's delivery unaffected");
}

/** @brief The add_child-installed notifiers: transport down / bus peer-down reach link_down. */
void test_departure_notifier_seam() {
    std::printf("departure-notifier seam (p2p down + bus peer-down):\n");
    graph_t g;
    fwd_router_t router(g);
    fake_link_t p2p;
    fake_bus_t bus;
    router.add_child("p2p", p2p);
    router.add_child("bus", bus);

    (void)g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    p2p.inject(b_fwd(fwd_op_t::WRITE, b_path({"s"}), b_path({"p2p"}), b_field_subscribers_append(),
                     b_subscriber("p")));
    bus.inject_peer("10.0.0.7:51001",
                    b_fwd(fwd_op_t::WRITE, b_path({"s"}), b_path({"10.0.0.7:51001"}),
                          b_field_subscribers_append(), b_subscriber("q")));
    bus.inject_peer("10.0.0.8:51002",
                    b_fwd(fwd_op_t::WRITE, b_path({"s"}), b_path({"10.0.0.8:51002"}),
                          b_field_subscribers_append(), b_subscriber("r")));
    p2p.drain();
    bus.peer("10.0.0.7:51001").drain();
    bus.peer("10.0.0.8:51002").drain();

    check(g.write(path_t("/s"), rope_t{make_value(b_value_u8(0x31))}).has_value(), "write /s");
    check(p2p.count() == 1 && bus.peer("10.0.0.7:51001").count() == 1 &&
              bus.peer("10.0.0.8:51002").count() == 1,
          "all three sessions deliver pre-departure");
    p2p.drain();
    bus.peer("10.0.0.7:51001").drain();
    bus.peer("10.0.0.8:51002").drain();

    p2p.die();                       // the transport's one connection died
    bus.peer_die("10.0.0.7:51001");  // ONE bus peer hung up (a closed browser tab)
    check(g.write(path_t("/s"), rope_t{make_value(b_value_u8(0x32))}).has_value(),
          "write /s post-departure");
    check(p2p.count() == 0, "p2p down notifier evicted the child's edges");
    check(bus.peer("10.0.0.7:51001").count() == 0, "departed bus peer evicted");
    check(bus.peer("10.0.0.8:51002").count() == 1, "the OTHER bus peer still delivers");
}

/** @brief Eviction concurrent with writes: no crash, no deadlock, coherent finish (TSan gate). */
void test_concurrent_evict_vs_writes() {
    std::printf("concurrent writer x evict/re-subscribe (TSan gate):\n");
    graph_t g;
    vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    vertex_handle_t u = g.register_vertex(path_t("/v/u"), role_t::STORED_VALUE);
    std::atomic<std::size_t> delivered{0};
    g.set_remote_delivery_sink(
        [&](const tr::graph::remote_delivery_t&, const rope_t&) { delivered.fetch_add(1); });
    check(wire_sub(g, v, "cli", "s0") && wire_sub(g, v, "keep", "k0"), "seed edges");

    std::thread writer([&] {
        for (int i = 0; i < 400; ++i) {
            (void)g.write(v, make_value({0x41}));
            (void)g.write(u, make_value({0x42}));  // exercises the bubbling counters too
        }
    });
    std::thread evictor([&] {
        for (int i = 0; i < 100; ++i) {
            (void)g.evict_link_edges("cli");
            (void)wire_sub(g, v, "cli", "sx");
        }
    });
    writer.join();
    evictor.join();

    (void)g.evict_link_edges("cli");
    check(true, "no crash/deadlock under eviction x write");
    // Coherent end state: exactly the surviving 'keep' edge fires.
    delivered.store(0);
    check(g.write(v, make_value({0x43})).has_value(), "final write");
    check(delivered.load() == 1, "exactly the surviving edge delivers after the storm");
}

}  // namespace

/** @brief Entry: run every eviction sub-test; exit nonzero on any failure. */
int main() {
    std::printf("== edge_eviction_test ==\n");
    test_evict_scoped_to_link();
    test_slot_reuse_and_index_stability();
    test_router_link_down();
    test_departure_notifier_seam();
    test_concurrent_evict_vs_writes();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
