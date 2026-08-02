/**
 * @file
 * @brief The per-module mount-routing primitives (ADR-0061 / RFC-0014 S2a).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Covers the three mechanisms strip-K routing is built from, ahead of the demux being
 * rewired onto them:
 *   - `child_registry_t::longest_prefix` — matches every registered key against the `dst`
 *     prefix in ONE pass, each slot against the prefix of its OWN width, WITHOUT building a
 *     key (the `allocs=0` forward gate), and keeps two modules' same-named connections
 *     distinct;
 *   - `child_registry_t::resolve_peer` — per-endpoint peer resolution (ADR-0061), so a
 *     peer is reachable only through the module it belongs to, never across buses;
 *   - `peek_fwd_dst` + `dst_seg_walk_t` + strip-K `rebuild_fwd_forward` — consuming K leading `dst`
 *     segments and growing `src` by the FULL mount path, which is what keeps a reply
 *     resolvable once names are per-module-scoped (the ADR-0061 erratum).
 *
 * It then covers the CONTROL plane over the same mounts (#516). The forward path and the
 * route-handle ADVERTISE/COMPACT plane must descend by the same rule; they did not, and no
 * test noticed, because every route-handle test wires FLAT single-segment children. So the
 * last two cases build a real `fwd_router_t` whose children are RFC-0014 qualified mounts
 * and drive an advertise+compact through it.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::net::child_registry_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A point-to-point transport that records nothing — an identity for the table. */
struct p2p_link_t : tr::net::transport_t {
    std::size_t received = 0; /**< @brief Frames this endpoint was handed. */
    void send(std::span<const std::byte>) override { ++received; }
    void send(std::span<const std::span<const std::byte>>) override { ++received; }
};

/** @brief A multi-peer transport whose peer table is a fixed name→endpoint list. */
struct bus_link_impl_t : tr::net::transport_t, tr::net::bus_link_t {
    std::vector<std::pair<std::string, p2p_link_t*>> peers;
    /** @brief Frames sent on the BUS endpoint itself — a real adapter fans these out to
     *         EVERY open peer, so any count here is a broadcast (ADR-0073 §3). */
    std::size_t broadcasts = 0;
    void send(std::span<const std::byte>) override { ++broadcasts; }
    void send(std::span<const std::span<const std::byte>>) override { ++broadcasts; }
    tr::net::bus_link_t* bus() override { return this; }
    tr::net::transport_t* peer_link(std::string_view name) override {
        for (auto& [n, l] : peers) {
            if (n == name) return l;
        }
        return nullptr;
    }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t& visit) const override {
        for (const auto& [n, l] : peers) visit(n);
    }
    /** @brief Drive an inbound frame up the peer-named slot, as a real bus adapter does. */
    void deliver(std::string_view peer, std::span<const std::byte> frame) {
        peer_rx_.deliver_borrowed(peer, frame);
    }
};

/**
 * @brief The EXACT-width lookup the old `by_segments` gave, expressed through the single pass.
 *
 * `longest_prefix` answers "the widest registered mount that PREFIXES this address", which is
 * the routing question. These cases ask the narrower one — "is there a slot spelled exactly
 * this?" — so the prefix answer is confirmed to have consumed the whole list.
 */
const child_registry_t::child_t* find(const child_registry_t& reg,
                                      std::initializer_list<std::string_view> segs) {
    const std::vector<std::string_view> v(segs);
    const child_registry_t::child_t* const c =
        reg.longest_prefix(std::span<const std::string_view>(v));
    return (c != nullptr && c->seg_count == v.size()) ? c : nullptr;
}

void emit_path(std::vector<std::byte>& out, std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
}

std::vector<std::byte> make_fwd(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    emit_path(body, src);
    const std::byte payload[2] = {std::byte{0x01}, std::byte{0x02}};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload, 2));
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/** @brief The NAME segments of the first PATH inside a rebuilt FWD body (dst), then src. */
std::vector<std::vector<std::string>> paths_of(std::span<const std::byte> frame) {
    std::vector<std::vector<std::string>> out;
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD) return out;
    for (const auto& child : dec->children) {
        if (child.type != type_t::PATH) continue;
        std::vector<std::string> segs;
        for (const auto& seg : child.children) {
            if (seg.type == type_t::NAME)
                segs.emplace_back(tr::detail::as_string_view(seg.payload));
        }
        out.push_back(std::move(segs));
    }
    return out;
}

// --- tests -------------------------------------------------------------------

/** @brief Two modules may hold the same connection NAME and stay distinct. */
void test_module_scoping() {
    std::printf("per-module scoping\n");
    child_registry_t reg;
    p2p_link_t ws;
    p2p_link_t tcp;
    reg.add("ws-client/foo", ws);
    reg.add("tcp-client/foo", tcp);

    const auto* a = find(reg, {"ws-client", "foo"});
    const auto* b = find(reg, {"tcp-client", "foo"});
    check(a != nullptr && a->link == &ws, "/net/ws-client/foo resolves to the ws link");
    check(b != nullptr && b->link == &tcp, "/net/tcp-client/foo resolves to the tcp link");
    check(a != b, "the same NAME in two modules is two distinct children");
    check(find(reg, {"ws-client", "nope"}) == nullptr, "an unknown name in a known module misses");
    check(find(reg, {"nope", "foo"}) == nullptr, "a known name in an unknown module misses");
    check(find(reg, {"ws-client"}) == nullptr, "the module segment alone is not a child");
    check(find(reg, {"ws-client", "foo", "extra"}) == nullptr,
          "an over-long path is not this child");
}

/** @brief A prefix that merely looks similar must not match (the `/` boundary is real). */
void test_no_prefix_confusion() {
    std::printf("qualified-key boundaries\n");
    child_registry_t reg;
    p2p_link_t a;
    reg.add("ws/client-foo", a);
    check(find(reg, {"ws", "client-foo"}) != nullptr, "the exact split matches");
    check(find(reg, {"ws/client", "foo"}) == nullptr, "a differently-placed separator does not");
    check(find(reg, {"ws", "client", "foo"}) == nullptr, "nor does a different arity");
}

/** @brief Shape is captured at add time; peers resolve only within their own endpoint. */
void test_scoped_peer_resolution() {
    std::printf("per-endpoint peer resolution\n");
    child_registry_t reg;
    bus_link_impl_t ws_srv;
    bus_link_impl_t tcp_srv;
    p2p_link_t alice_ws;
    p2p_link_t alice_tcp;
    p2p_link_t client;
    ws_srv.peers.emplace_back("alice", &alice_ws);
    tcp_srv.peers.emplace_back("alice", &alice_tcp);
    reg.add("ws-server/s", ws_srv);
    reg.add("tcp-server/s", tcp_srv);
    reg.add("ws-client/c", client);

    const auto* ws = find(reg, {"ws-server", "s"});
    const auto* tcp = find(reg, {"tcp-server", "s"});
    const auto* p2p = find(reg, {"ws-client", "c"});
    check(ws != nullptr && ws->multi_peer, "a bus child records multi_peer at add time");
    check(p2p != nullptr && !p2p->multi_peer, "a point-to-point child does not");

    check(child_registry_t::resolve_peer(*ws, "alice") == &alice_ws,
          "/net/ws-server/s/alice reaches the ws peer");
    check(child_registry_t::resolve_peer(*tcp, "alice") == &alice_tcp,
          "the tcp server's OWN alice is a different endpoint");
    check(child_registry_t::resolve_peer(*ws, "alice") !=
              child_registry_t::resolve_peer(*tcp, "alice"),
          "two servers' same-named peers never collide");
    check(child_registry_t::resolve_peer(*ws, "zzz") == nullptr,
          "an unknown peer is a clean miss, not a black hole");
    check(child_registry_t::resolve_peer(*p2p, "alice") == nullptr,
          "a point-to-point child resolves no peer at all");
}

/** @brief The peek opens the dst window; the walker reads segments lazily, to ANY depth. */
void test_peek_segments() {
    std::printf("multi-segment dst peek\n");
    const std::vector<std::byte> frame =
        make_fwd({"net", "ws-client", "foo", "sensor", "temp"}, {"reply"});
    const tr::wire::grammar::span_cursor cur{std::span<const std::byte>(frame)};
    tr::net::fwd_pre_t pre;
    check(tr::net::peek_fwd_dst(cur, pre), "a structured FWD opens its dst window");
    tr::net::dst_seg_walk_t<tr::wire::grammar::span_cursor> walk(cur, pre);
    const auto at = [&](std::size_t i) -> std::string {
        const auto s = walk.at(i);
        if (!s) return "<none>";
        return std::string(reinterpret_cast<const char*>(frame.data()) + s->first, s->second);
    };
    check(at(0) == "net", "segment 0 is the /net root");
    check(at(1) == "ws-client", "segment 1 is the module");
    check(at(2) == "foo", "segment 2 is the connection name");
    check(at(3) == "sensor", "segment 3 is the first residual segment");
    // The fifth segment is the one the old fixed-window peek could never see: it stopped at
    // `kMountPeekMax = 4`. Reading it is the whole width lift in one assertion (#523).
    check(at(4) == "temp", "segment 4 is reachable — there is no peek window any more");
    check(at(5) == "<none>", "past the end is a clean miss, not a stale repeat");
    // Backwards access restarts the walk rather than answering from a stale cursor — the
    // narrower-slot-after-a-wider-one case the single pass takes constantly.
    check(at(1) == "ws-client", "a backwards read restarts the walk and still answers");
    check(walk.end_of(2) == pre.dst_body_off + 4 + 3 + 4 + 9 + 4 + 3,
          "end_of(k-1) is where the consumed run stops");

    const std::vector<std::byte> shortf = make_fwd({"net", "can"}, {"reply"});
    const tr::wire::grammar::span_cursor scur{std::span<const std::byte>(shortf)};
    tr::net::fwd_pre_t spre;
    check(tr::net::peek_fwd_dst(scur, spre), "a short dst still opens");
    tr::net::dst_seg_walk_t<tr::wire::grammar::span_cursor> swalk(scur, spre);
    check(swalk.at(1).has_value() && !swalk.at(2).has_value(),
          "a short dst yields only what it has");
}

/** @brief strip-K consumes the mount and grows src by the WHOLE mount (the erratum). */
void test_strip_k_and_symmetric_src() {
    std::printf("strip-K + symmetric return route\n");
    const std::vector<std::byte> frame =
        make_fwd({"net", "ws-client", "foo", "sensor", "temp"}, {"origin"});
    const tr::wire::grammar::span_cursor cur{std::span<const std::byte>(frame)};
    const std::string_view mount[3] = {"net", "ws-server", "up"};
    const auto enc = tr::net::encode_mount_tlv(std::span<const std::string_view>(mount));
    check(enc.has_value(), "the mount prefix encodes");
    const auto rb = tr::net::rebuild_fwd_forward(cur, std::span<const std::byte>(*enc), {}, 3);
    check(rb.has_value() && rb->ok(), "the hop rebuilds");
    if (!rb || !rb->ok()) return;

    std::vector<std::byte> out;
    rb->gather(cur,
               [&](std::span<const std::byte> s) { out.insert(out.end(), s.begin(), s.end()); });
    const auto paths = paths_of(out);
    check(paths.size() == 2, "the rebuilt frame still carries dst and src");
    if (paths.size() != 2) return;

    const std::vector<std::string> want_dst = {"sensor", "temp"};
    check(paths[0] == want_dst, "dst lost exactly the 3 mount segments");
    const std::vector<std::string> want_src = {"net", "ws-server", "up", "origin"};
    check(paths[1] == want_src,
          "src grew by the FULL inbound mount path, so the reply stays unambiguous");
}

/** @brief A dst shorter than the mount is not forwardable — it falls to the terminus. */
void test_short_dst_is_not_forwardable() {
    std::printf("dst shorter than the mount\n");
    const std::vector<std::byte> frame = make_fwd({"net", "ws-client"}, {"origin"});
    const tr::wire::grammar::span_cursor cur{std::span<const std::byte>(frame)};
    const std::string_view mount[3] = {"net", "ws-server", "up"};
    const auto enc = tr::net::encode_mount_tlv(std::span<const std::string_view>(mount));
    check(!tr::net::rebuild_fwd_forward(cur, std::span<const std::byte>(*enc), {}, 3),
          "stripping more segments than dst holds is refused, not truncated");
}

// --- control plane over qualified mounts (#516) -------------------------------

/** @brief A link that records every frame it is asked to send. */
struct recording_link_t : tr::net::transport_t {
    std::vector<std::vector<std::byte>> sent;
    void send(std::span<const std::byte> f) override { sent.emplace_back(f.begin(), f.end()); }
};

/** @brief The route TLV carried by an ADVERTISE frame, as its NAME segments. */
std::vector<std::string> advertised_route(std::span<const std::byte> frame) {
    std::vector<std::string> segs;
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->children.size() < 2) return segs;
    for (const auto& seg : dec->children[1].children) {
        if (seg.type == type_t::NAME) segs.emplace_back(tr::detail::as_string_view(seg.payload));
    }
    return segs;
}

/**
 * @brief An ADVERTISE addressed to a `/net/<module>/<name>` mount FORWARDS, stripping K.
 *
 * The #516 regression. `on_advertise` resolved a single BARE leading segment, so this route
 * missed the registry entirely, fell through to the terminus arm, and bound the label to a
 * LOCAL route at a node that was only supposed to relay — every subsequent COMPACT was then
 * absorbed here instead of reaching the real target.
 */
void test_advertise_descends_the_mount() {
    std::printf("ADVERTISE over a qualified mount (#516)\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t up;
    recording_link_t down;
    router.add_child("net/ws-client/up", up);
    router.add_child("net/ws-server/down", down);

    std::vector<std::byte> route;
    emit_path(route, {"net", "ws-server", "down", "sink"});
    const std::vector<std::byte> adv = tr::net::encode_advertise(7, route);
    router.on_frame("net/ws-client/up", adv);

    check(down.sent.size() == 1, "the advertise is re-advertised downstream, not absorbed");
    if (down.sent.size() != 1) return;
    const std::vector<std::string> want = {"sink"};
    check(advertised_route(down.sent[0]) == want,
          "the egress route lost ALL 3 mount segments, not just the leading one");

    // And the label now relays: a COMPACT on the bound label must leave on `down`, and
    // must NOT be swallowed as a local delivery.
    const std::byte payload[2] = {std::byte{0xAA}, std::byte{0xBB}};
    std::vector<std::byte> value;
    tr::wire::emit_tlv(value, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload, 2));
    router.on_frame("net/ws-client/up", tr::net::encode_compact(7, value));
    check(down.sent.size() == 2, "a COMPACT on that label is forwarded downstream");
    check(up.sent.empty(), "and no NACK travels back — the binding resolved");
}

/**
 * @brief A frame from a bus PEER grows `src` by the FULL mount, not the bare peer (#510).
 *
 * The reply-direction twin of "two servers' same-named peers never collide". A peer has no
 * registry entry, so before #510 the hop encoded the bare peer segment into `src` — and two
 * buses carrying a peer with the same name produced IDENTICAL return routes. The forward
 * direction was pinned; this is the direction that was not.
 */
void test_bus_peer_src_carries_the_mount() {
    std::printf("bus-peer return route (#510)\n");
    // Two buses, each with a peer called "n5". A frame from each must come back
    // distinguishable.
    const auto src_of = [](std::string_view module, std::string_view conn) {
        bus_link_impl_t bus;
        p2p_link_t n5;
        bus.peers.emplace_back("n5", &n5);
        recording_link_t out;

        tr::graph::graph_t graph;
        tr::net::fwd_router_t router{graph};
        std::string child(module);
        child += '/';
        child += conn;
        router.add_child(std::string("net/") + child, bus);
        router.add_child("net/ws-client/out", out);

        // The bus hands the frame up tagged with the sending peer's name; the router's
        // per-child ctx is what supplies the mount. Driving it through set_peer_receiver
        // (rather than calling on_frame directly) is the point — that wiring is the fix.
        const std::vector<std::byte> frame =
            make_fwd({"net", "ws-client", "out", "sensor"}, {"origin"});
        bus.deliver("n5", frame);

        std::vector<std::string> src;
        if (out.sent.size() == 1) {
            const auto paths = paths_of(out.sent[0]);
            if (paths.size() == 2) src = paths[1];
        }
        return src;
    };

    const std::vector<std::string> a = src_of("can", "can0");
    const std::vector<std::string> b = src_of("ws-server", "srv");
    const std::vector<std::string> want_a = {"net", "can", "can0", "n5", "origin"};
    check(a == want_a, "src grew by the full net/<module>/<name>/<peer> mount");
    check(a != b, "two buses' same-named peers produce DIFFERENT return routes");
}

/**
 * @brief The route a hop grows must be routable BACK — the round trip, end to end.
 *
 * #513 patched a symptom (a reply leading with a bare peer segment resolved nowhere, so it
 * was absorbed at an intermediate node) with a `by_name` fallback, and shipped with no test.
 * #510 removes the cause: `src` now carries the full mount, so the ordinary scoped descent
 * resolves the reply and the fallback is dead code. This pins that directly — take the `src`
 * a forward hop produced, send it back as `dst`, and require it to reach the peer — so the
 * removal is covered by behaviour rather than by argument.
 */
void test_grown_src_round_trips() {
    std::printf("grown src is routable back (#510 supersedes the #513 fallback)\n");
    bus_link_impl_t bus;
    p2p_link_t n5;
    bus.peers.emplace_back("n5", &n5);
    recording_link_t out;

    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    router.add_child("net/can/can0", bus);
    router.add_child("net/ws-client/out", out);

    bus.deliver("n5", make_fwd({"net", "ws-client", "out", "sensor"}, {"origin"}));
    check(out.sent.size() == 1, "the peer's frame forwarded");
    if (out.sent.size() != 1) return;
    const auto paths = paths_of(out.sent[0]);
    if (paths.size() != 2) {
        check(false, "the forwarded frame carries dst and src");
        return;
    }

    // Feed that src back as a reply's dst. It must resolve to the peer — through the scoped
    // descent alone, with no bare-name fallback in the registry's way.
    std::vector<std::string_view> back(paths[1].begin(), paths[1].end());
    const auto segs = std::span<const std::string_view>(back);
    std::vector<std::byte> reply;
    {
        std::vector<std::byte> body;
        const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::REPLY)};
        tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
        std::vector<std::byte> dst;
        for (std::string_view s : segs) tr::wire::emit_name(dst, s);
        tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, dst);
        std::vector<std::byte> src;
        tr::wire::emit_name(src, "net");
        tr::wire::emit_name(src, "ws-client");
        tr::wire::emit_name(src, "out");
        tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, src);
        const std::byte payload[1] = {std::byte{0x07}};
        tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload, 1));
        tr::wire::emit_tlv(reply, type_t::FWD, opt_t{.pl = true}, body);
    }
    const std::size_t before = n5.received;
    router.on_frame("net/ws-client/out", reply);
    check(n5.received == before + 1,
          "the reply resolves through the scoped descent and reaches the peer endpoint");
}

// --- the bus NAME is not a routable next-hop (ADR-0073 §3 / RFC-0020, #741) ---

/** @brief The reply's (op, kind, registered error code) triple, or {-1,-1,-1} if unreadable. */
std::array<int, 3> reply_shape(std::span<const std::byte> frame) {
    std::array<int, 3> out{-1, -1, -1};
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD) return out;
    int value_seen = 0;
    for (const auto& child : dec->children) {
        if (child.type == type_t::VALUE && child.payload.size() == 1) {
            out[static_cast<std::size_t>(value_seen == 0 ? 0 : 1)] =
                std::to_integer<int>(child.payload[0]);
            ++value_seen;
        } else if (child.type == type_t::STATUS && !child.children.empty()) {
            const auto& err = child.children[0];
            if (err.type == type_t::ERROR && !err.children.empty() &&
                err.children[0].payload.size() == 2) {
                out[2] = std::to_integer<int>(err.children[0].payload[0]) |
                         (std::to_integer<int>(err.children[0].payload[1]) << 8);
            }
        }
    }
    return out;
}

/**
 * @brief An FWD routed THROUGH a bus link's own NAME is rejected, never broadcast (#741).
 *
 * ADR-0073 §3 / RFC-0020, amending RFC-0004 §B for multi-peer links: a `dst` is a directed
 * route to ONE terminus, so the only routable segments below a multi-peer mount are its
 * peer names. Before the fix this frame fell through to the bus transport's `send()` —
 * which fans out to every open peer, drawing N replies for one request and scrambling FIFO
 * reply correlation (the #409 topology-walk failure). The rejection is ANSWERED — a single
 * directed `kind=ERROR` reply with `tr::path::invalid` (0x0021) — because `src` is intact,
 * matching the terminus resolver's own drop-by-value vs answered split.
 */
void test_bus_name_hop_is_rejected() {
    std::printf("bus NAME + residual is rejected, never broadcast (ADR-0073 S3)\n");
    bus_link_impl_t bus;
    p2p_link_t alice;
    p2p_link_t bob;
    bus.peers.emplace_back("alice", &alice);
    bus.peers.emplace_back("bob", &bob);
    recording_link_t in;

    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    router.add_child("net/ws-server/srv", bus);
    router.add_child("net/ws-client/in", in);

    // The broadcast shape: the bus NAME with a residual below it that names NO peer.
    router.on_frame("net/ws-client/in",
                    make_fwd({"net", "ws-server", "srv", "sensor", "temp"}, {"origin"}));
    check(bus.broadcasts == 0, "the frame never egresses the bus endpoint (no fan-out)");
    check(alice.received == 0 && bob.received == 0, "no peer endpoint sees it either");
    check(in.sent.size() == 1, "the inbound link is ANSWERED - one directed reply, no timeout");
    if (in.sent.size() == 1) {
        const auto shape = reply_shape(in.sent[0]);
        check(shape[0] == static_cast<int>(tr::graph::fwd_op_t::REPLY), "the answer is a REPLY");
        check(shape[1] == 1, "kind=ERROR");
        check(shape[2] == 0x0021, "the registered code is tr::path::invalid (0x0021)");
        const auto paths = paths_of(in.sent[0]);
        check(paths.size() == 2 && paths[0] == std::vector<std::string>{"origin"},
              "the reply rides the request's accumulated src back");
    }

    // Positive control 1: a PEER-directed hop through the same mount still forwards.
    router.on_frame("net/ws-client/in",
                    make_fwd({"net", "ws-server", "srv", "alice", "sensor"}, {"origin"}));
    check(alice.received == 1, "a peer-directed hop still reaches the peer endpoint");
    check(bus.broadcasts == 0, "and still nothing egresses the bus endpoint itself");

    // Positive control 2: a dst naming the mount EXACTLY addresses the connection vertex
    // itself — it terminates HERE and is answered, exactly as before.
    const std::size_t answered = in.sent.size();
    router.on_frame("net/ws-client/in", make_fwd({"net", "ws-server", "srv"}, {"origin"}));
    check(in.sent.size() == answered + 1, "exact-mount addressing still terminates and answers");
    check(bus.broadcasts == 0, "without touching the bus endpoint");
}

/** @brief The same rejection answers a PEER-originated misroute back to THAT peer only. */
void test_bus_name_hop_reject_from_peer() {
    std::printf("bus NAME rejection from a peer routes back to the sender\n");
    bus_link_impl_t bus;
    p2p_link_t alice;
    p2p_link_t bob;
    bus.peers.emplace_back("alice", &alice);
    bus.peers.emplace_back("bob", &bob);

    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    router.add_child("net/ws-server/srv", bus);

    // alice addresses her OWN bus's NAME with a residual naming no peer.
    bus.deliver("alice", make_fwd({"net", "ws-server", "srv", "zzz"}, {"origin"}));
    check(bus.broadcasts == 0, "nothing fans out over the bus");
    check(alice.received == 1, "the sender alone is answered, over her directed endpoint");
    check(bob.received == 0, "the other peer never hears about it");
}

/** @brief A rope-delivering inbound link (ADR-0053 §5) — drives the ROPE arm's reject glue. */
struct rope_in_link_t : tr::net::transport_t {
    std::vector<std::vector<std::byte>> sent; /**< @brief Frames answered back on this link. */
    void send(std::span<const std::byte> f) override { sent.emplace_back(f.begin(), f.end()); }
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte> whole;
        for (const auto s : iov) whole.insert(whole.end(), s.begin(), s.end());
        sent.push_back(std::move(whole));
    }
    [[nodiscard]] bool delivers_ropes() const override { return true; }
    /** @brief Hand the frame up as a rope, as a scatter-gather transport does. */
    void inject(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
};

/** @brief A rope over @p bytes split at every @p cuts boundary (each cut = a link). */
tr::view::rope_t rope_over(std::span<const std::byte> bytes,
                           std::initializer_list<std::size_t> cuts) {
    tr::view::rope_t r;
    std::size_t prev = 0;
    const auto add = [&](std::size_t from, std::size_t to) {
        if (to <= from) return;
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(to - from);
        std::memcpy(seg->bytes.data(), bytes.data() + from, to - from);
        r.append(tr::view::view_t::over(std::move(seg)));
    };
    for (const std::size_t c : cuts) {
        add(prev, c);
        prev = c;
    }
    add(prev, bytes.size());
    return r;
}

/**
 * @brief The ROPE arm's bus-NAME rejection (the on_frame_rope materialize-then-reject
 *        glue) — the one leg the span-arm tests cannot reach, closed per the repo's
 *        untested-guard discipline.
 */
void test_bus_name_hop_rejected_rope_arm() {
    std::printf("bus NAME + residual is rejected on the ROPE arm too\n");
    bus_link_impl_t bus;
    p2p_link_t alice;
    bus.peers.emplace_back("alice", &alice);
    rope_in_link_t in;

    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    router.add_child("net/ws-server/srv", bus);
    router.add_child("net/ws-client/in", in);

    // The broadcast shape, delivered as a MULTI-LINK rope split mid-header (an
    // adversarial boundary the rope cursor must stitch across).
    const std::vector<std::byte> f =
        make_fwd({"net", "ws-server", "srv", "sensor", "temp"}, {"origin"});
    in.inject(rope_over(f, {2, 11}));
    check(bus.broadcasts == 0, "the rope frame never egresses the bus endpoint");
    check(alice.received == 0, "no peer endpoint sees it");
    check(in.sent.size() == 1, "the inbound link is ANSWERED (one directed reply)");
    if (in.sent.size() == 1) {
        const auto shape = reply_shape(in.sent[0]);
        check(shape[0] == static_cast<int>(tr::graph::fwd_op_t::REPLY) && shape[1] == 1 &&
                  shape[2] == 0x0021,
              "REPLY kind=ERROR with tr::path::invalid — same shape as the span arm");
    }

    // Positive control: the same rope split, peer-directed — still forwards.
    const std::vector<std::byte> ok =
        make_fwd({"net", "ws-server", "srv", "alice", "sensor"}, {"origin"});
    in.inject(rope_over(ok, {2, 11}));
    check(alice.received == 1, "a peer-directed rope hop still reaches the peer endpoint");
    check(bus.broadcasts == 0, "and nothing broadcast");
}

/** @brief A route naming the mount EXACTLY still terminates here (ADR-0038 §3a). */
void test_advertise_exact_mount_terminates() {
    std::printf("ADVERTISE naming the mount exactly\n");
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    recording_link_t up;
    recording_link_t down;
    router.add_child("net/ws-client/up", up);
    router.add_child("net/ws-server/down", down);

    std::vector<std::byte> route;
    emit_path(route, {"net", "ws-server", "down"});
    router.on_frame("net/ws-client/up", tr::net::encode_advertise(9, route));
    check(down.sent.empty(),
          "the connection vertex itself is a local address — nothing is relayed onward");
}

}  // namespace

int main() {
    test_module_scoping();
    test_no_prefix_confusion();
    test_scoped_peer_resolution();
    test_peek_segments();
    test_strip_k_and_symmetric_src();
    test_short_dst_is_not_forwardable();
    test_advertise_descends_the_mount();
    test_bus_peer_src_carries_the_mount();
    test_grown_src_round_trips();
    test_bus_name_hop_is_rejected();
    test_bus_name_hop_reject_from_peer();
    test_bus_name_hop_rejected_rope_arm();
    test_advertise_exact_mount_terminates();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
