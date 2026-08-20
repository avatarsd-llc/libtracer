/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a `FWD`'s `dst` is a SOURCE ROUTE, and every hop rewrites the frame.
 *
 * There is no routing table keyed on destinations here and no per-flow state: the whole route
 * rides in the frame. A hop matches the leading `dst` route segments against its own children,
 * and where one of its names matches it STRIPS that name's whole MOUNT RUN and PREPENDS to `src`
 * the mount run of the link the frame arrived on (RFC-0004 §B, ADR-0040, ADR-0061). `dst`
 * shrinks by exactly what this hop consumed; `src` grows by exactly how to get back here. The
 * frame that leaves is a different frame from the one that arrived, and the difference IS the
 * hop.
 *
 * Both mount runs here are one route segment wide, which is the trivial case and the clearest
 * one to read — route_qualified_mount is the same rule with multi-segment mounts, and it is the
 * general statement. "One segment per hop" is a property of this wiring, never of the rule.
 *
 * That is the whole forwarding rule, and it is why a forwarder is stateless: it never has to
 * remember a request in order to route the answer, because the answer's route is being built
 * on the way out (see route_reply_home for the other half).
 *
 * The check below is byte-exact rather than field-by-field. The frame this node emits is
 * compared against the frame a client one hop closer would have built from scratch — so the
 * assertion is "forwarding produced the canonical bytes", not "forwarding produced something
 * that decodes plausibly".
 *
 * The link is a recording stub rather than a socket or a `loopback_channel_t`: the point is the
 * BYTES a hop emits, and a stub makes them synchronous and inspectable with no threads and no
 * rendezvous. Runs under ctest as `example_route_dst_is_source_route`; returns non-zero on any
 * failed check.
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
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/**
 * @brief A `transport_t` that keeps every frame handed to it — the "wire", made inspectable.
 *
 * A real link would put these bytes on a socket. Keeping them lets the example assert what a
 * hop EMITS, which is the only externally visible thing forwarding does.
 */
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
    graph_t g;
    tr::net::fwd_router_t router(g);

    // This node knows exactly one next hop, under the name "b". That name is LOCAL to this
    // node: nothing downstream has to agree with it, which is what makes the route composable.
    recording_link_t to_b;
    if (!router.add_child("b", to_b)) {
        std::fprintf(stderr, "route_dst_is_source_route: add_child failed — nothing registered\n");
        return 1;
    }

    // A client on the link this node calls "cli" asks to write /b/sensor/temp.
    router.on_frame("cli", fwd_write({"b", "sensor", "temp"}, {}));
    check(ok, to_b.sent.size() == 1, "the frame went out on the child whose mount run dst named");
    if (to_b.sent.size() != 1) return 1;

    // The hop, byte for byte: dst lost "b", src gained "cli", the payload is untouched.
    const std::vector<std::byte> next_hop_would_build = fwd_write({"sensor", "temp"}, {"cli"});
    check(ok, to_b.sent[0] == next_hop_would_build,
          "dst shrank by this child's mount run and src grew by the inbound one — byte-exact");

    // Nothing was remembered. The router holds no per-request state at all: the route left
    // with the frame, and the return route left with it too.
    check(ok, router.handles().ingress_count() == 0 && router.handles().egress_count() == 0,
          "the hop stored NOTHING — a forwarder is stateless by construction");

    // The match is on whole route segments, not on a byte prefix: a `dst` whose first route
    // segment merely starts with a child's name is not that child's traffic.
    to_b.sent.clear();
    router.on_frame("cli", fwd_write({"bb", "sensor"}, {}));
    check(ok, to_b.sent.empty(),
          "\"bb\" is not \"b\" — a prefix in bytes is not a prefix in route segments");

    std::printf("in : dst=/b/sensor/temp src=/        (%zu bytes)\n",
                fwd_write({"b", "sensor", "temp"}, {}).size());
    std::printf("out: dst=/sensor/temp   src=/cli     (%zu bytes)\n", next_hop_would_build.size());
    return ok ? 0 : 1;
}
