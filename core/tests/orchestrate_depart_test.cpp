/**
 * @file
 * @brief #491 / ADR-0073 Consequences — third-party orchestration end-to-end: an
 *        orchestrator C wires a flow between two OTHER nodes (B producer → A consumer),
 *        then departs; delivery must continue after the departure.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Three REAL in-process nodes (peer symmetry — each is a full graph_t + fwd_router_t +
 * transport_vertex_t with config-constructed ws sockets on localhost; no provide_link,
 * no fake transports anywhere on the recipe path):
 *
 *     A (consumer)  <--ws dial--  B (producer)  <--ws-->  C (orchestrator, departs)
 *
 * The recipe under test is the ADR-0073 §Consequences three-write contract:
 *   1. C, connected to B, creates B's link toward A through B's creator surface (the
 *      `:children[]` SPEC spelling RFC-0014 supersedes-but-keeps, reference/13
 *      "Realisation status") under a NAME C CHOSE (`a-link`);
 *   2. C writes the subscription on B's PRODUCER with the B-rooted target it composed
 *      OFFLINE from the name it minted:
 *      `write /sensor/temp:subscribers[] += SUBSCRIBER{target=/net/ws-client/a-link/sink/val}`;
 *   3. C disconnects — REAL teardown (remove_connection: un-route, retire, close socket).
 *
 * Assertions:
 *   - negative control: BEFORE the subscription write, a producer write reaches A never;
 *   - step 1: the creator write round-trips RESULT and B's dial toward A is live;
 *   - step 2: the subscription write round-trips RESULT;
 *   - step 3 + proof: AFTER C's departure, a producer write on B is delivered to A's
 *     `/sink/val` (delivery-is-a-write), byte-exact.
 *
 * Diagnostics (no semantics): an inbound-FWD observer on C records deliveries that were
 * misrouted to the ORCHESTRATOR instead of toward A, and B's subscriber-slot count is
 * sampled after the departure — so a failure localizes itself (target ignored vs edge
 * evicted with C's session).
 *
 * Event/deadline driven (mailbox + cv), bounded, RAII stop->join->close teardown.
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
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer/transport_ws.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::net::transport_ws_server;
using tr::view::view_t;
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
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}
std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}
/** @brief FIELD{ NAME @p name, VALUE u8 index_mode=ELEMENT } — the ":<name>[]" append. */
std::vector<std::byte> b_field_append(std::string_view name) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, name);
    append(body, b_value_u8(1));  // index_mode = ELEMENT (append)
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FIELD, opt_t{.pl = true}, body);
    return out;
}
/** @brief SUBSCRIBER{ PATH target } — the consumer-initiated subscribe payload (ADR-0026). */
std::vector<std::byte> b_subscriber(const std::vector<std::byte>& target) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, target);
    return out;
}
/** @brief FWD{ op, dst[, field], src[, payload] } — the spec child order (RFC-0004). */
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

/** @brief Wrap @p bytes in an owning single-link view. */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/**
 * @brief A connection-creation SPEC (ADR-0027 / reference/05), optionally carrying the
 *        ws-private `peer_named` / `max_peers` keys (ADR-0043 §5).
 *
 * Built through the SHIPPED `tr::net::conn_spec_t` builder — the same bytes every other
 * creator call site emits, so nothing on the recipe path is a test-local re-encoding.
 */
tr::net::conn_spec_t spec_of(std::string_view type, std::string_view name, conn_role_t role,
                             std::uint16_t port, std::string_view kind = {},
                             std::string_view addr = {}, bool peer_named = false,
                             std::uint32_t max_peers = 0) {
    tr::net::conn_spec_t spec(type, name);
    spec.role(role).port(port);
    if (!kind.empty()) spec.kind(kind);
    if (!addr.empty()) spec.addr(addr);
    if (peer_named) spec.flag("peer_named", true).u32("max_peers", max_peers);
    return spec;
}

/** @brief The SPEC as an owned view — the payload of a LOCAL `/net:children[]` write. */
view_t conn_spec_view(std::string_view type, std::string_view name, conn_role_t role,
                      std::uint16_t port, std::string_view kind = {}, std::string_view addr = {},
                      bool peer_named = false, std::uint32_t max_peers = 0) {
    return spec_of(type, name, role, port, kind, addr, peer_named, max_peers).view();
}

/** @brief The same SPEC as raw TLV bytes (the wire payload of a creator write). */
std::vector<std::byte> conn_spec_bytes(std::string_view type, std::string_view name,
                                       conn_role_t role, std::uint16_t port,
                                       std::string_view kind = {}, std::string_view addr = {}) {
    return spec_of(type, name, role, port, kind, addr).bytes();
}

/**
 * @brief A bounded byte-blob mailbox (cv + deadline; no fixed sleeps): reply frames at C
 *        and delivered values at A both land in one of these.
 */
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
    std::size_t count() {
        const std::lock_guard lock(m);
        return q.size();
    }
};

/** @brief The u8 payload of a VALUE child (or -1). */
int value_u8(const tlv_t& v) {
    if (v.type != type_t::VALUE || v.payload.empty()) return -1;
    return std::to_integer<int>(v.payload[0]);
}

/** @brief Decode a REPLY frame and return its kind byte (or -1 on any shape mismatch). */
int reply_kind(std::span<const std::byte> frame) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD || dec->children.size() < 4) return -1;
    if (value_u8(dec->children[0]) != static_cast<int>(fwd_op_t::REPLY)) return -1;
    return value_u8(dec->children[3]);
}

/** @brief The trailing u32 VALUE payload of a delivered rope (A's consumer sink). */
std::uint32_t rope_value_u32(const tr::view::rope_t& value) {
    const view_t mat = value.materialize();
    const auto dec = tr::wire::decode(mat.bytes());
    if (!dec || dec->type != type_t::VALUE || dec->payload.size() != 4) return 0;
    return tr::detail::load_le<std::uint32_t>(dec->payload);
}

/** @brief Count of FWD{WRITE} frames C received inbound — the misroute diagnostic. */
struct misroute_counter_t {
    std::mutex m;
    std::size_t writes = 0;
    void note(const tlv_t& fwd) {
        if (fwd.children.empty() || value_u8(fwd.children[0]) != static_cast<int>(fwd_op_t::WRITE))
            return;
        const std::lock_guard lock(m);
        ++writes;
    }
    std::size_t snap() {
        const std::lock_guard lock(m);
        return writes;
    }
};

constexpr auto kBudget = 5000ms;
constexpr auto kQuietBudget = 300ms;

/** @brief EPHEMERAL listen ports (#1362): the SPEC asks for 0, the OS picks.
 *
 * These two listeners used to ask for fixed 47510/47511. No test port is ever RESERVED on
 * Linux: the default `net.ipv4.ip_local_port_range` is 32768-60999, so the whole 47xxx
 * block the repo treated as "unclaimed" is inside the range the kernel hands to ordinary
 * client sockets. When some other socket in this network namespace already owns the number,
 * `bind(2)` returns EADDRINUSE — and SO_REUSEADDR only forgives TIME_WAIT, not a live
 * socket — so `slot_server_t::bind_listen` fails, `ok()` stays false, the factory's
 * `make_checked` yields null and the SPEC write reports failure in 0.00 s, which is exactly
 * how #1362 presented. Port 0 removes the assumption instead of scheduling around it: the
 * bound port is read back from `local_port()` below and handed to the dialling side. */
constexpr std::uint16_t kPortEphemeral = 0;

/** @brief Poll @p pred with a deadline (control-plane sync only, never a data assert). */
template <class Pred>
bool poll_until(Pred pred, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

}  // namespace

int main() {
    std::printf(
        "#491 third-party orchestration: C wires B(producer) -> A(consumer), departs;\n"
        "delivery must continue after the departure (ADR-0073 Consequences recipe).\n\n");

    // ===== node A: the CONSUMER =====================================================
    // A full production node: its listener is config-constructed from a local
    // `:children[]` SPEC write — the identical code path an inbound creator write takes.
    graph_t graph_a;
    fwd_router_t router_a(graph_a);
    transport_vertex_t net_a(graph_a, router_a);
    (void)net_a.register_module(std::string(tr::net::kWsServerSuggestedModule), "ws",
                                conn_role_t::LISTEN);
    {
        const auto w = graph_a.write(
            path_t("/net:children[]"),
            conn_spec_view("listener", "l", conn_role_t::LISTEN, kPortEphemeral, "ws"));
        check(w.has_value(), "A: listener /net/ws-server/l created from a SPEC");
    }
    // The consumer endpoint: a delivery is an ordinary write here (RFC-0004 §D). The
    // local callback subscription is observation only — nothing on the recipe path.
    const auto sink_v = graph_a.register_vertex(*path_t::parse("/sink/val"), role_t::STORED_VALUE);
    mailbox_t a_rx;
    (void)graph_a.subscribe(
        path_t("/sink/val"),
        [](void* ctx, const tr::view::rope_t& value) {
            const std::uint32_t v = rope_value_u32(value);
            std::vector<std::byte> b(4);
            tr::detail::store_le<std::uint32_t>(b, v);
            static_cast<mailbox_t*>(ctx)->push(std::move(b));
        },
        &a_rx);
    auto* const srv_a =
        static_cast<transport_ws_server*>(net_a.link_of("net/ws-server/l"));  // kind=ws LISTEN
    check(srv_a != nullptr, "A: the SPEC-constructed listener is reachable via link_of");
    if (srv_a == nullptr) return 1;
    const std::uint16_t a_port = srv_a->local_port();

    // ===== node B: the PRODUCER =====================================================
    graph_t graph_b;
    fwd_router_t router_b(graph_b);
    transport_vertex_t net_b(graph_b, router_b);
    (void)net_b.register_module(std::string(tr::net::kWsClientSuggestedModule), "ws",
                                conn_role_t::DIAL);
    (void)net_b.register_module(std::string(tr::net::kWsServerSuggestedModule), "ws",
                                conn_role_t::LISTEN);
    // The producer. Volatile (durability 0): the ONLY delivery A can ever see is a
    // post-subscription producer write — no subscribe-time latch to confound the
    // after-departure assertion.
    const auto temp_v =
        graph_b.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    // B's ctrl listener — the orchestrator's door. peer_named: each accepted session gets
    // its own return-route identity (ADR-0044 / ADR-0073 §2), the web-UI session shape.
    {
        const auto w = graph_b.write(
            path_t("/net:children[]"),
            conn_spec_view("listener", "ctrl", conn_role_t::LISTEN, kPortEphemeral, "ws", {},
                           /*peer_named=*/true, /*max_peers=*/8));
        check(w.has_value(), "B: ctrl listener /net/ws-server/ctrl created from a SPEC");
    }
    auto* const srv_b = static_cast<transport_ws_server*>(net_b.link_of("net/ws-server/ctrl"));
    check(srv_b != nullptr, "B: the SPEC-constructed ctrl listener is reachable via link_of");
    if (srv_b == nullptr) return 1;
    const std::uint16_t b_port = srv_b->local_port();

    // ===== node C: the ORCHESTRATOR =================================================
    // Scoped: its destruction is part of the departure. C is a full node too — its
    // connection to B is config-constructed from its own local SPEC write.
    std::uint32_t kAfter = 0x0491'C0DE;
    {
        graph_t graph_c;
        fwd_router_t router_c(graph_c);
        transport_vertex_t net_c(graph_c, router_c);
        (void)net_c.register_module(std::string(tr::net::kWsClientSuggestedModule), "ws",
                                    conn_role_t::DIAL);
        mailbox_t c_replies;
        router_c.on_reply(
            [](void* ctx, const tr::view::rope_t& reply) {
                const view_t mat = reply.materialize();
                const auto b = mat.bytes();
                static_cast<mailbox_t*>(ctx)->push(std::vector<std::byte>(b.begin(), b.end()));
            },
            &c_replies);
        misroute_counter_t c_misroutes;
        router_c.on_inbound(
            [](void* ctx, std::string_view, const tlv_t& fwd) {
                static_cast<misroute_counter_t*>(ctx)->note(fwd);
            },
            &c_misroutes);
        {
            const auto w = graph_c.write(
                path_t("/net:children[]"),
                conn_spec_view("client", "b", conn_role_t::DIAL, b_port, "ws", "127.0.0.1"));
            check(w.has_value(), "C: connection to B /net/ws-client/b created from a SPEC");
        }
        tr::net::transport_t* const c_to_b = net_c.link_of("net/ws-client/b");
        check(c_to_b != nullptr, "C: the dialed link to B is live");
        if (c_to_b == nullptr) return 1;

        // ----- negative control: BEFORE the subscription, nothing reaches A ---------
        std::printf("\nNegative control (before the subscription write):\n");
        (void)graph_b.write(temp_v, owned(b_value_u32(0x00C0FFEE)));
        // Fan-out is synchronous on the writer thread; a short bounded quiet window
        // covers the (nonexistent) socket leg.
        check(!a_rx.wait(kQuietBudget).has_value(),
              "a producer write reaches A never (no subscription yet)");

        // ----- recipe step 1: C creates B's link toward A (name C CHOSE) ------------
        std::printf("\nStep 1 — C creates B's dial toward A through the creator surface:\n");
        c_to_b->send(b_fwd(
            fwd_op_t::WRITE, b_path({"net"}), b_path({"reply-ep"}), b_field_append("children"),
            conn_spec_bytes("client", "a-link", conn_role_t::DIAL, a_port, "ws", "127.0.0.1")));
        {
            const auto r = c_replies.wait(kBudget);
            check(r.has_value(), "C received a REPLY for the creator write");
            check(r && reply_kind(*r) == static_cast<int>(reply_kind_t::RESULT),
                  "creator write answered kind == RESULT");
        }
        check(net_b.link_of("net/ws-client/a-link") != nullptr,
              "B now owns a live dial toward A at /net/ws-client/a-link (C's name)");

        // ----- recipe step 2: the subscription, target composed OFFLINE -------------
        std::printf("\nStep 2 — C writes the subscription with the B-rooted target:\n");
        // /net/<module>/<name>/<consumer-path>, from the name C minted in step 1.
        const std::vector<std::byte> target = b_path({"net", "ws-client", "a-link", "sink", "val"});
        c_to_b->send(b_fwd(fwd_op_t::WRITE, b_path({"sensor", "temp"}), b_path({"reply-ep"}),
                           b_field_append("subscribers"), b_subscriber(target)));
        {
            const auto r = c_replies.wait(kBudget);
            check(r.has_value(), "C received a REPLY for the subscription write");
            check(r && reply_kind(*r) == static_cast<int>(reply_kind_t::RESULT),
                  "subscription write answered kind == RESULT");
        }

        // ----- diagnostic: where does a PRE-departure delivery go? ------------------
        std::printf("\nDiagnostic — a pre-departure producer write:\n");
        (void)graph_b.write(temp_v, owned(b_value_u32(0x0BEF0BEF)));
        const bool a_got_pre = a_rx.wait(2000ms).has_value();
        check(a_got_pre, "pre-departure delivery reaches A (the composed target routes)");
        std::printf("    (diagnostic: FWD{WRITE} frames delivered to the ORCHESTRATOR: %zu)\n",
                    c_misroutes.snap());

        // ----- recipe step 3: C departs — REAL teardown -----------------------------
        std::printf("\nStep 3 — C departs (remove_connection + node destruction):\n");
        const auto rm = net_c.remove_connection("net/ws-client/b");
        check(rm.has_value(), "C: remove_connection tears down its link to B");
        // Wait until B has SEEN the departure (its ctrl bus serves no peers) — the
        // departure hooks (link_down/eviction) run on B's recv thread.
        const bool b_saw_departure = poll_until(
            [&] {
                std::size_t peers = 0;
                if (auto* bus = srv_b->bus())
                    bus->enumerate_peers([&peers](std::string_view) { ++peers; });
                return peers == 0;
            },
            kBudget);
        check(b_saw_departure, "B observed the orchestrator's session depart");
    }  // C's graph/router/transport_vertex destruct here — the node is GONE.

    // ----- the proof: delivery continues WITHOUT the orchestrator -------------------
    std::printf("\nAfter departure — B produces, A must receive:\n");
    // Drain anything the pre-departure diagnostic left queued at A.
    while (a_rx.count() > 0) (void)a_rx.wait(0ms);
    (void)graph_b.write(temp_v, owned(b_value_u32(kAfter)));
    const auto delivered = a_rx.wait(kBudget);
    check(delivered.has_value(),
          "A received the producer's update AFTER the orchestrator departed");
    if (delivered) {
        check(delivered->size() == 4 && tr::detail::load_le<std::uint32_t>(*delivered) == kAfter,
              "the delivered value is byte-exact (u32 == 0x0491C0DE)");
    }
    // The subscription row is B's state: it must have SURVIVED the departure.
    {
        const auto subs = graph_b.read_subscribers(temp_v, {});
        check(subs.has_value() && !subs->empty(),
              "B's :subscribers[] row survived the orchestrator's departure");
    }
    (void)sink_v;

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
    // RAII teardown: net_b/router_b/graph_b, then net_a/router_a/graph_a — each owned
    // socket stops delivering before the router it feeds is gone (the documented order).
}
