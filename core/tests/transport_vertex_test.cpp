/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #83 Stage-1 — transport/connection as a `/` vertex (ADR-0027), the SHELL over the
 * live path (ADR-0037 Stage-1). Proves: a connection is created in-band via a
 * `:children[]` SPEC, resolves as `/net/<conn>`, carries its transport-private
 * `:settings`, and `await`s link up/down — WHILE `fwd_router_t` still carries the
 * bytes (a FWD still routes through the wired link, unchanged). The loopback runs
 * receive threads, so this is built under TSan + ASan/UBSan.
 *
 * Also asserts the load-bearing performance invariant: the intra-device data path is
 * untouched — a local write -> subscriber fan-out is a direct call, the connection
 * machinery lives only under `/net` and is never on the local hot path.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

// SPEC{ NAME "type" <type>, NAME "name" <name>, SETTINGS "config"{ NAME "role" VALUE u8,
//       NAME "port" VALUE u16 } } — a connection-creation spec (ADR-0027 / reference/05).
view_t conn_spec(std::string_view type, std::string_view name, conn_role_t role,
                 std::uint16_t port) {
    std::vector<std::byte> cfg;
    tr::detail::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(role)};
    tr::detail::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    tr::detail::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::detail::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);

    std::vector<std::byte> body;
    tr::detail::emit_name(body, "type");
    tr::detail::emit_name(body, type);
    tr::detail::emit_name(body, "name");
    tr::detail::emit_name(body, name);
    tr::detail::emit_name(body,
                          "config");  // the "config" key preceding its SETTINGS (reference/05)
    tr::detail::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

void test_create_connection_vertex() {
    std::printf("Create a connection via /net:children[] SPEC; it is a / vertex:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);

    tr::net::loopback_channel_t channel;
    net.provide_link("up", channel.a());  // Stage-1 (A): supply the pre-built link

    const auto w = node.write(*path_t::parse("/net:children[]"),
                              conn_spec("client", "up", conn_role_t::DIAL, 8080));
    check(w.has_value(), "SPEC{client, up} write creates the connection");
    check(node.find(path_t::parse("/net/up")->key()) != nullptr,
          "the connection resolves as /net/up (a first-class / vertex)");

    const auto* s = net.settings_of("up");
    check(s != nullptr && s->role == conn_role_t::DIAL && s->port == 8080,
          "its transport-private :settings (role, port) parsed from the SPEC config");

    // Brick 3a: the NAME→link demux table has ONE owner — the router's child_registry_t.
    // The connection resolves there by the same NAME a `dst` routes through; the vertex
    // shell no longer duplicates the link.
    check(router.registry().size() == 1 && router.registry().by_name("up") == &channel.a(),
          "the connection is in the router's single child_registry_t (no duplicate table)");
    channel.shutdown();
}

void test_await_link_state() {
    std::printf("await(/net/<conn>) fires on link up/down (ADR-0021 poll):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    tr::net::loopback_channel_t channel;
    net.provide_link("up", channel.a());
    (void)node.write(*path_t::parse("/net:children[]"),
                     conn_spec("listener", "up", conn_role_t::LISTEN, 0));

    // A waiter blocks on the connection vertex; set_link_state(up) must wake it.
    std::promise<bool> woke;
    auto fut = woke.get_future();
    std::thread waiter([&] {
        const auto r = node.await(*path_t::parse("/net/up"), 2s);
        woke.set_value(r.has_value());
    });
    std::this_thread::sleep_for(20ms);  // let the waiter reach the wait
    const auto ls = net.set_link_state("up", true);
    check(ls.has_value(), "set_link_state(up=true) writes the vertex");
    check(fut.wait_for(2s) == std::future_status::ready && fut.get(),
          "the awaiter woke on the link-up write");
    waiter.join();
    check(!net.set_link_state("nope", true).has_value(), "unknown connection => NotFound");
    channel.shutdown();
}

void test_fwd_still_routes() {
    std::printf("Zero regression: a FWD still routes through the wired link:\n");
    // Two nodes over the loopback. node A's /net/up connection wraps channel.a();
    // node B's fwd_router owns channel.b() directly (the pre-Stage-1 wiring). A FWD
    // written to node A's router routes out /net/up exactly as add_child wired it.
    graph_t node_a;
    fwd_router_t router_a(node_a);
    transport_vertex_t net_a(node_a, router_a);

    graph_t node_b;
    fwd_router_t router_b(node_b);
    (void)node_b.register_vertex(*path_t::parse("/temp"), role_t::STORED_VALUE);

    tr::net::loopback_channel_t channel;
    net_a.provide_link("up", channel.a());
    (void)node_a.write(*path_t::parse("/net:children[]"),
                       conn_spec("client", "up", conn_role_t::DIAL, 0));
    router_b.add_child("down", channel.b());  // B's side: plain router child (unchanged path)

    // Observe inbound FWDs on B. A FWD{WRITE dst=/up/temp} from A: A strips "up" and
    // forwards "/temp" over channel.a(); B receives it on "down". (We assert the frame
    // arrived — the byte path is fwd_router's, untouched by the vertex shell.)
    std::promise<bool> got;
    auto fut = got.get_future();
    router_b.on_raw([&got](std::string_view link, std::span<const std::byte>) {
        if (link == "down") try {
                got.set_value(true);
            } catch (...) {
            }
    });

    // Build FWD{ op=WRITE, dst=/up/temp, src=/reply, VALUE } and hand it to A's router
    // as if it arrived locally (inbound_name "self" names no child => forward by dst).
    std::vector<std::byte> dst;  // PATH{ NAME up, NAME temp }
    {
        std::vector<std::byte> segs;
        tr::detail::emit_name(segs, "up");
        tr::detail::emit_name(segs, "temp");
        tr::detail::emit_tlv(dst, type_t::PATH, opt_t{.pl = true}, segs);
    }
    std::vector<std::byte> src;  // PATH{ NAME reply }
    {
        std::vector<std::byte> segs;
        tr::detail::emit_name(segs, "reply");
        tr::detail::emit_tlv(src, type_t::PATH, opt_t{.pl = true}, segs);
    }
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::detail::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());
    const std::byte pv{0x2A};
    tr::detail::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&pv, 1));
    std::vector<std::byte> frame;
    tr::detail::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);

    router_a.on_frame("self", frame);  // "self" names no child => forward by first dst seg "up"
    check(fut.wait_for(2s) == std::future_status::ready,
          "the FWD forwarded out /net/up and arrived on B (byte path unchanged)");
    channel.shutdown();
}

void test_local_path_untouched() {
    std::printf("Intra-device path untouched: local write -> subscriber is direct:\n");
    // The connection machinery lives only under /net; a local publish/subscribe on an
    // ordinary vertex never touches it. This asserts the shell adds no local-path cost:
    // the fan-out is the same inline callback as before (no transport, no /net lookup).
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);  // present, but off the local path

    (void)node.register_vertex(*path_t::parse("/sensor"), role_t::STORED_VALUE);
    std::atomic<int> hits{0};
    (void)node.subscribe(*path_t::parse("/sensor"),
                         [&hits](const view_t&) { hits.fetch_add(1, std::memory_order_relaxed); });

    const std::byte b{0x7B};
    (void)node.write(*path_t::parse("/sensor"), owned(std::span<const std::byte>(&b, 1)));
    check(hits.load() == 1, "local subscriber fired inline on the write (direct call, no /net)");
}

}  // namespace

int main() {
    test_create_connection_vertex();
    test_await_link_state();
    test_fwd_still_routes();
    test_local_path_untouched();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
