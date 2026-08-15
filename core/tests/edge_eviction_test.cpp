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
 *   - an edge admitted through `graph_t::field_write` (the RFC-0009 §D.1
 *     `:subscribers[N]` replace, which stores the inbound link only as the gate
 *     context) is reclaimed by that link's teardown too (#943);
 *   - the `add_child`-installed departure notifiers (point-to-point down, bus
 *     peer-down) reach the same hook — the seam every transport teardown fires;
 *   - eviction racing a writer thread is crash/TSan-clean (the concurrency gate);
 *   - plain subscribe/deliver still works after eviction (regression);
 *   - the `subscription_t` handle is OPAQUE (#867) — neither half of the `{producer vertex,
 *     slot}` pair is readable, and the pair cannot be forged and fed to `unsubscribe` —
 *     asserted at compile time, which is the only place a visibility change is observable.
 */

#include <algorithm>
#include <atomic>
#include <concepts>
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
#include <type_traits>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "graph_sinks.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subscription_t;
using tr::graph::vertex_handle_t;
using tr::net::bus_link_t;
using tr::net::fwd_router_t;
using tr::net::peer_handle_t;
using tr::net::transport_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

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

/** @brief A VALUE TLV over a little-endian u32 (the FIELD `[N]` index child). */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::byte b[4];
    for (std::size_t i = 0; i < 4; ++i) b[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFU);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(b, 4));
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

/**
 * @brief SUBSCRIBER{ PATH @p marker, SETTINGS qos{ NAME "delivery_compact" VALUE u8 1 } } —
 *        the RFC-0004 §E.1 opt-in, which is what forces a cold half onto the slot.
 */
std::vector<std::byte> b_subscriber_compact(std::string_view marker) {
    std::vector<std::byte> body = b_path({marker});
    std::vector<std::byte> qos;
    append(qos, b_name("delivery_compact"));
    append(qos, b_value_u8(1));
    std::vector<std::byte> settings;
    tr::wire::emit_tlv(settings, type_t::SETTINGS, opt_t{.pl = true}, qos);
    append(body, settings);
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

/**
 * @brief FIELD{ NAME "subscribers", VALUE u32 @p n, VALUE u8 ELEMENT } — the
 *        ":subscribers[N]" indexed selector (RFC-0004 §C: index then index_mode).
 */
std::vector<std::byte> b_field_subscribers_index(std::uint32_t n) {
    std::vector<std::byte> body;
    append(body, b_name("subscribers"));
    append(body, b_value_u32(n));
    append(body, b_value_u8(1));  // index_mode = ELEMENT, with an index ⇒ "[N]"
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

using tr::testing::b_fwd;
using tr::testing::b_fwd_mint;

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

    /** @brief The handle's index into @ref names_ is the peer's name (#1294). */
    [[nodiscard]] std::string_view peer_name(peer_handle_t peer, std::span<char>) const override {
        if (!peer.valid() || peer.index >= names_.size()) return {};
        return names_[peer.index];
    }

    /** @brief The (created-on-first-use) endpoint for @p peer. */
    peer_ep_t& peer(std::string_view name) {
        std::unique_ptr<peer_ep_t>& ep = peers_[std::string(name)];
        if (!ep) ep = std::make_unique<peer_ep_t>();
        return *ep;
    }
    /** @brief The handle for @p name, minting one on first sight (#1294). */
    peer_handle_t mint(std::string_view name) {
        for (std::size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == name) return {static_cast<std::uint32_t>(i), 1};
        names_.emplace_back(name);
        return {static_cast<std::uint32_t>(names_.size() - 1), 1};
    }
    /** @brief Poke one inbound frame tagged with the sending peer's HANDLE (#1294). */
    void inject_peer(std::string_view peer_name, std::span<const std::byte> frame) {
        (void)peer(peer_name);  // a peer that speaks exists
        peer_rx_.deliver_borrowed(mint(peer_name), frame);
    }
    /** @brief Simulate the bus observing @p peer_name's session dead. */
    void peer_die(std::string_view peer_name) { notify_peer_down(mint(peer_name), peer_name); }
    /** @brief Simulate an ACCEPTING listener admitting @p peer_name — endpoint first (a peer
     *         that arrives is resolvable), then the arrival notifier, the `slot_server_t`
     *         order (#1254: the notifier is what registers the session's identity anchor). */
    void peer_arrive(std::string_view peer_name) {
        (void)peer(peer_name);
        notify_peer_up(mint(peer_name), peer_name);
    }
    /** @brief Simulate the session FULLY torn down: the endpoint gone (so `peer_link`
     *         answers null, the RFC-0020 reject precondition), then the notifier. */
    void peer_kill(std::string_view peer_name) {
        peers_.erase(std::string(peer_name));
        notify_peer_down(mint(peer_name), peer_name);
    }

   private:
    std::map<std::string, std::unique_ptr<peer_ep_t>> peers_; /**< @brief name → endpoint. */
    std::vector<std::string> names_; /**< @brief index → peer name — the handle's meaning. */
};

/**
 * @brief One half of an in-memory point-to-point link PAIR: `send()` delivers synchronously
 *        into the twin's router-installed receiver, so two `fwd_router_t` nodes exchange
 *        real frames on one call stack — the #1223 two-node harness shape.
 */
class pipe_link_t : public transport_t {
   public:
    /** @brief Wire this half to its twin (call on both halves, crosswise). */
    void set_peer(pipe_link_t& p) { peer_ = &p; }
    /** @brief Count and forward one outbound frame into the twin node's ingress. */
    void send(std::span<const std::byte> frame) override {
        ++sent_;
        log_.emplace_back(frame.begin(), frame.end());
        if (peer_ != nullptr) peer_->inject(frame);
    }
    /** @brief The recorded outbound frames (test oracle). */
    [[nodiscard]] const std::vector<std::vector<std::byte>>& log() const { return log_; }
    /** @brief Poke one inbound frame into the router-installed receiver. */
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    /** @brief The number of frames this half has sent. */
    [[nodiscard]] std::size_t count() const { return sent_; }

   private:
    pipe_link_t* peer_ = nullptr; /**< @brief The twin half, registered on the other node. */
    std::size_t sent_ = 0;        /**< @brief Outbound frame count (single-thread harness). */
    std::vector<std::vector<std::byte>> log_; /**< @brief Recorded outbound frames. */
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
    const tr::testing::remote_sink_guard_t sink_guard(
        g, [&](const tr::graph::remote_delivery_t& d, const rope_t&) {
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

/**
 * @brief #1071: a departure costs the departing peer's OWN subscribed vertices, not the
 *        graph's whole subscribed set.
 *
 * The regression this pins is a LATENCY one, and latency is not directly assertable, so the
 * instrument is `graph_t::link_edge_candidates` — the count of vertices the eviction will
 * visit, which is exactly what the old whole-tree walk made proportional to the graph. The
 * test stages a deliberate imbalance: one vertex subscribed by the peer under test, many
 * subscribed by OTHER peers. Before the index the departing peer's cost was all of them.
 *
 * Asserting a RATIO rather than an absolute keeps this honest as the harness grows: the
 * property is that the bystanders contribute nothing, so the cost must not move when their
 * number does. It is measured twice, with a different bystander count each time.
 */
void test_departure_cost_is_scoped_to_the_peer() {
    std::printf("#1071 — a departure's cost tracks the departing peer, not the graph:\n");
    graph_t g;
    const tr::testing::remote_sink_guard_t sink_guard2(
        g, [](const tr::graph::remote_delivery_t&, const rope_t&) {});

    vertex_handle_t mine = g.register_vertex(path_t("/mine"), role_t::STORED_VALUE);
    check(wire_sub(g, mine, "cli", "m0"), "the peer under test subscribes on ONE vertex");
    check(g.link_edge_candidates("cli") == 1, "its departure visits exactly that one vertex");

    // Twenty bystander vertices, each subscribed by a DIFFERENT peer. Under the old walk
    // every one of these became a candidate for 'cli' as well, because the predicate was
    // "this vertex has any subscriber", with the link tested only per edge afterwards.
    for (int i = 0; i < 20; ++i) {
        const std::string p = "/bystander" + std::to_string(i);
        vertex_handle_t b = g.register_vertex(path_t(p.c_str()), role_t::STORED_VALUE);
        check(wire_sub(g, b, "peer" + std::to_string(i), "b"), "a bystander peer subscribes");
    }
    check(g.link_edge_candidates("cli") == 1,
          "20 bystander subscriptions later, the cost is STILL one vertex");

    for (int i = 20; i < 60; ++i) {
        const std::string p = "/bystander" + std::to_string(i);
        vertex_handle_t b = g.register_vertex(path_t(p.c_str()), role_t::STORED_VALUE);
        check(wire_sub(g, b, "peer" + std::to_string(i), "b"), "another bystander peer");
    }
    check(g.link_edge_candidates("cli") == 1,
          "60 bystanders: the departing peer's cost has not moved at all");

    // And the cost that DOES track the peer tracks it exactly: its own second vertex counts.
    vertex_handle_t mine2 = g.register_vertex(path_t("/mine2"), role_t::STORED_VALUE);
    check(wire_sub(g, mine2, "cli", "m1"), "the peer subscribes on a second vertex of its own");
    check(g.link_edge_candidates("cli") == 2, "its own subscription DOES add to its cost");
    check(wire_sub(g, mine2, "cli", "m2"), "a SECOND edge on a vertex it already subscribed");
    check(g.link_edge_candidates("cli") == 2,
          "a repeat subscription on the same vertex does not grow the cost (dedup)");

    // The eviction itself is still exact — cheapness must not have cost correctness.
    check(g.evict_link_edges("cli") == 3, "evict('cli') still reclaims all three of its edges");
    check(g.link_edge_candidates("cli") == 0, "and its index entry is gone with them");
    check(g.link_edge_candidates("peer0") == 1, "a bystander's own cost is untouched");
}

/**
 * @brief #1071: the index survives a full departure — a redialing peer that reuses the SAME
 *        link NAME is evictable again.
 *
 * The hazard is specific to indexing and invisible to the old walk, which rediscovered every
 * vertex from the tree each time and so could not go stale. A whole-link eviction REMOVES the
 * index entry (otherwise a departed link's list would be immortal); if the re-subscribe after
 * it failed to recreate the entry, the second departure would find no candidates and silently
 * leak the redialed session's edges — the same disclosure class #1223 spent five steps on.
 */
void test_index_recreated_after_a_full_eviction() {
    std::printf("#1071 — a same-NAME redial is indexed, and evictable, again:\n");
    graph_t g;
    std::size_t cli = 0;
    const tr::testing::remote_sink_guard_t sink_guard3(
        g, [&](const tr::graph::remote_delivery_t& d, const rope_t&) {
            if (d.link == "cli") ++cli;
        });
    vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);

    check(wire_sub(g, v, "cli", "first"), "the first session subscribes");
    check(g.evict_link_edges("cli") == 1, "it departs and its edge is reclaimed");
    check(g.link_edge_candidates("cli") == 0, "the index entry went with it");

    check(wire_sub(g, v, "cli", "second"), "a redial reuses the SAME link name and subscribes");
    check(g.link_edge_candidates("cli") == 1, "the entry is RECREATED — not lost with the first");
    cli = 0;
    check(g.write(v, make_value({0x01})).has_value(), "write reaches the redialed session");
    check(cli == 1, "the redialed edge delivers");

    check(g.evict_link_edges("cli") == 1, "the SECOND departure reclaims the redial's edge too");
    cli = 0;
    check(g.write(v, make_value({0x02})).has_value(), "write after the second departure");
    check(cli == 0, "and nothing is delivered to the dead session — no leaked edge");
}

/**
 * @brief #1266: the index insert is IDEMPOTENT, and many peers sharing a name prefix stay
 *        independent of one another across a departure.
 *
 * `index_link_vertex`'s declaration has always promised that "a vertex already listed for
 * `link` is not listed twice" — the promise the candidate list's bound rests on. The code did
 * not keep it: it appended unconditionally and squashed duplicates in a later amortized
 * compaction, so a peer renewing a subscription it already holds grew the list to 2D+8 and
 * paid a sort for it, repeatedly, forever. `link_edge_candidates` could not see the gap
 * because it compacts before reporting, which is exactly why the claim survived so long.
 *
 * This pins it where a caller CAN see it: a long run of repeat subscriptions must leave the
 * departure's cost where one subscription left it, and it must do so with the compaction
 * floor (8) crossed many times over, so a regression to append-then-compact shows up as a
 * count above the distinct set rather than as a silent slowdown.
 *
 * The bystander half uses the names the ESP link actually mints (`<ip>:<port>`, #1071's own
 * scenario), so the prefix-sharing case is covered by construction: 96 peers whose keys agree
 * for their first ten characters must each index exactly their own vertices, and one of them
 * departing must disturb no other.
 */
void test_index_insert_is_idempotent() {
    std::printf("#1266 — the index insert is idempotent, and peers stay independent:\n");
    graph_t g;
    const tr::testing::remote_sink_guard_t sink_guard_many(
        g, [](const tr::graph::remote_delivery_t&, const rope_t&) {});

    constexpr int kLinks = 96;  // well past any node's live-link count, prefixes all shared
    std::vector<std::string> peers;
    std::vector<vertex_handle_t> vs;
    for (int i = 0; i < kLinks; ++i) {
        peers.push_back("192.168.4." + std::to_string(i % 250 + 1) + ":" +
                        std::to_string(40000 + i));
        vs.push_back(g.register_vertex(path_t("/v" + std::to_string(i)), role_t::STORED_VALUE));
    }
    // Each peer subscribes on its OWN vertex and on one it shares with its neighbour, so a
    // confusion between two records shows up as a candidate count of 1 or 3, not just as a
    // failed lookup.
    for (int i = 0; i < kLinks; ++i) {
        check(wire_sub(g, vs[i], peers[i], "own"), "the peer subscribes on its own vertex");
        check(wire_sub(g, vs[(i + 1) % kLinks], peers[i], "shared"), "and on its neighbour's");
    }
    bool all_two = true;
    for (int i = 0; i < kLinks; ++i) all_two &= g.link_edge_candidates(peers[i]) == 2;
    check(all_two, "every one of the 96 prefix-sharing peers indexes exactly its own 2 vertices");

    // The idempotence itself: 200 renewals over the two vertices this peer already holds.
    // That is 25x the compaction floor, so an append-then-compact insert reaches 2D+8 and
    // sorts repeatedly on the way; an idempotent one does nothing at all after the search.
    for (int rep = 0; rep < 200; ++rep)
        check(wire_sub(g, vs[0], peers[0], "again"), "peer 0 re-subscribes on a vertex it holds");
    check(g.link_edge_candidates(peers[0]) == 2,
          "200 renewals later its departure still visits exactly its 2 vertices");
    // And a genuinely NEW vertex still lands — idempotence must not have become inertness.
    check(wire_sub(g, vs[3], peers[0], "new"), "the same peer subscribes on a THIRD vertex");
    check(g.link_edge_candidates(peers[0]) == 3, "which does add to its departure's cost");

    // A departure in the middle of the live set disturbs exactly one peer.
    const int mid = kLinks / 2;
    check(g.evict_link_edges(peers[mid]) == 2, "a middle peer departs, taking both its edges");
    check(g.link_edge_candidates(peers[mid]) == 0, "its index entry is gone");
    bool rest_intact = true;
    for (int i = 1; i < kLinks; ++i)
        if (i != mid) rest_intact &= g.link_edge_candidates(peers[i]) == 2;
    check(rest_intact, "every other prefix-sharing peer still indexes exactly its own 2 vertices");

    // The departed name is free to come back, on its own fresh entry.
    check(wire_sub(g, vs[mid], peers[mid], "redial"), "the departed peer redials");
    check(g.link_edge_candidates(peers[mid]) == 1, "and is indexed on its own, one vertex");
}

/**
 * @brief #1071: the index is a SUPERSET, and a stale entry is a no-op rather than a fault.
 *
 * Individual removals — an explicit unsubscribe, a route-scoped reclaim — deliberately do
 * NOT un-index their vertex, because the cost of being wrong in that direction is a wasted
 * comparison while the other direction leaks an edge. This pins the tolerated asymmetry so a
 * later "optimization" that prunes on unsubscribe has to argue with a test rather than with a
 * comment.
 */
void test_stale_index_entries_are_harmless() {
    std::printf("#1071 — a stale index entry costs a no-op, never a wrong answer:\n");
    graph_t g;
    std::size_t cli = 0;
    const tr::testing::remote_sink_guard_t sink_guard4(
        g, [&](const tr::graph::remote_delivery_t& d, const rope_t&) {
            if (d.link == "cli") ++cli;
        });
    vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    vertex_handle_t w = g.register_vertex(path_t("/w"), role_t::STORED_VALUE);

    check(wire_sub(g, v, "cli", "a"), "the peer subscribes at /v");
    check(wire_sub(g, w, "cli", "b"), "and at /w");
    check(g.link_edge_candidates("cli") == 2, "two candidate vertices");

    // The route-scoped reclaim takes BOTH edges: wire_sub stores the link name as the return
    // route, so one route matches both. That leaves the link with no edges at all — and, by
    // design, with both of its index entries still standing.
    const std::vector<std::byte> route = b_path({std::string("cli")});
    check(g.evict_route_edges("cli", route) == 2,
          "the narrow route-scoped reclaim takes both edges");
    check(g.link_edge_candidates("cli") == 2,
          "both index entries are now STALE — the narrow path does not un-index");

    cli = 0;
    check(g.write(v, make_value({0x01})).has_value(), "write /v after the narrow reclaim");
    check(g.write(w, make_value({0x02})).has_value(), "write /w after the narrow reclaim");
    check(cli == 0, "the edges really are gone — the staleness is in the index, not the graph");

    // The whole-link eviction now runs against two vertices that hold nothing. The contract
    // is that this is a NO-OP reporting zero, not a miscount and not a fault.
    check(g.evict_link_edges("cli") == 0, "the whole-link eviction over stale entries reports 0");
    check(g.link_edge_candidates("cli") == 0, "and clears them, so the staleness is not immortal");
}

/**
 * @brief #1071 + #943: a FIELD-WRITE-admitted edge is indexed under the link that admitted
 *        it, on a vertex that link never subscribed on by any other door.
 *
 * The index key must be `vertex_t::evict_link_edges`'s own spelling — `remote->link`, falling
 * back to `remote->caller` — and this is the case that separates the two. A `:subscribers[N]`
 * replace arriving over a link lands in `graph_t::field_write`, which stores the inbound link
 * ONLY as the gate context and leaves `link` empty precisely so dispatch does not grow a
 * phantom remote leg (#943). Keying the index on `link` alone therefore un-indexes exactly
 * these edges, and a departure walks straight past them.
 *
 * #943's own test cannot see that: it seeds slot 0 with a `subscribe_wire` append over the
 * SAME link, which indexes the vertex anyway, so the fallback is masked. Here the seed comes
 * over a DIFFERENT link, so the only thing that can put this vertex on 'cli's candidate list
 * is the caller fallback. Ablating the fallback to `s.remote->link` turns the two assertions
 * below red — that is the check that this test is not decorative.
 */
void test_field_write_admitted_edge_is_indexed_under_its_caller() {
    std::printf("#1071/#943 — a field-write-admitted edge is indexed by its CALLER:\n");
    graph_t g;
    fwd_router_t router(g);
    fake_link_t seed_link;
    fake_link_t cli;
    (void)router.add_child("seed", seed_link);
    (void)router.add_child("cli", cli);

    (void)g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/t"), role_t::STORED_VALUE);

    const auto value_of = [&](const char* p) -> std::vector<std::byte> {
        const auto r = g.read(path_t(p));
        return r ? rope_bytes(**r) : std::vector<std::byte>{};
    };

    // Seed slot 0 over a DIFFERENT link, so /p is indexed for 'seed' and NOT for 'cli'.
    // A `[N]` replace requires the slot to exist, and this is the door that creates one.
    seed_link.inject(b_fwd(fwd_op_t::WRITE, b_path({"p"}), b_path({"seed"}),
                           b_field_subscribers_append(), b_subscriber("seed")));
    seed_link.drain();
    check(g.link_edge_candidates("seed") == 1, "/p is a candidate for the SEEDING link");
    check(g.link_edge_candidates("cli") == 0, "and is NOT yet a candidate for 'cli'");

    // The door under test: a `:subscribers[0]` REPLACE arriving over 'cli'. It goes through
    // field_write, which leaves `remote->link` empty and stores 'cli' as the caller only.
    cli.inject(b_fwd(fwd_op_t::WRITE, b_path({"p"}), b_path({"cli"}), b_field_subscribers_index(0),
                     b_subscriber("t")));
    cli.drain();

    check(g.write(path_t("/p"), rope_t{make_value(b_value_u8(0x51))}).has_value(),
          "write /p while the 'cli' session is up");
    check(value_of("/t") == b_value_u8(0x51), "the field-write-admitted edge IS delivering");

    // THE ASSERTION. Without the caller fallback this reads 0 and the edge is invisible to
    // 'cli's teardown — a subscriber edge that outlives the session that created it.
    check(g.link_edge_candidates("cli") == 1,
          "the caller fallback put /p on 'cli's candidate list");

    cli.die();  // the transport reports the connection dead → fwd_router_t::link_down

    check(g.write(path_t("/p"), rope_t{make_value(b_value_u8(0x52))}).has_value(),
          "write /p after the 'cli' session departed");
    check(value_of("/t") == b_value_u8(0x51),
          "the departed session's edge delivered NOTHING — it was found and reclaimed");
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
        return r ? rope_bytes(**r) : std::vector<std::byte>{};
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
    const tr::testing::remote_sink_guard_t sink_guard5(
        g,
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
    (void)router.add_child("cli", cli);
    (void)router.add_child("other", other);

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
    (void)router.add_child("p2p", p2p);
    (void)router.add_child("bus", bus);

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

/**
 * @brief #943 — a FIELD-WRITE-admitted subscriber edge is reclaimed by its link's teardown.
 *
 * `graph_t::field_write`'s `:subscribers[N]` arm (RFC-0009 §D.1 replace) admits an edge whose
 * cold half stores ONLY the gate context: the edge re-dispatches to a LOCAL target, so it has
 * no return route and no delivery link. `vertex_t::evict_link_edges` matched on the delivery
 * link alone, so this edge was never reclaimed — boot-lifetime, still `active`, still holding
 * its slot against `add_edge` reuse, and still writing into its target under a departed
 * session's context.
 *
 * Driven through the PRODUCTION wiring (a `fwd_router_t` child fed real `FWD` frames), because
 * the whole question is which door the wire actually enters. The oracle is the re-dispatch
 * TARGET's stored value, not a callback on it: delivery terminates at the target (ADR-0051),
 * so the target's own subscribers never fire.
 */
void test_evict_reaches_field_write_admitted_edges() {
    std::printf("link teardown reclaims a FIELD-WRITE-admitted edge (#943):\n");
    graph_t g;
    fwd_router_t router(g);
    fake_link_t cli;
    (void)router.add_child("cli", cli);

    (void)g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/t"), role_t::STORED_VALUE);

    /** @brief The flat stored bytes at @p p (empty on error). */
    const auto value_of = [&](const char* p) -> std::vector<std::byte> {
        const auto r = g.read(path_t(p));
        return r ? rope_bytes(**r) : std::vector<std::byte>{};
    };

    // A `[N]` replace needs slot N to EXIST (admit_subscriber refuses OUT_OF_RANGE), so seed
    // slot 0 the ordinary way: a wire `:subscribers[]` append, which the resolver diverts to
    // subscribe_wire — that door stores the link and has always been evictable.
    cli.inject(b_fwd(fwd_op_t::WRITE, b_path({"p"}), b_path({"cli"}), b_field_subscribers_append(),
                     b_subscriber("seed")));
    cli.drain();

    // The door under test: a REMOTE `:subscribers[0]` REPLACE bearing a SUBSCRIBER whose PATH
    // names the local vertex /t. This one lands in graph_t::field_write, not subscribe_wire.
    cli.inject(b_fwd(fwd_op_t::WRITE, b_path({"p"}), b_path({"cli"}), b_field_subscribers_index(0),
                     b_subscriber("t")));
    cli.drain();

    check(g.write(path_t("/p"), rope_t{make_value(b_value_u8(0x51))}).has_value(),
          "write /p while the session is up");
    check(value_of("/t") == b_value_u8(0x51),
          "the field-write-admitted edge IS delivering into /t (the test is not vacuous)");
    // The edge carries no return route, so it must take the LOCAL leg only. This is why the
    // fix is in the eviction predicate and not an `r.link.assign(caller)` at the admission
    // door: a non-empty link there would make dispatch_edge add a phantom remote leg here.
    check(cli.count() == 0, "no phantom remote delivery for a local-target edge");

    cli.die();  // the transport observes its one connection dead → fwd_router_t::link_down

    check(g.write(path_t("/p"), rope_t{make_value(b_value_u8(0x52))}).has_value(),
          "write /p after the session departed");
    check(value_of("/p") == b_value_u8(0x52), "the producer itself still took the write");
    check(value_of("/t") == b_value_u8(0x51),
          "the departed session's edge delivered NOTHING after teardown");
    const auto slot0 = g.read(path_t("/p:subscribers[0]"));
    check(!slot0.has_value() || rope_bytes(**slot0).empty(),
          "slot 0 was RECLAIMED, not just left active (free for add_edge reuse)");
    check(g.evict_link_edges("cli") == 0, "a second eviction for 'cli' finds nothing left");
}

/**
 * @brief #1056 — an eviction keyed on the EMPTY link name matches nothing.
 *
 * The predicate keys on the link an edge was ADMITTED over: `subscriber_remote_t::link` when
 * the cold half carries one, the stored `caller` gate context otherwise (#943). A LOCAL door
 * passes the empty context, so a local edge's admitting spelling is empty too — and an empty
 * PARAMETER compared equal to it, reclaiming the edge. The case is reachable because a local
 * edge CAN carry a cold half: the shared SUBSCRIBER parse calls `ensure_remote()` for the
 * `delivery_compact` opt-in at every door, local ones included.
 *
 * `graph_t::evict_link_edges` returns a count and has no error channel, so the empty key is a
 * no-op returning 0, not a new status. Two arms, and the CONTROL arm is what identifies the
 * mechanism: the compact edge (cold half, both spellings empty) is the one that was reclaimed;
 * the plain edge (no cold half at all, so `s.remote == nullptr` skips it) never was, and
 * asserting on it alone would pass no matter what the predicate did.
 */
void test_empty_link_name_evicts_nothing() {
    std::printf("an eviction keyed on the EMPTY link name matches nothing (#1056):\n");
    graph_t g;
    vertex_handle_t p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/tc"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/tp"), role_t::STORED_VALUE);

    /** @brief The flat stored bytes at @p at (empty on error). */
    const auto value_of = [&](const char* at) -> std::vector<std::byte> {
        const auto r = g.read(path_t(at));
        return r ? rope_bytes(**r) : std::vector<std::byte>{};
    };

    // Both admitted through the LOCAL `:subscribers[]` field-write door — the 3-arg write, so
    // the caller context is empty. `/tc` opts into delivery_compact (⇒ a cold half whose `link`
    // AND `caller` are both empty); `/tp` carries no settings at all (⇒ no cold half).
    const auto append_fp = path_t::parse("/p:subscribers[]");
    check(append_fp.has_value(), "the :subscribers[] append field-path parses");
    check(append_fp.has_value() &&
              g.write(p, append_fp->field(), make_value(b_subscriber_compact("tc"))).has_value(),
          "admit a LOCAL delivery_compact subscriber under an empty caller");
    check(append_fp.has_value() &&
              g.write(p, append_fp->field(), make_value(b_subscriber("tp"))).has_value(),
          "admit a plain LOCAL subscriber under an empty caller");

    check(g.write(path_t("/p"), rope_t{make_value(b_value_u8(0x61))}).has_value(), "write /p");
    check(value_of("/tc") == b_value_u8(0x61),
          "the compact local edge IS delivering (the test is not vacuous)");
    check(value_of("/tp") == b_value_u8(0x61), "the plain local edge IS delivering");

    check(g.evict_link_edges("") == 0, "evict('') reclaims nothing and reports 0");

    check(g.write(path_t("/p"), rope_t{make_value(b_value_u8(0x62))}).has_value(),
          "write /p after the empty-key eviction");
    check(value_of("/tc") == b_value_u8(0x62),
          "the compact local edge STILL delivers after evict('')");
    check(value_of("/tp") == b_value_u8(0x62), "the plain local edge still delivers");

    // The listener bookkeeping must be untouched too: a DESCENDANT write still bubbles to /p's
    // edges (RFC-0005). A silent over-unwind here would strand the counters below zero.
    vertex_handle_t pd = g.register_vertex(path_t("/p/d"), role_t::STORED_VALUE);
    check(g.write(pd, make_value(b_value_u8(0x63))).has_value(), "descendant write /p/d");
    check(value_of("/tc") == b_value_u8(0x63) && value_of("/tp") == b_value_u8(0x63),
          "a descendant write still bubbles to both edges (RFC-0005 counters intact)");
}

/** @brief Eviction concurrent with writes: no crash, no deadlock, coherent finish (TSan gate). */
void test_concurrent_evict_vs_writes() {
    std::printf("concurrent writer x evict/re-subscribe (TSan gate):\n");
    graph_t g;
    vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    vertex_handle_t u = g.register_vertex(path_t("/v/u"), role_t::STORED_VALUE);
    std::atomic<std::size_t> delivered{0};
    const tr::testing::remote_sink_guard_t sink_guard6(
        g, [&](const tr::graph::remote_delivery_t&, const rope_t&) { delivered.fetch_add(1); });
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

/**
 * @brief Clearing a wire edge RECLAIMS its retained state — it does not merely deactivate it.
 *
 * `clear_edge` used to flip `active` and nothing else, so a cleared edge kept its
 * `target_key` buffer, its `source_view` **segment pin** and the whole cold `remote` half
 * resident until an unrelated `add_edge` happened to reuse that index. On a node whose
 * subscribers come and go, that is a frame segment held alive indefinitely by a dead edge.
 *
 * Observed through the segment's own refcount, because that is the thing that actually
 * matters and a behavioural test cannot see it: a deactivated slot and a reclaimed slot route
 * identically. It must be a WIRE edge — the in-process `subscribe(path, callback)` sugar
 * retains no view, so the same assertion against it would pass no matter what `clear_edge`
 * did, which is a test that cannot fail.
 *
 * The 80-byte slot SHELL must survive: RFC-0009 §D.2 makes `:subscribers[]` indices stable.
 */
void test_clear_edge_releases_the_slot_pin() {
    std::printf("clear_edge reclaims retained slot state:\n");
    graph_t g;
    vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);

    // Build the SUBSCRIBER view over a segment this test also holds, so the slot's retained
    // pin is visible here as an extra reference.
    const std::vector<std::byte> sub_tlv = b_subscriber("cli:9");
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(sub_tlv.size());
    std::memcpy(seg->bytes.data(), sub_tlv.data(), sub_tlv.size());
    const auto refs = [&] { return seg->refcount.load_acquire(); };
    const std::uint_least32_t held = refs();

    check(g.subscribe_wire(a, view_t::over(tr::view::segment_ptr_t(seg)),
                           make_value(b_path({"cli:9"})), "cli:9")
              .has_value(),
          "wire subscriber bound");
    const std::uint_least32_t pinned = refs();
    check(pinned > held, "the slot PINS the SUBSCRIBER segment while the edge is live");

    // Clear it through the wire door (`:subscribers[0]` cleared), then require the pin gone.
    const auto clear_fp = path_t::parse("/a:subscribers[0]");
    check(clear_fp.has_value(), "the clear field-path parses");
    check(g.write(a, clear_fp->field(), make_value({0x09, 0x00, 0x00, 0x00})).has_value(),
          "clear the slot through the :subscribers[0] field-write door");
    check(refs() == held, "clearing RELEASES the pin — the segment is no longer retained");
}

/** @brief True iff `T` hands a caller the producer vertex under its pre-#867 member name. */
template <typename T>
concept reads_producer_vertex = requires(T& s) { s.vertex; };

/** @brief True iff `T` hands a caller the `:subscribers[]` index under its pre-#867 name. */
template <typename T>
concept reads_slot_index = requires(T& s) { s.slot; };

/** @brief True iff either half of the pair is reachable under its post-#867 name. */
template <typename T>
concept reads_renamed_pair = requires(T& s) { s.vertex_; } || requires(T& s) { s.slot_; };

/**
 * @brief The #867 encapsulation guard: `subscription_t` is opaque exactly as `vertex_handle_t`
 *        is — the producer `vertex_t*` and the slot index are `graph_t`'s state, not the API
 *        user's, and the compiler is what enforces that.
 *
 * Access checking happens during substitution, so an inaccessible member makes a
 * requires-expression FALSE rather than ill-formed: these assertions observe the visibility
 * itself, which no runtime test can. Before the change the handle was an aggregate of two PUBLIC
 * members, so `sub.vertex->store(...)` and `sub.vertex->mark_unregistered()` compiled for any
 * caller — lock-contract mutators the graph's own locking discipline owns — and a forged
 * `subscription_t{any_ptr, any_index}` could be fed to `unsubscribe()`. Reverting the header hunk
 * turns this TU into a compile error on all of these EXCEPT @ref reads_renamed_pair, which guards
 * the other direction: re-publishing the members under their current names.
 */
static_assert(!reads_producer_vertex<subscription_t>,
              "#867: the producer vertex must not be reachable through a subscription handle");
static_assert(!reads_slot_index<subscription_t>,
              "#867: the :subscribers[] slot index must not be readable from a handle");
static_assert(!reads_renamed_pair<subscription_t>,
              "#867: renaming the members does not make them public again");
static_assert(!std::is_constructible_v<subscription_t, tr::graph::vertex_t*, std::size_t>,
              "#867: a caller must not be able to forge a handle from a pointer and an index");
static_assert(!std::is_aggregate_v<subscription_t>,
              "#867: aggregate init is the forging route a public pair leaves open");

/** @brief What deliberately STAYS public: default-construct, pass by value, compare. */
static_assert(std::is_default_constructible_v<subscription_t>);
static_assert(std::is_trivially_copyable_v<subscription_t>);
static_assert(std::equality_comparable<subscription_t>);

}  // namespace

/** @brief Entry: run every eviction sub-test; exit nonzero on any failure. */
/**
 * @brief The in-process unsubscribe handle (graph_t::subscribe → subscription_t → unsubscribe):
 *        the edge goes silent AND the RFC-0005 bubbling counters unwind, and the freed slot reuses.
 */
void test_local_unsubscribe() {
    std::printf("in-process unsubscribe by subscription_t handle:\n");
    graph_t g;
    vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    vertex_handle_t ab = g.register_vertex(path_t("/a/b"), role_t::STORED_VALUE);

    std::size_t local = 0;
    auto on_local = [&](const rope_t&) { ++local; };

    const auto sub = g.subscribe(path_t("/a"), on_local);
    check(sub.has_value(), "subscribe returns a subscription_t handle");

    check(g.write(a, make_value({0x01})).has_value(), "write /a while subscribed");
    check(local == 1, "the callback fires while subscribed");

    // RFC-0005: a descendant write bubbles to /a's local edge while subscribed.
    local = 0;
    check(g.write(ab, make_value({0x02})).has_value(), "descendant write /a/b while subscribed");
    check(local == 1, "a descendant write bubbles to the local subscriber");

    // Unsubscribe by handle: the edge goes silent AND the bubbling counters unwind.
    check(sub.has_value() && g.unsubscribe(*sub).has_value(), "unsubscribe by handle succeeds");
    local = 0;
    check(g.write(a, make_value({0x03})).has_value(), "write /a post-unsubscribe");
    check(local == 0, "the callback does NOT fire after unsubscribe");
    check(g.write(ab, make_value({0x04})).has_value(), "descendant write /a/b post-unsubscribe");
    check(local == 0, "the descendant no longer bubbles (RFC-0005 counters unwound)");

    // Defensive: a second unsubscribe and a default-constructed handle are NOT_FOUND no-ops.
    check(sub.has_value() && !g.unsubscribe(*sub).has_value(),
          "a second unsubscribe is NOT_FOUND (slot already inactive)");
    check(!g.unsubscribe(subscription_t{}).has_value(), "a null handle unsubscribes to NOT_FOUND");

    // §D.2 slot reuse: a fresh subscribe reuses the freed slot and delivers; the old stays silent.
    std::size_t again = 0;
    auto on_again = [&](const rope_t&) { ++again; };
    const auto sub2 = g.subscribe(path_t("/a"), on_again);
    // The handle is opaque, so the reuse is observed the only way a caller can observe it:
    // the fresh handle compares EQUAL to the retired one — same producer, same slot index.
    check(sub2.has_value() && sub.has_value() && *sub2 == *sub,
          "re-subscribe reuses the freed slot (index stability §D.2)");
    check(g.write(a, make_value({0x05})).has_value(), "write /a after re-subscribe");
    check(again == 1 && local == 0, "the new subscriber delivers; the old one stays silent");
}

/**
 * @brief The `FWD{REPLY, kind=ERROR, STATUS{ERROR{code}}}` refusal frame `reject_bus_name_hop`
 *        emits, hand-built so the test can vary the CODE — the reclaim's discriminator.
 */
std::vector<std::byte> refusal_frame(const std::vector<std::byte>& refused_route,
                                     std::uint16_t code) {
    std::vector<std::byte> code_v;
    const std::byte c[2] = {static_cast<std::byte>(code & 0xFFU),
                            static_cast<std::byte>(code >> 8)};
    tr::wire::emit_tlv(code_v, type_t::VALUE, opt_t{}, std::span<const std::byte>(c, 2));
    std::vector<std::byte> err;
    tr::wire::emit_tlv(err, type_t::ERROR, opt_t{.pl = true}, code_v);
    std::vector<std::byte> status;
    tr::wire::emit_tlv(status, type_t::STATUS, opt_t{.pl = true}, err);
    std::vector<std::byte> payload = b_value_u8(1);  // kind VALUE u8 = ERROR
    append(payload, status);
    return b_fwd(fwd_op_t::REPLY, b_path({}), refused_route, {}, payload);
}

/**
 * @brief #1223 step 5 — a delivery refused `tr::path::invalid` (RFC-0020) reclaims the ONE
 *        edge that stored the refused route, with zero new wire bytes and no link bounce.
 *
 * Two nodes on one call stack. A hosts client sessions on a bus mount; B is the producer one
 * hop away (A names B "b", B names A "a"). Session `p0` subscribes THROUGH A to B's
 * `/sensor`, so B holds a remote edge keyed by the arrival link `"a"` whose stored return
 * route ends at `p0` — the #1223 leak shape: `p0`'s death is visible only to A, and B's
 * `link_down("a")` never fires because the A<->B link stays healthy.
 *
 * The reclaim closes the loop on frames that ALREADY ship: A's RFC-0020 bus-residual reject
 * echoes the refused route back as the reply `src` ("so it can correlate"), and B now
 * correlates. Assertions:
 *   - positive control FIRST (the #1223 refutation rule): live sessions receive;
 *   - one bounced publish after `p0` dies reclaims exactly `p0`'s edge — the next publish
 *     emits nothing for it, while a second live session's edge on the SAME mount and link
 *     survives and keeps delivering;
 *   - the discriminator is the STATUS code, proven order-dependently: a refusal echoing the
 *     LIVE session's route with a non-`path::invalid` code evicts nothing, then the same
 *     route with `tr::path::invalid` evicts — so the hold was the code, not the route.
 */
void test_refused_route_reclaims_the_edge() {
    std::printf("refused-route reclaim (#1223 step 5, the RFC-0020 producer side):\n");
    graph_t ga, gb;
    fwd_router_t ra(ga), rb(gb);
    pipe_link_t a_to_b, b_to_a;
    a_to_b.set_peer(b_to_a);
    b_to_a.set_peer(a_to_b);
    fake_bus_t bus;
    (void)ra.add_child("bus", bus);
    (void)ra.add_child("b", a_to_b);
    (void)rb.add_child("a", b_to_a);
    vertex_handle_t sensor = gb.register_vertex(path_t("/sensor"), role_t::STORED_VALUE);

    // Sessions p0 (doomed) and q1 (stays live) subscribe to /b/sensor through A.
    bus.inject_peer("p0", b_fwd(fwd_op_t::WRITE, b_path({"b", "sensor"}), b_path({}),
                                b_field_subscribers_append(), b_subscriber("doomed")));
    bus.inject_peer("q1", b_fwd(fwd_op_t::WRITE, b_path({"b", "sensor"}), b_path({}),
                                b_field_subscribers_append(), b_subscriber("alive")));
    const auto subs0 = gb.read_subscribers(sensor);
    check(subs0.has_value() && subs0->size() == 2, "B holds two remote edges");

    // Positive control FIRST (the #1223 refutation rule: no eviction claim without it).
    // Counts are read as DELTAS: the subscribes above already put one ack on each session.
    std::size_t p0_seen = bus.peer("p0").count();
    std::size_t q1_seen = bus.peer("q1").count();
    check(gb.write(sensor, make_value(b_value_u8(0x11))).has_value(), "publish 1");
    check(bus.peer("p0").count() == p0_seen + 1 && bus.peer("q1").count() == q1_seen + 1,
          "positive control: both live sessions receive publish 1");

    // p0 dies. A knows; B does not — the edge survives, and would forever (#1223).
    bus.peer_kill("p0");
    const auto subs1 = gb.read_subscribers(sensor);
    check(subs1.has_value() && subs1->size() == 2, "B still holds p0's edge (the leak shape)");

    // Publish 2: p0's delivery bounces off A (its residual names no current peer — the
    // RFC-0020 reject), and the addressed refusal reclaims exactly p0's edge on B.
    q1_seen = bus.peer("q1").count();
    check(gb.write(sensor, make_value(b_value_u8(0x22))).has_value(),
          "publish 2 (one delivery bounces)");
    const auto subs2 = gb.read_subscribers(sensor);
    check(subs2.has_value() && subs2->size() == 1, "the refused route's edge is reclaimed");
    check(bus.peer("q1").count() == q1_seen + 1, "the live session still received publish 2");

    // Publish 3: B emits exactly ONE delivery toward A — the reclaimed edge is gone from
    // the fan-out, and the A<->B link never went down.
    const std::size_t before = b_to_a.count();
    q1_seen = bus.peer("q1").count();
    check(gb.write(sensor, make_value(b_value_u8(0x33))).has_value(), "publish 3");
    check(b_to_a.count() == before + 1, "one delivery only: the dead edge left the fan-out");
    check(bus.peer("q1").count() == q1_seen + 1, "the live session received publish 3");

    // The discriminator is the STATUS code. A forged/other refusal echoing the LIVE route
    // with a non-path::invalid code must not evict (a TIMEOUT names a live route having a
    // bad day) — and the SAME route with path::invalid then does, which proves the first
    // frame was held back by its code, not by a route mismatch.
    const std::vector<std::byte> live_route = b_path({"bus", "q1"});
    b_to_a.inject(refusal_frame(live_route, 0x0041));  // some code that is not 0x0021
    check(gb.read_subscribers(sensor)->size() == 1, "a non-invalid status evicts nothing");
    b_to_a.inject(refusal_frame(live_route, 0x0021));  // tr::path::invalid
    check(gb.read_subscribers(sensor)->empty(),
          "the same route with tr::path::invalid evicts — the discriminator is the code");
}

/**
 * @brief #1223 steps 3+4 (RFC-0024 §7.1 amendment 1): the reverse-direction mint closes the
 *        DISCLOSURE — a recycled session name no longer inherits the dead session's stream.
 *
 * Two nodes, real frames on one call stack. A holds the accepting bus (sessions get #1254
 * identity anchors via the arrival notifier); B is the producer. A mint-flagged subscribe
 * from session p0 makes the forwarding hop (A) contribute p0's anchor element, and B
 * completes + stores the reverse list with the edge. Every claim follows the #1223
 * refutation rule: the POSITIVE control (the legitimate session receives its own publish,
 * over the bound leg) is shown before any does-not-receive claim.
 */
void test_reverse_mint_closes_the_disclosure() {
    std::printf("reverse mint closes the disclosure (#1223 steps 3+4):\n");
    graph_t ga, gb;
    fwd_router_t ra(ga), rb(gb);
    pipe_link_t a_to_b, b_to_a;
    a_to_b.set_peer(b_to_a);
    b_to_a.set_peer(a_to_b);
    fake_bus_t bus;
    (void)ra.add_child("srv", bus);
    (void)ra.add_child("b", a_to_b);
    // B's connection vertex for its link to A MUST exist BEFORE add_child: the bound-path
    // join resolves `conn_slot` at registration, and it is what the responder's reverse
    // completion and the delivery-time element-0 consumption both dereference. Production
    // wiring gets this from the `/net:children[]` SPEC door; the harness registers it bare.
    (void)gb.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    (void)rb.add_child("a", b_to_a);
    vertex_handle_t sensor = gb.register_vertex(path_t("/sensor"), role_t::STORED_VALUE);

    // S1 arrives as p0 — the accepting listener fires the arrival notifier, so p0 holds a
    // #1254 identity anchor on A — and subscribes with the MINT flag set. A (the forwarding
    // hop) creates the reverse list with p0's anchor element; B completes it with its own
    // connection-vertex element and stores both with the edge.
    bus.peer_arrive("p0");
    check(ga.find_session_anchor(fwd_router_t::session_anchor_id("srv", "p0")).has_value(),
          "p0's identity anchor exists on A");
    bus.inject_peer("p0", b_fwd_mint(fwd_op_t::WRITE, b_path({"b", "sensor"}), b_path({}),
                                     b_field_subscribers_append(), b_subscriber("s1")));
    // RFC-0024 §7.1 amendment 2, on the wire: A's forwarded request carries the reverse list
    // as a TRAILING CHILD OF ITS OWN TYPE — `PATH_REF_REVERSE` (0x15), not a positionally
    // identified `PATH_REF`. Asserted here rather than only through the behaviour below,
    // because the type byte IS the amendment; the positional shape would deliver identical
    // behaviour on this frame and is exactly what must not be relied on.
    {
        const auto& lg = a_to_b.log();
        check(!lg.empty(), "A forwarded the mint-flagged subscribe to B");
        const auto dec = tr::wire::decode(lg.back());
        check(dec.has_value() && !dec->children.empty() &&
                  dec->children.back().type == tr::wire::type_t::PATH_REF_REVERSE &&
                  dec->children.back().payload.size() == 8,
              "the forwarded request's LAST child is a one-element PATH_REF_REVERSE (0x15)");
        // And the payload the WRITE actually carries is still a SUBSCRIBER — the reverse
        // child is additive, never a replacement for the closed RFC-0004 §B child list.
        bool has_subscriber = false;
        for (const auto& c : dec->children)
            if (c.type == tr::wire::type_t::SUBSCRIBER) has_subscriber = true;
        check(has_subscriber, "the forwarded request still carries its SUBSCRIBER payload");
    }
    const auto subs0 = gb.read_subscribers(sensor);
    check(subs0.has_value() && subs0->size() == 1, "B holds S1's remote edge");

    // POSITIVE CONTROL: S1 receives its own publish over the bound delivery leg, and the
    // frame it receives is the CANONICAL delivery shape — the origin never sees the bound
    // form (dst re-headed as a PATH, RFC-0024 §7.1 amendment 1's last-hop rule).
    std::size_t p0_seen = bus.peer("p0").count();
    const std::size_t wire_before = b_to_a.log().size();
    check(gb.write(sensor, make_value(b_value_u8(0x11))).has_value(), "publish 1");
    check(bus.peer("p0").count() == p0_seen + 1, "positive control: S1 receives publish 1");
    // The wire between B and A carries the BOUND form (dst = a one-element PATH_REF): the
    // proof the positive control rode the reverse list, not the canonical fallback.
    {
        const auto& lg = b_to_a.log();
        check(lg.size() == wire_before + 1, "exactly one B->A delivery frame");
        const auto dec = tr::wire::decode(lg.back());
        check(dec.has_value() && dec->children.size() >= 2 &&
                  dec->children[1].type == tr::wire::type_t::PATH_REF &&
                  dec->children[1].payload.size() == 8,
              "the B->A delivery dst is a one-element PATH_REF (the bound leg engaged)");
    }
    {
        const std::vector<std::vector<std::byte>> got = bus.peer("p0").drain();
        check(!got.empty() && !got.back().empty() &&
                  static_cast<tr::wire::type_t>(got.back()[0]) == tr::wire::type_t::FWD,
              "the delivered frame is a FWD");
        const auto dec = tr::wire::decode(got.back());
        check(dec.has_value() && dec->children.size() >= 2 &&
                  dec->children[1].type == tr::wire::type_t::PATH,
              "the client-visible dst is a canonical PATH — the bound form never reaches an "
              "origin");
    }

    // S1 dies; the SAME name is recycled by a brand-new session that subscribed to NOTHING.
    // The kill retires the anchor (generation bump), the re-arrival revives it one higher.
    bus.peer_kill("p0");
    bus.peer_arrive("p0");
    const auto subs1 = gb.read_subscribers(sensor);
    check(subs1.has_value() && subs1->size() == 1, "B still holds the dead session's edge");

    // Publish 2 — THE DISCLOSURE PROBE. Pre-amendment, S2 (new socket, same name, no
    // subscription) received this byte-for-byte. Now the delivery's last element carries
    // the RETIRED generation, A's deref refuses (§5.1), S2 receives NOTHING — and §5.3's
    // NACK echo lets B's step-5 reclaim retire the stale edge on this very publish.
    p0_seen = bus.peer("p0").count();
    check(gb.write(sensor, make_value(b_value_u8(0x22))).has_value(),
          "publish 2 (the dead session's delivery must refuse)");
    check(bus.peer("p0").count() == p0_seen,
          "THE FIX: the recycled session does NOT inherit the dead session's delivery");
    const auto subs2 = gb.read_subscribers(sensor);
    check(subs2.has_value() && subs2->empty(),
          "the stale edge is reclaimed by the bound refusal (steps 4+5 compose)");

    // And the recycled session is a fully working session: its own mint-flagged subscribe
    // binds at the REVIVED generation and deliveries flow to it.
    bus.inject_peer("p0", b_fwd_mint(fwd_op_t::WRITE, b_path({"b", "sensor"}), b_path({}),
                                     b_field_subscribers_append(), b_subscriber("s2")));
    p0_seen = bus.peer("p0").count();
    check(gb.write(sensor, make_value(b_value_u8(0x33))).has_value(), "publish 3");
    check(bus.peer("p0").count() == p0_seen + 1,
          "the successor's OWN subscription delivers at the revived generation");
}

int main() {
    std::printf("== edge_eviction_test ==\n");
    test_evict_scoped_to_link();
    test_departure_cost_is_scoped_to_the_peer();
    test_index_recreated_after_a_full_eviction();
    test_index_insert_is_idempotent();
    test_stale_index_entries_are_harmless();
    test_field_write_admitted_edge_is_indexed_under_its_caller();
    test_local_unsubscribe();
    test_clear_edge_releases_the_slot_pin();
    test_slot_reuse_and_index_stability();
    test_router_link_down();
    test_evict_reaches_field_write_admitted_edges();
    test_empty_link_name_evicts_nothing();
    test_departure_notifier_seam();
    test_refused_route_reclaims_the_edge();
    test_reverse_mint_closes_the_disclosure();
    test_concurrent_evict_vs_writes();
    return tr::testing::summary("edge_eviction");
}
