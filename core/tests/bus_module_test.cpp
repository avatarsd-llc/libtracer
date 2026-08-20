/**
 * @file
 * @brief The ADR-0044 BUS module as a build-time-closed seam (#375 deliverable 3) — what
 *        `tr::graph::default_config_t::kBusLinks` closes, and what it must NOT disturb.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A bus link reaches MANY peers and names each of them, so the routing plane carries a second
 * addressing tier for it: a mount's bus SHAPE, per-peer resolution, in-band peer enumeration,
 * the peer-named receiver and the two peer-lifecycle notifiers. A node whose links are all
 * point-to-point pays flash for every byte of that. `kBusLinks = false` closes it, and every
 * consumer reaches the module through the ONE door `tr::net::bus_of` so the closure is a
 * compile-time fold rather than a run-time branch.
 *
 * This file follows the binding by `if constexpr`, exactly as `reclaim_test.cpp` follows
 * ADR-0080's, so ONE executable serves both legs of the seam. Five properties:
 *
 *   (a) the GATE itself — `bus_of` answers a point-to-point link nullptr under every binding,
 *       and answers a genuine bus link its facet iff this build carries the module. The link's
 *       OWN `bus()` is deliberately untouched either way: a transport is still allowed to be a
 *       bus, the routing plane is what stops asking;
 *   (b) the REGISTRY — a bus child's mount records the bus shape, resolves its peers through
 *       `resolve_peer`, and answers `by_name`'s peer fallback, iff the module is present. With
 *       it closed the same link registers as an ordinary point-to-point child, which is the
 *       correct reading and not a lost refusal: no bus mount can exist for a residual segment
 *       to sit below, because (d) refuses the one door that could create one;
 *   (c) a FLAT listener is untouched by the knob — it comes up, exposes no facet, and
 *       registers as the point-to-point child it always was. This is the property that makes
 *       the closure free rather than merely small;
 *   (d) a PEER-NAMED listener is REFUSED, never quietly demoted: `ok()` is false on a build
 *       that carries no bus module, so the came-up predicate every caller already checks
 *       (`make_checked`, #1059) turns the configuration error into a link that did not come
 *       up. A silent demotion to FLAT would leave the listener's own per-frame tier select
 *       delivering peer-named into a sink the router never installed;
 *   (e) the same refusal through the IN-BAND door — a `SPEC{listener, kind=ws|tcp,
 *       peer_named=1}` write is answered with an error instead of creating a connection,
 *       while the same SPEC without the key is created under both bindings.
 *
 * Ablation (2026-08-20, and re-run at every claim below): reverting `bus_of` to `link.bus()`
 * unconditionally reddens (a)'s gate assertion and every assertion in (b) at the closed
 * binding; reverting `slot_server_t::ok()`'s refusal limb reddens (d); dropping the two SPEC
 * factory refusals reddens (e). None of them touches (c) — the flat arm stays green
 * throughout, which is what says these guards are not vacuous.
 */

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/child_registry.hpp"
#include "libtracer/conn_spec.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer/transport_ws.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::net::bus_of;
using tr::net::child_registry_t;
using tr::net::conn_role_t;
using tr::net::conn_spec_t;
using tr::net::fwd_router_t;
using tr::net::kBusLinks;
using tr::net::transport_vertex_t;
using tr::testing::check;

/** @brief The ephemeral-port request every listener here makes — the OS picks a free one. */
constexpr std::uint16_t kEphemeral = 0;

/** @brief A point-to-point transport that swallows whatever is sent to it. */
struct sink_link_t : tr::net::transport_t {
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
};

/**
 * @brief A genuine BUS link with a fixed, single-peer census — the smallest thing that can
 *        answer `peer_link` with a real endpoint, which is what (b) needs.
 */
struct fake_bus_t : tr::net::transport_t, tr::net::bus_link_t {
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
    tr::net::bus_link_t* bus() override { return this; }
    tr::net::transport_t* peer_link(std::string_view peer) override {
        return peer == "n7" ? &peer_endpoint : nullptr;
    }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t& visit) const override {
        visit("n7");
    }
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t peer,
                                             std::span<char>) const override {
        return peer.valid() ? std::string_view("n7") : std::string_view();
    }
    sink_link_t peer_endpoint; /**< @brief The directed endpoint `peer_link("n7")` hands out. */
};

/** @brief This test application's module declarations (ADR-0073 §4: declared-only). */
void declare_listener_modules(transport_vertex_t& net) {
    (void)net.register_module(std::string(tr::net::kWsServerSuggestedModule), "ws",
                              conn_role_t::LISTEN);
    (void)net.register_module(std::string(tr::net::kTcpServerSuggestedModule), "tcp",
                              conn_role_t::LISTEN);
}

/** @brief A LISTENER SPEC for @p kind, optionally carrying the `peer_named` key. */
tr::view::view_t listener_spec(std::string_view kind, std::string_view name, bool peer_named,
                               bool with_key) {
    conn_spec_t spec("listener", name);
    (void)spec.role(conn_role_t::LISTEN).port(kEphemeral).kind(kind);
    if (with_key) (void)spec.flag("peer_named", peer_named);
    return spec.view();
}

/** @brief (a) The gate: what `bus_of` answers, against what the link itself says. */
void test_the_gate_is_the_only_door() {
    std::printf("(a) tr::net::bus_of is the routing plane's one door to the bus module:\n");
    sink_link_t p2p;
    fake_bus_t bus;

    check(p2p.bus() == nullptr && bus_of(p2p) == nullptr,
          "a point-to-point link has no facet, and the gate agrees — under every binding");
    check(bus.bus() == &bus,
          "the link's OWN bus() is untouched by the knob: a transport may still BE a bus");
    check((bus_of(bus) != nullptr) == kBusLinks,
          "the GATE answers that facet iff this build carries the bus module");
}

/** @brief (b) The registry: mount shape, scoped peer resolution, and the `by_name` fallback. */
void test_the_registry_follows_the_gate() {
    std::printf("(b) child_registry_t resolves peers iff the module is present:\n");
    child_registry_t registry;
    fake_bus_t bus;
    check(registry.add("can/bus0", bus), "the bus link registers as a child");

    const std::string_view segs[] = {"can", "bus0"};
    const child_registry_t::child_t* const mount =
        registry.longest_prefix(std::span<const std::string_view>(segs));
    check(mount != nullptr, "the mount resolves by its own qualified name");

    const bool shaped = mount != nullptr && mount->egress().multi_peer;
    check(shaped == kBusLinks,
          "the mount records the BUS shape iff the module is present (kBusShapeBit)");

    const bool scoped =
        mount != nullptr && child_registry_t::resolve_peer(*mount, "n7") == &bus.peer_endpoint;
    check(scoped == kBusLinks, "resolve_peer reaches the directed peer endpoint iff present");
    check((registry.by_name("n7") == &bus.peer_endpoint) == kBusLinks,
          "by_name's peer fallback answers iff present");
    check(registry.by_name("can/bus0") == &bus,
          "the mount's OWN name resolves either way — a bus link is still a child");
}

/** @brief (c) A FLAT listener is what the knob must not disturb. */
void test_a_flat_listener_is_untouched() {
    std::printf("(c) a FLAT listener comes up and stays flat under both bindings:\n");
    tr::net::transport_ws_server flat(kEphemeral, &tr::mem::heap_backend(),
                                      /*max_frame=*/1024, /*max_peers=*/4,
                                      /*peer_named=*/false);
    check(flat.ok(), "a peer_named=false ws listener comes up on every binding");
    check(flat.bus() == nullptr && bus_of(flat) == nullptr, "and exposes no bus facet, either way");
    check(!flat.peer_named(), "its mode authority answers FLAT");

    child_registry_t registry;
    check(registry.add("ws-server/flat", flat), "it registers as an ordinary child");
    const std::string_view segs[] = {"ws-server", "flat"};
    const child_registry_t::child_t* const mount =
        registry.longest_prefix(std::span<const std::string_view>(segs));
    check(mount != nullptr && !mount->egress().multi_peer,
          "and carries the point-to-point shape, on both bindings");
}

/** @brief (d) A PEER-NAMED listener is refused rather than demoted, when the module is closed. */
void test_a_peer_named_listener_is_refused_when_closed() {
    std::printf("(d) a peer_named listener: served when present, REFUSED when closed:\n");
    tr::net::transport_ws_server named(kEphemeral, &tr::mem::heap_backend(),
                                       /*max_frame=*/1024, /*max_peers=*/4,
                                       /*peer_named=*/true);
    check(named.ok() == kBusLinks,
          "ok() — the came-up predicate make_checked asks — is true iff the module is present");
    check((named.bus() != nullptr) == kBusLinks, "and the facet follows it");
    check(named.peer_named() == kBusLinks, "so does the mode authority: one answer, not two");
}

/** @brief (e) The same refusal through the in-band SPEC door, for both stream kinds. */
void test_the_spec_factory_refuses_the_key_when_closed() {
    std::printf("(e) SPEC{listener, peer_named=1} is answered with an error when closed:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_listener_modules(net);

    // The CONTROL, on both kinds: no `peer_named` key at all. Created under every binding —
    // this is the shape a bus-less target actually deploys, and it must not regress.
    check(node.write(path_t("/net:children[]"),
                     listener_spec("ws", "plain-ws", false, /*with_key=*/false))
              .has_value(),
          "a ws listener with no peer_named key is created under both bindings");
    check(node.write(path_t("/net:children[]"),
                     listener_spec("tcp", "plain-tcp", false, /*with_key=*/false))
              .has_value(),
          "a tcp listener with no peer_named key is created under both bindings");

    // The SUBJECT: the key asserted true. Answered, not silently downgraded.
    const auto ws_named =
        node.write(path_t("/net:children[]"), listener_spec("ws", "bus-ws", true, true));
    check(ws_named.has_value() == kBusLinks,
          "SPEC{kind=ws, peer_named=1} is served iff this build carries the bus module");
    check(ws_named.has_value() || ws_named.error() == tr::graph::status_t::TYPE_MISMATCH,
          "and its refusal is TYPE_MISMATCH — permanent, not the transient TRANSPORT_DOWN");
    check((net.link_of("net/ws-server/bus-ws") != nullptr) == kBusLinks,
          "a refused SPEC leaves NO connection behind");

    const auto tcp_named =
        node.write(path_t("/net:children[]"), listener_spec("tcp", "bus-tcp", true, true));
    check(tcp_named.has_value() == kBusLinks,
          "SPEC{kind=tcp, peer_named=1} answers exactly as the ws twin does");
    check(tcp_named.has_value() || tcp_named.error() == tr::graph::status_t::TYPE_MISMATCH,
          "with the same permanent status");
}

}  // namespace

int main() {
    std::printf("bus module bound by this build: kBusLinks = %s\n", kBusLinks ? "true" : "false");
    test_the_gate_is_the_only_door();
    test_the_registry_follows_the_gate();
    test_a_flat_listener_is_untouched();
    test_a_peer_named_listener_is_refused_when_closed();
    test_the_spec_factory_refuses_the_key_when_closed();
    return tr::testing::summary("bus_module");
}
