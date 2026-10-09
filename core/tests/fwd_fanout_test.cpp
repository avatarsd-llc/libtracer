/**
 * @file
 * @brief RFC-0004 / ADR-0035 slice 4 (#136) — the PRODUCER remote fan-out.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A plain `graph.write` to a vertex that has a remote subscriber (bound by an inbound
 * `:subscribers[]` WRITE through fwd_router_t) fans out a delivery back over the
 * subscriber's link with no explicit send call. Assertions:
 *
 *   - a write fans out a full-route `FWD{WRITE, dst=return_route, payload=VALUE}`
 *     to the remote subscriber, byte-exact, routed to the subscribe's `src`;
 *   - that fan-out is ZERO-COPY: a multi-link rope's payload reaches `send(iov)` as spans that
 *     point at the ORIGINAL segment memory, never a gathered copy (the latency-moat guard);
 *   - a transient-local (durability==1) producer LATCHES its current value to a
 *     fresh subscriber on subscribe (one immediate delivery), a volatile one does not;
 *   - an older peer's retired `delivery_compact` opt-in, and the retired ADVERTISE /
 *     COMPACT / HANDLE_NACK frames it may send, are unknown members and types (#1951).
 *
 * Uses an in-memory fake transport (no sockets) for deterministic byte assertions.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

// --- wire builders (canonical bytes via the production emit helpers) ---------
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}
std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}
/**
 * @brief FIELD{ NAME "subscribers", VALUE u8 index_mode=ELEMENT } — the ":subscribers[]" append.
 */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    append(body, b_name("subscribers"));
    append(body, b_value_u8(1));  // index_mode = ELEMENT (append)
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}
/** @brief A VALUE TLV holding the little-endian 16 bits of @p v (the RFC-0022 §3.A
 *         `delivery_policy` shape). */
std::vector<std::byte> b_value_u16(std::uint16_t v) {
    const std::array<std::byte, 2> b{std::byte{static_cast<std::uint8_t>(v & 0xFF)},
                                     std::byte{static_cast<std::uint8_t>(v >> 8)}};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(b));
    return out;
}
/** @brief SUBSCRIBER{ PATH target, SETTINGS qos{ NAME "delivery_compact"  VALUE u8,
 *                                                NAME "delivery_policy"   VALUE u16 } } —
 *         both keys in the ONE SETTINGS child, per RFC-0022 §3.A. The policy member is
 *         omitted when zero, which is the absent (default) case. `delivery_compact` is the
 *         member an OLDER peer sends; this node no longer knows it (#1951). */
std::vector<std::byte> b_subscriber(const std::vector<std::byte>& target, bool compact,
                                    std::uint16_t policy = 0) {
    std::vector<std::byte> body;
    append(body, target);
    std::vector<std::byte> qos;
    append(qos, b_name("delivery_compact"));
    append(qos, b_value_u8(compact ? 1 : 0));
    if (policy != 0) {
        append(qos, b_name("delivery_policy"));
        append(qos, b_value_u16(policy));
    }
    std::vector<std::byte> settings;
    tr::wire::emit_tlv(settings, type_t::SETTINGS, opt_t{.pl = true}, qos);
    append(body, settings);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}
using tr::testing::b_fwd;

/** @brief One captured egress span — its ORIGIN pointer + size (NOT a copy of the bytes). */
struct span_rec_t {
    const std::byte* data;
    std::size_t size;
};

/**
 * @brief An in-memory transport that records every frame send()'s; the router installs its receiver
 *        into the base slot, which the test pokes to inject inbound frames.
 *
 * Overrides the scatter-gather `send(iov)` (NOT just the single-span form) so the router's iovec
 * reaches us INTACT — the base `transport_t::send(iov)` default gathers-into-temp, which would hide
 * the zero-copy egress property behind a flatten. Each send records both the reassembled bytes (for
 * the byte-exact decode assertions) AND the per-span ORIGIN pointers (for the zero-copy address
 * assertion): a delivered rope's payload spans must point at the ORIGINAL segment memory, never a
 * gathered copy. Mutex-guarded so the concurrent (TSan) sub-test can capture sends from the writer
 * thread safely.
 */
class fake_link_t : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        const std::lock_guard lock(m_);
        sent_.emplace_back(frame.begin(), frame.end());
        iovs_.push_back({span_rec_t{frame.data(), frame.size()}});
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        const std::lock_guard lock(m_);
        std::vector<std::byte> flat;
        std::vector<span_rec_t> rec;
        rec.reserve(iov.size());
        for (const auto& s : iov) {
            rec.push_back(span_rec_t{s.data(), s.size()});
            flat.insert(flat.end(), s.begin(), s.end());  // reassemble for the decode checks
        }
        sent_.push_back(std::move(flat));
        iovs_.push_back(std::move(rec));
    }
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
    std::vector<std::vector<std::byte>> drain() {
        const std::lock_guard lock(m_);
        iovs_.clear();
        return std::exchange(sent_, {});
    }
    /** @brief The per-span ORIGIN records of every send since the last drain (parallel to drain()).
     */
    std::vector<std::vector<span_rec_t>> drain_iovs() {
        const std::lock_guard lock(m_);
        sent_.clear();
        return std::exchange(iovs_, {});
    }
    std::size_t count() {
        const std::lock_guard lock(m_);
        return sent_.size();
    }

   private:
    std::mutex m_;
    std::vector<std::vector<std::byte>> sent_;
    std::vector<std::vector<span_rec_t>> iovs_;
};

// --- decode helpers ----------------------------------------------------------
/** @brief The op byte of a decoded FWD (or -1 if it is not a FWD with a leading VALUE op). */
int fwd_op(const tlv_t& f) {
    if (f.type != type_t::FWD || f.children.empty()) return -1;
    const tlv_t& op = f.children[0];
    if (op.type != type_t::VALUE || op.payload.empty()) return -1;
    return std::to_integer<int>(op.payload[0]);
}
/** @brief The trailing VALUE-payload u32 of a FWD{WRITE} delivery (its last VALUE child). */
std::uint32_t fwd_payload_u32(const tlv_t& f) {
    for (auto it = f.children.rbegin(); it != f.children.rend(); ++it)
        if (it->type == type_t::VALUE && it->payload.size() == 4)
            return tr::detail::load_le<std::uint32_t>(it->payload);
    return 0;
}
/** @brief The dst PATH (second child) of a FWD, re-encoded for a byte-exact compare. */
std::vector<std::byte> fwd_dst_bytes(const tlv_t& f) {
    if (f.children.size() < 2 || f.children[1].type != type_t::PATH) return {};
    return tr::wire::encode(f.children[1]);
}

// ---- tests ------------------------------------------------------------------

void test_full_route_fanout() {
    std::printf("Full-route producer fan-out:\n");
    graph_t graph;
    fwd_router_t router(graph);
    fake_link_t link;
    (void)router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    auto v = graph.register_vertex(*p, role_t::STORED_VALUE);  // volatile (durability 0)
    // Subscribe: dst=/sensor/temp, src=/client (the return route).
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), false)));
    link.drain();  // discard the subscribe REPLY

    (void)graph.write(v, make_value(b_value_u32(0xCAFEBABE)));
    const auto sent = link.drain();
    check(sent.size() == 1, "one delivery frame fanned out");
    if (sent.size() == 1) {
        const auto d = tr::wire::decode(sent[0]);
        check(d.has_value() && fwd_op(*d) == static_cast<int>(fwd_op_t::WRITE),
              "delivery is a FWD{WRITE}");
        check(d && fwd_dst_bytes(*d) == b_path({"client"}),
              "dst == the subscribe src (return route)");
        check(d && fwd_payload_u32(*d) == 0xCAFEBABE, "payload VALUE == the written value");
    }

    // A volatile producer does NOT latch: a second, later subscriber sees no immediate
    // delivery (only future writes).
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), false)));
    const auto after = link.drain();
    bool any_write = false;
    for (const auto& f : after) {
        const auto d = tr::wire::decode(f);
        if (d && fwd_op(*d) == static_cast<int>(fwd_op_t::WRITE)) any_write = true;
    }
    check(!any_write, "volatile producer does NOT latch on subscribe");
}

/**
 * @brief ADR-0053 ⑤: a MULTI-LINK stored value fans out to a remote subscriber scatter-gathered
 *        over the rope's links — deliver_remote emits one FWD{WRITE} iov (head + route + empty src
 *        + each value segment) with NO interim flatten.
 *
 * The receiver must reassemble the byte-identical payload from the scattered
 * segments, exactly as a single-link value would.
 */
void test_full_route_fanout_multilink() {
    std::printf("Full-route fan-out of a multi-link value (scatter-gather, no flatten):\n");
    graph_t graph;
    fwd_router_t router(graph);
    fake_link_t link;
    (void)router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    auto v = graph.register_vertex(*p, role_t::STORED_VALUE);
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), false)));
    link.drain();  // discard the subscribe REPLY

    // Split one VALUE TLV across three rope links (zero-copy subviews of one
    // segment) — the fan-out must gather them back into the intact VALUE.
    const std::vector<std::byte> vbytes = b_value_u32(0xFEEDFACE);
    const tr::view::view_t whole = make_value(vbytes);
    const std::size_t n = whole.length;
    const std::size_t c1 = n / 3;
    const std::size_t c2 = 2 * n / 3;
    tr::view::rope_t value;
    value.append(whole.subview(0, c1));
    value.append(whole.subview(c1, c2 - c1));
    value.append(whole.subview(c2, n - c2));
    check(value.link_count() == 3 && value.total_length() == n,
          "value is a genuine 3-link rope over the VALUE TLV");

    (void)graph.write(v, std::move(value));
    const auto sent = link.drain();
    check(sent.size() == 1, "one delivery frame fanned out");
    if (sent.size() == 1) {
        const auto d = tr::wire::decode(sent[0]);
        check(d.has_value() && fwd_op(*d) == static_cast<int>(fwd_op_t::WRITE),
              "delivery is a FWD{WRITE}");
        check(d && fwd_dst_bytes(*d) == b_path({"client"}),
              "dst == the subscribe src (return route)");
        check(d && fwd_payload_u32(*d) == 0xFEEDFACE,
              "scatter-gathered payload reassembles to the intact VALUE");
    }
}

/**
 * @brief The default WRITE fan-out egress is ZERO-COPY: the transport receives the rope's own
 *        segment memory as iovec spans, never a gathered copy (the latency-moat property).
 *
 * `test_full_route_fanout_multilink` proves the payload REASSEMBLES; this proves it is not COPIED
 * on the way out. We record each segment's ORIGIN pointer before the rope is moved into the graph
 * (store moves the refcounted links, it does not copy the bytes), then assert those very pointers
 * reach `transport_t::send(iov)`. A regression that reintroduced a `materialize()`/gather on the
 * egress path would hand the transport a fresh buffer — a different address — and fail here.
 *
 * This is the missing enforcement of an already-decided invariant: the net-plane's no-flatten
 * egress (ADR-0055 "the router performs no decode and no flatten", ADR-0053 ⑤ scatter-gather
 * fan-out, ADR-0038 buffer-lifetime). Those ADRs commit to it; nothing asserted it until now.
 *
 * Scope: the full-route WRITE delivery. The CAN / QUIC transports copy once (CAN re-fragments to
 * 8-byte frames; msquic requires send buffers to outlive the async call) — those are the known,
 * intentional exceptions, not covered by this guard. Orthogonal to the LKV store's own copy leg for
 * BORROWED/small ingress frames (ADR-0042/0060): this value is owning, so store moves it without a
 * copy and any gather here would be the EGRESS path's own — which is the point.
 */
void test_full_route_fanout_zerocopy() {
    std::printf("Full-route fan-out egress is ZERO-COPY (spans point at the original segments):\n");
    graph_t graph;
    fwd_router_t router(graph);
    fake_link_t link;
    (void)router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    auto v = graph.register_vertex(*p, role_t::STORED_VALUE);
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), false)));
    link.drain();  // discard the subscribe REPLY (and its recorded iovs)

    // A 3-link rope over ONE segment (zero-copy subviews). Record each link's ORIGIN pointer BEFORE
    // the move: the stored+delivered rope shares this refcounted segment, so a zero-copy egress
    // must hand these very pointers to the transport.
    const std::vector<std::byte> vbytes = b_value_u32(0x0DDBA11Eu);
    const tr::view::view_t whole = make_value(vbytes);
    const std::size_t n = whole.length;
    const std::size_t c1 = n / 3, c2 = 2 * n / 3;
    const std::byte* a0 = whole.subview(0, c1).bytes().data();
    const std::byte* a1 = whole.subview(c1, c2 - c1).bytes().data();
    const std::byte* a2 = whole.subview(c2, n - c2).bytes().data();
    tr::view::rope_t value;
    value.append(whole.subview(0, c1));
    value.append(whole.subview(c1, c2 - c1));
    value.append(whole.subview(c2, n - c2));
    check(value.link_count() == 3, "value is a genuine 3-link rope");

    (void)graph.write(v, std::move(value));
    const auto iovs = link.drain_iovs();
    check(iovs.size() == 1, "one delivery iovec fanned out");
    if (iovs.size() == 1) {
        const auto& iov = iovs[0];
        // A gather/flatten regression collapses the payload into one owned buffer; the zero-copy
        // path rides the frame as head + route + src + one span PER rope link, so > 1 entry.
        check(iov.size() > 1,
              "delivery is scatter-gathered (multiple iovec entries, not one flat buffer)");
        auto has_origin = [&](const std::byte* a) {
            for (const auto& s : iov)
                if (s.data == a) return true;
            return false;
        };
        check(has_origin(a0) && has_origin(a1) && has_origin(a2),
              "every payload span points at the ORIGINAL segment memory (no gather copy)");
    }
}

void test_transient_local_latch() {
    std::printf("Transient-local latch on subscribe:\n");
    graph_t graph;
    fwd_router_t router(graph);
    fake_link_t link;
    (void)router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    auto v = graph.register_vertex(*p, role_t::STORED_VALUE);
    (void)graph.write(v, make_value(b_value_u32(0x11223344)));  // seed BEFORE subscribe

    // RFC-0022 §3.A: the REMOTE subscriber asks for the latch in its SETTINGS child; the
    // producer carries no durability flag any more.
    link.inject(b_fwd(
        fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
        b_field_subscribers_append(),
        b_subscriber(b_path({"client"}), false, tr::graph::delivery_policy_t::kDurabilityRequest)));
    const auto sent = link.drain();
    int writes = 0;
    std::uint32_t latched = 0;
    for (const auto& f : sent) {
        const auto d = tr::wire::decode(f);
        if (d && fwd_op(*d) == static_cast<int>(fwd_op_t::WRITE)) {
            ++writes;
            latched = fwd_payload_u32(*d);
        }
    }
    check(writes == 1, "exactly one latched delivery on subscribe");
    check(latched == 0x11223344, "latched delivery carries the current LKV");

    // The ablation: the same producer, the same seeded LKV, a second remote subscriber
    // that asks for nothing. It gets NO replay — which before RFC-0022 was impossible to
    // express, because the producer's one flag latched for every subscriber.
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), false)));
    int plain_writes = 0;
    for (const auto& f : link.drain()) {
        const auto d = tr::wire::decode(f);
        if (d && fwd_op(*d) == static_cast<int>(fwd_op_t::WRITE)) ++plain_writes;
    }
    check(plain_writes == 0, "a subscriber that did NOT request durability gets no latch");
}

/**
 * @brief An OLDER peer's COMPACT opt-in is an unknown member (#1951): the subscription is
 *        admitted, and every delivery is the same `FWD{WRITE}` a plain subscriber gets.
 *
 * A peer built before the label tables were deleted still sends
 * `SUBSCRIBER.SETTINGS{ NAME "delivery_compact" VALUE u8 = 1 }`. The member is skipped like any
 * SETTINGS name this node does not know: no refusal, no ADVERTISE, no COMPACT, and no state
 * filed for the flow.
 */
void test_retired_compact_opt_in_is_unknown() {
    std::printf("an older peer's delivery_compact opt-in is an unknown member:\n");
    graph_t graph;
    fwd_router_t router(graph);
    fake_link_t link;
    (void)router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    auto v = graph.register_vertex(*p, role_t::STORED_VALUE);
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), true)));
    const auto reply = link.drain();
    check(reply.size() == 1, "the subscribe is answered once");
    if (reply.size() == 1) {
        const auto d = tr::wire::decode(reply[0]);
        check(d && fwd_op(*d) == static_cast<int>(fwd_op_t::REPLY),
              "the answer is a REPLY: the opt-in was not refused");
    }

    for (const std::uint32_t x : {0xA1A1A1A1u, 0xB2B2B2B2u}) {
        (void)graph.write(v, make_value(b_value_u32(x)));
        const auto sent = link.drain();
        check(sent.size() == 1, "each delivery is ONE frame (no ADVERTISE ahead of it)");
        if (sent.size() != 1) continue;
        const auto d = tr::wire::decode(sent[0]);
        check(d && fwd_op(*d) == static_cast<int>(fwd_op_t::WRITE),
              "the delivery is a FWD{WRITE}, never a COMPACT");
        check(d && fwd_dst_bytes(*d) == b_path({"client"}), "routed home over the return route");
        check(d && fwd_payload_u32(*d) == x, "carrying the written value");
    }
}

/**
 * @brief The retired route-handle frames an older peer may still SEND (ADVERTISE 0x11, COMPACT
 *        0x12, HANDLE_NACK 0x13) are unknown types: each is counted in `malformed_rx` and
 *        answered `ERROR{tr::schema::type_mismatch}`, nothing is applied, and the link keeps
 *        working (#1951).
 */
void test_retired_label_frames_are_unknown() {
    std::printf("an older peer's ADVERTISE / COMPACT / HANDLE_NACK frames are unknown types:\n");
    graph_t graph;
    fwd_router_t router(graph);
    fake_link_t link;
    (void)router.add_child("client", link);
    const auto p = path_t::parse("/sensor/temp");
    auto v = graph.register_vertex(*p, role_t::STORED_VALUE);
    const std::vector<std::byte> before = b_value_u32(0x11111111);
    (void)graph.write(v, make_value(before));

    // The exact shapes an older peer emitted: `{VALUE label(u16), <route | payload>?}`.
    const auto label_frame = [](std::uint8_t type, std::span<const std::byte> tail) {
        std::vector<std::byte> body;
        tr::wire::emit_tlv(body, type_t::VALUE, opt_t{},
                           std::array<std::byte, 2>{std::byte{0x09}, std::byte{0x00}});
        body.insert(body.end(), tail.begin(), tail.end());
        std::vector<std::byte> out;
        tr::wire::emit_tlv(out, static_cast<type_t>(type), opt_t{.pl = true}, body);
        return out;
    };
    const std::vector<std::byte> route = b_path({"sensor", "temp"});
    const std::vector<std::byte> payload = b_value_u32(0xDEADBEEF);
    // Each one is COUNTED and ANSWERED, never dropped in silence (RFC-0032 §6.1): one bare
    // `ERROR{tr::schema::type_mismatch}` back on the link it came in on, and one count in
    // `retired_rx`. RFC-0002 §C's worked bytes: `08 40 06 00 | 01 00 02 00 30 00`.
    const std::array<std::byte, 10> bare_error{
        std::byte{0x08}, std::byte{0x40}, std::byte{0x06}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x30}, std::byte{0x00}};
    const tr::net::router_stats_t stats_before = router.drop_stats();
    std::size_t answered = 0;
    for (const std::uint8_t type : {std::uint8_t{0x11}, std::uint8_t{0x12}, std::uint8_t{0x13}}) {
        link.inject(label_frame(type, type == 0x11   ? std::span<const std::byte>(route)
                                      : type == 0x12 ? std::span<const std::byte>(payload)
                                                     : std::span<const std::byte>{}));
        const auto back = link.drain();
        if (back.size() == 1 &&
            std::equal(back[0].begin(), back[0].end(), bare_error.begin(), bare_error.end()))
            ++answered;
    }
    check(answered == 3,
          "each retired frame draws exactly one bare ERROR{tr::schema::type_mismatch}");
    check(router.drop_stats().retired_rx == stats_before.retired_rx + 3,
          "and each one is counted in retired_rx");
    check(router.drop_stats().malformed_rx == stats_before.malformed_rx,
          "and none is counted as malformed");

    // A bare outer ERROR — the answer itself, looped back — is a report, not a request: no
    // frame goes out and nothing is counted, so two nodes never exchange more than one answer.
    link.inject(std::vector<std::byte>(bare_error.begin(), bare_error.end()));
    check(link.drain().empty(), "a bare ERROR in draws no answer");
    check(router.drop_stats().retired_rx == stats_before.retired_rx + 3,
          "and is not counted as retired");

    const auto lkv = graph.read(*p);
    const tr::view::view_t flat = lkv ? (*lkv)->flatten() : tr::view::view_t{};
    check(lkv && std::equal(flat.bytes().begin(), flat.bytes().end(), before.begin(), before.end()),
          "and nothing was written: the COMPACT payload never reached /sensor/temp");

    // The link is not poisoned: a FWD after them still subscribes and is still delivered to.
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), false)));
    link.drain();
    (void)graph.write(v, make_value(b_value_u32(0x01020304)));
    const auto sent = link.drain();
    check(sent.size() == 1, "a FWD after the retired frames is served as before");
}

}  // namespace

int main() {
    test_full_route_fanout();
    test_full_route_fanout_multilink();
    test_full_route_fanout_zerocopy();
    test_transient_local_latch();
    test_retired_compact_opt_in_is_unknown();
    test_retired_label_frames_are_unknown();
    return tr::testing::summary("fwd_fanout");
}
