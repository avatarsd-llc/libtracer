/**
 * @file
 * @brief The per-module mount-routing primitives (ADR-0061 / RFC-0014 S2a).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Covers the three mechanisms strip-K routing is built from, ahead of the demux being
 * rewired onto them:
 *   - `child_registry_t::by_segments` — matches a qualified `"<module>/<name>"` key
 *     against raw segment spans WITHOUT building a key (the `allocs=0` forward gate),
 *     and keeps two modules' same-named connections distinct;
 *   - `child_registry_t::resolve_peer` — per-endpoint peer resolution (ADR-0061), so a
 *     peer is reachable only through the module it belongs to, never across buses;
 *   - `peek_fwd_dst_segs` + strip-K `rebuild_fwd_forward` — consuming K leading `dst`
 *     segments and growing `src` by the FULL mount path, which is what keeps a reply
 *     resolvable once names are per-module-scoped (the ADR-0061 erratum).
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
    void send(std::span<const std::byte>) override {}
};

/** @brief A multi-peer transport whose peer table is a fixed name→endpoint list. */
struct bus_link_impl_t : tr::net::transport_t, tr::net::bus_link_t {
    std::vector<std::pair<std::string, p2p_link_t*>> peers;
    void send(std::span<const std::byte>) override {}
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
};

/** @brief `by_segments` over a segment list, spelled the way the demux will call it. */
const child_registry_t::child_t* find(const child_registry_t& reg,
                                      std::initializer_list<std::string_view> segs) {
    const std::vector<std::string_view> v(segs);
    return reg.by_segments(std::span<const std::string_view>(v));
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

/** @brief The peek reads the leading dst segments by offset, bounded. */
void test_peek_segments() {
    std::printf("multi-segment dst peek\n");
    const std::vector<std::byte> frame =
        make_fwd({"net", "ws-client", "foo", "sensor", "temp"}, {"reply"});
    const tr::wire::grammar::span_cursor cur{std::span<const std::byte>(frame)};
    std::array<std::pair<std::size_t, std::size_t>, tr::net::kMountPeekMax> segs{};
    const std::size_t n = tr::net::peek_fwd_dst_segs(cur, segs);
    check(n == tr::net::kMountPeekMax, "peeks up to the mount maximum, no further");
    const auto at = [&](std::size_t i) {
        return std::string(reinterpret_cast<const char*>(frame.data()) + segs[i].first,
                           segs[i].second);
    };
    check(at(0) == "net", "segment 0 is the /net root");
    check(at(1) == "ws-client", "segment 1 is the module");
    check(at(2) == "foo", "segment 2 is the connection name");
    check(at(3) == "sensor", "segment 3 is the first residual segment");

    const std::vector<std::byte> shortf = make_fwd({"net", "can"}, {"reply"});
    const tr::wire::grammar::span_cursor scur{std::span<const std::byte>(shortf)};
    std::array<std::pair<std::size_t, std::size_t>, tr::net::kMountPeekMax> ssegs{};
    check(tr::net::peek_fwd_dst_segs(scur, ssegs) == 2, "a short dst yields only what it has");
}

/** @brief strip-K consumes the mount and grows src by the WHOLE mount (the erratum). */
void test_strip_k_and_symmetric_src() {
    std::printf("strip-K + symmetric return route\n");
    const std::vector<std::byte> frame =
        make_fwd({"net", "ws-client", "foo", "sensor", "temp"}, {"origin"});
    const tr::wire::grammar::span_cursor cur{std::span<const std::byte>(frame)};
    const std::string_view mount[3] = {"net", "ws-server", "up"};
    const auto rb = tr::net::rebuild_fwd_forward(cur, std::span<const std::string_view>(mount), 3);
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
    check(!tr::net::rebuild_fwd_forward(cur, std::span<const std::string_view>(mount), 3),
          "stripping more segments than dst holds is refused, not truncated");
}

}  // namespace

int main() {
    test_module_scoping();
    test_no_prefix_confusion();
    test_scoped_peer_resolution();
    test_peek_segments();
    test_strip_k_and_symmetric_src();
    test_short_dst_is_not_forwardable();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
