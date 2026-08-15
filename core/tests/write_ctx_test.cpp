/**
 * @file
 * @brief #375 — a HANDLER's `on_write` receives the writer's SUBJECT (`tr::graph::write_ctx_t`).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section defect The defect
 *
 * `on_write` was `std::function<result_t<void>(const rope_t&)>`: the vertex's ACL gate
 * resolved the caller one stack frame earlier and then threw it away, so a HANDLER — the one
 * seam where application code REACTS to a write — could not see WHO wrote. An app that needs
 * per-writer behaviour (an auth endpoint is the motivating one) had no reachable identity at
 * all. The fix hands the handler the value the gate just used, in a `write_ctx_t` whose
 * `subject` is BORROWED for the call.
 *
 * @section cases What each case pins
 *
 *  - @ref test_local_host_write_is_the_owner_sentinel — the OWNER path. A local API write
 *    presents the EMPTY subject, which `write_ctx_t::is_local_owner` reports; it is the same
 *    discriminator `graph_t::acl_allows` short-circuits on (#905), so it is a sentinel no
 *    remote writer can spell rather than a magic string. (Deliberately NOT `OWNER@` —
 *    ADR-0020's erratum, #1033, withdrew that name.)
 *  - @ref test_two_links_are_distinguishable — two remote writers on two point-to-point
 *    links, one HANDLER: the two subjects differ and each names its inbound link.
 *  - @ref test_two_bus_peers_on_one_link_are_distinguishable — the load-bearing one. TWO
 *    PEERS ON ONE LINK, one HANDLER, one mount: the subjects still differ and name the
 *    PEERS, not the link they share. Before #375 the handler saw nothing at all; a handler
 *    keyed on the link name would see one principal where there are two.
 *  - @ref test_subscription_delivery_presents_the_edge_subject — a delivered subscription
 *    terminates at its target as a write under the EDGE's stored fan-in context (ADR-0051
 *    §6.2), so the target handler sees that context and not the producer's writer.
 *
 * @section ablation Ablation
 *
 * Reverting `graph_t::store_value`'s `caller` argument to `{}` (the pre-#375 wiring, where
 * the handler could only have been told "local") reddens the two distinguishability cases
 * and the two-links case's per-link assertions, while the owner case still passes — which is
 * exactly the shape a vacuous guard would NOT have.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::graph::write_ctx_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::check;
using tr::testing::make_value;

/** @brief A point-to-point transport that swallows whatever the router sends it. */
struct sink_link_t : tr::net::transport_t {
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
};

/** @brief A multi-peer transport with no peer table — it only drives INBOUND peer-tagged
 *         frames, which is the whole of what these cases need from a bus. */
struct bus_link_impl_t : tr::net::transport_t, tr::net::bus_link_t {
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
    tr::net::bus_link_t* bus() override { return this; }
    tr::net::transport_t* peer_link(std::string_view) override { return nullptr; }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t&) const override {}
    /** @brief The handle's index into @ref names is the peer's name (#1294). */
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t peer,
                                             std::span<char>) const override {
        if (!peer.valid() || peer.index >= names.size()) return {};
        return names[peer.index];
    }
    /**
     * @brief Drive an inbound frame up the peer-named slot, as a real bus adapter does —
     *        tagged with the peer's HANDLE (#1294), minted here on first sight of a name.
     */
    void deliver(std::string_view peer, std::span<const std::byte> frame) {
        peer_rx_.deliver_borrowed(mint(peer), frame);
    }
    /** @brief The handle for @p peer, appending it to the census if it is new. */
    tr::net::peer_handle_t mint(std::string_view peer) {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == peer) return {static_cast<std::uint32_t>(i), 1};
        names.emplace_back(peer);
        return {static_cast<std::uint32_t>(names.size() - 1), 1};
    }
    std::vector<std::string> names; /**< @brief index → peer name, the handle's meaning. */
};

/**
 * @brief What one `on_write` call saw — the subject COPIED, per the seam's own contract.
 *
 * The `string_view` handed to a handler is borrowed for the call, so a recorder that stored
 * the view would be reading freed router memory by the time a case asserted on it. Copying
 * here is not test hygiene; it is the documented usage this file also demonstrates.
 */
struct seen_t {
    std::string subject;      /**< @brief The subject token's bytes, copied out of the call. */
    bool local_owner = false; /**< @brief What `write_ctx_t::is_local_owner` answered. */
};

/** @brief A HANDLER at @p path recording every write's subject into @p log. */
vertex_handle_t register_recorder(graph_t& g, std::string_view path, std::vector<seen_t>& log) {
    handlers_t h;
    h.on_write = [&log](const tr::view::rope_t&,
                        const write_ctx_t& ctx) -> tr::graph::result_t<void> {
        log.push_back(
            seen_t{.subject = std::string(ctx.subject), .local_owner = ctx.is_local_owner()});
        return {};
    };
    return g.register_vertex(*path_t::parse(path), role_t::HANDLER, std::move(h));
}

/** @brief A `PATH` TLV over the given `/`-segments. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief One byte of opaque payload — these cases assert on the SUBJECT, never the value. */
std::vector<std::byte> b_payload() {
    const std::byte one[1] = {std::byte{0x42}};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(one, 1));
    return out;
}

/**
 * @brief A LOCAL host write presents the empty OWNER subject.
 *
 * Both local doors — the handle form and the path form — take the trusted local context, so
 * both must present the same sentinel. This is the case that must SURVIVE the ablation: it
 * asserts the value the pre-#375 wiring would have produced everywhere, and a suite where
 * only this passed would be exactly the vacuous guard the other cases exist to rule out.
 */
void test_local_host_write_is_the_owner_sentinel() {
    std::printf("a local host write reaches on_write as the OWNER (empty subject):\n");
    graph_t g;
    std::vector<seen_t> log;
    const vertex_handle_t h = register_recorder(g, "/sink", log);

    check(g.write(h, make_value(b_payload())).has_value(), "the handle-form local write lands");
    check(g.write(*path_t::parse("/sink"), make_value(b_payload())).has_value(),
          "the path-form local write lands too");
    check(log.size() == 2, "the handler ran once per write");
    if (log.size() == 2) {
        check(log[0].subject.empty() && log[0].local_owner,
              "the handle-form write is the empty OWNER token");
        check(log[1].subject.empty() && log[1].local_owner,
              "and so is the path-form write — one sentinel, both doors");
    }
}

/**
 * @brief Two remote writers on two links reach one HANDLER as two distinct subjects.
 *
 * The baseline shape: distinct links, distinct inbound names. It is the weaker of the two
 * distinguishability cases — it would also pass on a design that keyed identity off the link
 * — and it is here so the bus case below cannot be read as merely "the router delivers".
 */
void test_two_links_are_distinguishable() {
    std::printf("two links writing one HANDLER present two distinct subjects:\n");
    graph_t g;
    std::vector<seen_t> log;
    (void)register_recorder(g, "/sink", log);

    fwd_router_t router(g);
    sink_link_t a;
    sink_link_t b;
    (void)router.add_child("peer-a", a);
    (void)router.add_child("peer-b", b);

    router.on_frame("peer-a",
                    b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({"origin"}), {}, b_payload()));
    router.on_frame("peer-b",
                    b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({"origin"}), {}, b_payload()));

    check(log.size() == 2, "both remote writes reached the handler");
    if (log.size() == 2) {
        check(!log[0].local_owner && !log[1].local_owner,
              "neither remote write is presented as the local owner");
        check(log[0].subject == "peer-a", "the first write names its inbound link");
        check(log[1].subject == "peer-b", "the second names the other");
        check(log[0].subject != log[1].subject, "so the two writers are distinguishable");
    }
}

/**
 * @brief TWO PEERS ON ONE LINK, one HANDLER — still two distinct subjects (#375's goal).
 *
 * This is the case that cannot be satisfied by the link name: both frames arrive on the same
 * registered child (`net/ws-server/srv`), tagged only by the sending peer. The router keys
 * the terminus write on the bare PEER, so the handler sees `alice` and `bob` where a
 * link-keyed design sees one principal twice — which is exactly the collapse #375 reported.
 */
void test_two_bus_peers_on_one_link_are_distinguishable() {
    std::printf("two peers on ONE link present two distinct subjects:\n");
    graph_t g;
    std::vector<seen_t> log;
    (void)register_recorder(g, "/sink", log);

    fwd_router_t router(g);
    bus_link_impl_t bus;
    (void)router.add_child("net/ws-server/srv", bus);

    bus.deliver("alice",
                b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({"origin"}), {}, b_payload()));
    bus.deliver("bob",
                b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({"origin"}), {}, b_payload()));

    check(log.size() == 2, "both peers' writes reached the handler");
    if (log.size() == 2) {
        check(!log[0].local_owner && !log[1].local_owner,
              "neither peer's write is presented as the local owner");
        check(log[0].subject == "alice" && log[1].subject == "bob",
              "each write names the SENDING PEER, not the shared link");
        check(log[0].subject != log[1].subject,
              "so two peers sharing one link are distinguishable at the handler");
    }
}

/**
 * @brief A delivered subscription presents the EDGE's fan-in context, not the producer's.
 *
 * Delivery terminates at the target as an ordinary write under the edge's stored caller
 * (ADR-0051 / #81), and that is the context the target's ACL was just gated on — so it is
 * also what the target's handler must see. A locally-wired edge carries the empty context,
 * so the target handler sees the owner even though the producer was written remotely: the
 * subject describes THIS write's authority, never the chain that provoked it.
 */
void test_subscription_delivery_presents_the_edge_subject() {
    std::printf("a delivered subscription presents the EDGE's subject at the target:\n");
    graph_t g;
    std::vector<seen_t> log;
    (void)g.register_vertex(*path_t::parse("/src"), role_t::STORED_VALUE);
    (void)register_recorder(g, "/dst", log);
    check(g.subscribe(*path_t::parse("/src"), *path_t::parse("/dst")).has_value(),
          "the local edge is wired");

    fwd_router_t router(g);
    sink_link_t a;
    (void)router.add_child("peer-a", a);
    router.on_frame("peer-a",
                    b_fwd(fwd_op_t::WRITE, b_path({"src"}), b_path({"origin"}), {}, b_payload()));

    check(log.size() == 1, "the delivery reached the target handler");
    if (log.size() == 1)
        check(log[0].subject.empty() && log[0].local_owner,
              "under the LOCAL edge's own context, not the remote producer's writer");
}

}  // namespace

int main() {
    test_local_host_write_is_the_owner_sentinel();
    test_two_links_are_distinguishable();
    test_two_bus_peers_on_one_link_are_distinguishable();
    test_subscription_delivery_presents_the_edge_subject();
    return tr::testing::summary("write_ctx");
}
