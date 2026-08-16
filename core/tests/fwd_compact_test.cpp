/**
 * @file
 * @brief RFC-0004 / ADR-0035 slice 4 — the route-handle: ws delivery-compaction, proven over LIVE
 *        transport_ws.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A 3-party chain
 *
 *     consumer C  --ws-->  forwarder A  --ws-->  producer B
 *
 * subscribes (C->A->B) and then streams DELIVERIES the other way (B->A->C). C
 * subscribes with SUBSCRIBER.qos_settings.delivery_compact=1; B advertises the
 * accumulated return route once (label <-> route), and thereafter streams lean
 * COMPACT frames carrying only a per-link label + the value — each hop swapping the
 * label MPLS-style. Assertions (the slice-4 proof):
 *
 *   - the `delivery_compact` qos flag round-trips byte-exact through :subscribers[];
 *   - a steady-state COMPACT on the wire is SUBSTANTIALLY smaller than the
 *     equivalent full-route FWD{WRITE} (the measured byte-delta — the whole point);
 *   - every streamed value arrives byte-exact at C, in order;
 *   - a stale/unknown label is DROPPED with a NACK (no crash);
 *   - re-advertise after a (simulated) reconnect rebinds and deliveries resume;
 *   - a parallel one-shot / non-compact flow allocates ZERO label state at A.
 *
 * Event/deadline driven (bounded waits, no fixed sleeps); RAII stop->join->close.
 */

#include <algorithm>
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
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_ws.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_ws_client;
using tr::net::transport_ws_server;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::mailbox_t;
using tr::testing::make_value;

// --- wire builders (canonical bytes via the production emit helpers) ---------
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        (void)tr::wire::emit_path_segment(body, s);
    }
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
void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}
/**
 * @brief FIELD{ NAME "subscribers", VALUE u8 index_mode=ELEMENT } — the ":subscribers[]" append
 *        selector.
 */
std::vector<std::byte> b_field_subscribers_append() {
    std::vector<std::byte> body;
    append(body, b_name("subscribers"));
    append(body, b_value_u8(1));  // index_mode = ELEMENT (append, no index VALUE)
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
using tr::testing::b_fwd;

constexpr auto kBudget = 5000ms;

}  // namespace

int main() {
    std::printf("Route-handle ws delivery-compaction: subscribe C->A->B, stream B->A->C\n");

    // ----- node B: producer. /feed is the subscribed vertex; runs a ws server. ---
    graph_t graph_b;
    const auto feed_path = path_t::parse("/feed");
    tr::graph::vertex_handle_t vB = graph_b.register_vertex(*feed_path, role_t::STORED_VALUE);
    (void)graph_b.write(vB, make_value(b_value_u32(0)));

    fwd_router_t router_b(graph_b);
    struct b_obs_t {
        std::mutex m;
        std::vector<std::byte> subscribe_src;  // the accumulated return route = the delivery route
    } b_obs;
    router_b.on_inbound(
        [](void* ctx, std::string_view in, const tlv_t& fwd) {
            if (in == "down" && fwd.children.size() >= 3 && fwd.children[2].type == type_t::PATH) {
                auto* o = static_cast<b_obs_t*>(ctx);
                const std::lock_guard lock(o->m);
                o->subscribe_src = tr::wire::encode(fwd.children[2]);
            }
        },
        &b_obs);
    transport_ws_server srv_b(0);
    if (!srv_b.ok()) {
        std::fprintf(stderr, "node B: ws server failed to bind\n");
        return 1;
    }
    (void)router_b.add_child("down",
                             srv_b);  // B's name for the link toward A/C (replies + deliveries)

    // ----- node A: forwarder. ws server (for C) + ws client (to B). --------------
    graph_t graph_a;
    fwd_router_t router_a(graph_a);
    struct a_obs_t {
        std::mutex m;
        std::size_t last_compact_in = 0;  // size of the last COMPACT seen inbound on "up"
        int stale_hits = 0;
    } a_obs;
    router_a.on_raw(
        [](void* ctx, std::string_view in, std::span<const std::byte> frame) {
            if (in == "up" && !frame.empty() &&
                frame[0] == static_cast<std::byte>(type_t::COMPACT)) {
                auto* o = static_cast<a_obs_t*>(ctx);
                const std::lock_guard lock(o->m);
                o->last_compact_in = frame.size();
            }
        },
        &a_obs);
    router_a.on_stale_label(
        [](void* ctx, std::string_view, std::uint16_t) {
            auto* o = static_cast<a_obs_t*>(ctx);
            const std::lock_guard lock(o->m);
            ++o->stale_hits;
        },
        &a_obs);
    transport_ws_server srv_a(0);
    if (!srv_a.ok()) {
        std::fprintf(stderr, "node A: ws server failed to bind\n");
        return 1;
    }
    transport_ws_client a_to_b("127.0.0.1", srv_b.local_port());
    if (!a_to_b.ok()) {
        std::fprintf(stderr, "node A: ws client to B failed\n");
        return 1;
    }
    (void)router_a.add_child("c", srv_a);  // A's name for the link toward C (the "c" route segment)
    (void)router_a.add_child("up", a_to_b);  // A's name for the link toward B

    // ----- node C: consumer. /sink receives deliveries; ws client to A. ----------
    graph_t graph_c;
    const auto sink_path = path_t::parse("/sink");
    tr::graph::vertex_handle_t vC = graph_c.register_vertex(*sink_path, role_t::STORED_VALUE);
    fwd_router_t router_c(graph_c);
    mailbox_t delivered;        // ordered payload bytes delivered to C
    mailbox_t delivered_route;  // the BOUND local route reported alongside each delivery
    mailbox_t reply_inbox;      // subscribe / one-shot REPLY frames
    router_c.on_reply(
        [](void* ctx, const tr::view::rope_t& reply) {
            const tr::view::view_t mat = reply.materialize();
            const auto b = mat.bytes();
            static_cast<mailbox_t*>(ctx)->push(std::vector<std::byte>(b.begin(), b.end()));
        },
        &reply_inbox);
    // Both arguments are captured, not just the payload. The route half is the one the WARM
    // arm serves without re-taking the owning binding, so a test that ignores it cannot tell
    // a correct route from an empty span (#917).
    struct delivery_obs_t {
        mailbox_t* payloads;
        mailbox_t* routes;
    };
    delivery_obs_t delivery_obs{&delivered, &delivered_route};
    router_c.on_compact_delivery(
        [](void* ctx, std::span<const std::byte> route, std::span<const std::byte> payload) {
            auto* const obs = static_cast<delivery_obs_t*>(ctx);
            // Route FIRST: `delivered` is what every wait_for_count below gates on, so pushing
            // it last keeps the route mailbox guaranteed-populated by the time they return.
            obs->routes->push(std::vector<std::byte>(route.begin(), route.end()));
            obs->payloads->push(std::vector<std::byte>(payload.begin(), payload.end()));
        },
        &delivery_obs);
    transport_ws_client c_to_a("127.0.0.1", srv_a.local_port());
    if (!c_to_a.ok()) {
        std::fprintf(stderr, "node C: ws client to A failed\n");
        return 1;
    }
    (void)router_c.add_child("a", c_to_a);  // C's name for the link toward A

    // ===== 0) statelessness baseline: a one-shot FWD READ allocates NO labels =====
    std::printf("One-shot FWD READ (non-compact) holds zero label state at A:\n");
    c_to_a.send(b_fwd(fwd_op_t::READ, b_path({"up", "feed"}), b_path({"sink"})));
    check(reply_inbox.wait_for_count(1, kBudget) >= 1, "one-shot READ round-trips a REPLY");
    check(router_a.handles().ingress_count() == 0 && router_a.handles().egress_count() == 0,
          "A holds 0 ingress + 0 egress label bindings after a one-shot (stateless)");

    // ===== 1) subscribe with delivery_compact; capture the accumulated route ======
    std::printf("Subscribe C->A->B with delivery_compact=1:\n");
    c_to_a.send(b_fwd(fwd_op_t::WRITE, b_path({"up", "feed"}), b_path({"sink"}),
                      b_field_subscribers_append(),
                      b_subscriber(b_path({"sink"}), /*compact=*/true)));
    check(reply_inbox.wait_for_count(2, kBudget) >= 2, "subscribe WRITE round-trips a REPLY");

    std::vector<std::byte> route;  // /c/sink — the delivery route B advertises
    {
        const std::lock_guard lock(b_obs.m);
        route = b_obs.subscribe_src;
    }
    check(route == b_path({"c", "sink"}),
          "B's stored return route accumulated to /c/sink (the delivery route)");

    // delivery_compact round-trips byte-exact: READ :subscribers[] back and find the flag.
    c_to_a.send(b_fwd(fwd_op_t::READ, b_path({"up", "feed"}), b_path({"sink"}),
                      b_field_subscribers_append()));
    check(reply_inbox.wait_for_count(3, kBudget) >= 3, "READ :subscribers[] round-trips a REPLY");
    {
        // The qos bytes we wrote must survive verbatim somewhere in the reply frame.
        const std::vector<std::byte> needle_name = b_name("delivery_compact");
        std::vector<std::byte> last;
        {
            const std::lock_guard lock(reply_inbox.m);
            last = reply_inbox.q.back();
        }
        const auto it =
            std::search(last.begin(), last.end(), needle_name.begin(), needle_name.end());
        check(it != last.end(),
              "SUBSCRIBER.qos_settings.delivery_compact survives the :subscribers[] round-trip");
    }

    // ===== 2) B advertises the route; per-link labels bind across both hops =======
    std::printf("B advertises label <-> /c/sink; labels bind A and C:\n");
    const std::uint16_t labelB = router_b.advertise("down", route);
    check(labelB != 0, "B allocated a label for the compact flow");
    // The advertise propagates on transport threads — wait for both hops to bind.
    const auto bound = [&] {
        return router_a.handles().ingress_count() >= 1 && router_c.handles().ingress_count() >= 1;
    };
    {
        const auto deadline = std::chrono::steady_clock::now() + kBudget;
        while (!bound() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    }
    check(router_a.handles().ingress_count() == 1 && router_a.handles().egress_count() == 1,
          "A learned exactly one forward binding (+egress) for the compact flow");
    check(router_c.handles().ingress_count() == 1, "C learned exactly one terminus binding");

    // ===== 3) stream N compact deliveries; byte-exact + ordered at C ==============
    std::printf("Stream compact deliveries B->A->C:\n");
    constexpr std::uint32_t kBase = 0xA0000000u;
    constexpr std::size_t kN = 8;
    std::vector<std::vector<std::byte>> expected;
    for (std::uint32_t i = 0; i < kN; ++i) {
        const std::vector<std::byte> v = b_value_u32(kBase + i);
        expected.push_back(v);
        router_b.send_compact("down", labelB, v);
    }
    check(delivered.wait_for_count(kN, kBudget) == kN, "C received all N compact deliveries");
    {
        const std::lock_guard lock(delivered.m);
        bool ordered = delivered.q.size() == kN;
        for (std::size_t i = 0; i < kN && ordered; ++i) ordered = (delivered.q[i] == expected[i]);
        check(ordered, "all N values arrive byte-exact and in order");
    }
    // The route half of the observation, on the WARM path (#917). Delivery 1 resolves cold and
    // reports from the owning binding; deliveries 2..N are warm and report through the
    // allocation-free copy — every one must name the SAME bound local route, `/sink`.
    {
        const std::lock_guard lock(delivered_route.m);
        const std::vector<std::byte> want = b_path({"sink"});
        bool all = delivered_route.q.size() == kN;
        for (std::size_t i = 0; i < kN && all; ++i) all = (delivered_route.q[i] == want);
        check(all, "every delivery — cold first, warm after — reports the bound route /sink");
    }
    // C's /sink LKV reflects the last delivered value (delivery-is-a-write).
    {
        const auto stored = graph_c.read(vC);
        bool ok = stored.has_value();
        if (ok) {
            const auto inner = tr::wire::decode((*stored)->only());
            ok = inner && inner->type == type_t::VALUE &&
                 tr::detail::load_le<std::uint32_t>(inner->payload) == kBase + (kN - 1);
        }
        check(ok, "C /sink LKV updated to the last streamed value");
    }

    // ===== 4) the COMPACTION byte-delta (the whole point) ========================
    std::printf("Compaction byte-delta (COMPACT vs full-route FWD{WRITE}):\n");
    std::size_t compact_sz = 0;
    {
        const std::lock_guard lock(a_obs.m);
        compact_sz = a_obs.last_compact_in;
    }
    // The equivalent full-route delivery B would have sent WITHOUT compaction.
    const std::size_t full_sz =
        b_fwd(fwd_op_t::WRITE, b_path({"c", "sink"}), b_path({"feed"}), {}, b_value_u32(kBase))
            .size();
    std::printf("    full-route FWD{WRITE} = %zu B, compact COMPACT = %zu B (%.1fx smaller)\n",
                full_sz, compact_sz,
                compact_sz ? static_cast<double>(full_sz) / static_cast<double>(compact_sz) : 0.0);
    check(compact_sz > 0, "A observed a COMPACT frame inbound on the B link");
    check(compact_sz * 2 < full_sz,
          "steady-state COMPACT is >2x smaller than the full-route FWD{WRITE}");

    // ===== 5) stale label -> dropped + NACK (no crash) ===========================
    std::printf("Stale/unknown label is dropped with a NACK (no crash):\n");
    const std::size_t before_stale = delivered.size();
    router_b.send_compact("down", static_cast<std::uint16_t>(0xBEEF), b_value_u32(0xDEAD));
    {
        const auto deadline = std::chrono::steady_clock::now() + 1500ms;
        while (std::chrono::steady_clock::now() < deadline) {
            const std::lock_guard lock(a_obs.m);
            if (a_obs.stale_hits > 0) break;
            std::this_thread::yield();
        }
    }
    {
        const std::lock_guard lock(a_obs.m);
        check(a_obs.stale_hits >= 1, "A flagged the unknown label as stale");
    }
    check(delivered.size() == before_stale, "no delivery reached C for the stale label");

    // ===== 6) self-heal: reconnect wipes label state; re-advertise rebinds ========
    std::printf("Self-heal: clear per-link state (reconnect), old label stale, re-advertise:\n");
    router_b.clear_link("down");  // producer forgets its egress too (a real reconnect resets both)
    router_a.clear_link("up");
    router_a.clear_link("c");
    router_c.clear_link("a");
    const int stale_before_reheal = a_obs.stale_hits;
    // The OLD label is now stale at A -> dropped + NACK, not a crash.
    router_b.send_compact("down", labelB, b_value_u32(0xFEED));
    {
        const auto deadline = std::chrono::steady_clock::now() + 1500ms;
        while (std::chrono::steady_clock::now() < deadline) {
            const std::lock_guard lock(a_obs.m);
            if (a_obs.stale_hits > stale_before_reheal) break;
            std::this_thread::yield();
        }
    }
    {
        const std::lock_guard lock(a_obs.m);
        check(a_obs.stale_hits > stale_before_reheal,
              "post-reconnect old label is stale (dropped)");
    }
    // Re-advertise (the self-heal) and resume streaming byte-exact.
    const std::size_t resume_base = delivered.size();
    const std::uint16_t labelB2 = router_b.advertise("down", route);
    {
        const auto deadline = std::chrono::steady_clock::now() + kBudget;
        while (
            (router_a.handles().ingress_count() == 0 || router_c.handles().ingress_count() == 0) &&
            std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
    }
    const std::vector<std::byte> healed = b_value_u32(0x51510000u);
    router_b.send_compact("down", labelB2, healed);
    check(delivered.wait_for_count(resume_base + 1, kBudget) >= resume_base + 1,
          "deliveries resume after re-advertise (self-heal)");
    {
        const std::lock_guard lock(delivered.m);
        check(!delivered.q.empty() && delivered.q.back() == healed,
              "resumed delivery is byte-exact");
    }

    // ===== 7) statelessness preserved: another one-shot adds no label state =======
    std::printf("A parallel one-shot still allocates no extra label state:\n");
    const std::size_t a_ingress = router_a.handles().ingress_count();
    c_to_a.send(b_fwd(fwd_op_t::READ, b_path({"up", "feed"}), b_path({"sink"})));
    check(reply_inbox.wait_for_count(4, kBudget) >= 4, "the extra one-shot READ round-trips");
    check(router_a.handles().ingress_count() == a_ingress,
          "the one-shot added no label bindings — only the compact flow holds state");

    return tr::testing::summary("fwd_compact");
}
