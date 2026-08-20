/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a chain is just the one-hop rule applied again, and the MIDDLE hop
 *        stores nothing at all.
 *
 * Three nodes: A holds a child called `b`, B holds a child called `c`, C holds the vertex.
 * A client writes `/b/c/sensor/temp`. There is no route discovery, no flooding, no forwarding
 * information base and no per-flow entry anywhere: each node applies the same test the
 * one-hop examples showed, and the route consumes itself as it travels
 * (RFC-0004 §B, ADR-0040).
 *
 * The check that matters is the negative one. After the write has crossed B, B's routing plane
 * holds exactly what it held before: zero label bindings, zero link shells, and the same
 * receiver-context count. That is the slice-3 stateless-forwarder property, and it is what
 * bounds a forwarder's memory by its TOPOLOGY (how many links it has) rather than by its
 * TRAFFIC (how many flows cross it) — the reason a 16 KB node can be a forwarder at all.
 *
 * Each hop is driven explicitly: the frame a node emits is handed to the next node's
 * `on_frame`. A `loopback_channel_t` or a socket would do the same thing with threads in
 * between (`two_node_fwd` shows that shape); doing it by hand keeps the example synchronous
 * and puts the intermediate bytes where they can be asserted on.
 *
 * Runs under ctest as `example_route_multi_hop`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief A `transport_t` that keeps every frame handed to it — the "wire", made inspectable. */
struct recording_link_t : tr::net::transport_t {
    std::vector<std::vector<std::byte>> sent; /**< @brief Frames emitted on this link, in order. */
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte> flat;
        for (const auto part : iov) flat.insert(flat.end(), part.begin(), part.end());
        sent.push_back(std::move(flat));
    }
};

/** @brief A `PATH` TLV over @p segs — RFC-0018 packed segment records, `opt.PL` clear. */
std::vector<std::byte> path_tlv(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A one-byte `VALUE` TLV carrying @p v. */
std::vector<std::byte> value_tlv(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief `FWD[.pl]{ VALUE op, PATH dst, PATH src, VALUE payload }` (RFC-0004 §B child order). */
std::vector<std::byte> fwd_write(std::initializer_list<std::string_view> dst,
                                 std::initializer_list<std::string_view> src) {
    std::vector<std::byte> body = value_tlv(static_cast<std::uint8_t>(fwd_op_t::WRITE));
    for (const auto& part : {path_tlv(dst), path_tlv(src), value_tlv(0x2A)}) {
        body.insert(body.end(), part.begin(), part.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

}  // namespace

int main() {
    bool ok = true;
    graph_t graph_a, graph_b, graph_c;
    tr::net::fwd_router_t router_a(graph_a), router_b(graph_b), router_c(graph_c);
    recording_link_t a_to_b, b_to_c;

    // A knows B as "b"; B knows C as "c". Neither name means anything to any other node —
    // the client's address /b/c/... is the composition of the two, spelled by whoever holds
    // both mounts, which is what makes the route explicit and loop-free by construction.
    if (!router_a.add_child("b", a_to_b) || !router_b.add_child("c", b_to_c)) {
        std::fprintf(stderr, "route_multi_hop: add_child failed — nothing registered\n");
        return 1;
    }
    (void)graph_c.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    // Whatever B held before the flow — nothing, but read it rather than assume it.
    const std::size_t b_ingress_before = router_b.handles().ingress_count();
    const std::size_t b_links_before = router_b.handles().link_count();
    const std::size_t b_ctx_before = router_b.receiver_ctx_count();

    // Hop 1: A strips "b", grows src by "cli".
    router_a.on_frame("cli", fwd_write({"b", "c", "sensor", "temp"}, {}));
    check(ok, a_to_b.sent.size() == 1, "A forwarded toward B");
    if (a_to_b.sent.size() != 1) return 1;
    check(ok, a_to_b.sent[0] == fwd_write({"c", "sensor", "temp"}, {"cli"}),
          "the frame on the A→B wire: dst=/c/sensor/temp, src=/cli");

    // Hop 2: B applies the SAME rule to the SAME frame — nothing about it is hop-aware.
    router_b.on_frame("a", a_to_b.sent[0]);
    check(ok, b_to_c.sent.size() == 1, "B forwarded toward C");
    if (b_to_c.sent.size() != 1) return 1;
    check(ok, b_to_c.sent[0] == fwd_write({"sensor", "temp"}, {"a", "cli"}),
          "the frame on the B→C wire: dst=/sensor/temp, src=/a/cli — the return route, growing");

    // Hop 3: at C the leading route segment names no child, so C is the terminus and writes.
    router_c.on_frame("b", b_to_c.sent[0]);
    check(ok, graph_c.read(path_t("/sensor/temp")).has_value(),
          "C was the terminus and the write landed three hops from the client");

    // The point of the example: B is exactly as it was.
    check(ok,
          router_b.handles().ingress_count() == b_ingress_before &&
              router_b.handles().egress_count() == 0,
          "the middle hop bound NO label state for the flow it just carried");
    check(ok, router_b.handles().link_count() == b_links_before,
          "and created no per-link shell — its memory is a function of topology, not traffic");
    check(ok, router_b.receiver_ctx_count() == b_ctx_before,
          "and its receiver-context chain did not grow");

    std::printf("3 hops, 1 rule, %zu per-flow binding(s) on the forwarder\n",
                router_b.handles().ingress_count() + router_b.handles().egress_count());
    return ok ? 0 : 1;
}
