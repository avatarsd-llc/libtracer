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
 * name — `connection_ref`, `hop_mint`, and through them `adopt_binding` and the reply-mint
 * contribution — was handed the DEAD context, and one `child_rx_ctx_t` (+ its mount run)
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
 *   - a REMOVED child stops answering `connection_ref`, even while its connection vertex is
 *     still registered (the tombstone must be on the ctx, not inferred from the graph);
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
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::path_ref_element_t;
using tr::wire::type_t;

int g_failures = 0;

/** @brief Record one assertion's outcome on stdout and in the process exit status. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A transport that only counts and keeps what it was handed — no socket, no thread. */
class sink_link_t : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        sent_.emplace_back(frame.begin(), frame.end());
    }

    /** @brief Number of frames this link was asked to carry. */
    [[nodiscard]] std::size_t sends() const noexcept { return sent_.size(); }

   private:
    std::vector<std::vector<std::byte>> sent_;
};

/** @brief A minimal BUS link — a mount whose `send` broadcasts, so it takes NO `conn_slot`. */
class bus_sink_link_t : public sink_link_t, public tr::net::bus_link_t {
   public:
    tr::net::bus_link_t* bus() override { return this; }
    void enumerate_peers(const peer_visitor_t&) const override {}
    tr::net::transport_t* peer_link(std::string_view) override { return nullptr; }
};

/** @brief Canonical `PATH{ NAME… }` bytes for @p segs, via the production emit helpers. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

using tr::testing::b_fwd;

/** @brief The mount run the tests churn, and the vertex key that is its connection vertex. */
constexpr std::string_view kMount = "net/mod/a";

/** @brief Register the connection vertex `/net/mod/a` — what makes @ref kMount bindable. */
void make_conn_vertex(graph_t& g) {
    const auto p = path_t::parse("/net/mod/a");
    (void)g.register_vertex(*p, role_t::STORED_VALUE);
}

// --- the tests ---------------------------------------------------------------

/**
 * @brief A REMOVED child stops answering `connection_ref` — the tombstone is on the CTX.
 *
 * The connection vertex is deliberately left registered here. The shipped teardown
 * (`transport_vertex_t::remove_connection`) retires it, and a retired vertex is refused by
 * `vertex_slot_at` on its own — which is exactly what made the stale ctx look harmless. Take
 * the graph's refusal away and the ctx has to carry the fact itself, or a removed child keeps
 * minting references to a link this node no longer has.
 */
void test_removed_child_stops_referencing() {
    std::printf("a removed child answers no connection_ref\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    sink_link_t link;

    check(router.add_child(std::string(kMount), link), "registered the child");
    check(router.connection_ref(kMount).has_value(), "a LIVE child has a connection ref");
    check(router.remove_child(kMount), "removed it");
    check(!router.connection_ref(kMount).has_value(),
          "a REMOVED child has none — even with its connection vertex still registered");
}

/**
 * @brief A re-added child resolves to the NEW tenancy, and its element egresses the NEW link.
 *
 * The ordering is the one `add_child`'s own contract calls out — "a child registered before
 * its connection vertex exists simply has none" — so the two tenancies genuinely differ:
 * unbindable first, bindable second. With the stale ctx answering first, the re-created child
 * was permanently unbindable and every bound route through it silently fell back to canonical.
 */
void test_readd_resolves_the_new_tenancy() {
    std::printf("a re-added child resolves to the CURRENT tenancy\n");
    graph_t g;
    fwd_router_t router(g);
    sink_link_t first;
    sink_link_t second;

    check(router.add_child(std::string(kMount), first), "registered the child, vertex-less");
    check(!router.connection_ref(kMount).has_value(),
          "unbindable while it has no connection vertex");

    check(router.remove_child(kMount), "removed it");
    make_conn_vertex(g);
    check(router.add_child(std::string(kMount), second), "re-added it over a SECOND link");

    const std::optional<path_ref_element_t> e = router.connection_ref(kMount);
    check(e.has_value(), "the re-added child is bindable now its connection vertex exists");
    check(e && router.bound_egress(*e, {}, acl_right_t::READ) == &second,
          "and element 0 egresses over the NEW link");
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
    std::printf("a name re-added as a BUS mount answers no connection_ref\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    sink_link_t p2p;
    bus_sink_link_t bus;

    check(router.add_child(std::string(kMount), p2p), "registered it point-to-point");
    check(router.connection_ref(kMount).has_value(), "bindable while point-to-point");
    check(router.remove_child(kMount), "removed it");
    check(router.add_child(std::string(kMount), bus), "re-added the NAME as a bus mount");
    check(!router.connection_ref(kMount).has_value(),
          "a bus mount is unbindable — it does not inherit the p2p slot");
}

/** @brief Churn on a stable name set occupies ONE receiver ctx — the #884 growth half. */
void test_churn_does_not_grow_the_chain() {
    std::printf("create/remove churn does not grow the receiver chain\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    sink_link_t a;
    sink_link_t b;

    check(router.add_child(std::string(kMount), a), "registered the child");
    check(router.receiver_ctx_count() == 1, "one ctx for one child");
    bool churned = true;
    for (int i = 0; i < 50; ++i) {
        churned = churned && router.remove_child(kMount);
        churned = churned && router.add_child(std::string(kMount), (i % 2 == 0) ? b : a);
    }
    churned = churned && router.remove_child(kMount);
    churned = churned && router.add_child(std::string(kMount), b);
    check(churned, "51 remove/re-add rounds, every one of them accepted");
    // Reported unconditionally: the growth defect is a NUMBER, and a bare FAIL line would not
    // say whether the chain grew by one per cycle or by something else.
    std::printf("  (receiver ctx count after 51 rounds: %zu)\n", router.receiver_ctx_count());
    check(router.receiver_ctx_count() == 1, "51 create/remove rounds still occupy ONE ctx");
    check(router.registry().size() == 1, "and ONE registry slot, as #494 already pinned");

    const std::optional<path_ref_element_t> e = router.connection_ref(kMount);
    check(e.has_value(), "the churned name is still bindable");
    check(e && router.bound_egress(*e, {}, acl_right_t::READ) == &b,
          "and resolves to the link the LAST add bound, not the first");

    sink_link_t other;
    check(router.add_child("net/mod/other", other), "registered a second, distinct name");
    check(router.receiver_ctx_count() == 2, "a genuinely new NAME appends");
}

/** @brief A duplicate add of a LIVE name rebinds its ctx rather than shadowing it. */
void test_duplicate_add_rebinds() {
    std::printf("a duplicate add of a live name rebinds\n");
    graph_t g;
    fwd_router_t router(g);
    make_conn_vertex(g);
    sink_link_t a;
    sink_link_t b;

    check(router.add_child(std::string(kMount), a), "registered the child");
    check(router.add_child(std::string(kMount), b), "registered the SAME name again");
    check(router.receiver_ctx_count() == 1, "no shadow ctx — the registry's rule, one layer out");
    const std::optional<path_ref_element_t> e = router.connection_ref(kMount);
    check(e && router.bound_egress(*e, {}, acl_right_t::READ) == &b,
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

    check(router.add_child("net/mod/in", upstream), "registered the inbound child");
    check(router.add_child(std::string(kMount), first), "registered the outbound child");
    check(router.remove_child(kMount), "removed the outbound child");
    check(router.add_child(std::string(kMount), second), "re-added it over a SECOND link");

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
    test_churn_does_not_grow_the_chain();
    test_duplicate_add_rebinds();
    test_forward_reaches_the_new_link();
    if (g_failures != 0) std::printf("\n%d check(s) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
