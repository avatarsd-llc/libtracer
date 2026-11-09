/**
 * @file
 * @brief Receiver-ctx churn (#884): a re-added NAME resolves to its CURRENT tenancy, and
 *        remove/re-add cycles do not grow the router's published receiver chain.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `child_registry_t` learned the one-slot-per-NAME rule in #494/#521 and is pinned by
 * `registry_teardown_test`. The router's OWN per-child table — the `child_rx_ctx_t` chain
 * every name-keyed and slot-keyed bound-path lookup walks — never learned it: `remove_child`
 * left the ctx resolving, `add_child` of the same name appended a second, and `ctx_by_name`
 * answers with the FIRST match. So after one create/remove/create cycle every consumer of the
 * name — `hop_mint`, and through it the reply-mint contribution and the responder's reverse
 * element — was handed the DEAD context, and one `child_rx_ctx_t` (+ its mount run)
 * leaked per cycle onto a chain the bound hop walks per frame.
 *
 * Two defects, and they are different KINDS of defect. The growth one is a leak, and on a
 * bounded node a leak per connection churn cycle is a reboot. The resolution one is **not** a
 * use-after-free and this file is arranged to show that rather than assert it: the ctx lives
 * in a deque that is never popped, its `entry` points into registry chunks that are never
 * freed, and `conn_slot` is an INDEX that `graph_t::vertex_slot_at` bounds-checks against a
 * pinned insert-only table. Every pointer stays live; what goes wrong is that a live pointer
 * names the wrong tenancy. Run this file under ASan and it is clean either way — the failures
 * below are assertion failures, never a sanitizer report, which is the proof of that claim.
 *
 * Asserted:
 *   - a REMOVED child stops minting its reference (read off a relayed reply's `src`), even
 *     while its connection vertex is still registered (the tombstone must be on the ctx, not
 * inferred from the graph);
 *   - a re-added child answers with the NEW tenancy's element, and that element egresses over
 *     the NEW link — including the ordering the shipped wiring does NOT take, where the child
 *     was first registered before its connection vertex existed;
 *   - a re-add as a BUS mount stops answering, rather than keeping the point-to-point slot it
 *     used to have;
 *   - N remove/re-add cycles occupy ONE receiver ctx, and a duplicate add of a LIVE name does
 *     not append a shadow one either;
 *   - the canonical forward plane still routes to the CURRENT link after churn — the leg that
 *     makes "resolves to the new tenancy" mean delivery and not just a matching integer.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/path_pair.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "pair_body.hpp"
#include "test_support.hpp"

namespace tr::net {
/** @brief Test-only door: who holds a session slot, asked through either cache pointer. */
struct fwd_router_test_door_t {
    /** @brief The slot holder a route or a mint sees: through the FACET (`peer_tokens`). */
    static peer_handle_t holder(const fwd_router_t& r, std::string_view name, std::uint32_t slot) {
        const fwd_router_t::child_rx_ctx_t* const ctx = r.ctx_by_name(name);
        return ctx == nullptr ? peer_handle_t{}
                              : fwd_router_t::session_tenant(
                                    ctx->peer_tokens.load(std::memory_order_acquire), slot);
    }
    /** @brief The slot holder the arrival and departure seams see: the OWNING pointer. */
    static peer_handle_t owned_holder(const fwd_router_t& r, std::string_view name,
                                      std::uint32_t slot) {
        const fwd_router_t::child_rx_ctx_t* const ctx = r.ctx_by_name(name);
        return ctx == nullptr ? peer_handle_t{}
                              : fwd_router_t::session_tenant(ctx->peer_tokens_own, slot);
    }
};
}  // namespace tr::net

namespace {

using door_t = tr::net::fwd_router_test_door_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::path_ref_element_t;
using tr::wire::type_t;

using tr::testing::check;

/** @brief A transport that only counts and keeps what it was handed — no socket, no thread. */
class sink_link_t : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        sent_.emplace_back(frame.begin(), frame.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte>& f = sent_.emplace_back();
        for (const std::span<const std::byte>& s : iov) f.insert(f.end(), s.begin(), s.end());
    }

    /** @brief Number of frames this link was asked to carry. */
    [[nodiscard]] std::size_t sends() const noexcept { return sent_.size(); }
    /** @brief The last frame this link was asked to carry (empty when none). */
    [[nodiscard]] std::span<const std::byte> last() const noexcept {
        return sent_.empty() ? std::span<const std::byte>{}
                             : std::span<const std::byte>(sent_.back());
    }

   private:
    std::vector<std::vector<std::byte>> sent_;
};

/** @brief A minimal BUS link — a mount whose `send` broadcasts, so it takes NO `conn_slot`. */
class bus_sink_link_t : public sink_link_t, public tr::net::bus_link_t {
   public:
    tr::net::bus_link_t* bus() override { return this; }
    using tr::net::bus_link_t::notify_peer_down;  // the tests play the listener's seams
    using tr::net::bus_link_t::notify_peer_up;
    void enumerate_peers(const peer_visitor_t&) const override {}
    tr::net::transport_t* peer_link(std::string_view) override { return nullptr; }
    /** @brief No peers to name — this double never delivers through the seam (#1294). */
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t,
                                             std::span<char>) const override {
        return {};
    }
};

/** @brief Canonical `PATH{ NAME… }` bytes for @p segs, via the production emit helpers. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

using tr::testing::b_fwd;

/** @brief The mount run the tests churn, and the vertex key that is its connection vertex. */
constexpr std::string_view kMount = "net/mod/a";

/** @brief The inbound child the probes below send from, and relay replies onto. */
constexpr std::string_view kIn = "net/mod/in";

/** @brief Register the connection vertex `/net/mod/a` — what makes @ref kMount bindable. */
void make_conn_vertex(graph_t& g) {
    const auto p = path_t::parse("/net/mod/a");
    (void)g.register_vertex(*p, role_t::STORED_VALUE);
}

/**
 * @brief The PAIR this router mints for @ref kMount, read where production reads it: the
 *        element it prepends to the `src` of a REPLY that came back over that link
 *        (RFC-0029 §6.2), relayed onto @p in (the child @ref kIn).
 *
 * The same `hop_mint` answers the responder's reverse-element seam. `nullopt` is the hop
 * contributing its NAME run instead, or relaying nothing: it has no reference to give.
 */
std::optional<path_ref_element_t> minted_ref(fwd_router_t& r, const sink_link_t& in) {
    std::vector<std::byte> far;
    tr::testing::emit_path_pair(far, path_ref_element_t{.index = 7, .generation = 0});
    std::vector<std::byte> src;
    tr::wire::emit_tlv(src, type_t::PATH, opt_t{}, far);
    const std::size_t before = in.sends();
    r.on_frame(kMount, tr::testing::b_fwd_reply(tr::graph::reply_kind_t::RESULT,
                                                b_path({"net", "mod", "in", "reply-ep"}), src));
    // A reply over a REMOVED child is stopped at the ingress (#2302) and relays nothing: that
    // is the strongest form of "mints no reference", so it answers nullopt too.
    if (in.sends() == before) return std::nullopt;
    const auto f = tr::wire::tlv_node_t::over(in.last());
    if (!f) return std::nullopt;
    std::size_t at = 0;  // children: op, dst, src, kind, payload
    for (const tr::wire::tlv_node_t c : f->children())
        if (at++ == 2) return tr::wire::path_pair_at(c.payload(), 0);
    return std::nullopt;
}

/**
 * @brief Forward a bound `FWD{READ}` from @ref kIn whose `dst` starts at @p e — the hop
 *        consumes @p e and egresses over the link it names (RFC-0029 §5).
 */
void bound_forward(fwd_router_t& r, path_ref_element_t e) {
    std::vector<std::byte> body;
    tr::testing::emit_path_pair(body, e);
    tr::testing::emit_path_pair(body, path_ref_element_t{.index = 7, .generation = 0});
    std::vector<std::byte> dst;
    tr::wire::emit_tlv(dst, type_t::PATH, opt_t{}, body);
    r.on_frame(kIn, b_fwd(fwd_op_t::READ, dst, b_path({"reply-ep"})));
}

// --- the tests ---------------------------------------------------------------

/**
 * @brief A REMOVED child stops minting its reference — the tombstone is on the CTX.
 *
 * The connection vertex is deliberately left registered here. The shipped teardown
 * (`transport_vertex_t::remove_connection`) retires it, and a retired vertex is refused by
 * `vertex_slot_at` on its own — which is exactly what made the stale ctx look harmless. Take
 * the graph's refusal away and the ctx has to carry the fact itself, or a removed child keeps
 * minting references to a link this node no longer has.
 */
void test_removed_child_stops_referencing() {
    std::printf("a removed child mints no reference\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    sink_link_t link;
    sink_link_t in;

    check(router.attach_link(std::string(kIn), in), "registered the inbound child");
    check(router.attach_link(std::string(kMount), link), "registered the child");
    check(minted_ref(router, in).has_value(), "a LIVE child mints a reference");
    check(router.remove_child(kMount), "removed it");
    check(!minted_ref(router, in).has_value(),
          "a REMOVED child mints none — even with its connection vertex still registered");
}

/**
 * @brief A re-added child resolves to the NEW tenancy, and its element egresses the NEW link.
 *
 * A child with no connection vertex is refused outright (#1940), so the first attempt
 * registers nothing; the second, over its vertex, and the re-add over a SECOND link are two
 * real tenancies. With the stale ctx answering first, the re-added child egressed the old
 * link, or none.
 */
void test_readd_resolves_the_new_tenancy() {
    std::printf("a re-added child resolves to the CURRENT tenancy\n");
    graph_t g;
    fwd_router_t router(g);
    sink_link_t first;
    sink_link_t second;
    sink_link_t in;

    check(router.attach_link(std::string(kIn), in), "registered the inbound child");
    check(!router.add_child(std::string(kMount), first),
          "refused while it has no connection vertex");
    check(!minted_ref(router, in).has_value(), "so it mints no reference");

    make_conn_vertex(g);
    check(router.add_child(std::string(kMount), first), "registered once its vertex exists");
    check(router.remove_child(kMount), "removed it");
    check(router.attach_link(std::string(kMount), second), "re-added it over a SECOND link");

    const std::optional<path_ref_element_t> e = minted_ref(router, in);
    check(e.has_value(), "the re-added child is bindable now its connection vertex exists");
    if (e) bound_forward(router, *e);
    check(second.sends() == 1 && first.sends() == 0, "and its element egresses over the NEW link");
}

/**
 * @brief A name re-added as a BUS mount answers nothing — the refusal is per tenancy.
 *
 * The other direction of the same fact. A bus mount takes no `conn_slot` at all (its `send`
 * BROADCASTS, ADR-0073 §3), so a point-to-point slot inherited from the name's previous life
 * would hand the origin a bindable-looking reference to a link that cannot carry a directed
 * operation.
 */
void test_readd_as_bus_drops_the_slot() {
    std::printf("a name re-added as a BUS mount mints no reference\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    sink_link_t p2p;
    bus_sink_link_t bus;
    sink_link_t in;

    check(router.attach_link(std::string(kIn), in), "registered the inbound child");
    check(router.attach_link(std::string(kMount), p2p), "registered it point-to-point");
    check(minted_ref(router, in).has_value(), "bindable while point-to-point");
    check(router.remove_child(kMount), "removed it");
    check(router.attach_link(std::string(kMount), bus), "re-added the NAME as a bus mount");
    check(!minted_ref(router, in).has_value(),
          "a bus mount is unbindable — it does not inherit the p2p slot");
}

/** @brief The session the bus tests seat in slot 1 of @ref kMount, and its name. */
constexpr tr::net::peer_handle_t kSession{tr::wire::pair_t{.index = 1, .generation = 2}};
/** @brief @ref kSession's routable name (`p<slot>~<gen>`). */
constexpr std::string_view kSessionName = "p1~2";

/** @brief Is @ref kSession's slot anchor live on @p g? */
bool anchored(const graph_t& g) {
    return g.find_session_anchor(fwd_router_t::session_anchor_id(kMount, kSessionName)).has_value();
}

/**
 * @brief A bus mount re-added point-to-point names no slot holder, and re-added as a bus
 *        mount again it has its tenancy words back (#2413).
 *
 * The words live on the bus token cache, which the ctx keeps across every re-add. A route
 * asks through the facet, so the point-to-point life answers nothing; the cache itself still
 * holds the word, so the next bus life finds the session it seated.
 */
void test_bus_p2p_bus_keeps_the_words() {
    std::printf("bus -> point-to-point -> bus: no holder in between, the words come back\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    bus_sink_link_t bus;
    sink_link_t p2p;

    check(router.attach_link(std::string(kMount), bus), "registered the bus mount");
    bus.notify_peer_up(kSession, kSessionName);
    check(door_t::holder(router, kMount, 1) == kSession, "the arrival seated the session");
    check(anchored(g), "and anchored it");

    check(router.remove_child(kMount), "removed the bus mount");
    check(router.attach_link(std::string(kMount), p2p), "re-added the NAME point-to-point");
    check(!door_t::holder(router, kMount, 1).valid(),
          "a point-to-point child names no slot holder");
    check(door_t::owned_holder(router, kMount, 1) == kSession,
          "the cache still holds the word underneath");

    check(router.remove_child(kMount), "removed the point-to-point child");
    check(router.attach_link(std::string(kMount), bus), "re-added the NAME as a bus mount");
    check(door_t::holder(router, kMount, 1) == kSession, "the bus mount has its words back");
}

/**
 * @brief A departure that lands while the name is point-to-point still retires the anchor and
 *        clears the word, so no stale word comes back on a later bus re-add (#2413).
 *
 * The departing bus still holds the ctx as its notifier context. The departure seam reads the
 * holder through the OWNING pointer: through the facet it would see no holder, skip the
 * retire, and hand the next bus life a word for a session that is gone.
 */
void test_late_departure_while_p2p() {
    std::printf("a late departure while point-to-point clears the word\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    bus_sink_link_t bus;
    sink_link_t p2p;

    check(router.attach_link(std::string(kMount), bus), "registered the bus mount");
    bus.notify_peer_up(kSession, kSessionName);
    check(router.remove_child(kMount), "removed the bus mount");
    check(router.attach_link(std::string(kMount), p2p), "re-added the NAME point-to-point");
    check(anchored(g), "control: the anchor outlived the registration");
    check(door_t::owned_holder(router, kMount, 1) == kSession, "control: the word is held");

    bus.notify_peer_down(kSession, kSessionName);
    check(!anchored(g), "the late departure retired the anchor");
    check(!door_t::owned_holder(router, kMount, 1).valid(), "and cleared the word");

    check(router.remove_child(kMount), "removed the point-to-point child");
    check(router.attach_link(std::string(kMount), bus), "re-added the NAME as a bus mount");
    check(!door_t::holder(router, kMount, 1).valid(), "no stale word came back");
}

/** @brief Churn on a stable name set occupies ONE receiver ctx — the #884 growth half. */
void test_churn_does_not_grow_the_chain() {
    std::printf("create/remove churn does not grow the receiver chain\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    sink_link_t a;
    sink_link_t b;

    check(router.attach_link(std::string(kMount), a), "registered the child");
    check(router.receiver_ctx_count() == 1, "one ctx for one child");
    bool churned = true;
    for (int i = 0; i < 50; ++i) {
        churned = churned && router.remove_child(kMount);
        churned = churned && router.attach_link(std::string(kMount), (i % 2 == 0) ? b : a);
    }
    churned = churned && router.remove_child(kMount);
    churned = churned && router.attach_link(std::string(kMount), b);
    check(churned, "51 remove/re-add rounds, every one of them accepted");
    // Reported unconditionally: the growth defect is a NUMBER, and a bare FAIL line would not
    // say whether the chain grew by one per cycle or by something else.
    std::printf("  (receiver ctx count after 51 rounds: %zu)\n", router.receiver_ctx_count());
    check(router.receiver_ctx_count() == 1, "51 create/remove rounds still occupy ONE ctx");
    check(router.registry().size() == 1, "and ONE registry slot, as #494 already pinned");

    sink_link_t in;
    check(router.attach_link(std::string(kIn), in), "registered a second, distinct name");
    check(router.receiver_ctx_count() == 2, "a genuinely new NAME appends");

    const std::optional<path_ref_element_t> e = minted_ref(router, in);
    check(e.has_value(), "the churned name is still bindable");
    const std::size_t a_before = a.sends();
    const std::size_t b_before = b.sends();
    if (e) bound_forward(router, *e);
    check(b.sends() == b_before + 1 && a.sends() == a_before,
          "and resolves to the link the LAST add bound, not the first");
}

/** @brief A duplicate add of a LIVE name rebinds its ctx rather than shadowing it. */
void test_duplicate_add_rebinds() {
    std::printf("a duplicate add of a live name rebinds\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    sink_link_t a;
    sink_link_t b;
    sink_link_t in;

    check(router.attach_link(std::string(kMount), a), "registered the child");
    check(router.attach_link(std::string(kMount), b), "registered the SAME name again");
    check(router.receiver_ctx_count() == 1, "no shadow ctx — the registry's rule, one layer out");
    check(router.attach_link(std::string(kIn), in), "registered the inbound child");
    const std::optional<path_ref_element_t> e = minted_ref(router, in);
    if (e) bound_forward(router, *e);
    check(e && b.sends() == 1 && a.sends() == 0,
          "and the name egresses over the link the re-add bound");
}

/**
 * @brief The canonical forward plane delivers to the CURRENT link after churn.
 *
 * The delivery leg: it is what stops the assertions above from being a statement about
 * integers. A `FWD{READ}` addressed through the churned mount must leave over the link the
 * last `add_child` bound and over no other.
 */
void test_forward_reaches_the_new_link() {
    std::printf("a forward after churn leaves over the CURRENT link\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    sink_link_t first;
    sink_link_t second;
    sink_link_t upstream;

    check(router.attach_link("net/mod/in", upstream), "registered the inbound child");
    check(router.attach_link(std::string(kMount), first), "registered the outbound child");
    check(router.remove_child(kMount), "removed the outbound child");
    check(router.attach_link(std::string(kMount), second), "re-added it over a SECOND link");

    const std::vector<std::byte> frame =
        b_fwd(fwd_op_t::READ, b_path({"net", "mod", "a", "sensor", "temp"}),
              b_path({"net", "mod", "in", "reply-ep"}));
    router.on_frame("net/mod/in", frame);
    check(second.sends() == 1, "the forward left over the NEW link");
    check(first.sends() == 0, "and never over the one the removed tenancy held");
}

}  // namespace

int main() {
    test_removed_child_stops_referencing();
    test_readd_resolves_the_new_tenancy();
    test_readd_as_bus_drops_the_slot();
    test_bus_p2p_bus_keeps_the_words();
    test_late_departure_while_p2p();
    test_churn_does_not_grow_the_chain();
    test_duplicate_add_rebinds();
    test_forward_reaches_the_new_link();
    return tr::testing::summary("child_rx_churn");
}
