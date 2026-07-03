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
//       NAME "port" VALUE u16 [, NAME "kind" NAME <kind>][, NAME "addr" NAME <addr>] } }
// — a connection-creation spec (ADR-0027 / reference/05). `kind`/`addr` empty = omitted.
view_t conn_spec(std::string_view type, std::string_view name, conn_role_t role, std::uint16_t port,
                 std::string_view kind = {}, std::string_view addr = {}) {
    std::vector<std::byte> cfg;
    tr::detail::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(role)};
    tr::detail::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    tr::detail::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::detail::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);
    if (!kind.empty()) {
        tr::detail::emit_name(cfg, "kind");
        tr::detail::emit_name(cfg, kind);
    }
    if (!addr.empty()) {
        tr::detail::emit_name(cfg, "addr");
        tr::detail::emit_name(cfg, addr);
    }

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

// FWD{ op, dst=<segs...>, src=<segs...> } with no payload — a remote READ request.
std::vector<std::byte> fwd_read(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::READ)};
    tr::detail::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    std::vector<std::byte> segs;
    for (std::string_view s : dst) tr::detail::emit_name(segs, s);
    tr::detail::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, segs);
    segs.clear();
    for (std::string_view s : src) tr::detail::emit_name(segs, s);
    tr::detail::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, segs);
    std::vector<std::byte> frame;
    tr::detail::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

void test_config_constructed_udp() {
    std::printf("Config-constructed sockets: two nodes over UDP from :children[] SPECs:\n");
    // No provide_link anywhere — both nodes' transports are CONSTRUCTED from the SPEC
    // config (`kind=udp`) and OWNED by their connection vertices. Declaration order
    // matters: each transport_vertex_t (owning the sockets, hence the recv threads)
    // is declared AFTER the router it feeds, so it destructs FIRST.
    graph_t node_a;
    graph_t node_b;
    fwd_router_t router_a(node_a);
    fwd_router_t router_b(node_b);
    transport_vertex_t net_a(node_a, router_a);
    transport_vertex_t net_b(node_b, router_b);

    // A's reply sink is set BEFORE the sockets exist: a config-constructed transport's
    // recv thread is live the moment the SPEC write returns, so router sinks follow
    // the same "configure before frames flow" contract as add_child.
    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    router_a.on_reply([&got](const tr::wire::tlv_t& reply) {
        try {
            got.set_value(tr::wire::encode(reply));
        } catch (...) {
        }
    });

    // B: a stored value at /temp (an encoded VALUE TLV — the reply embeds the LKV
    // verbatim), and a udp LISTENER on a fixed localhost port.
    (void)node_b.register_vertex(*path_t::parse("/temp"), role_t::STORED_VALUE);
    std::vector<std::byte> tv;
    const std::byte tb{0x2A};
    tr::detail::emit_tlv(tv, type_t::VALUE, opt_t{}, std::span<const std::byte>(&tb, 1));
    (void)node_b.write(*path_t::parse("/temp"), owned(tv));
    const auto wb = node_b.write(*path_t::parse("/net:children[]"),
                                 conn_spec("listener", "a", conn_role_t::LISTEN, 47120, "udp"));
    check(wb.has_value(), "B: SPEC{listener, kind=udp, port} constructs the bound socket");
    check(router_b.registry().by_name("a") != nullptr, "B: the socket is wired into the router");

    // A: a udp CLIENT dialing B's port — also purely from config.
    const auto wa =
        node_a.write(*path_t::parse("/net:children[]"),
                     conn_spec("client", "b", conn_role_t::DIAL, 47120, "udp", "127.0.0.1"));
    check(wa.has_value(), "A: SPEC{client, kind=udp, addr, port} constructs the dialing socket");
    const auto* s = net_a.settings_of("b");
    check(s != nullptr && s->kind == "udp" && s->addr == "127.0.0.1" && s->port == 47120,
          "A: the parsed :settings carry kind/addr/port");

    // Construction wrote the link state up — the /net/b vertex value is VALUE{0x01}.
    const auto lv = node_a.read(*path_t::parse("/net/b"));
    bool up = false;
    if (lv) {
        const auto t = tr::wire::view_as_tlv(*lv);
        up = t.has_value() && t->type == type_t::VALUE && t->payload.size() == 1 &&
             t->payload[0] == std::byte{0x01};
    }
    check(up, "A: link state written up at creation (await-able bring-up)");

    // End-to-end: FWD{READ dst=/b/temp} from A crosses A's config-created socket to
    // B's terminus, and the REPLY source-routes back to A's reply sink — B's listener
    // learned A's ephemeral source address from the request datagram.
    router_a.on_frame("self", fwd_read({"b", "temp"}, {"reply-ep"}));
    const bool replied = fut.wait_for(3s) == std::future_status::ready;
    check(replied, "the READ reached B and the REPLY returned over the learned peer");
    if (replied) {
        const std::vector<std::byte> reply_bytes = fut.get();  // owns; decode borrows
        const auto dec = tr::wire::decode(reply_bytes);
        bool has_value = false;
        if (dec && dec->type == type_t::FWD)
            for (const auto& c : dec->children)
                if (c.type == type_t::VALUE && c.payload.size() == 1 &&
                    c.payload[0] == std::byte{0x2A})
                    has_value = true;
        check(has_value, "the REPLY carries B's stored /temp value");
    }
}

void test_provide_link_wins() {
    std::printf("provide_link precedence: a staged link beats config construction:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    tr::net::loopback_channel_t channel;
    net.provide_link("up", channel.a());  // staged first — must win over kind=udp

    const auto w =
        node.write(*path_t::parse("/net:children[]"),
                   conn_spec("client", "up", conn_role_t::DIAL, 47121, "udp", "127.0.0.1"));
    check(w.has_value(), "SPEC with kind=udp still creates the connection");
    check(router.registry().by_name("up") == &channel.a(),
          "the staged provide_link transport is the wired one (no socket constructed)");
    const auto* s = net.settings_of("up");
    check(s != nullptr && s->kind == "udp", "the config kind is still parsed into :settings");
    channel.shutdown();
}

void test_creation_errors() {
    std::printf("Creation errors are clean statuses, never crashes:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);

    // Unknown transport kind => SCHEMA_NOT_FOUND (unsupported catalog entry), no vertex.
    const auto w1 =
        node.write(*path_t::parse("/net:children[]"),
                   conn_spec("client", "x", conn_role_t::DIAL, 47122, "pigeon", "127.0.0.1"));
    check(!w1.has_value() && w1.error() == status_t::SCHEMA_NOT_FOUND,
          "unknown kind => SCHEMA_NOT_FOUND");
    check(node.find(path_t::parse("/net/x")->key()) == nullptr, "no /net/x vertex was created");

    // A udp DIAL without addr (and a LISTEN without port) => TYPE_MISMATCH, no vertex.
    const auto w2 = node.write(*path_t::parse("/net:children[]"),
                               conn_spec("client", "y", conn_role_t::DIAL, 47123, "udp"));
    check(!w2.has_value() && w2.error() == status_t::TYPE_MISMATCH,
          "udp client without addr => TYPE_MISMATCH");
    const auto w3 = node.write(*path_t::parse("/net:children[]"),
                               conn_spec("listener", "z", conn_role_t::LISTEN, 0, "udp"));
    check(!w3.has_value() && w3.error() == status_t::TYPE_MISMATCH,
          "udp listener without port => TYPE_MISMATCH");
    check(node.find(path_t::parse("/net/y")->key()) == nullptr &&
              node.find(path_t::parse("/net/z")->key()) == nullptr,
          "no vertices were created for the failed configs");

    // No kind and no staged link => NOT_FOUND (nothing can carry the bytes).
    const auto w4 = node.write(*path_t::parse("/net:children[]"),
                               conn_spec("client", "w", conn_role_t::DIAL, 8080));
    check(!w4.has_value() && w4.error() == status_t::NOT_FOUND,
          "no kind + no provide_link => NOT_FOUND");
}

}  // namespace

int main() {
    test_create_connection_vertex();
    test_await_link_state();
    test_fwd_still_routes();
    test_local_path_untouched();
    test_config_constructed_udp();
    test_provide_link_wins();
    test_creation_errors();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
