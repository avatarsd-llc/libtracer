/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — nothing remembers the request: the `src` a frame accumulated on the way
 *        out IS the route its `FWD{REPLY}` comes home along.
 *
 * Every hop that stripped a `dst` mount run prepended its own name for the inbound link to `src`
 * (route_dst_is_source_route). By the time the request reaches the terminus, `src` spells the
 * whole way back — in the frame, in the hands of the node that has to answer. So the terminus
 * builds `FWD{REPLY}` with `dst = the request's src` and sends it back over the bidirectional
 * link the request arrived on; each hop retraces its own link the same way, consuming one mount
 * run of that `dst` as it goes. No reply address and no correlation id are needed anywhere
 * (RFC-0004 §D, `CONTEXT.md` §Path-as-route).
 *
 * That is the other half of the statelessness in route_multi_hop. A forwarder with a request
 * table would need an entry per in-flight request, a timeout to reap it, and a policy for what
 * happens when it overflows. Carrying the return route instead costs bytes on the wire and
 * nothing on the hop — and it is exactly the cost the route-handle label plane buys back for
 * flows that repeat (route_label_compact).
 *
 * The reply terminates where `dst` runs out of route segments that name a child: at the origin,
 * whose `on_reply` sink fires (ADR-0055 — rope-native, no decode and no flatten in the router).
 *
 * Runs under ctest as `example_route_reply_home`; returns non-zero on any failed check.
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
using tr::graph::reply_kind_t;
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

/** @brief What the origin's reply sink saw — the terminal `FWD{REPLY}`, flattened once. */
struct reply_sink_t {
    std::size_t count = 0;             /**< @brief Replies that terminated here. */
    std::vector<std::byte> last_frame; /**< @brief The last one's complete bytes. */
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

/** @brief `FWD[.pl]{ VALUE op, PATH dst, PATH src }` — a READ carries no payload. */
std::vector<std::byte> fwd_read(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src) {
    std::vector<std::byte> body = value_tlv(static_cast<std::uint8_t>(fwd_op_t::READ));
    for (const auto& part : {path_tlv(dst), path_tlv(src)}) {
        body.insert(body.end(), part.begin(), part.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief The `PATH` child at index @p i of @p frame, re-encoded — or empty if absent. */
std::vector<std::byte> path_child(std::span<const std::byte> frame, std::size_t i) {
    const auto tlv = tr::wire::decode(frame);
    if (!tlv || tlv->children.size() <= i || tlv->children[i].type != type_t::PATH) return {};
    return tr::wire::encode(tlv->children[i]);
}

}  // namespace

int main() {
    bool ok = true;

    // The origin: it holds the link to A and a sink for replies that get all the way back.
    graph_t graph_o;
    tr::net::fwd_router_t router_o(graph_o);
    recording_link_t o_to_a;

    // The terminus: it holds the vertex and its link back toward the origin.
    graph_t graph_b;
    tr::net::fwd_router_t router_b(graph_b);
    recording_link_t b_to_o;

    if (!router_o.add_child("a", o_to_a) || !router_b.add_child("o", b_to_o)) {
        std::fprintf(stderr, "route_reply_home: add_child failed — nothing registered\n");
        return 1;
    }
    const auto v = graph_b.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)graph_b.write(v, *tr::view::over_bytes(value_tlv(0x2A)));

    reply_sink_t sink;
    router_o.on_reply(
        [](void* ctx, const tr::view::rope_t& reply) {
            auto* s = static_cast<reply_sink_t*>(ctx);
            ++s->count;
            // The router hands the reply over rope-native; a sink that wants contiguous bytes
            // materializes once and keeps the view alive while it reads (ADR-0052/ADR-0055).
            const tr::view::view_t flat = reply.materialize();
            const auto b = flat.bytes();
            s->last_frame.assign(b.begin(), b.end());
        },
        &sink);

    // The app hands its READ to its own router over a link the router calls "app". The frame
    // carries an EMPTY src: the origin has nothing to prepend yet.
    router_o.on_frame("app", fwd_read({"a", "sensor", "temp"}, {}));
    check(ok, o_to_a.sent.size() == 1, "the origin forwarded the READ toward A");
    if (o_to_a.sent.size() != 1) return 1;
    check(ok, path_child(o_to_a.sent[0], 2) == path_tlv({"app"}),
          "and grew src to /app — the one hop the request has taken so far");

    // The terminus resolves /sensor/temp locally and answers. It sends the reply on the link
    // the request ARRIVED on, addressed to the src it was handed.
    router_b.on_frame("o", o_to_a.sent[0]);
    check(ok, b_to_o.sent.size() == 1, "the terminus answered on the link the request came in on");
    if (b_to_o.sent.size() != 1) return 1;
    check(ok, path_child(b_to_o.sent[0], 1) == path_tlv({"app"}),
          "the REPLY's dst IS the request's accumulated src — no correlation table was consulted");
    check(ok, path_child(b_to_o.sent[0], 2) == path_tlv({"sensor", "temp"}),
          "and its src names the responder, which is how the answer carries its provenance");

    // Back at the origin, "app" names no child, so the route is spent and the reply terminates.
    router_o.on_frame("a", b_to_o.sent[0]);
    check(ok, sink.count == 1, "the reply reached the origin's on_reply sink");
    check(ok, !sink.last_frame.empty() && sink.last_frame == b_to_o.sent[0],
          "byte-for-byte the frame the terminus emitted — the origin was one hop away");

    const auto reply = tr::wire::decode(sink.last_frame);
    const bool is_result =
        reply && reply->children.size() >= 4 && reply->children[3].payload.size() == 1 &&
        static_cast<reply_kind_t>(reply->children[3].payload[0]) == reply_kind_t::RESULT;
    check(ok, is_result, "and it is kind=RESULT, carrying the value the terminus read");

    check(ok, router_o.handles().ingress_count() == 0 && router_b.handles().ingress_count() == 0,
          "neither node stored anything to make the round trip work");

    std::printf("request src /app -> reply dst /app; %zu correlation entries anywhere\n",
                router_o.handles().ingress_count() + router_b.handles().ingress_count());
    return ok ? 0 : 1;
}
