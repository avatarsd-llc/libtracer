/**
 * @file
 * @brief RFC-0004 / ADR-0035 slice 4 (#136) — the PRODUCER remote fan-out.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Where
 * fwd_compact_test drives advertise()/send_compact() explicitly, this test proves
 * the AUTO path: a plain `graph.write` to a vertex that has a remote subscriber
 * (bound by an inbound `:subscribers[]` WRITE through fwd_router_t) fans out a
 * delivery back over the subscriber's link with NO explicit advertise/send call.
 * Assertions:
 *
 *   - a write fans out a full-route `FWD{WRITE, dst=return_route, payload=VALUE}`
 *     to the remote subscriber, byte-exact, routed to the subscribe's `src`;
 *   - that fan-out is ZERO-COPY: a multi-link rope's payload reaches `send(iov)` as spans that
 *     point at the ORIGINAL segment memory, never a gathered copy (the latency-moat guard);
 *   - a transient-local (durability==1) producer LATCHES its current value to a
 *     fresh subscriber on subscribe (one immediate delivery), a volatile one does not;
 *   - a `delivery_compact` subscriber AUTO-promotes: the first delivery emits one
 *     ADVERTISE then a COMPACT; subsequent deliveries emit COMPACT only; the COMPACT
 *     is substantially smaller than the equivalent full-route FWD{WRITE};
 *   - clear_link (a reconnect) makes the next delivery re-advertise (self-heal);
 *   - a concurrent writer thread × a clear_link thread race cleanly (TSan gate).
 *
 * Uses an in-memory fake transport (no sockets) for deterministic byte assertions.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::settings_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

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
    for (std::string_view s : segs) append(body, b_name(s));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
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
/** @brief SUBSCRIBER{ PATH target, SETTINGS qos{ NAME "delivery_compact" VALUE u8 } }. */
std::vector<std::byte> b_subscriber(const std::vector<std::byte>& target, bool compact) {
    std::vector<std::byte> body;
    append(body, target);
    std::vector<std::byte> qos;
    append(qos, b_name("delivery_compact"));
    append(qos, b_value_u8(compact ? 1 : 0));
    std::vector<std::byte> settings;
    tr::wire::emit_tlv(settings, type_t::SETTINGS, opt_t{.pl = true}, qos);
    append(body, settings);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& field = {},
                             const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    append(body, b_value_u8(static_cast<std::uint8_t>(op)));
    append(body, dst);
    if (!field.empty()) append(body, field);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

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
    router.add_child("client", link);

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
    router.add_child("client", link);

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
 * Scope: the DEFAULT full-route WRITE delivery. COMPACT delivery flattens BY DESIGN (encode_compact
 * needs a contiguous payload; single-link is a zero-copy adopt), and the CAN / QUIC transports copy
 * once (CAN re-fragments to 8-byte frames; msquic requires send buffers to outlive the async call)
 * — those are the known, intentional exceptions, not covered by this guard. Orthogonal to the LKV
 * store's own copy leg for BORROWED/small ingress frames (ADR-0042/0060): this value is owning, so
 * store moves it without a copy and any gather here would be the EGRESS path's own — which is the
 * point.
 */
void test_full_route_fanout_zerocopy() {
    std::printf("Full-route fan-out egress is ZERO-COPY (spans point at the original segments):\n");
    graph_t graph;
    fwd_router_t router(graph);
    fake_link_t link;
    router.add_child("client", link);

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
    router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    settings_t s;
    s.durability = 1;  // transient-local
    auto v = graph.register_vertex(*p, role_t::STORED_VALUE, {}, s);
    (void)graph.write(v, make_value(b_value_u32(0x11223344)));  // seed BEFORE subscribe

    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), false)));
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
}

void test_compact_auto_promote() {
    std::printf("delivery_compact auto-promotion:\n");
    graph_t graph;
    fwd_router_t router(graph);
    fake_link_t link;
    router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    auto v = graph.register_vertex(*p, role_t::STORED_VALUE);
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), true)));
    link.drain();  // discard the subscribe REPLY

    (void)graph.write(v, make_value(b_value_u32(0xA1A1A1A1)));
    const auto first = link.drain();
    check(first.size() == 2, "first compact delivery = ADVERTISE + COMPACT");
    std::size_t compact_len = 0;
    if (first.size() == 2) {
        const auto adv = tr::wire::decode(first[0]);
        const auto cmp = tr::wire::decode(first[1]);
        check(adv && adv->type == type_t::ADVERTISE, "first frame is ADVERTISE");
        check(cmp && cmp->type == type_t::COMPACT, "second frame is COMPACT");
        compact_len = first[1].size();
    }

    (void)graph.write(v, make_value(b_value_u32(0xB2B2B2B2)));
    const auto second = link.drain();
    check(second.size() == 1, "subsequent delivery = COMPACT only (no re-advertise)");
    if (second.size() == 1) {
        const auto cmp = tr::wire::decode(second[0]);
        check(cmp && cmp->type == type_t::COMPACT, "steady-state frame is COMPACT");
    }

    // The whole point: a COMPACT is much smaller than the full-route FWD{WRITE} it replaces.
    const std::vector<std::byte> full =
        b_fwd(fwd_op_t::WRITE, b_path({"client"}), b_path({}), {}, b_value_u32(0xB2B2B2B2));
    check(compact_len > 0 && compact_len < full.size(),
          "COMPACT is smaller than the equivalent full-route FWD{WRITE}");

    // Reconnect self-heal: clear_link drops the binding, so the next delivery re-advertises.
    router.clear_link("client");
    (void)graph.write(v, make_value(b_value_u32(0xC3C3C3C3)));
    const auto healed = link.drain();
    check(healed.size() == 2, "post-reconnect delivery re-advertises (ADVERTISE + COMPACT)");
}

void test_concurrent_writer_vs_clear() {
    std::printf("Concurrent writer x clear_link (TSan gate):\n");
    graph_t graph;
    fwd_router_t router(graph);
    fake_link_t link;
    router.add_child("client", link);

    const auto p = path_t::parse("/sensor/temp");
    auto v = graph.register_vertex(*p, role_t::STORED_VALUE);
    link.inject(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"client"}),
                      b_field_subscribers_append(), b_subscriber(b_path({"client"}), true)));
    link.drain();

    std::atomic<bool> go{false};
    std::thread writer([&] {
        while (!go.load()) {
        }
        for (int i = 0; i < 500; ++i)
            (void)graph.write(v, make_value(b_value_u32(0xD0D0'0000u + i)));
    });
    std::thread healer([&] {
        while (!go.load()) {
        }
        for (int i = 0; i < 50; ++i) router.clear_link("client");
    });
    go.store(true);
    writer.join();
    healer.join();
    check(link.count() > 0, "deliveries flowed under concurrent clear_link (no race / crash)");
}

}  // namespace

int main() {
    test_full_route_fanout();
    test_full_route_fanout_multilink();
    test_full_route_fanout_zerocopy();
    test_transient_local_latch();
    test_compact_auto_promote();
    test_concurrent_writer_vs_clear();
    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
