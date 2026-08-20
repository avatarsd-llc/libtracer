/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a label the receiver no longer knows is DROPPED and NACK'd, and the NACK
 *        is what puts the flow back together.
 *
 * Compaction (route_label_compact) makes a delivery depend on state the two ends must agree
 * about. Anything that can desynchronise it — a reconnect, a restart, an eviction to stay
 * inside a bounded table — leaves an upstream happily streaming onto a label the downstream has
 * forgotten. The design answer is that a stale label is never dereferenced and never guessed
 * at: the frame is dropped, a `HANDLE_NACK` goes back, and receiving that NACK makes the
 * producer re-advertise (ADR-0035, ADR-0030).
 *
 * Re-advertising IS the self-heal. There is no separate repair protocol, no sequence numbers to
 * reconcile and no teardown handshake — which is why `clear_link` is a safe thing for a
 * transport to call on every (re)connect: the worst case is one dropped frame and one round
 * trip.
 *
 * `clear_link` below stands in for the reconnect. It is the same call a transport makes from
 * its connect/disconnect hook, and calling it for a live or unknown link is deliberately safe.
 *
 * Runs under ctest as `example_route_label_stale`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

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

/** @brief What the consumer observed — deliveries taken, and labels refused. */
struct observer_t {
    std::size_t deliveries = 0;   /**< @brief COMPACTs that resolved to a local terminus. */
    std::size_t stale = 0;        /**< @brief COMPACTs dropped for an unknown label. */
    std::uint16_t last_stale = 0; /**< @brief The last refused label. */
    std::string last_stale_link;  /**< @brief The link it arrived on. */
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

}  // namespace

int main() {
    bool ok = true;

    graph_t graph_p, graph_c;
    tr::net::fwd_router_t producer(graph_p), consumer(graph_c);
    recording_link_t p_to_c, c_to_p;
    if (!producer.add_child("c", p_to_c) || !consumer.add_child("p", c_to_p)) {
        std::fprintf(stderr, "route_label_stale: add_child failed — nothing registered\n");
        return 1;
    }
    (void)graph_c.register_vertex(path_t("/mirror"), role_t::STORED_VALUE);

    observer_t obs;
    consumer.on_compact_delivery(
        [](void* ctx, std::span<const std::byte>, std::span<const std::byte>) {
            ++static_cast<observer_t*>(ctx)->deliveries;
        },
        &obs);
    consumer.on_stale_label(
        [](void* ctx, std::string_view inbound, std::uint16_t label) {
            auto* o = static_cast<observer_t*>(ctx);
            ++o->stale;
            o->last_stale = label;
            o->last_stale_link.assign(inbound);
        },
        &obs);

    // An established flow: advertise, then stream.
    const std::vector<std::byte> route = path_tlv({"mirror"});
    const std::vector<std::byte> payload = value_tlv(0x2A);
    const std::uint16_t label = producer.advertise("c", route);
    if (label == 0 || p_to_c.sent.size() != 1) {
        std::fprintf(stderr, "route_label_stale: the flow would not establish\n");
        return 1;
    }
    consumer.on_frame("p", p_to_c.sent[0]);
    p_to_c.sent.clear();
    producer.send_compact("c", label, payload);
    consumer.on_frame("p", p_to_c.sent.front());
    check(ok, obs.deliveries == 1 && obs.stale == 0, "the flow is established and delivering");

    // The reconnect. The consumer forgets every label on this link; the producer, which never
    // saw it, keeps streaming onto the label it still holds.
    consumer.clear_link("p");
    check(ok, consumer.handles().ingress_count() == 0, "clear_link dropped the link's bindings");

    p_to_c.sent.clear();
    c_to_p.sent.clear();
    producer.send_compact("c", label, payload);
    consumer.on_frame("p", p_to_c.sent.front());
    check(ok, obs.stale == 1 && obs.last_stale == label,
          "the unknown label was REFUSED — not dereferenced, not guessed at");
    check(ok, obs.last_stale_link == "p",
          "and the refusal is reported against the link it came on");
    check(ok, obs.deliveries == 1, "nothing was delivered: a dropped frame, not a misrouted one");

    // The refusal is not silent. A HANDLE_NACK goes back on the same link.
    check(ok, c_to_p.sent.size() == 1, "a HANDLE_NACK went back toward the producer");
    if (c_to_p.sent.size() != 1) return 1;

    // Handing the NACK to the producer is the whole repair: it re-advertises by itself.
    p_to_c.sent.clear();
    producer.on_frame("c", c_to_p.sent[0]);
    check(ok, p_to_c.sent.size() == 1,
          "receiving the NACK made the producer re-advertise — no repair protocol");

    // The re-advertise rebinds the consumer, and the next COMPACT lands again.
    consumer.on_frame("p", p_to_c.sent[0]);
    check(ok, consumer.handles().ingress_count() == 1, "the consumer is bound again");
    p_to_c.sent.clear();
    producer.send_compact("c", label, payload);
    consumer.on_frame("p", p_to_c.sent.front());
    check(ok, obs.deliveries == 2, "and the flow resumed");
    check(ok, obs.stale == 1, "with exactly ONE frame lost to the desynchronisation");

    // Calling the hook for a link that has no label state at all is a no-op, which is what
    // lets a transport call it unconditionally from its connect path.
    consumer.clear_link("no-such-link");
    check(ok, consumer.handles().ingress_count() == 1, "clearing an unknown link changed nothing");

    std::printf("reconnect cost: %zu dropped frame, 1 NACK, 1 re-advertise, %zu deliveries kept\n",
                obs.stale, obs.deliveries);
    return ok ? 0 : 1;
}
