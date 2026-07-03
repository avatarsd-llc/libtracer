/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0004 / ADR-0035 slice 3 — multi-hop FWD forwarding + zero-copy `src`
 * accumulation, proven over LIVE transport_ws links. A 3-party chain
 *
 *     client  --ws-->  node A (forwarder)  --ws-->  node B (terminus)
 *
 * carries a FWD{op, dst=/up/sensor, src=/reply-ep} from the client. A resolves the
 * leading dst segment "up" to its transport child (the ws client dialed to B),
 * STRIPS it, PREPENDS to src the NAME by which A addresses the inbound client link
 * ("cli"), and forwards FWD{dst=/sensor, src=/cli/reply-ep} to B. B resolves
 * /sensor locally and source-routes the FWD{REPLY} home A -> client.
 *
 * Assertions (the slice-3 proof):
 *   - B receives dst shrunk to /sensor and src grown to /cli/reply-ep, BYTE-EXACT
 *     (the dst-shrink / src-grow prepend invariant, RFC-0004 §B);
 *   - the client receives kind=RESULT carrying the EXACT stored VALUE (byte-exact
 *     end to end), with src = /sensor (the responder endpoint, provenance);
 *   - a forwarded WRITE updates B's LKV and acks RESULT;
 *   - a non-resolvable dst routes ERROR(NOT_FOUND) back to the client.
 *
 * Event/deadline driven (the ws_interop PORT-handshake + future/deadline idiom):
 * NO fixed sleeps, bounded, clean stop->join->close teardown.
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_ws.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_ws_client;
using tr::net::transport_ws_server;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// --- wire builders (canonical bytes via the production emit helpers) ---------
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::detail::emit_name(out, s);
    return out;
}
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        const std::vector<std::byte> n = b_name(s);
        body.insert(body.end(), n.begin(), n.end());
    }
    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> body;
    std::vector<std::byte> opv;
    const std::byte ob{static_cast<std::uint8_t>(op)};
    tr::detail::emit_tlv(opv, type_t::VALUE, opt_t{}, std::span<const std::byte>(&ob, 1));
    append(body, opv);
    append(body, dst);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

std::uint8_t value_u8(const tlv_t& v) { return tr::detail::load_le<std::uint8_t>(v.payload); }

// A bounded reply mailbox: the client's reply sink pushes the encoded REPLY here;
// the test thread waits with a deadline. No fixed sleeps.
struct mailbox_t {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::vector<std::byte>> q;

    void push(std::vector<std::byte> v) {
        {
            const std::lock_guard lock(m);
            q.push_back(std::move(v));
        }
        cv.notify_all();
    }
    std::optional<std::vector<std::byte>> wait(std::chrono::milliseconds budget) {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, budget, [&] { return !q.empty(); })) return std::nullopt;
        std::vector<std::byte> v = std::move(q.front());
        q.erase(q.begin());
        return v;
    }
};

// What B saw inbound for the last request — captured byte-exact for the
// dst-shrink / src-grow assertion (guarded; B observes on its recv thread).
struct observed_t {
    std::mutex m;
    std::vector<std::byte> dst;
    std::vector<std::byte> src;
    void set(const tlv_t& fwd) {
        const std::lock_guard lock(m);
        if (fwd.children.size() < 3) return;
        dst = tr::wire::encode(fwd.children[1]);
        src = tr::wire::encode(fwd.children[2]);
    }
    std::vector<std::byte> snap_dst() {
        const std::lock_guard lock(m);
        return dst;
    }
    std::vector<std::byte> snap_src() {
        const std::lock_guard lock(m);
        return src;
    }
};

constexpr auto kBudget = 5000ms;

}  // namespace

int main() {
    std::printf("Multi-hop FWD over live transport_ws: client -> A (forward) -> B (terminus)\n");

    // ----- node B: the terminus. /sensor holds a known VALUE; runs a ws server. -
    graph_t graph_b;
    const auto sensor_path = path_t::parse("/sensor");
    tr::graph::vertex_t* vB = *graph_b.register_vertex(*sensor_path, role_t::STORED_VALUE);
    const std::uint32_t kStored = 0xDEADBEEFu;
    (void)graph_b.write(vB, make_value(b_value_u32(kStored)));

    observed_t b_seen;
    fwd_router_t router_b(graph_b);
    router_b.on_inbound([&](std::string_view in, const tlv_t& fwd) {
        if (in == "b-in") b_seen.set(fwd);
    });
    transport_ws_server srv_b(0);
    if (!srv_b.ok()) {
        std::fprintf(stderr, "node B: ws server failed to bind\n");
        return 1;
    }
    router_b.add_child("b-in", srv_b);  // B replies back over whatever link a request came in on

    // ----- node A: the forwarder. ws server (for the client) + ws client (to B). -
    graph_t graph_a;  // A holds no local data — it only forwards.
    fwd_router_t router_a(graph_a);
    transport_ws_server srv_a(0);
    if (!srv_a.ok()) {
        std::fprintf(stderr, "node A: ws server failed to bind\n");
        return 1;
    }
    transport_ws_client a_to_b("127.0.0.1", srv_b.local_port());
    if (!a_to_b.ok()) {
        std::fprintf(stderr, "node A: ws client to B failed to connect\n");
        return 1;
    }
    router_a.add_child("cli", srv_a);  // A's name for the inbound client link (prepended to src)
    router_a.add_child("up", a_to_b);  // A's name for the link to B (the dst segment routed)

    // ----- the client: originator. ws client to A; collects the REPLY. ----------
    graph_t graph_c;
    mailbox_t inbox;
    fwd_router_t router_c(graph_c);
    router_c.on_reply([&](const tlv_t& reply) { inbox.push(tr::wire::encode(reply)); });
    transport_ws_client c_to_a("127.0.0.1", srv_a.local_port());
    if (!c_to_a.ok()) {
        std::fprintf(stderr, "client: ws client to A failed to connect\n");
        return 1;
    }
    router_c.add_child("a", c_to_a);  // replies from A arrive here; /reply-ep terminates locally

    // ===== 1) READ /up/sensor -> byte-exact dst-shrink / src-grow + round-trip ===
    std::printf("READ forwarded A->B; reply source-routed back:\n");
    c_to_a.send(b_fwd(fwd_op_t::READ, b_path({"up", "sensor"}), b_path({"reply-ep"})));
    auto r1 = inbox.wait(kBudget);
    check(r1.has_value(), "client received a REPLY for the forwarded READ");
    if (r1) {
        const auto dec = tr::wire::decode(*r1);
        check(dec.has_value() && dec->type == type_t::FWD, "REPLY decodes as a FWD");
        const tlv_t& r = *dec;
        check(r.children.size() == 5, "REPLY has 5 children (op,dst,src,kind,value)");
        check(value_u8(r.children[0]) == static_cast<std::uint8_t>(fwd_op_t::REPLY), "op == REPLY");
        check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
              "kind == RESULT");
        check(r.children[1].type == type_t::PATH &&
                  tr::wire::equal(r.children[1], *tr::wire::decode(b_path({"reply-ep"}))),
              "REPLY dst fully consumed to /reply-ep at the client");
        check(r.children[2].type == type_t::PATH &&
                  tr::wire::equal(r.children[2], *tr::wire::decode(b_path({"sensor"}))),
              "REPLY src == /sensor (B's responder endpoint — provenance)");
        check(r.children[4].type == type_t::VALUE && r.children[4].payload.size() == 4 &&
                  tr::detail::load_le<std::uint32_t>(r.children[4].payload) == kStored,
              "REPLY payload == the exact stored VALUE u32=0xDEADBEEF (byte-exact end to end)");
    }
    // The dst-shrink / src-grow proof, observed at B.
    check(b_seen.snap_dst() == b_path({"sensor"}),
          "B saw dst shrunk to /sensor (the 'up' segment stripped at A)");
    check(b_seen.snap_src() == b_path({"cli", "reply-ep"}),
          "B saw src grown to /cli/reply-ep (A's inbound-link NAME prepended, byte-exact)");

    // ===== 2) WRITE /up/sensor -> B's LKV updated + RESULT ack ===================
    std::printf("WRITE forwarded A->B; LKV updated + OK ack:\n");
    const std::uint32_t kWritten = 0x12345678u;
    c_to_a.send(b_fwd(fwd_op_t::WRITE, b_path({"up", "sensor"}), b_path({"reply-ep"}),
                      b_value_u32(kWritten)));
    auto r2 = inbox.wait(kBudget);
    check(r2.has_value(), "client received a REPLY for the forwarded WRITE");
    if (r2) {
        const auto dec = tr::wire::decode(*r2);
        const tlv_t& r = *dec;
        check(r.children.size() == 4, "WRITE REPLY has 4 children (no payload)");
        check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
              "WRITE REPLY kind == RESULT");
    }
    const auto stored = graph_b.read(vB);
    check(stored.has_value(), "B /sensor readable after forwarded WRITE");
    if (stored) {
        const auto inner = tr::wire::view_as_tlv(*stored);
        check(inner && inner->type == type_t::VALUE && inner->payload.size() == 4 &&
                  tr::detail::load_le<std::uint32_t>(inner->payload) == kWritten,
              "B /sensor LKV updated to the forwarded value (byte-exact)");
    }

    // ===== 3) non-resolvable dst -> ERROR(NOT_FOUND) routed back =================
    std::printf("READ of a non-resolvable dst -> ERROR(NOT_FOUND) routed home:\n");
    c_to_a.send(b_fwd(fwd_op_t::READ, b_path({"up", "nope"}), b_path({"reply-ep"})));
    auto r3 = inbox.wait(kBudget);
    check(r3.has_value(), "client received a REPLY for the non-resolvable READ");
    if (r3) {
        const auto dec = tr::wire::decode(*r3);
        const tlv_t& r = *dec;
        check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::ERROR),
              "REPLY kind == ERROR for an unknown dst");
        // RFC-0002 §C: STATUS{ ERROR{ VALUE u16 LE registered code } }.
        const bool err_shape = r.children[4].type == type_t::STATUS &&
                               r.children[4].children.size() == 1 &&
                               r.children[4].children[0].type == type_t::ERROR &&
                               !r.children[4].children[0].children.empty() &&
                               r.children[4].children[0].children[0].type == type_t::VALUE;
        check(err_shape && tr::detail::load_le<std::uint16_t>(
                               r.children[4].children[0].children[0].payload) ==
                               0x0020 /*tr::path::not_found*/,
              "ERROR payload == STATUS{ ERROR{ VALUE u16=0x0020 tr::path::not_found } }");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
    // RAII teardown: client transport -> router_c -> A transports -> router_a ->
    // B transport -> router_b, each dtor stop->join->close (bounded poll).
}
