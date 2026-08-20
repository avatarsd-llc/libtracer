/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a repeating flow buys its route back: an ADVERTISE binds `label ↔ route`
 *        on ONE link, and every later delivery carries the label instead of the route.
 *
 * "A delivery IS a FWD WRITE" (RFC-0004 §D) is the right model and the wrong bill. Taken
 * literally it makes every streamed sample re-carry its full return route — roughly 16× overhead
 * on a small, high-rate sample (RFC-0004 §E.1). The route-handle is header elision generalized:
 * the producer advertises the route once, the consumer records `label → binding`, and the
 * steady state becomes `COMPACT{ label, payload }` (ADR-0035, `route_handle_t`).
 *
 * Three properties the example makes visible:
 *
 *  - **The label is per LINK, not global.** It is minted by `advertise` against one child name
 *    and means nothing anywhere else; a forwarding hop SWAPS it, MPLS-style, exactly as a CAN
 *    ID is re-resolved against each bus.
 *  - **Only flagged flows pay.** A binding exists because someone advertised. A cold, one-shot
 *    or non-compact flow allocates no entry at all, so the stateless-forwarder property of
 *    route_multi_hop survives alongside this one.
 *  - **The label is minted once per `(link, route)` and then REUSED** (#913): advertising the
 *    same route again re-sends the frame — which is the reconnect self-heal — but grows no
 *    table.
 *
 * The consumer's binding here is a TERMINUS binding: the advertised route names a vertex on the
 * consumer itself, so a COMPACT is written straight to that vertex and reported through
 * `on_compact_delivery`. The delivery is a write, label or no label — the compaction changes
 * the bytes, never the semantics.
 *
 * Runs under ctest as `example_route_label_compact`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/route_handle.hpp"
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

/** @brief What the consumer's compact-delivery sink saw. */
struct compact_sink_t {
    std::size_t deliveries = 0;    /**< @brief COMPACTs that resolved to a local terminus. */
    std::size_t payload_bytes = 0; /**< @brief The last delivery's payload TLV size. */
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

/** @brief The full-route form of the same delivery — what a COMPACT is measured against. */
std::vector<std::byte> fwd_write(std::initializer_list<std::string_view> dst) {
    std::vector<std::byte> body = value_tlv(static_cast<std::uint8_t>(fwd_op_t::WRITE));
    for (const auto& part : {path_tlv(dst), path_tlv({}), value_tlv(0x2A)}) {
        body.insert(body.end(), part.begin(), part.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

}  // namespace

int main() {
    bool ok = true;

    // Producer P and consumer C, one link between them. Each knows the other by its OWN name.
    graph_t graph_p, graph_c;
    tr::net::fwd_router_t producer(graph_p), consumer(graph_c);
    recording_link_t p_to_c, c_to_p;
    if (!producer.add_child("c", p_to_c) || !consumer.add_child("p", c_to_p)) {
        std::fprintf(stderr, "route_label_compact: add_child failed — nothing registered\n");
        return 1;
    }
    (void)graph_c.register_vertex(path_t("/mirror"), role_t::STORED_VALUE);

    compact_sink_t sink;
    consumer.on_compact_delivery(
        [](void* ctx, std::span<const std::byte> /*route*/, std::span<const std::byte> payload) {
            auto* s = static_cast<compact_sink_t*>(ctx);
            ++s->deliveries;
            s->payload_bytes = payload.size();
        },
        &sink);

    // 1. The producer advertises the delivery route ONCE, over the link it calls "c".
    const std::vector<std::byte> route = path_tlv({"mirror"});
    const std::uint16_t label = producer.advertise("c", route);
    check(ok, label != 0, "advertise minted a label (0 would mean: no such link, or table full)");
    check(ok, p_to_c.sent.size() == 1, "and put exactly one ADVERTISE frame on that link");
    if (label == 0 || p_to_c.sent.size() != 1) return 1;

    check(ok, consumer.handles().ingress_count() == 0,
          "the consumer holds nothing before it arrives");
    consumer.on_frame("p", p_to_c.sent[0]);
    check(ok, consumer.handles().ingress_count() == 1,
          "the consumer learned exactly one ingress binding for this (link, label)");

    // 2. The steady state: the route does not ride any more.
    p_to_c.sent.clear();
    const std::vector<std::byte> payload = value_tlv(0x2A);
    producer.send_compact("c", label, payload);
    check(ok, p_to_c.sent.size() == 1, "a COMPACT went out");
    if (p_to_c.sent.size() != 1) return 1;
    const std::size_t compact_bytes = p_to_c.sent[0].size();
    const std::size_t full_bytes = fwd_write({"mirror"}).size();
    check(ok, compact_bytes < full_bytes,
          "and it is smaller than the equivalent full-route FWD{WRITE} — the whole point");

    consumer.on_frame("p", p_to_c.sent[0]);
    check(ok, sink.deliveries == 1, "the label resolved to a LOCAL terminus binding");
    check(ok, graph_c.read(path_t("/mirror")).has_value(),
          "and the delivery was a write, exactly as the full-route form would have been");

    // 3. Re-advertising the same route reuses the label and binds nothing new. That is what
    //    makes the reconnect self-heal (route_label_stale) cheap enough to run unconditionally.
    p_to_c.sent.clear();
    const std::uint16_t again = producer.advertise("c", route);
    check(ok, again == label, "the same (link, route) mints the SAME label — minted once, reused");
    check(ok, p_to_c.sent.size() == 1, "the ADVERTISE frame is re-sent...");
    consumer.on_frame("p", p_to_c.sent[0]);
    check(ok, consumer.handles().ingress_count() == 1, "...and the consumer's table did not grow");

    // 4. The label is scoped to its link. Nothing is bound on any other name.
    check(ok, consumer.handles().link_count() == 1,
          "one link shell: a label means nothing on a link it was not bound for");

    std::printf(
        "COMPACT %zu B vs full-route FWD{WRITE} %zu B (%.0f%% of the frame elided)\n",
        compact_bytes, full_bytes,
        100.0 * static_cast<double>(full_bytes - compact_bytes) / static_cast<double>(full_bytes));
    return ok ? 0 : 1;
}
