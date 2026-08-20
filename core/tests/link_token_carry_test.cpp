/**
 * @file
 * @brief #1266 / #1417 — the CARRIED link token, and the four things about it that have to be
 *        BUILT and SHOWN rather than asserted.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The subscriber index is now a dense slot vector addressed by a `tr::graph::link_id_t` the
 * transport plane mints at link-up and carries to `subscribe_wire`. PR #1416 measured why
 * (42–56 % of the index operation net of its control, and 27 % of its bytes); this file pins
 * the parts of that
 * design whose failure mode is a LEAKED SUBSCRIBER EDGE rather than a slow one — which is the
 * failure #1071 and #943 exist to prevent, and the reason #1366 declined handle-keying twice.
 *
 * `core/tests/edge_eviction_test.cpp` is the guard for the index's own contract and is
 * deliberately UNMODIFIED by the carry: every property it pins — same-NAME redial ordering,
 * the admitted-over key with its `caller` fallback, the index-as-superset invariant, the
 * idempotent insert, `link_down` — is reached through name-taking doors whose signatures did
 * not move. That it stays green unchanged is half the evidence. This file is the other half.
 *
 * Assertions:
 *
 *   - **Two links get two tokens.** #1366's second objection was that `peer_handle_t`s are
 *     meaningful only to the minting link, so two links both minting `kSolePeerHandle` cannot
 *     key one index. A `link_id_t` is not a handle: it is minted by `graph_t` from a
 *     node-scoped slot space against the NAME, so the objection does not apply — shown, not
 *     argued.
 *   - **A same-NAME redial re-enters its own slot** and strands nothing (#1263's pinned
 *     property, at the token door this time).
 *   - **A released slot is reused, and its successor inherits nothing** — neither the
 *     predecessor's candidate list nor a token that still validates.
 *   - **A WRONG token cannot mis-index.** The index verifies that the token's slot spells the
 *     key it is about to index under, so the two admissions whose key is NOT the arrival link
 *     — a mount-routed target, and the `caller` fallback #943 needs — index where they always
 *     did.
 *   - **The seam is asked LAZILY.** Exactly once per remote subscribe and ZERO times for a
 *     read, a write, an await or a forwarding hop. A control-plane saving charged to every
 *     terminus frame is what killed #1290's prototype, so this is a gate and not a note.
 *   - **A census-bus peer is indexed and evicts.** An announce-census bus never calls
 *     `set_peer_up_notifier`, so nothing ever fills its per-peer token cache eagerly; the
 *     supplier must mint lazily on the miss. Without that, every CAN peer would pay a name
 *     lookup forever — and if the lazy mint were wrong instead of merely absent, its edges
 *     would not be evictable at all.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/link_id.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::inbound_ref_t;
using tr::graph::link_id_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::net::bus_link_t;
using tr::net::fwd_router_t;
using tr::net::peer_handle_t;
using tr::net::transport_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::check;
using tr::testing::make_value;

/** @brief Append @p src's bytes to @p dst — the builders' shared tail. */
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

/** @brief NAME{ @p s } — the path/field record every builder below spells with. */
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::NAME, opt_t{},
                       std::span(reinterpret_cast<const std::byte*>(s.data()), s.size()));
    return out;
}

/** @brief VALUE{ u8 @p v } — a one-byte payload. */
std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span(&b, 1));
    return out;
}

/** @brief PATH{ NAME @p seg, ... } — a canonical path TLV over @p segs. */
std::vector<std::byte> b_path_of(const std::vector<std::string>& segs) {
    std::vector<std::byte> body;
    for (const std::string& s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief SUBSCRIBER{ PATH @p target } — the wire subscribe body. */
std::vector<std::byte> b_subscriber_at(const std::vector<std::string>& target) {
    const std::vector<std::byte> body = b_path_of(target);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}

/** @brief FIELD{ NAME "subscribers", VALUE u8 ELEMENT } — the `:subscribers[]` append. */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    append(body, b_name("subscribers"));
    append(body, b_value_u8(1));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}

/**
 * @brief Bind one wire subscriber at @p v arriving over @p link, carrying @p token.
 *
 * The direct door, so a test can hand the index a token the transport plane would never
 * produce — which is the only way to show that a wrong one is REFUSED rather than trusted.
 */
bool wire_sub(graph_t& g, vertex_handle_t v, std::string_view link, std::string_view marker,
              link_id_t token = {}) {
    return g
        .subscribe_wire(v, make_value(b_subscriber_at({std::string(marker)})),
                        make_value(b_path_of({std::string(link)})), std::string(link), view_t{},
                        std::string{}, token)
        .has_value();
}

/**
 * @brief An announce-census BUS: it names peers and delivers their frames, and it NEVER
 *        fires an arrival notifier.
 *
 * That absence is the whole point of the double. `set_peer_up_notifier` is what an ACCEPTING
 * listener calls; a CAN bus learns of a peer only because one spoke. So nothing eagerly fills
 * the router's per-peer token cache for these peers, and the carry has to mint on the miss —
 * the fallback the 2026-08-15 re-scope demanded when it retired the `add_child`-only mint
 * point.
 */
class census_bus_t : public transport_t, public bus_link_t {
   public:
    /** @brief One peer's directed recording endpoint. */
    class peer_ep_t : public transport_t {
       public:
        /** @brief Record one outbound frame to this peer. */
        void send(std::span<const std::byte> frame) override {
            const std::lock_guard lock(m_);
            ++sent_;
            (void)frame;
        }
        /** @brief The recorded send count. */
        std::size_t count() {
            const std::lock_guard lock(m_);
            return sent_;
        }

       private:
        std::mutex m_;         /**< @brief Guards @ref sent_. */
        std::size_t sent_ = 0; /**< @brief Recorded frame count. */
    };

    /** @brief Undirected sends are not part of this facet's contract; drop. */
    void send(std::span<const std::byte>) override {}
    /** @brief Expose the bus facet, like every multi-peer link. */
    bus_link_t* bus() override { return this; }
    /** @brief Visit the peers that ever spoke (a live-traffic census). */
    void enumerate_peers(const peer_visitor_t& visit) const override {
        for (const std::string& n : names_) visit(n);
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
    /** @brief The (created-on-first-use) endpoint for @p name. */
    peer_ep_t& peer(std::string_view name) {
        std::unique_ptr<peer_ep_t>& ep = peers_[std::string(name)];
        if (!ep) ep = std::make_unique<peer_ep_t>();
        return *ep;
    }
    /** @brief The handle for @p name, minting one on first sight. */
    peer_handle_t mint(std::string_view name) {
        for (std::size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == name) return {static_cast<std::uint32_t>(i), 1};
        names_.emplace_back(name);
        return {static_cast<std::uint32_t>(names_.size() - 1), 1};
    }
    /** @brief Poke one inbound frame tagged with the sending peer's HANDLE. NO arrival
     *         notifier fires — a census peer is learned of by SPEAKING. */
    void inject_peer(std::string_view name, std::span<const std::byte> frame) {
        (void)peer(name);
        peer_rx_.deliver_borrowed(mint(name), frame);
    }
    /** @brief Simulate the bus observing @p name's session dead. */
    void peer_die(std::string_view name) { notify_peer_down(mint(name), name); }

   private:
    std::map<std::string, std::unique_ptr<peer_ep_t>> peers_; /**< @brief name → endpoint. */
    std::vector<std::string> names_; /**< @brief index → peer name — the handle's meaning. */
};

// ---------------------------------------------------------------------------

/**
 * @brief #1366 objection 2, answered: the token is NODE-scoped, so two links get two of them.
 *
 * The objection is exactly right about `peer_handle_t` and this is why the carry does not use
 * one. Both links below would mint `tr::net::kSolePeerHandle` and be indistinguishable by
 * handle; they are distinguishable by token because `graph_t` mints it, from its own slot
 * space, against the NAME.
 */
void test_two_links_get_two_tokens() {
    std::printf("#1366 objection 2 — the token is node-scoped, not link-scoped:\n");
    graph_t g;
    const link_id_t a = g.intern_link("up");
    const link_id_t b = g.intern_link("down");
    check(a.valid() && b.valid(), "both links mint a valid token");
    check(!(a == b), "and the two tokens differ, though both links would mint kSolePeerHandle");
    check(!link_id_t{}.valid(), "a default-constructed token is 'no token', never a live slot");
    check(link_id_t::from_bits(a.bits()) == a, "the packed form round-trips");
}

/**
 * @brief #1263's pinned same-NAME redial, at the token door: one spelling, one live slot.
 *
 * `intern_link` is idempotent by NAME, so a peer that redials under `p3` re-enters the slot
 * `p3` already has. If it minted a second slot, the predecessor's candidate list would be
 * stranded behind a name no door could reach any more — a leaked departure cost that the
 * name-taking tests could not see, because they would still find the NEWER slot.
 */
void test_same_name_redial_reuses_its_slot() {
    std::printf("#1263 — a same-NAME redial re-enters its own slot:\n");
    graph_t g;
    vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    const link_id_t first = g.intern_link("p3");
    check(wire_sub(g, v, "p3", "a", first), "the first session subscribes with its token");
    const link_id_t again = g.intern_link("p3");
    check(again == first, "re-interning the SAME name answers the SAME token");
    check(g.link_edge_candidates("p3") == 1, "and the name door still finds the one slot");

    vertex_handle_t w = g.register_vertex(path_t("/w"), role_t::STORED_VALUE);
    check(wire_sub(g, w, "p3", "b", again), "a redial subscribes on a second vertex");
    check(g.link_edge_candidates("p3") == 2,
          "both vertices are on ONE list — nothing was stranded behind a second slot");
    check(g.evict_link_edges("p3") == 2, "and one departure reclaims both edges");
}

/**
 * @brief A released slot is REUSED, and the link that takes it inherits nothing.
 *
 * The hazard the stamp exists for. Slot reuse is what keeps a churning node's index bounded
 * by its CONCURRENT link count instead of its lifetime's — but a successor that inherited the
 * predecessor's candidate list would evict a departed link's vertices on ITS departure
 * (harmless, a no-op per vertex) and, far worse, a predecessor's still-cached token would
 * address the successor's list and index a live edge under a dead link's name, where no
 * departure could ever reach it.
 */
void test_a_released_slot_inherits_nothing() {
    std::printf("release + reuse — the successor inherits neither list nor token:\n");
    graph_t g;
    vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    vertex_handle_t w = g.register_vertex(path_t("/w"), role_t::STORED_VALUE);

    const link_id_t dead = g.intern_link("gone");
    check(wire_sub(g, v, "gone", "a", dead), "a link subscribes");
    check(g.link_edge_candidates("gone") == 1, "and is indexed");
    g.release_link(dead);
    check(g.link_edge_candidates("gone") == 0, "release_link retires the slot and its list");

    const link_id_t fresh = g.intern_link("new");
    check(fresh.slot == dead.slot, "the freed slot is REUSED — the footprint stays bounded");
    check(!(fresh == dead), "but the stamp moved, so the predecessor's token is not this one");
    check(wire_sub(g, w, "new", "b", fresh), "the successor subscribes with ITS token");
    check(g.link_edge_candidates("new") == 1, "the successor's list holds exactly its own vertex");
    check(g.link_edge_candidates("gone") == 0, "and the dead name reaches nothing at all");

    // The predecessor's token, still in a router cache somewhere, aimed at the live slot.
    vertex_handle_t x = g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    check(wire_sub(g, x, "gone", "c", dead), "a subscribe arrives carrying the STALE token");
    check(g.link_edge_candidates("new") == 1, "THE FIX: it did not land on the successor's list");
    check(g.link_edge_candidates("gone") == 1,
          "it was interned by NAME instead — indexed, and evictable, where it belongs");
}

/**
 * @brief A token for the WRONG link cannot mis-index — the guard the two rebinding
 *        admissions need.
 *
 * `subscribe_wire` does not always index under the link the frame arrived on. A mount-routed
 * target (RFC-0021 §4.B.1) rebinds the delivery link to the MOUNT's, and a `field_write`
 * admission has only its `caller` (#943). A carry that trusted the caller's token would index
 * both under the ARRIVAL link — where the mount's teardown could never reach them, and where
 * the writer's departure would evict a subscription that is supposed to outlive it, which is
 * the whole point of RFC-0021 §4.C.
 *
 * So the index checks that the token's slot SPELLS the key, and this shows it: a valid,
 * live token for link `A` handed to an admission keyed by link `B` lands on `B`.
 */
void test_a_wrong_token_cannot_mis_index() {
    std::printf("a token for the wrong link is refused, not trusted:\n");
    graph_t g;
    vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    const link_id_t a = g.intern_link("link-a");
    const link_id_t b = g.intern_link("link-b");
    check(a.valid() && b.valid(), "both links are interned and live");

    // A subscribe whose key is `link-b`, carrying `link-a`'s perfectly valid token.
    check(wire_sub(g, v, "link-b", "m", a), "the subscribe is admitted");
    check(g.link_edge_candidates("link-b") == 1, "it is indexed under the key, `link-b`");
    check(g.link_edge_candidates("link-a") == 0, "and NOT under the token's link");
    check(g.evict_link_edges("link-a") == 0, "link-a's departure reclaims nothing of it");
    check(g.evict_link_edges("link-b") == 1, "link-b's departure reclaims it — reachable");
}

/**
 * @brief The seam is asked LAZILY: once per remote subscribe, never on any other frame.
 *
 * The gate, not a note. #1290's prototype was killed for paying a control-plane saving on
 * every frame, and `op_resolver_t` already resolves the ACL subject once per resolve — so a
 * token seam resolved the same way would charge every read, write, await and forwarding hop
 * for a lookup only a subscribe can use. A counting supplier is the only way to observe that
 * from outside, since a carried token is otherwise observationally identical to a name
 * lookup by construction.
 */
void test_the_token_seam_is_asked_only_at_a_subscribe() {
    std::printf("the link-token seam is LAZY — a subscribe asks, nothing else does:\n");
    graph_t g;
    op_resolver_t resolver(g);
    vertex_handle_t v = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    check(g.write(v, make_value(b_value_u8(0x01))).has_value(), "seed a value");

    struct counter_t {
        std::size_t asks = 0; /**< @brief How many times the seam was consulted. */
        graph_t* g = nullptr; /**< @brief Where a real token comes from. */
        std::string link;     /**< @brief The link the answer is for. */
    } counter{.g = &g, .link = "cli"};
    resolver.on_link_id(
        [](void* c, const inbound_ref_t& inbound) -> link_id_t {
            auto* const cc = static_cast<counter_t*>(c);
            ++cc->asks;
            return cc->g->intern_link(inbound.link);
        },
        &counter);

    const auto run = [&](const std::vector<std::byte>& fwd) {
        const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
        check(arena.has_value(), "the request decodes");
        (void)resolver.resolve(*arena, "cli");
    };

    run(b_fwd(fwd_op_t::READ, b_path_of({"s"}), b_path_of({"cli"}), {}, {}));
    check(counter.asks == 0, "a READ does not ask");
    run(b_fwd(fwd_op_t::WRITE, b_path_of({"s"}), b_path_of({"cli"}), {}, b_value_u8(0x02)));
    check(counter.asks == 0, "a WRITE does not ask");

    const std::size_t fallbacks_before = g.link_index_name_lookups();
    run(b_fwd(fwd_op_t::WRITE, b_path_of({"s"}), b_path_of({"cli"}), b_field_subscribers_append(),
              b_subscriber_at({"ui"})));
    check(counter.asks == 1, "a remote SUBSCRIBE asks exactly once");
    check(g.link_edge_candidates("cli") == 1, "and the vertex is indexed under the link");
    // The carry, OBSERVED. A valid token and a name lookup leave byte-identical index state,
    // which is what makes a wrong token harmless and also what makes a MISSING carry
    // invisible — so the index counts its own fallbacks and this is the assertion that fails
    // if the seam stops being reached, stops being installed, or starts answering nonsense.
    check(g.link_index_name_lookups() == fallbacks_before,
          "THE CARRY FIRED: the index reached its entry by SUBSCRIPT, not by name");

    run(b_fwd(fwd_op_t::READ, b_path_of({"s"}), b_path_of({"cli"}), {}, {}));
    check(counter.asks == 1, "a later READ still does not ask");
}

/**
 * @brief A CENSUS-BUS peer — one that never announced its arrival — is indexed and evicts.
 *
 * The mandatory fallback. An announce-census bus never calls `set_peer_up_notifier`, so the
 * router's per-peer token cache is never filled ahead of time and `link_id_of` MISSES on this
 * peer's first subscribe. The supplier must then mint through `graph_t::intern_link` and cache
 * the answer itself; if it simply answered "no token", correctness would survive (the index
 * interns the name) but every CAN peer would pay the lookup for its whole life — and if the
 * lazy mint were wrong rather than absent, this peer's edges would not be reachable by its
 * own departure at all, which is the #1071 disclosure class.
 *
 * Driven end to end through `fwd_router_t`, so what is exercised is the shipped supplier and
 * the shipped notifier wiring, not a stand-in for them.
 */
void test_a_census_bus_peer_is_indexed_and_evicts() {
    std::printf("census bus (no arrival notifier) — lazy mint, indexed, evictable:\n");
    graph_t g;
    fwd_router_t router(g);
    census_bus_t bus;
    check(router.add_child("can", bus), "the census bus mounts");
    vertex_handle_t s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    (void)s;

    // `n7` speaks for the first time AND subscribes, with no arrival notifier ever fired.
    bus.inject_peer("n7", b_fwd(fwd_op_t::WRITE, b_path_of({"s"}), b_path_of({"n7"}),
                                b_field_subscribers_append(), b_subscriber_at({"ui"})));
    check(g.link_edge_candidates("n7") == 1,
          "the never-announced peer IS indexed under its own name");
    // The lazy mint went through `intern_link`, so the INDEX itself never fell back to a
    // name lookup — which is the difference between "the census peer is correct" (it would
    // be either way) and "the census peer is carried" (only if the supplier minted).
    const std::size_t census_fallbacks = g.link_index_name_lookups();
    check(census_fallbacks == 0, "and it was CARRIED — the index took no name lookup at all");

    // A second subscribe from the same peer now takes the cached token; the observable is
    // that it still lands on the one list rather than on a second slot.
    vertex_handle_t t = g.register_vertex(path_t("/t"), role_t::STORED_VALUE);
    (void)t;
    bus.inject_peer("n7", b_fwd(fwd_op_t::WRITE, b_path_of({"t"}), b_path_of({"n7"}),
                                b_field_subscribers_append(), b_subscriber_at({"ui2"})));
    check(g.link_edge_candidates("n7") == 2, "its second subscribe joins the SAME list");
    check(g.link_index_name_lookups() == census_fallbacks,
          "the per-peer cache HIT — the second subscribe minted nothing and looked up nothing");

    // A bystander peer stays independent — the cache is per peer, not per mount.
    bus.inject_peer("n9", b_fwd(fwd_op_t::WRITE, b_path_of({"s"}), b_path_of({"n9"}),
                                b_field_subscribers_append(), b_subscriber_at({"ui3"})));
    check(g.link_edge_candidates("n9") == 1, "a second census peer gets its own list");
    check(g.link_edge_candidates("n7") == 2, "and does not disturb the first's");

    check(g.evict_link_edges("n7") == 2, "n7's departure reclaims exactly its two edges");
    check(g.link_edge_candidates("n9") == 1, "n9's are untouched");

    // And the departure notifier drops the cached token, so a successor at the same peer
    // index mints afresh instead of asking with a stamp that can never validate again.
    bus.peer_die("n9");
    check(g.link_edge_candidates("n9") == 0, "peer_die evicted n9 through link_down");
    bus.inject_peer("n9", b_fwd(fwd_op_t::WRITE, b_path_of({"s"}), b_path_of({"n9"}),
                                b_field_subscribers_append(), b_subscriber_at({"ui4"})));
    check(g.link_edge_candidates("n9") == 1, "a redial at the same peer index is indexed again");
    check(g.evict_link_edges("n9") == 1, "and is evictable — the cache healed, not stranded");
}

/**
 * @brief A point-to-point child's carry: the FLAT tier's resolved-once word.
 *
 * The other half of "both tiers", and the one the retired `add_child`-only mint point covered.
 * Minted lazily here too, so a registered child that never carries a subscription charges the
 * graph's arena nothing at all — which matters because that arena is the budget #1160 is
 * making configurable on the C6.
 */
void test_a_flat_child_carries_its_token() {
    std::printf("the FLAT tier — a point-to-point child's resolved-once token:\n");
    /** @brief A point-to-point transport that records nothing and can be poked. */
    class flat_link_t : public transport_t {
       public:
        void send(std::span<const std::byte>) override {}
        /** @brief Poke one inbound frame into the router-installed receiver. */
        void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
        /** @brief Simulate the transport observing its one connection dead. */
        void die() { notify_down(); }
    };
    graph_t g;
    fwd_router_t router(g);
    flat_link_t cli;
    check(router.add_child("cli", cli), "the child registers");
    vertex_handle_t s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    (void)s;
    vertex_handle_t t = g.register_vertex(path_t("/t"), role_t::STORED_VALUE);
    (void)t;

    cli.inject(b_fwd(fwd_op_t::WRITE, b_path_of({"s"}), b_path_of({"cli"}),
                     b_field_subscribers_append(), b_subscriber_at({"ui"})));
    check(g.link_edge_candidates("cli") == 1, "the first subscribe mints and indexes");
    cli.inject(b_fwd(fwd_op_t::WRITE, b_path_of({"t"}), b_path_of({"cli"}),
                     b_field_subscribers_append(), b_subscriber_at({"ui2"})));
    check(g.link_edge_candidates("cli") == 2, "the second joins the same list through the cache");
    check(g.link_index_name_lookups() == 0,
          "and BOTH were carried — the flat tier's resolved-once word did its job");

    cli.die();
    check(g.link_edge_candidates("cli") == 0, "the down notifier evicted both");
    cli.inject(b_fwd(fwd_op_t::WRITE, b_path_of({"s"}), b_path_of({"cli"}),
                     b_field_subscribers_append(), b_subscriber_at({"ui3"})));
    check(g.link_edge_candidates("cli") == 1,
          "a reconnect over the same registration is indexed again");
    check(g.evict_link_edges("cli") == 1, "and evictable — the cached token was dropped, not kept");
}

/**
 * @brief A link name longer than the slot's inline buffer still works, both doors.
 *
 * The inline name is what makes the carry pay in bytes (a `std::pmr::string` here is 40 bytes
 * of header before a character is stored), and it is bounded at 19 characters. Every name the
 * transport plane mints fits; a registered child name need not, so the overflow path is real
 * code and gets a real test rather than a comment saying it is rare.
 */
void test_a_long_link_name_overflows_and_still_resolves() {
    std::printf("a link name past the inline bound overflows and still resolves:\n");
    graph_t g;
    vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    const std::string longer = "sensor-gateway-north-annex-3";  // 28 chars, past the bound
    const std::string shorter = "sensor-gateway";               // 14, inline
    const link_id_t a = g.intern_link(longer);
    const link_id_t b = g.intern_link(shorter);
    check(a.valid() && !(a == b), "both intern, and to different slots");
    check(g.intern_link(longer) == a, "the long name is idempotent too");

    check(wire_sub(g, v, longer, "m", a), "a subscribe over the long-named link");
    check(g.link_edge_candidates(longer) == 1, "the NAME door finds it");
    check(g.link_edge_candidates(shorter) == 0, "and does not confuse it with its prefix-sharer");
    check(g.evict_link_edges(longer) == 1, "and its departure reclaims it");

    // Release must forget the overflow entry too, or the freed slot would keep answering to
    // a name no live link has.
    const link_id_t c = g.intern_link("reused");
    check(c.slot == a.slot, "the long name's slot is reused");
    check(g.link_edge_candidates(longer) == 0, "and the long name reaches nothing");
    check(g.link_edge_candidates("reused") == 0, "while its successor starts empty");
}

}  // namespace

int main() {
    std::printf("== link_token_carry_test ==\n");
    test_two_links_get_two_tokens();
    test_same_name_redial_reuses_its_slot();
    test_a_released_slot_inherits_nothing();
    test_a_wrong_token_cannot_mis_index();
    test_the_token_seam_is_asked_only_at_a_subscribe();
    test_a_census_bus_peer_is_indexed_and_evicts();
    test_a_flat_child_carries_its_token();
    test_a_long_link_name_overflows_and_still_resolves();
    return tr::testing::summary("link_token_carry");
}
