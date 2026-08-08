/**
 * @file
 * @brief #83 Stage-1 — transport/connection as a `/` vertex (ADR-0027), the SHELL over the live
 *        path (ADR-0037 Stage-1).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Proves: a connection is created in-band via a
 * `:children[]` SPEC, resolves as `/net/<conn>`, carries its transport-private
 * `:settings`, and `await`s link up/down — WHILE `fwd_router_t` still carries the
 * bytes (a FWD still routes through the wired link, unchanged). The loopback runs
 * receive threads, so this is built under TSan + ASan/UBSan.
 *
 * Also asserts the load-bearing performance invariant: the intra-device data path is
 * untouched — a local write -> subscriber fan-out is a direct call, the connection
 * machinery lives only under `/net` and is never on the local hot path.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory_resource>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"

namespace {

using namespace std::chrono_literals;
namespace ws = tr::net::ws;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::link_state_t;
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

/**
 * @brief A connection-creation spec (ADR-0027 / reference/05). `kind`/`addr` empty = omitted.
 *
 * SPEC{ NAME "type" <type>, NAME "name" <name>, SETTINGS "config"{ NAME "role" VALUE u8,
 *       NAME "port" VALUE u16 [, NAME "kind" NAME <kind>][, NAME "addr" NAME <addr>] } }
 */
view_t conn_spec(std::string_view type, std::string_view name, conn_role_t role, std::uint16_t port,
                 std::string_view kind = {}, std::string_view addr = {}, std::uint32_t backoff = 0,
                 std::uint32_t connect_timeout = 0, std::uint32_t max_frame = 0) {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(role)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    tr::wire::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);
    if (!kind.empty()) {
        tr::wire::emit_name(cfg, "kind");
        tr::wire::emit_name(cfg, kind);
    }
    if (!addr.empty()) {
        tr::wire::emit_name(cfg, "addr");
        tr::wire::emit_name(cfg, addr);
    }
    const auto emit_u32 = [&cfg](std::string_view key, std::uint32_t val) {
        tr::wire::emit_name(cfg, key);
        std::vector<std::byte> vb(4);
        tr::detail::store_le(vb, val, 4);
        tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, vb);
    };
    if (backoff != 0) emit_u32("backoff", backoff);
    if (connect_timeout != 0) emit_u32("connect_timeout", connect_timeout);
    if (max_frame != 0) emit_u32("max_frame", max_frame);

    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    tr::wire::emit_name(body,
                        "config");  // the "config" key preceding its SETTINGS (reference/05)
    tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

/**
 * @brief This test application's module declarations (ADR-0073 §4: declared-only).
 *
 * The library auto-registers NO module names — linking a built-in transport registers
 * nothing. Every module segment is minted HERE, by the application (this test), under
 * the built-ins' suggested names.
 */
void declare_builtin_modules(transport_vertex_t& net) {
    (void)net.register_module(std::string(tr::net::kUdpClientSuggestedModule), "udp",
                              conn_role_t::DIAL);
    (void)net.register_module(std::string(tr::net::kUdpServerSuggestedModule), "udp",
                              conn_role_t::LISTEN);
    (void)net.register_module(std::string(tr::net::kWsClientSuggestedModule), "ws",
                              conn_role_t::DIAL);
    (void)net.register_module(std::string(tr::net::kWsServerSuggestedModule), "ws",
                              conn_role_t::LISTEN);
}

void test_create_connection_vertex() {
    std::printf("Create a connection via /net:children[] SPEC; it is a / vertex:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);

    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "up", channel.a());  // Stage-1 (A): supply the pre-built link

    const auto w =
        node.write(path_t("/net:children[]"), conn_spec("client", "up", conn_role_t::DIAL, 8080));
    check(w.has_value(), "SPEC{client, up} write creates the connection");
    check(node.find(path_t::parse("/net/ws-client/up")->key()).has_value(),
          "the connection resolves as /net/ws-client/up (a first-class / vertex)");

    const auto* s = net.settings_of("net/ws-client/up");
    check(s != nullptr && s->role == conn_role_t::DIAL && s->port == 8080,
          "its transport-private :settings (role, port) parsed from the SPEC config");

    // Brick 3a: the NAME→link demux table has ONE owner — the router's child_registry_t.
    // The connection resolves there by the same NAME a `dst` routes through; the vertex
    // shell no longer duplicates the link.
    check(router.registry().size() == 1 &&
              router.registry().by_name("net/ws-client/up") == &channel.a(),
          "the connection is in the router's single child_registry_t (no duplicate table)");
    channel.shutdown();
}

void test_await_link_state() {
    std::printf("await(/net/<conn>) fires on link up/down (ADR-0021 poll):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "up", channel.a());
    (void)node.write(path_t("/net:children[]"),
                     conn_spec("listener", "up", conn_role_t::LISTEN, 0));

    // A waiter blocks on the connection vertex; set_link_state(up) must wake it.
    std::promise<bool> woke;
    auto fut = woke.get_future();
    std::thread waiter([&] {
        const auto r = node.await(path_t("/net/ws-client/up"), 2s);
        woke.set_value(r.has_value());
    });
    std::this_thread::sleep_for(20ms);  // let the waiter reach the wait
    const auto ls = net.set_link_state("net/ws-client/up", link_state_t::UP);
    check(ls.has_value(), "set_link_state(UP) writes the vertex");
    check(fut.wait_for(2s) == std::future_status::ready && fut.get(),
          "the awaiter woke on the link-up write");
    waiter.join();
    check(!net.set_link_state("net/ws-client/nope", link_state_t::UP).has_value(),
          "unknown connection => NotFound");
    channel.shutdown();
}

/** @brief The 1-byte payload of a connection vertex's stored link-liveness VALUE TLV. */
std::uint8_t read_link_state_byte(graph_t& g, std::string_view path) {
    const auto h = g.find(path_t::parse(path)->key());
    if (!h) return 0xFF;
    const auto v = g.read(*h);
    if (!v) return 0xFF;
    const auto bytes = (*v)->materialize().bytes();
    // VALUE TLV of a 1-byte payload — the payload is the last byte.
    return bytes.empty() ? 0xFF : static_cast<std::uint8_t>(bytes.back());
}

void test_liveness_enum_value() {
    std::printf("Link-liveness value is the RFC-0014 6-state enum (S1):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "l", channel.a());
    (void)node.write(path_t("/net:children[]"), conn_spec("client", "l", conn_role_t::DIAL, 0));

    // Each manual state publishes its own byte (table order 0..5), read back from the LKV.
    (void)net.set_link_state("net/ws-client/l", link_state_t::DORMANT);
    check(read_link_state_byte(node, "/net/ws-client/l") == 0,
          "DORMANT publishes 0x00 (the old 'down')");
    (void)net.set_link_state("net/ws-client/l", link_state_t::RECONNECTING);
    check(read_link_state_byte(node, "/net/ws-client/l") == 2, "RECONNECTING publishes 0x02");
    (void)net.set_link_state("net/ws-client/l", link_state_t::UP);
    check(read_link_state_byte(node, "/net/ws-client/l") == 3, "UP publishes 0x03");
    channel.shutdown();
}

void test_constructed_link_reports_role_state() {
    std::printf("A config-constructed socket self-reports UP (DIAL) / LISTENING (LISTEN):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_builtin_modules(net);  // ADR-0073 §4: the application mints the module names

    // A udp LISTENER binds at creation => it reports LISTENING (0x04), not UP.
    const auto wl = node.write(path_t("/net:children[]"),
                               conn_spec("listener", "srv", conn_role_t::LISTEN, 47131, "udp"));
    check(wl.has_value(), "SPEC{listener, kind=udp} constructs the bound socket");
    check(read_link_state_byte(node, "/net/udp-server/srv") == 4,
          "a constructed LISTEN reports LISTENING (0x04)");

    // A udp CLIENT is UP the moment its socket is constructed (0x03).
    const auto wc =
        node.write(path_t("/net:children[]"),
                   conn_spec("client", "cli", conn_role_t::DIAL, 47131, "udp", "127.0.0.1"));
    check(wc.has_value(), "SPEC{client, kind=udp} constructs the socket");
    check(read_link_state_byte(node, "/net/udp-client/cli") == 3,
          "a constructed DIAL reports UP (0x03)");
}

void test_backoff_connect_timeout_parsed() {
    std::printf("conn_settings_t parses backoff / connect_timeout (dormant until S5):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "c", channel.a());
    (void)node.write(path_t("/net:children[]"),
                     conn_spec("client", "c", conn_role_t::DIAL, 0, {}, {}, 250, 3000));
    const auto* s = net.settings_of("net/ws-client/c");
    check(s != nullptr && s->backoff_ms == 250 && s->connect_timeout_ms == 3000,
          "backoff=250 / connect_timeout=3000 parsed into the transport-private settings");
    channel.shutdown();
}

void test_fwd_still_routes() {
    std::printf("Zero regression: a FWD still routes through the wired link:\n");
    // Two nodes over the loopback. node A's /net/ws-client/up connection wraps channel.a();
    // node B's fwd_router owns channel.b() directly (the pre-Stage-1 wiring). A FWD
    // written to node A's router routes out /net/ws-client/up exactly as add_child wired it.
    graph_t node_a;
    fwd_router_t router_a(node_a);
    transport_vertex_t net_a(node_a, router_a);

    graph_t node_b;
    fwd_router_t router_b(node_b);
    (void)node_b.register_vertex(path_t("/temp"), role_t::STORED_VALUE);

    tr::net::loopback_channel_t channel;
    net_a.provide_link("ws-client", "up", channel.a());
    (void)node_a.write(path_t("/net:children[]"), conn_spec("client", "up", conn_role_t::DIAL, 0));
    router_b.add_child("down", channel.b());  // B's side: plain router child (unchanged path)

    // Observe inbound FWDs on B. A FWD{WRITE dst=/up/temp} from A: A strips "up" and
    // forwards "/temp" over channel.a(); B receives it on "down". (We assert the frame
    // arrived — the byte path is fwd_router's, untouched by the vertex shell.)
    std::promise<bool> got;
    auto fut = got.get_future();
    router_b.on_raw(
        [](void* ctx, std::string_view link, std::span<const std::byte>) {
            if (link == "down") try {
                    static_cast<std::promise<bool>*>(ctx)->set_value(true);
                } catch (...) {
                }
        },
        &got);

    // Build FWD{ op=WRITE, dst=/up/temp, src=/reply, VALUE } and hand it to A's router
    // as if it arrived locally (inbound_name "self" names no child => forward by dst).
    std::vector<std::byte> dst;  // PATH{ NAME net, NAME ws-client, NAME up, NAME temp }
    {
        std::vector<std::byte> segs;
        tr::wire::emit_name(segs, "net");
        tr::wire::emit_name(segs, "ws-client");
        tr::wire::emit_name(segs, "up");
        tr::wire::emit_name(segs, "temp");
        tr::wire::emit_tlv(dst, type_t::PATH, opt_t{.pl = true}, segs);
    }
    std::vector<std::byte> src;  // PATH{ NAME reply }
    {
        std::vector<std::byte> segs;
        tr::wire::emit_name(segs, "reply");
        tr::wire::emit_tlv(src, type_t::PATH, opt_t{.pl = true}, segs);
    }
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());
    const std::byte pv{0x2A};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&pv, 1));
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);

    router_a.on_frame("self", frame);  // "self" names no child => forward via the mount
    check(fut.wait_for(2s) == std::future_status::ready,
          "the FWD forwarded out /net/ws-client/up and arrived on B (byte path unchanged)");
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

    (void)node.register_vertex(path_t("/sensor"), role_t::STORED_VALUE);
    std::atomic<int> hits{0};
    auto on_sensor = [&hits](const tr::view::rope_t&) {
        hits.fetch_add(1, std::memory_order_relaxed);
    };
    (void)node.subscribe(path_t("/sensor"), on_sensor);

    const std::byte b{0x7B};
    (void)node.write(path_t("/sensor"), owned(std::span<const std::byte>(&b, 1)));
    check(hits.load() == 1, "local subscriber fired inline on the write (direct call, no /net)");
}

/** @brief FWD{ op, dst=<segs...>, src=<segs...> } with no payload — a remote READ request. */
std::vector<std::byte> fwd_read(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::READ)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    std::vector<std::byte> segs;
    for (std::string_view s : dst) tr::wire::emit_name(segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, segs);
    segs.clear();
    for (std::string_view s : src) tr::wire::emit_name(segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, segs);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
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
    declare_builtin_modules(net_a);  // ADR-0073 §4: the application mints the module names
    declare_builtin_modules(net_b);

    // A's reply sink is set BEFORE the sockets exist: a config-constructed transport's
    // recv thread is live the moment the SPEC write returns, so router sinks follow
    // the same "configure before frames flow" contract as add_child.
    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    router_a.on_reply(
        [](void* ctx, const tr::view::rope_t& reply) {
            try {
                const tr::view::view_t mat = reply.materialize();
                const auto b = mat.bytes();
                static_cast<std::promise<std::vector<std::byte>>*>(ctx)->set_value(
                    std::vector<std::byte>(b.begin(), b.end()));
            } catch (...) {
            }
        },
        &got);

    // B: a stored value at /temp (an encoded VALUE TLV — the reply embeds the LKV
    // verbatim), and a udp LISTENER on a fixed localhost port.
    (void)node_b.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
    std::vector<std::byte> tv;
    const std::byte tb{0x2A};
    tr::wire::emit_tlv(tv, type_t::VALUE, opt_t{}, std::span<const std::byte>(&tb, 1));
    (void)node_b.write(path_t("/temp"), owned(tv));
    const auto wb = node_b.write(path_t("/net:children[]"),
                                 conn_spec("listener", "a", conn_role_t::LISTEN, 47120, "udp"));
    check(wb.has_value(), "B: SPEC{listener, kind=udp, port} constructs the bound socket");
    check(router_b.registry().by_name("net/udp-server/a") != nullptr,
          "B: the socket is wired into the router");

    // A: a udp CLIENT dialing B's port — also purely from config.
    const auto wa =
        node_a.write(path_t("/net:children[]"),
                     conn_spec("client", "b", conn_role_t::DIAL, 47120, "udp", "127.0.0.1"));
    check(wa.has_value(), "A: SPEC{client, kind=udp, addr, port} constructs the dialing socket");
    const auto* s = net_a.settings_of("net/udp-client/b");
    check(s != nullptr && s->kind == "udp" && s->addr == "127.0.0.1" && s->port == 47120,
          "A: the parsed :settings carry kind/addr/port");

    // Construction reported the DIAL socket UP — the /net/b vertex value is VALUE{UP}
    // (0x03, the RFC-0014 liveness enum; S1 replaced the binary 0x01 "up").
    const auto lv = node_a.read(path_t("/net/udp-client/b"));
    bool up = false;
    if (lv) {
        const auto t = tr::wire::decode((*lv)->only());
        up = t.has_value() && t->type == type_t::VALUE && t->payload.size() == 1 &&
             t->payload[0] == std::byte{static_cast<std::uint8_t>(link_state_t::UP)};
    }
    check(up, "A: link state written UP at creation (await-able bring-up)");

    // End-to-end: FWD{READ dst=/b/temp} from A crosses A's config-created socket to
    // B's terminus, and the REPLY source-routes back to A's reply sink — B's listener
    // learned A's ephemeral source address from the request datagram.
    router_a.on_frame("self", fwd_read({"net", "udp-client", "b", "temp"}, {"reply-ep"}));
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
    net.provide_link("ws-client", "up", channel.a());  // staged first — must win over kind=udp

    const auto w =
        node.write(path_t("/net:children[]"),
                   conn_spec("client", "up", conn_role_t::DIAL, 47121, "udp", "127.0.0.1"));
    check(w.has_value(), "SPEC with kind=udp still creates the connection");
    check(router.registry().by_name("net/ws-client/up") == &channel.a(),
          "the staged provide_link transport is the wired one (no socket constructed)");
    const auto* s = net.settings_of("net/ws-client/up");
    check(s != nullptr && s->kind == "udp", "the config kind is still parsed into :settings");
    channel.shutdown();
}

/**
 * @brief A link that records the state of its own receiver slot at `start_receiving()` time.
 *
 * `rx_` is the protected delivery slot every `transport_t` carries, so a link can answer
 * "was a sink installed by the time I was told to start receiving?" about itself — which is
 * the whole ordering claim of #1025, observed from where it matters.
 */
struct arm_probe_link_t : tr::net::transport_t {
    std::atomic<int> starts{0};           /**< @brief How many times it was armed. */
    std::atomic<bool> sink_at_arm{false}; /**< @brief Was a sink installed at that instant? */

    /** @brief Egress is not what this probe measures. */
    void send(std::span<const std::byte> /*frame*/) override {}

    /** @brief Record the slot's state, then count the call. */
    void start_receiving() override {
        sink_at_arm.store(rx_.has_any(), std::memory_order_relaxed);
        starts.fetch_add(1, std::memory_order_relaxed);
    }
};

/**
 * @brief #1025 — `make_connection` arms the link only AFTER it is fully wired.
 *
 * A DIAL transport that starts its receive thread inside its own constructor is already
 * decoding while the creation path is still registering the vertex and running
 * `fwd_router_t::add_child` — so a message the peer pushes on connect lands in an empty
 * receiver slot and is dropped silently. The fix hands the ordering to the owner: the
 * factory builds the socket without a recv thread, and creation arms it here. This pins the
 * ORDER — the arm must come after the receiver install, not merely happen.
 */
void test_link_is_armed_after_wiring() {
    std::printf("make_connection arms the link only after add_child wired it (#1025):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);

    arm_probe_link_t link;
    check(link.starts.load() == 0, "a freshly constructed link has not been armed");
    net.provide_link("ws-client", "up", link);

    const auto w =
        node.write(path_t("/net:children[]"), conn_spec("client", "up", conn_role_t::DIAL, 8080));
    check(w.has_value(), "the SPEC created the connection over the staged link");
    check(link.starts.load() > 0, "make_connection armed the link");
    check(link.starts.load() == 1, "exactly once");
    check(link.sink_at_arm.load(), "and its receiver was ALREADY installed when it did");
}

/**
 * @brief Read from @p fd until `done(buf)` answers true or the budget expires.
 */
template <typename Done>
std::vector<std::byte> read_until(int fd, Done done, std::chrono::milliseconds budget) {
    std::vector<std::byte> buf;
    std::array<std::byte, 1024> chunk;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done(buf) && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, 50) <= 0) continue;
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) break;
        buf.insert(buf.end(), chunk.data(), chunk.data() + n);
    }
    return buf;
}

/** @brief Append one UNMASKED server→client RFC 6455 frame (FIN=1, payload < 126) to @p out. */
void append_server_frame(std::vector<std::byte>& out, ws::opcode_t op,
                         std::span<const std::byte> payload) {
    out.push_back(static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(op)));
    out.push_back(static_cast<std::byte>(payload.size()));  // MASK=0: a server frame
    out.insert(out.end(), payload.begin(), payload.end());
}

/** @brief FWD{ op=WRITE, dst=<segs...>, src=/reply-ep, VALUE @p value } — a terminus write. */
std::vector<std::byte> fwd_write(std::initializer_list<std::string_view> dst, std::byte value) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    std::vector<std::byte> segs;
    for (std::string_view s : dst) tr::wire::emit_name(segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, segs);
    segs.clear();
    tr::wire::emit_name(segs, "reply-ep");
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, segs);
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&value, 1));
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/**
 * @brief A graph value backend that PARKS one allocation — the test's handle on the map lock.
 *
 * `graph_t::read_children_folded` frames each member's POINT header from the injected ADR-0060
 * value backend WHILE it holds the shared `map_mutex_`, so a backend that blocks inside `alloc`
 * leaves that lock held for exactly as long as the test wants. That is what lets the case below
 * stretch the instant `make_connection` spends between "the socket is up" and "the receiver is
 * wired" — a few microseconds of `register_vertex_key` + `add_child`, which is far too short to
 * observe — into a window a raw peer can be raced against on purpose. Nothing else changes:
 * every allocation but the single armed one goes straight to the heap backend.
 *
 * The window is not an invention of the test. #1025 is precisely the claim that this span is
 * long enough for a pushed message to be decoded inside it; the gate makes the span
 * MEASURABLE instead of leaving the guard at the mercy of how fast this host happens to start
 * a thread.
 */
class map_lock_gate_t final : public tr::mem::mem_backend_t {
   public:
    map_lock_gate_t() noexcept : mem_backend_t("map_lock_gate") {}

    /** @brief Arm the gate: the NEXT allocation parks until @ref release. */
    void arm() { armed_.store(true, std::memory_order_relaxed); }

    /** @brief Wait until an armed allocation has parked — i.e. the map lock IS now held. */
    bool wait_parked(std::chrono::milliseconds budget) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, budget, [this] { return parked_; });
    }

    /** @brief Let the parked allocation go, and the map lock with it. */
    void release() {
        {
            const std::lock_guard lock(m_);
            released_ = true;
        }
        cv_.notify_all();
    }

    /** @brief Park here once if armed, then allocate exactly as the heap backend would. */
    tr::view::segment_t* alloc(std::size_t size, tr::mem::alloc_hint_t hint) override {
        if (armed_.exchange(false, std::memory_order_relaxed)) {
            std::unique_lock lock(m_);
            parked_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this] { return released_; });
        }
        return tr::mem::heap_backend().alloc(size, hint);
    }

    /** @brief Unreachable in practice: the heap backend stamps the segments it makes. */
    void destroy(tr::view::segment_t* seg) noexcept override {
        tr::mem::heap_backend().destroy(seg);
    }

   private:
    std::atomic<bool> armed_{false}; /**< @brief Whether the next alloc parks. */
    std::mutex m_;                   /**< @brief Guards the two flags below. */
    std::condition_variable cv_;     /**< @brief Parked/released handoff. */
    bool parked_ = false;            /**< @brief An armed allocation is sitting in the lock. */
    bool released_ = false;          /**< @brief The test let it go. */
};

/**
 * @brief #1025 — a SPEC-created `kind=ws` DIAL delivers the message its peer pushed on connect.
 *
 * The other two guards each hold one end and neither covers the middle. The raw-peer case in
 * `ws_transport_test` constructs `transport_ws_client` DIRECTLY, so the factory's argument list
 * is never read; `test_link_is_armed_after_wiring` above goes in through `provide_link`, which
 * takes the staged-link branch and never calls a factory at all. The `defer_recv` argument the
 * built-in `ws` factory passes (`core/src/builtin_transport_ws.cpp`) sits between them. Drop it
 * and the SPEC-created DIAL is one-phase again: the recv thread is spawned inside the
 * constructor, `make_connection`'s `start_receiving()` finds the one-shot latch already set and
 * does nothing, and the push-on-connect message is back to racing the receiver install —
 * silently, because a decode into an empty `receiver_slot_t` moves no counter at all.
 *
 * So this drives the raw-peer harness through the PRODUCTION creation path — a graph write of
 * SPEC{client, kind=ws, addr, port} — and observes DOWNSTREAM of the link, where a drop shows.
 * The peer writes its `101` and a COMPLETE BINARY message carrying `FWD{WRITE dst=/temp}` in
 * ONE `::send`, so the whole message is off the wire and in the client's handshake carry-over
 * before the constructor returns; two writes would leave the client parked in `recv` and
 * reproduce nothing. The factory wires the router as the receiver, so a DELIVERED frame reaches
 * the terminus and lands in the LKV — `/temp` takes a write it had not taken before. A frame
 * decoded before the wiring reaches nothing, and `/temp` stays as it was.
 *
 * The window is held open on purpose (see @ref map_lock_gate_t). Left to the host, the span
 * between the factory returning and `add_child` wiring the receiver is a couple of
 * microseconds while a fresh `pthread` needs tens to reach its first read — so the one-phase
 * shape would usually win the race by accident and the guard would assert nothing. Measured
 * here before the gate existed: 20 runs of the reverted factory, 20 deliveries. The gate stalls
 * the creation path INSIDE that span, which is the only state in which the two shapes differ.
 */
void test_factory_built_ws_dial_delivers_push_on_connect() {
    std::printf("a SPEC-created kind=ws DIAL delivers the peer's push-on-connect (#1025):\n");

    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    check(lfd >= 0, "raw listener created");
    const int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    check(::bind(lfd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "listener bound");
    check(::listen(lfd, 1) == 0, "listener listening");
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&bound), &blen);

    // The pushed message: a remote WRITE of one byte into this node's own /temp.
    const std::byte kPushed{0x5A};
    const std::vector<std::byte> pushed = fwd_write({"temp"}, kPushed);
    // How long the creation path is held between the socket and the wiring. Three orders of
    // magnitude more than a one-phase recv thread needs to decode what it was handed, so the
    // reverted factory fails here deterministically rather than flakily.
    constexpr auto kHeld = 200ms;

    std::promise<bool> one_write_done;  // the 101 and the message went out as ONE send
    std::promise<void> peer_answered;   // ...and it is on the wire NOW (the hold's anchor)
    std::promise<void> test_done;       // safe to close the peer socket
    auto one_write_fut = one_write_done.get_future();
    auto answered_fut = peer_answered.get_future();
    auto done_fut = test_done.get_future();

    std::thread pusher([&] {
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            one_write_done.set_value(false);
            return;
        }
        const auto req = read_until(
            cfd,
            [](const std::vector<std::byte>& b) {
                return std::string_view(reinterpret_cast<const char*>(b.data()), b.size())
                           .find("\r\n\r\n") != std::string_view::npos;
            },
            2s);
        const std::string_view text(reinterpret_cast<const char*>(req.data()), req.size());
        const std::size_t kpos = text.find("Sec-WebSocket-Key: ");
        if (kpos == std::string_view::npos) {
            one_write_done.set_value(false);
            ::close(cfd);
            return;
        }
        const std::size_t vstart = kpos + std::string_view("Sec-WebSocket-Key: ").size();
        const std::string key(text.substr(vstart, text.find("\r\n", vstart) - vstart));

        std::string head =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
        head += ws::accept_key(key);
        head += "\r\n\r\n";
        std::vector<std::byte> blob;
        for (const char c : head) blob.push_back(static_cast<std::byte>(c));
        append_server_frame(blob, ws::opcode_t::BINARY, pushed);
        // ONE syscall: the handshake reply and the push behind it coalesce into a single
        // segment, so the client's handshake read takes the whole message off the socket with
        // it. This is the push-on-connect shape, and it is what makes the window real.
        const ssize_t sent = ::send(cfd, blob.data(), blob.size(), 0);
        one_write_done.set_value(sent == static_cast<ssize_t>(blob.size()));
        peer_answered.set_value();

        done_fut.wait();
        ::close(cfd);
    });

    // Declared before the graph they serve: the gate IS the graph's value backend, and the
    // counter and its callable outlive the vertex they are subscribed on.
    map_lock_gate_t gate;
    std::atomic<int> temp_writes{0};
    auto on_temp = [&temp_writes](const tr::view::rope_t&) {
        temp_writes.fetch_add(1, std::memory_order_relaxed);
    };

    graph_t node(std::pmr::get_default_resource(), &gate);
    fwd_router_t router(node);
    (void)node.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
    (void)node.subscribe(path_t("/temp"), on_temp);
    // The vertex whose `:children[]` fold parks the gate — it needs one REGISTERED child,
    // because the member headers are what the fold draws from the value backend inside the
    // map lock (the outer header is framed after the lock is dropped).
    const tr::graph::vertex_handle_t gate_v =
        node.register_vertex(path_t("/gate"), role_t::STORED_VALUE);
    (void)node.register_vertex(path_t("/gate/x"), role_t::STORED_VALUE);
    const int before = temp_writes.load(std::memory_order_relaxed);

    {
        // The FULL ctor — the one that installs the built-in `kind` catalog, so `kind=ws`
        // below is served by core/src/builtin_transport_ws.cpp and by nothing else. Declared
        // in its own scope so the socket (and its recv thread) is torn down before the router
        // and graph it delivers into.
        transport_vertex_t net(node, router);
        declare_builtin_modules(net);  // ADR-0073 §4: the application mints the module names
        // Pre-create the `/net/<module>` grouping vertex `make_connection` would otherwise
        // create lazily. That lazy call is a `register_vertex_key` BEFORE the factory runs, so
        // leaving it would park the creation path on the gate with no socket built yet — the
        // held window has to start AFTER the client exists to be the window #1025 is about.
        (void)node.try_register_vertex(path_t("/net/ws-client"), role_t::STORED_VALUE);

        gate.arm();
        std::thread holder([&] { (void)node.read_children_folded(gate_v); });
        check(gate.wait_parked(5s), "the gate parked inside the graph's map lock");

        // The creation path is about to block, so the release cannot come from this thread.
        // The hold is anchored to the instant the peer's `101` hit the wire — the client's
        // constructor returns microseconds later — rather than to a wall-clock guess about how
        // long a loopback handshake takes, so a loaded runner cannot eat the window.
        std::chrono::steady_clock::time_point released_at;
        std::thread releaser([&] {
            (void)answered_fut.wait_for(5s);
            std::this_thread::sleep_for(kHeld);
            released_at = std::chrono::steady_clock::now();
            gate.release();
        });

        const auto w = node.write(
            path_t("/net:children[]"),
            conn_spec("client", "up", conn_role_t::DIAL, ntohs(bound.sin_port), "ws", "127.0.0.1"));
        const auto spec_returned = std::chrono::steady_clock::now();
        releaser.join();  // ...and with it the happens-before edge on `released_at`
        holder.join();

        check(w.has_value(), "the SPEC created the connection through the built-in ws factory");
        check(net.link_of("net/ws-client/up") != nullptr,
              "the link is a CONSTRUCTED one (no provide_link staged anything here)");
        check(one_write_fut.wait_for(3s) == std::future_status::ready && one_write_fut.get(),
              "the peer put the 101 and a COMPLETE pushed message in ONE write");
        // Without this the case is vacuous: it would be asserting delivery across a window too
        // short for either shape to lose, and would pass whatever the factory passes. The write
        // returning only AFTER the gate let go is what proves it was gated — a creation path
        // that ran to completion on its own would have returned long before.
        check(spec_returned >= released_at,
              "and the creation path was still INSIDE make_connection when the gate let go — "
              "the window really was held open across it");

        bool delivered = false;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (temp_writes.load(std::memory_order_relaxed) > before) {
                delivered = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        check(delivered,
              "the pushed message reached the receiver the factory-built link was wired to: "
              "/temp took a write it had not taken before the SPEC");

        const auto stored = node.read(path_t("/temp"));
        bool carried = false;
        if (stored) {
            const auto inner = tr::wire::decode((*stored)->only());
            carried = inner.has_value() && inner->type == type_t::VALUE &&
                      inner->payload.size() == 1 && inner->payload[0] == kPushed;
        }
        check(carried, "carrying the pushed frame's own value");
    }

    test_done.set_value();
    pusher.join();
    ::close(lfd);
}

void test_creation_errors() {
    std::printf("Creation errors are clean statuses, never crashes:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_builtin_modules(net);  // ADR-0073 §4: the application mints the module names

    // Undeclared MODULE => SCHEMA_NOT_FOUND at the module_for gate (ADR-0073 §4), no
    // vertex. "pigeon" has no register_module entry, so this exercises the declared-only
    // gate — the factory lookup is never reached.
    const auto w1 =
        node.write(path_t("/net:children[]"),
                   conn_spec("client", "x", conn_role_t::DIAL, 47122, "pigeon", "127.0.0.1"));
    check(!w1.has_value() && w1.error() == status_t::SCHEMA_NOT_FOUND,
          "undeclared module => SCHEMA_NOT_FOUND");
    check(!node.find(path_t::parse("/net/x")->key()).has_value(), "no /net/x vertex was created");

    // The DISCRIMINATING twin: a DECLARED module whose kind has NO registered factory —
    // the creation now passes module_for and must fail at the factory-lookup gate with
    // the same status. Without this case the factory-missing branch is exercised by no
    // test (the pre-ADR-0073 "pigeon" case used to cover it).
    check(net.register_module("pigeon-x", "pigeon", conn_role_t::DIAL).has_value(),
          "a module can be declared for a kind with no factory (declaration is naming)");
    const auto w1b =
        node.write(path_t("/net:children[]"),
                   conn_spec("client", "x2", conn_role_t::DIAL, 47122, "pigeon", "127.0.0.1"));
    check(!w1b.has_value() && w1b.error() == status_t::SCHEMA_NOT_FOUND,
          "declared module, missing factory => SCHEMA_NOT_FOUND at the factory gate");
    check(!node.find(path_t::parse("/net/pigeon-x/x2")->key()).has_value(),
          "no vertex was created for the factory-less kind");

    // A udp DIAL without addr (and a LISTEN without port) => TYPE_MISMATCH, no vertex.
    const auto w2 = node.write(path_t("/net:children[]"),
                               conn_spec("client", "y", conn_role_t::DIAL, 47123, "udp"));
    check(!w2.has_value() && w2.error() == status_t::TYPE_MISMATCH,
          "udp client without addr => TYPE_MISMATCH");
    const auto w3 = node.write(path_t("/net:children[]"),
                               conn_spec("listener", "z", conn_role_t::LISTEN, 0, "udp"));
    check(!w3.has_value() && w3.error() == status_t::TYPE_MISMATCH,
          "udp listener without port => TYPE_MISMATCH");
    check(!node.find(path_t::parse("/net/y")->key()).has_value() &&
              !node.find(path_t::parse("/net/z")->key()).has_value(),
          "no vertices were created for the failed configs");

    // No kind and no staged link => NOT_FOUND (nothing can carry the bytes).
    const auto w4 =
        node.write(path_t("/net:children[]"), conn_spec("client", "w", conn_role_t::DIAL, 8080));
    check(!w4.has_value() && w4.error() == status_t::NOT_FOUND,
          "no kind + no provide_link => NOT_FOUND");
}

void test_link_of_accessor() {
    std::printf("link_of reaches a SPEC-constructed owned transport (#374):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_builtin_modules(net);  // ADR-0073 §4: the application mints the module names

    // A ws LISTENER built purely from config — the owned server socket is otherwise
    // unreachable by the app (no accessor existed before #374).
    const auto w = node.write(path_t("/net:children[]"),
                              conn_spec("listener", "srv", conn_role_t::LISTEN, 47131, "ws"));
    check(w.has_value(), "SPEC{listener, kind=ws} constructs the owned server");

    tr::net::transport_t* const link = net.link_of("net/ws-server/srv");
    check(link != nullptr, "link_of resolves the owned transport (previously unreachable)");
    auto* const srv = dynamic_cast<tr::net::transport_ws_server*>(link);
    check(srv != nullptr, "the owned transport is a transport_ws_server");
    if (srv != nullptr)
        check(srv->ok() && srv->local_port() != 0, "it is the live, bound owned socket");
    check(net.link_of("net/ws-client/absent") == nullptr, "link_of of an unknown NAME is nullptr");
}

void test_link_name_collision_rejected() {
    std::printf("Bug #373 is closed by ADDRESSING: a link may share a first-level name:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    // A first-level vertex the link would shadow.
    (void)node.register_vertex(path_t("/system"), role_t::STORED_VALUE);

    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "system", channel.a());  // stage a link named exactly "system"
    // #373 existed because a connection NAME was the FIRST dst segment, so a link sharing a
    // name with a first-level vertex black-holed every /system/... read onto the transport.
    // A connection is now addressed /net/<module>/<name> (RFC-0014, ADR-0061), so the two
    // names live in different places and cannot collide — the guard is not just unnecessary,
    // keeping it would wrongly reject a connection merely NAMED AFTER a local vertex.
    const auto w = node.write(path_t("/net:children[]"),
                              conn_spec("listener", "system", conn_role_t::LISTEN, 0));
    check(w.has_value(), "a link may now be named after a first-level vertex");
    check(node.find(path_t::parse("/net/ws-client/system")->key()).has_value(),
          "it mounts at /net/ws-client/system, nowhere near /system");
    check(node.find(path_t::parse("/system")->key()).has_value(),
          "the unrelated first-level /system vertex is untouched");
    check(router.registry().by_name("net/ws-client/system") == &channel.a(),
          "and it IS wired into the router under its mount path");

    // Control: a non-colliding name still works end-to-end.
    net.provide_link("ws-client", "uplink", channel.a());
    const auto w2 = node.write(path_t("/net:children[]"),
                               conn_spec("listener", "uplink", conn_role_t::LISTEN, 0));
    check(w2.has_value(), "a non-colliding link name still registers");
    check(node.find(path_t::parse("/net/ws-client/uplink")->key()).has_value() &&
              router.registry().by_name("net/ws-client/uplink") == &channel.a(),
          "the non-colliding connection resolves and is wired into the router");
    channel.shutdown();
}

void test_link_name_collision_placeholder_parent() {
    std::printf("Bug #373: a link name matching a PLACEHOLDER first-level parent is fine too:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    // Only /system/mode is registered, so /system stays a structural placeholder. Under the
    // old flat routing the router shadowed that first segment; under mount addressing there
    // is nothing to shadow.
    (void)node.register_vertex(path_t("/system/mode"), role_t::STORED_VALUE);
    check(!node.find(path_t::parse("/system")->key()).has_value(),
          "/system is a placeholder (find sees no registered vertex there)");

    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "system", channel.a());
    const auto w = node.write(path_t("/net:children[]"),
                              conn_spec("listener", "system", conn_role_t::LISTEN, 0));
    check(w.has_value(), "a link name matching a placeholder first-level parent is accepted");
    check(router.registry().by_name("net/ws-client/system") == &channel.a(),
          "and it is wired under its mount path, leaving /system/mode reachable");
    channel.shutdown();
}

/**
 * @brief A ws LISTENER spec carrying the ws-private `peer_named` / `max_peers` keys.
 *
 * SPEC{ NAME "type" "listener", NAME "name" <name>, SETTINGS "config"{ NAME "role" VALUE u8=1,
 *       NAME "port" VALUE u16, NAME "kind" NAME "ws", NAME "peer_named" VALUE u8,
 *       NAME "max_peers" VALUE u32 } }
 */
view_t ws_listener_spec(std::string_view name, std::uint16_t port, bool peer_named,
                        std::uint32_t max_peers) {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(conn_role_t::LISTEN)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    tr::wire::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);
    tr::wire::emit_name(cfg, "kind");
    tr::wire::emit_name(cfg, "ws");
    tr::wire::emit_name(cfg, "peer_named");
    const std::byte pn{static_cast<std::uint8_t>(peer_named ? 1 : 0)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&pn, 1));
    tr::wire::emit_name(cfg, "max_peers");
    std::vector<std::byte> mb(4);
    tr::detail::store_le(mb, max_peers, 4);
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, mb);

    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, "listener");
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    tr::wire::emit_name(body, "config");
    tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

/** @brief The NAME of every POINT member of a synthesized listing. */
std::set<std::string> member_names(const tr::wire::tlv_t& point) {
    std::set<std::string> names;
    for (const auto& m : point.children) {
        if (m.type != type_t::POINT) continue;
        for (const auto& f : m.children)
            if (f.type == type_t::NAME)
                names.insert(std::string(tr::detail::as_string_view(f.payload)));
    }
    return names;
}

/** @brief Decode a connection vertex's synthesized `:children[]` peer listing. */
std::set<std::string> enumerate_peers(graph_t& g, const char* path) {
    const auto r = g.read(*path_t::parse(path));
    if (!r) return {};
    const tr::view::view_t flat = (*r)->flatten();
    const auto dec = tr::wire::decode(flat.bytes());
    if (!dec || dec->type != type_t::POINT) return {};
    return member_names(*dec);
}

/**
 * @brief An OS-granted free TCP port (bind 0, read it back, release).
 *
 * The SPEC config contract forbids `port == 0` for LISTEN (`dial_or_listen`,
 * TYPE_MISMATCH), so an in-band-created listener cannot ask for an ephemeral port —
 * and a fixed literal was an EADDRINUSE flake across parallel CI matrix legs. The
 * tiny close-to-bind race is acceptable in a test.
 */
[[nodiscard]] std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    std::uint16_t port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

/** @brief Poll until `pred` holds — the peer table is fed by the server's accept thread. */
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

/**
 * @brief #926 — the universal `max_frame` key reaches the `udp` factory's transport.
 *
 * `max_frame` is parsed once, centrally, into `conn_settings_t` for every kind; honoring it is
 * each factory's job. The `udp` factory used to construct its transport without it, so the key
 * was inert on this kind — read back by `:settings` as though honored. This asserts the whole
 * door, from the SPEC write to the constructed socket's own cap, against a CONTROL connection
 * that omits the key.
 */
void test_udp_max_frame_reaches_the_transport() {
    std::printf("#926: a config max_frame reaches the SPEC-constructed udp transport:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_builtin_modules(net);

    // The CONTROL: no max_frame in the config => the transport's own kMaxDatagram ceiling.
    const auto plain = node.write(
        path_t("/net:children[]"),
        conn_spec("listener", "plain", conn_role_t::LISTEN, free_port(), "udp", {}, 0, 0, 0));
    check(plain.has_value(), "SPEC{listener, kind=udp} with no max_frame constructs the socket");
    auto* const plain_link =
        dynamic_cast<tr::net::udp_transport_t*>(net.link_of("net/udp-server/plain"));
    check(plain_link != nullptr &&
              plain_link->effective_max_frame() == tr::net::udp_transport_t::kMaxDatagram,
          "without the key the connection carries the full datagram ceiling");

    // The SUBJECT: the same write plus `max_frame = 4096`.
    const auto capped = node.write(path_t("/net:children[]"),
                                   conn_spec("listener", "capped", conn_role_t::LISTEN, free_port(),
                                             "udp", {}, 0, 0, /*max_frame=*/4096));
    check(capped.has_value(), "SPEC{listener, kind=udp, max_frame=4096} constructs the socket");
    const auto* const s = net.settings_of("net/udp-server/capped");
    check(s != nullptr && s->max_frame == 4096, "the key was parsed into conn_settings_t");
    auto* const capped_link =
        dynamic_cast<tr::net::udp_transport_t*>(net.link_of("net/udp-server/capped"));
    check(capped_link != nullptr, "the owned transport is a live udp_transport_t");
    check(capped_link != nullptr && capped_link->effective_max_frame() == 4096,
          "and the CONFIGURED cap is the one the socket honors");
}

/**
 * @brief #408 / ADR-0043 §5 / ADR-0044: the ws-private `peer_named` config key makes the
 *        Brick-C peer listing reachable from a purely IN-BAND SPEC write.
 *
 * Before this key a SPEC-created ws listener was always constructed `peer_named=false`, so
 * its `bus()` was null, `make_connection` never installed `on_children`, and ADR-0044's peer
 * enumeration was creatable ONLY by direct construction + provide_link — unreachable to the
 * in-band creator (a web UI forming a link on a remote device) that ADR-0027 exists for.
 */
void test_ws_peer_named_config() {
    std::printf("#408: the ws-private peer_named key wires ADR-0044 peer enumeration in-band:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_builtin_modules(net);  // ADR-0073 §4: the application mints the module names

    // ----- the CONTROL: no peer_named => a plain point-to-point link, no listing. -----
    const auto plain =
        node.write(path_t("/net:children[]"),
                   conn_spec("listener", "plain", conn_role_t::LISTEN, free_port(), "ws"));
    check(plain.has_value(), "SPEC{listener, kind=ws} with no peer_named constructs the server");
    auto* const plain_srv =
        dynamic_cast<tr::net::transport_ws_server*>(net.link_of("net/ws-server/plain"));
    check(plain_srv != nullptr && plain_srv->bus() == nullptr,
          "without the key the SPEC-created server has a NULL bus (no ADR-0044 facet)");

    // ----- the SUBJECT: peer_named=1 => the bus facet, hence the synthesized listing. -----
    const auto w =
        node.write(path_t("/net:children[]"),
                   ws_listener_spec("bus", free_port(), /*peer_named=*/true, /*max_peers=*/8));
    check(w.has_value(), "SPEC{listener, kind=ws, peer_named=1} constructs the owned server");
    auto* const srv = dynamic_cast<tr::net::transport_ws_server*>(net.link_of("net/ws-server/bus"));
    check(srv != nullptr && srv->ok(), "the owned transport is a live transport_ws_server");
    check(srv != nullptr && srv->bus() != nullptr,
          "the ws-private key exposed the bus_link_t facet on a CONFIG-constructed link");

    // A fresh listener is audible to nobody.
    check(enumerate_peers(node, "/net/ws-server/bus:children[]").empty(),
          "/net/bus:children[] is empty before any peer dials in");

    // Two real ws clients dial the SPEC-created listener; each completes an RFC 6455
    // handshake, so each becomes an audible peer of the bus.
    const std::uint16_t bus_port = srv != nullptr ? srv->local_port() : 0;
    tr::net::transport_ws_client c1("127.0.0.1", bus_port);
    tr::net::transport_ws_client c2("127.0.0.1", bus_port);
    check(c1.ok() && c2.ok(), "both ws clients handshook against the in-band-created listener");

    const bool listed = wait_until(
        [&] { return enumerate_peers(node, "/net/ws-server/bus:children[]").size() == 2; }, 2s);
    check(listed, "/net/bus:children[] synthesizes exactly the 2 live peers (ADR-0044 Brick C)");

    // The peer names are the routable `p<slot>` fallback (#426, ADR-0073 §2) — the slot
    // assignment is arrival-order-dependent, so assert the shape, never a literal.
    const auto peers = enumerate_peers(node, "/net/ws-server/bus:children[]");
    const bool shaped = std::all_of(peers.begin(), peers.end(), [](const std::string& p) {
        return p.size() >= 2 && p[0] == 'p' &&
               p.find_first_not_of("0123456789", 1) == std::string::npos &&
               tr::graph::valid_segment(p);
    });
    check(shaped, "each synthesized peer name is the routable p<slot> segment");

    // NO vertex is created for a peer — the listing is synthesized on every read, so the
    // /net subtree still holds exactly the two CONNECTIONS (ADR-0044: peers are never
    // registered — though a peer name IS a legal next-hop segment since #426).
    // RFC-0014 §1: /net enumerates MODULES; the connections themselves sit one level down
    // under theirs. Either way no peer gains a vertex, which is what this asserts.
    check(enumerate_peers(node, "/net:children[]") == std::set<std::string>{"ws-server"},
          "/net:children[] lists the module");
    check(
        enumerate_peers(node, "/net/ws-server:children[]") == std::set<std::string>{"bus", "plain"},
        "/net/ws-server:children[] lists only the two connections — no vertex per peer");
}

/**
 * @brief ADR-0073 §4 / #621: modules are declared-only — an unregistered kind answers
 *        SCHEMA_NOT_FOUND instead of silently mounting under a library-derived name.
 *
 * Ablation-verified: before the fix this creation SUCCEEDED and mounted the connection
 * under the derived `/net/udp-client/...`, so both checks below failed.
 */
void test_unregistered_kind_is_schema_not_found() {
    std::printf("#621: an unregistered kind fails creation (no derived module name):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    // Deliberately NO register_module: `udp` has a transport FACTORY (linked built-in),
    // but this application never declared a module for it.
    const auto w = node.write(path_t("/net:children[]"), conn_spec("client", "x", conn_role_t::DIAL,
                                                                   47150, "udp", "127.0.0.1"));
    check(!w.has_value() && w.error() == status_t::SCHEMA_NOT_FOUND,
          "kind with a factory but no declared module => SCHEMA_NOT_FOUND");
    check(!node.find(path_t::parse("/net/udp-client/x")->key()).has_value(),
          "nothing mounted under the old derived /net/udp-client name");
}

/** @brief ADR-0073 §1/§4: register_module is a minting boundary — the shared segment
 *         predicate rejects a reserved-character module name with INVALID_PATH. */
void test_register_module_rejects_reserved_chars() {
    std::printf("#621: register_module gates the name with the shared segment predicate:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    for (const char* bad : {"a/b", "a:b", "a.b", "a*b", "a?b", ""}) {
        const auto r = net.register_module(bad, "udp", conn_role_t::DIAL);
        check(!r.has_value() && r.error() == status_t::INVALID_PATH,
              "a reserved-character / empty module name answers INVALID_PATH");
    }
    // The failed registrations minted nothing: the kind still has no module.
    check(!net.module_for("udp", conn_role_t::DIAL).has_value(),
          "a rejected registration declares nothing (module_for stays SCHEMA_NOT_FOUND)");
    // And a legal name goes through the same boundary.
    check(net.register_module("uplink", "udp", conn_role_t::DIAL).has_value(),
          "a legal segment registers");
    const auto m = net.module_for("udp", conn_role_t::DIAL);
    check(m.has_value() && *m == "uplink", "module_for answers the app-minted name");
}

/**
 * @brief ADR-0073 §4 / #621: the application owns its whole path shape — a transport
 *        registered under an app-chosen root AND module name (`/io/w`, `/io/l`) creates
 *        and carries traffic end-to-end. This was impossible while the module half of
 *        the path was library-derived.
 */
void test_app_chosen_root_and_module() {
    std::printf("#621: app-chosen custom root + module name, end-to-end over UDP:\n");
    graph_t node_a;
    graph_t node_b;
    fwd_router_t router_a(node_a);
    fwd_router_t router_b(node_b);
    transport_vertex_t net_a(node_a, router_a, "/io");
    transport_vertex_t net_b(node_b, router_b, "/io");
    // The application's naming decisions: dial links mount under /io/w, listeners under
    // /io/l — nothing resembling the built-ins' suggested "<kind>-client" shape.
    (void)net_a.register_module("w", "udp", conn_role_t::DIAL);
    (void)net_b.register_module("l", "udp", conn_role_t::LISTEN);

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    router_a.on_reply(
        [](void* ctx, const tr::view::rope_t& reply) {
            try {
                const tr::view::view_t mat = reply.materialize();
                const auto b = mat.bytes();
                static_cast<std::promise<std::vector<std::byte>>*>(ctx)->set_value(
                    std::vector<std::byte>(b.begin(), b.end()));
            } catch (...) {
            }
        },
        &got);

    // B: a stored value and a listener created through the app-chosen names.
    (void)node_b.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
    std::vector<std::byte> tv;
    const std::byte tb{0x5C};
    tr::wire::emit_tlv(tv, type_t::VALUE, opt_t{}, std::span<const std::byte>(&tb, 1));
    (void)node_b.write(path_t("/temp"), owned(tv));
    const auto wb = node_b.write(path_t("/io:children[]"),
                                 conn_spec("listener", "a", conn_role_t::LISTEN, 47151, "udp"));
    check(wb.has_value(), "B: SPEC{listener, kind=udp} creates under the app-chosen /io/l");
    check(node_b.find(path_t::parse("/io/l/a")->key()).has_value(),
          "B: the connection vertex is /io/l/a — the app's shape, not the library's");
    check(router_b.registry().by_name("io/l/a") != nullptr, "B: the socket is routed under io/l/a");

    const auto wa =
        node_a.write(path_t("/io:children[]"),
                     conn_spec("client", "b", conn_role_t::DIAL, 47151, "udp", "127.0.0.1"));
    check(wa.has_value(), "A: SPEC{client, kind=udp} creates under the app-chosen /io/w");
    check(node_a.find(path_t::parse("/io/w/b")->key()).has_value(),
          "A: the connection vertex is /io/w/b");
    const auto* s = net_a.settings_of("io/w/b");
    check(s != nullptr && s->kind == "udp" && s->port == 47151,
          "A: settings_of resolves by the app-chosen qualified key");

    // End-to-end: a READ routed through /io/w/b reaches B and the reply returns.
    router_a.on_frame("self", fwd_read({"io", "w", "b", "temp"}, {"reply-ep"}));
    const bool replied = fut.wait_for(3s) == std::future_status::ready;
    check(replied, "the READ crossed the /io/w link and the REPLY returned");
    if (replied) {
        const std::vector<std::byte> reply_bytes = fut.get();
        const auto dec = tr::wire::decode(reply_bytes);
        bool has_value = false;
        if (dec && dec->type == type_t::FWD)
            for (const auto& c : dec->children)
                if (c.type == type_t::VALUE && c.payload.size() == 1 &&
                    c.payload[0] == std::byte{0x5C})
                    has_value = true;
        check(has_value, "the REPLY carries B's stored /temp value");
    }
}

/**
 * @brief #688 open trace — a peer-created child DOES reach `fwd_router_t::add_child`, and
 *        THE segment predicate runs strictly before it.
 *
 * The trace the ruling left open, executed against the PRODUCTION wiring rather than a
 * hand-built harness (the RFC-0014 lesson: two silent misroutes shipped because no test
 * drove the real path). The reachable chain is
 * `graph_t::write(/net:children[]) -> graph_t::create_child -> child_types_["client"] ->
 * transport_vertex_t::make_connection -> fwd_router_t::add_child`, so a peer-supplied
 * `SPEC` name becomes the last segment of a routable MOUNT name — which is exactly why
 * the predicate must gate it, and why gating it anywhere later would be too late.
 *
 * The positive control proves the chain is real (a legal name lands in the router's
 * registry); each reserved-character case proves the door shuts first — `INVALID_PATH`
 * from `create_child`, before the catalog lookup, before key composition, before the
 * factory, hence before `add_child`. Ablation-verified: delete the `valid_segment` call
 * in `graph_t::create_child` and every negative case below registers a mount named
 * `net/udp-client/a:b` (etc.) that no conforming `dst` can address.
 *
 * `routable_mount_name` (fwd_router.cpp, #523) is a SIBLING of the predicate, not a
 * second copy of it: it bounds a whole multi-segment mount PATH, where `/` is legal and
 * `kMaxSegments` is the bound; `valid_segment` bounds ONE segment, where `/` is not. It
 * accepts every name below, so it is not, and must not be mistaken for, this gate.
 */
void test_wire_name_reaches_add_child() {
    std::printf("#688 trace: create_child -> make_connection -> add_child, predicate first:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_builtin_modules(net);  // ADR-0073 §4: the application mints the module names

    // ----- POSITIVE CONTROL: the chain is real. A legal peer name becomes a mount. -----
    const auto ok = node.write(
        path_t("/net:children[]"),
        conn_spec("client", "ok-name", conn_role_t::DIAL, free_port(), "udp", "127.0.0.1"));
    check(ok.has_value(), "a legal peer-supplied name creates the connection");
    check(router.registry().by_name("net/udp-client/ok-name") != nullptr,
          "and it REACHED fwd_router_t::add_child — the peer's name is now a mount segment");
    const std::size_t mounts_after_control = router.registry().size();
    check(mounts_after_control == 1, "exactly one mount is registered so far");

    // ----- SUBJECT: one reserved character per case, same production door. -----
    for (const std::string_view bad : {"a/b", "a:b", "a.b", "a*b", "a?b"}) {
        const auto w = node.write(
            path_t("/net:children[]"),
            conn_spec("client", bad, conn_role_t::DIAL, free_port(), "udp", "127.0.0.1"));
        char label[96];
        std::snprintf(label, sizeof label, "name \"%.*s\" => INVALID_PATH before add_child",
                      static_cast<int>(bad.size()), bad.data());
        check(!w.has_value() && w.error() == status_t::INVALID_PATH, label);

        std::string qualified("net/udp-client/");
        qualified += bad;
        check(router.registry().by_name(qualified) == nullptr,
              "no unaddressable mount entered the router registry");
    }
    check(router.registry().size() == mounts_after_control,
          "the registry gained NOTHING from the five rejected creates");
    // No socket was constructed either: the gate is upstream of the transport factory.
    check(net.link_of("net/udp-client/a:b") == nullptr, "no link was constructed for a bad name");
}

}  // namespace

int main() {
    test_create_connection_vertex();
    test_await_link_state();
    test_liveness_enum_value();
    test_constructed_link_reports_role_state();
    test_backoff_connect_timeout_parsed();
    test_fwd_still_routes();
    test_local_path_untouched();
    test_config_constructed_udp();
    test_provide_link_wins();
    test_link_is_armed_after_wiring();
    test_factory_built_ws_dial_delivers_push_on_connect();
    test_creation_errors();
    test_link_of_accessor();
    test_link_name_collision_rejected();
    test_link_name_collision_placeholder_parent();
    test_udp_max_frame_reaches_the_transport();
    test_ws_peer_named_config();
    test_unregistered_kind_is_schema_not_found();
    test_register_module_rejects_reserved_chars();
    test_wire_name_reaches_add_child();
    test_app_chosen_root_and_module();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
