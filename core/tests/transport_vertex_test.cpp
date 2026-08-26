/**
 * @file
 * @brief #83 Stage-1 — transport/connection as a `/` vertex (ADR-0027), the SHELL over the live
 *        path (ADR-0037 Stage-1).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Proves: a connection is created in-band via a SPEC written to its module's creator
 * endpoint `/net/<module>/conn` (RFC-0014 §2), resolves as `/net/<module>/<name>`, carries
 * its transport-private
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
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/backend.hpp"
#include "libtracer/builtin_transports.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/config_reader.hpp"
#include "libtracer/error.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"
#include "test_support.hpp"

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

using tr::testing::check;

view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/**
 * @brief The connection-creation SPEC, from the library's own public builder (#902).
 *
 * The hand-emitted near-copy this file used to carry is gone: `tr::net::conn_spec` IS the
 * shape below, so the test now exercises the encoder a consumer actually ships with rather
 * than a private lookalike that could drift away from it. Extra config keys — the u32s
 * below, and any kind-private pair — are spelled with @ref tr::net::conn_spec_t directly.
 */
using tr::net::conn_spec;
using tr::net::conn_spec_t;

/**
 * @brief The LISTEN spelling for "any free port — the OS picks" (#1362).
 *
 * No port number is RESERVED on Linux: `net.ipv4.ip_local_port_range` defaults to
 * 32768-60999, so the 47xxx block these tests used to treat as unclaimed is inside the range
 * the kernel hands to ordinary client sockets. A fixed literal therefore races the ephemeral
 * allocator (and any concurrent copy of the suite) for its own port, and losing that race is
 * an EADDRINUSE at `bind(2)` — creation fails in 0.00 s, which is exactly how #1362
 * presented. Asking for 0 removes the contention instead of narrowing the window; the
 * granted port is read back off the constructed link with `local_port()`.
 */
constexpr std::uint16_t kEphemeral = 0;

/** @brief A nonzero port for SPECs REFUSED before any socket is constructed — it is never
 *         bound and never dialled, so its value cannot collide with anything. */
constexpr std::uint16_t kNeverBound = 1;

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
    std::printf("Create a connection via a /net/<module>/conn SPEC; it is a / vertex:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    // ADR-0073 §4: the application mints the module name; RFC-0014 §1: the module fixes the
    // role, so the DIAL below is declared here and said nowhere on the wire.
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);

    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "up", channel.a());  // Stage-1 (A): supply the pre-built link

    const auto w = node.write(path_t("/net/ws-client/conn"), conn_spec("up", 8080));
    check(w.has_value(), "SPEC{name=up} written to the endpoint creates the connection");
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
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);
    net.provide_link("ws-client", "up", channel.a());
    (void)node.write(path_t("/net/ws-client/conn"), conn_spec("up", 0));

    // A waiter blocks on the connection vertex; set_link_state(up) must wake it.
    std::promise<bool> woke;
    auto fut = woke.get_future();
    std::thread waiter([&] {
        const auto r = node.await(path_t("/net/ws-client/up"), 2s);
        woke.set_value(r.has_value());
    });
    // `await` is EDGE-triggered: `graph_t::await` snapshots `write_seq_` on ENTRY, on the
    // WAITER's own thread (`seq0 = v->current_seq()`, core/src/graph.cpp), then blocks for a
    // bump past it. A link-up write that lands before the waiter got there is therefore never
    // observed, and the waiter runs to its full 2 s timeout. That is intended — reference/04
    // says await blocks until the NEXT write — so the fix is here, not in the library. The
    // `sleep_for(20ms)` this replaced was a guess that 20 ms is enough for a thread to reach a
    // blocking call, and on a loaded CI container it is not (#1418).
    //
    // Re-arm the edge instead, until the waiter is SEEN to have taken it: `set_link_state` has
    // no "already UP" short-circuit, so every repeat reaches `graph_.write` and bumps the
    // sequence again. The deadline is mandatory and exceeds the waiter's own 2 s timeout, so a
    // genuinely broken wake path still FAILS (the waiter times out, sets `false`, and the
    // check below rejects it) instead of spinning here forever.
    auto ls = net.set_link_state("net/ws-client/up", link_state_t::UP);
    check(ls.has_value(), "set_link_state(UP) writes the vertex");
    std::future_status st = fut.wait_for(20ms);
    // A BACKSTOP, not a synchronization window: a broken wake path still makes the waiter's own
    // 2 s `await` time out and set the promise, which ends this loop and fails the check below
    // at ~2 s. So the deadline never fires in a real failure, and making it generous costs
    // nothing while removing the one way this fix could reintroduce the defect it removes —
    // giving up re-arming while the waiter is merely starved (reachable under oversubscription).
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (st != std::future_status::ready && ls.has_value() &&
           std::chrono::steady_clock::now() < deadline) {
        ls = net.set_link_state("net/ws-client/up", link_state_t::UP);
        st = fut.wait_for(20ms);
    }
    check(st == std::future_status::ready && fut.get(), "the awaiter woke on a link-up write");
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

/**
 * @brief Is a BUILT-IN DIAL kind engine-managed in THIS build (RFC-0014 §4 S5, #1548)?
 *
 * The built-in `udp`/`tcp`/`ws` factories register `self_heal_dial` conditioned on the
 * `kSelfHealLinks` module knob (`%kBuiltinPointToPointTraits`), so a DIAL creation is
 * DORMANT-with-no-socket on a stock build and eager on a closed-out one. Several cases below
 * assert a different — and equally required — outcome per shape; this names the fork so each
 * of them reads as one claim about the build rather than a conditional about nothing.
 */
constexpr bool kBuiltinDialIsEngineManaged = tr::net::kSelfHealLinks;

/** @brief Poll @p path's liveness byte until it is @p want, or the backstop expires. */
[[nodiscard]] bool await_link_state_byte(graph_t& g, std::string_view path, std::uint8_t want) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (read_link_state_byte(g, path) == want) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

void test_liveness_enum_value() {
    std::printf("Link-liveness value is the RFC-0014 6-state enum (S1):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    tr::net::loopback_channel_t channel;
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);
    net.provide_link("ws-client", "l", channel.a());
    (void)node.write(path_t("/net/ws-client/conn"), conn_spec("l", 0));

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
    const auto wl = node.write(path_t("/net/udp-server/conn"), conn_spec("srv", kEphemeral, "udp"));
    check(wl.has_value(), "a SPEC to the udp LISTEN module's endpoint constructs the bound socket");
    check(read_link_state_byte(node, "/net/udp-server/srv") == 4,
          "a constructed LISTEN reports LISTENING (0x04)");
    auto* const srv = dynamic_cast<tr::net::udp_transport_t*>(net.link_of("net/udp-server/srv"));
    const std::uint16_t srv_port = srv != nullptr ? srv->local_port() : 0;
    check(srv_port != 0, "the LISTENING socket reports its OS-granted bind port");

    // The DIAL half is where the #1548 flip shows: engine-managed, a creation constructs no
    // socket and reports DORMANT; on a build that closed the engine out the built-in declares
    // eager and the socket is UP the moment it is constructed, exactly as it always was.
    const auto wc =
        node.write(path_t("/net/udp-client/conn"), conn_spec("cli", srv_port, "udp", "127.0.0.1"));
    check(wc.has_value(), "a SPEC to the udp DIAL module's endpoint creates the connection");
    if constexpr (kBuiltinDialIsEngineManaged) {
        check(read_link_state_byte(node, "/net/udp-client/cli") == 0,
              "an engine-managed DIAL reports DORMANT (0x00) — no socket yet (#1548)");
    } else {
        check(read_link_state_byte(node, "/net/udp-client/cli") == 3,
              "an eagerly constructed DIAL reports UP (0x03)");
    }
}

void test_backoff_connect_timeout_parsed() {
    std::printf("conn_settings_t parses backoff / connect_timeout (dormant until S5):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    tr::net::loopback_channel_t channel;
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);
    net.provide_link("ws-client", "c", channel.a());
    (void)node.write(path_t("/net/ws-client/conn"),
                     conn_spec_t("c").port(0).backoff_ms(250).connect_timeout_ms(3000).view());
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
    (void)net_a.register_module("ws-client", "ws", conn_role_t::DIAL);
    net_a.provide_link("ws-client", "up", channel.a());
    (void)node_a.write(path_t("/net/ws-client/conn"), conn_spec("up", 0));
    (void)router_b.add_child("down", channel.b());  // B's side: plain router child (unchanged path)

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
        (void)tr::wire::emit_path_segment(segs, "net");
        (void)tr::wire::emit_path_segment(segs, "ws-client");
        (void)tr::wire::emit_path_segment(segs, "up");
        (void)tr::wire::emit_path_segment(segs, "temp");
        tr::wire::emit_tlv(dst, type_t::PATH, opt_t{}, segs);
    }
    std::vector<std::byte> src;  // PATH{ NAME reply }
    {
        std::vector<std::byte> segs;
        (void)tr::wire::emit_path_segment(segs, "reply");
        tr::wire::emit_tlv(src, type_t::PATH, opt_t{}, segs);
    }
    std::vector<std::byte> payload;
    const std::byte pv{0x2A};
    tr::wire::emit_tlv(payload, type_t::VALUE, opt_t{}, std::span<const std::byte>(&pv, 1));
    const auto frame = tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE, dst, src, {}, payload);

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

/**
 * @brief #873: a SLIM-constructed net plane REPORTS the egress store it was built with.
 *
 * Before this ctor took the argument, `egress_src_` was a body-internal `&mem::heap_source()`
 * on the SLIM path — so `egress_source()` answered the process heap on a slim node no matter
 * what the composition root had chosen, and a factory registered through
 * `register_transport_type` (the documented consumer of that accessor) was handed the wrong
 * store. There was no way to observe that from a test, which is exactly why it went unnoticed.
 *
 * Both halves matter: the injected node must report the injected store, and the DEFAULTED node
 * must still report `mem::heap_source()` — behaviour unchanged for every existing caller.
 */
void test_slim_net_reports_its_injected_egress_store() {
    std::printf("SLIM transport_vertex_t: egress_source() answers for the injected store:\n");
    std::array<std::byte, 256> slab{};
    tr::mem::bump_source_t store(std::span<std::byte>(slab), tr::mem::null_source());

    graph_t injected_graph;
    fwd_router_t injected_router(injected_graph);
    transport_vertex_t injected(injected_graph, injected_router, "/net", &tr::mem::heap_backend(),
                                tr::net::slim_net, &store);
    check(&injected.egress_source() == &store,
          "a SLIM node reports the store its constructor was given");

    graph_t defaulted_graph;
    fwd_router_t defaulted_router(defaulted_graph);
    transport_vertex_t defaulted(defaulted_graph, defaulted_router, "/net",
                                 &tr::mem::heap_backend(), tr::net::slim_net);
    check(&defaulted.egress_source() == &tr::mem::heap_source(),
          "and the DEFAULTED SLIM node still reports the process heap (unchanged)");

    // The nullptr guard moved from the FULL ctor into the SLIM one, so it must still hold on
    // BOTH doors — an explicit null means the process heap, not a null dereference.
    graph_t null_graph;
    fwd_router_t null_router(null_graph);
    transport_vertex_t null_slim(null_graph, null_router, "/net", &tr::mem::heap_backend(),
                                 tr::net::slim_net, nullptr);
    check(&null_slim.egress_source() == &tr::mem::heap_source(),
          "an explicit nullptr on the SLIM ctor is still the process heap");

    graph_t full_graph;
    fwd_router_t full_router(full_graph);
    transport_vertex_t full(full_graph, full_router, "/net", &tr::mem::heap_backend(), &store);
    check(&full.egress_source() == &store, "and the FULL ctor still threads its own store");
}

/** @brief FWD{ op, dst=<segs...>, src=<segs...> } with no payload — a remote READ request. */
std::vector<std::byte> fwd_read(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src) {
    return tr::testing::b_fwd(tr::graph::fwd_op_t::READ, tr::testing::b_path(dst),
                              tr::testing::b_path(src));
}

void test_config_constructed_udp() {
    std::printf("Config-constructed sockets: two nodes over UDP from endpoint SPECs:\n");
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
    const auto wb = node_b.write(path_t("/net/udp-server/conn"), conn_spec("a", kEphemeral, "udp"));
    check(wb.has_value(), "B: SPEC{kind=udp, port} at the LISTEN endpoint constructs the socket");
    check(router_b.registry().by_name("net/udp-server/a") != nullptr,
          "B: the socket is wired into the router");
    // The OS granted the bind port; A dials the port B actually got, not a literal.
    auto* const b_link = dynamic_cast<tr::net::udp_transport_t*>(net_b.link_of("net/udp-server/a"));
    const std::uint16_t b_port = b_link != nullptr ? b_link->local_port() : 0;
    check(b_port != 0, "B: the ephemeral bind port is readable back off the link");

    // A: a udp CLIENT dialing B's port — also purely from config.
    const auto wa =
        node_a.write(path_t("/net/udp-client/conn"), conn_spec("b", b_port, "udp", "127.0.0.1"));
    check(wa.has_value(), "A: SPEC{kind=udp, addr, port} at the DIAL endpoint creates the dialer");
    const auto* s = net_a.settings_of("net/udp-client/b");
    check(s != nullptr && s->kind == "udp" && s->addr == "127.0.0.1" && s->port == b_port,
          "A: the parsed :settings carry kind/addr/port");

    // Since the #1548 S5 flip the DIAL half of a built-in kind is ENGINE-MANAGED, so creation
    // constructs no socket and the vertex value is VALUE{DORMANT} (0x00); the op below is what
    // wakes it. On a build that closed the engine out the built-in declares eager and the
    // value is VALUE{UP} at creation, as it always was. Either way the value is the
    // await-able bring-up signal and the application side of this case is unchanged.
    check(read_link_state_byte(node_a, "/net/udp-client/b") ==
              (kBuiltinDialIsEngineManaged ? static_cast<std::uint8_t>(link_state_t::DORMANT)
                                           : static_cast<std::uint8_t>(link_state_t::UP)),
          "A: the DIAL link's creation-time liveness matches this build's dial model");

    // End-to-end: FWD{READ dst=/b/temp} from A AUTO-WAKES the dormant link (the §4 op door:
    // one bounded dial attempt, then the frame goes), crosses A's socket to
    // B's terminus, and the REPLY source-routes back to A's reply sink — B's listener
    // learned A's ephemeral source address from the request datagram.
    router_a.on_frame("self", fwd_read({"net", "udp-client", "b", "temp"}, {"reply-ep"}));
    check(await_link_state_byte(node_a, "/net/udp-client/b",
                                static_cast<std::uint8_t>(link_state_t::UP)),
          "A: the link is UP — the op woke it if this build's dial model is the engine's");
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
    declare_builtin_modules(net);  // kind=udp + DIAL is declared to mount under udp-client
    tr::net::loopback_channel_t channel;
    // Staged under the module the SPEC below is written to. Precedence is a WITHIN-module
    // question (#883): the SPEC and the staging have to be talking about the same mount
    // before "which one wins" is even a question. A staging under some other module is a
    // different connection — `test_kind_module_beats_unrelated_staging` covers that.
    net.provide_link(std::string(tr::net::kUdpClientSuggestedModule), "up", channel.a());

    const auto w = node.write(path_t("/net/udp-client/conn"),
                              conn_spec("up", kNeverBound, "udp", "127.0.0.1"));
    check(w.has_value(), "SPEC with kind=udp still creates the connection");
    check(router.registry().by_name("net/udp-client/up") == &channel.a(),
          "the staged provide_link transport is the wired one (no socket constructed)");
    const auto* s = net.settings_of("net/udp-client/up");
    check(s != nullptr && s->kind == "udp", "the config kind is still parsed into :settings");
    channel.shutdown();
}

/** @brief A link that carries nothing — enough to be staged, wired and looked up by identity. */
struct stub_link_t : tr::net::transport_t {
    /** @brief Egress is not what these tests measure. */
    void send(std::span<const std::byte> /*frame*/) override {}
};

/** @brief How many links the `stub_link_t` factory below has constructed. */
int g_stub_built = 0;

/**
 * @brief Declare @p kind for @p role under module @p module, with a factory that builds a
 *        `stub_link_t` and counts itself.
 *
 * Registering the module and registering the factory are two different registries: a create
 * that binds a STAGED link never reaches the factory, so a test that only needs the module
 * declaration can skip the type. Here both are present so "the factory ran" is observable.
 */
void declare_stub_kind(transport_vertex_t& net, std::string module, std::string kind,
                       conn_role_t role) {
    (void)net.register_module(std::move(module), kind, role);
    net.register_transport_type(
        std::move(kind),
        [](const tr::net::conn_settings_t&,
           const tr::wire::tlv_t*) -> tr::graph::result_t<std::unique_ptr<tr::net::transport_t>> {
            ++g_stub_built;
            return std::make_unique<stub_link_t>();
        });
}

/**
 * @brief #883 — two stagings sharing a leaf NAME each bind their OWN module.
 *
 * `provide_link` keys its staging `<module>/<name>`; the creation path used to match only the
 * substring after the last `/`, adopt the module of whichever key came first in the map's
 * lexicographic order, and never compare the module half against anything.
 *
 * The order below is what makes this non-vacuous. `mod-a/x` sorts first, so the connection
 * created FIRST here is the one meaning `mod-b` — under the old scan it mounted at
 * `/net/mod-a/x` wired to `mod-a`'s link, i.e. the wrong module AND the wrong transport,
 * silently. (The creating SPEC's own intent was the only thing that knew better, and the scan
 * never looked at it.) Since RFC-0014 S7 the creator's intent is not merely a config pair but
 * the ENDPOINT it writes to, so `provide_link`'s module half is matched against the module in
 * the path. The assertions therefore land BETWEEN the two creates: a successful create
 * consumes its staging, so by the time the second one runs only one staging is left and the
 * wrong answer has become indistinguishable from the right one.
 */
void test_same_leaf_name_under_two_modules() {
    std::printf("#883: two links staged under different modules, same leaf NAME:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_stub_kind(net, "mod-a", "stub-a", conn_role_t::DIAL);
    declare_stub_kind(net, "mod-b", "stub-b", conn_role_t::DIAL);

    stub_link_t link_a;
    stub_link_t link_b;
    net.provide_link("mod-a", "x", link_a);
    net.provide_link("mod-b", "x", link_b);
    g_stub_built = 0;

    // Each SPEC goes to the endpoint of the module that says WHICH staging it means.
    const auto wb = node.write(path_t("/net/mod-b/conn"), conn_spec("x", 0, "stub-b"));
    check(wb.has_value(), "SPEC{name=x} to mod-b's endpoint creates");
    check(node.find(path_t::parse("/net/mod-b/x")->key()).has_value(),
          "it mounts at /net/mod-b/x — the module whose endpoint took the SPEC");
    check(router.registry().by_name("net/mod-b/x") == &link_b,
          "and is wired to the link staged under mod-b, not to mod-a's by map order");
    check(!node.find(path_t::parse("/net/mod-a/x")->key()).has_value(),
          "mod-a gained nothing — its staging is untouched and still stageable");

    const auto wa = node.write(path_t("/net/mod-a/conn"), conn_spec("x", 0, "stub-a"));
    check(wa.has_value(), "SPEC{name=x} to mod-a's endpoint then creates too — not stranded");
    check(router.registry().by_name("net/mod-a/x") == &link_a,
          "net/mod-a/x is wired to the link staged under mod-a");
    check(g_stub_built == 0, "and neither factory ran — both creates found their own staging");
}

/**
 * @brief #883 — a kind-less SPEC that TWO declarations answer to is REFUSED, not guessed.
 *
 * The ruling this pins is unchanged; RFC-0014 S7 moved where it can be reached. While the
 * `:children[]` door resolved a kind-less SPEC by scanning the stagings, the ambiguity lived
 * between two stagings sharing a leaf NAME under different modules. That scan is gone — the
 * endpoint's path names the module outright — so the surviving ambiguity is the one
 * `declaration_for_locked` refuses: ONE module declared for two kinds, and a SPEC that names
 * neither. Declaration order carries no intent either, so a clear error still beats a silent
 * wrong bind (`core/src/transport_vertex.cpp`, the `hits > 1` arm, which cites this ruling).
 *
 * The refusal must also be total: the staging is not consumed, so naming the kind afterwards
 * still reaches it, and the module's other kind still constructs.
 */
void test_ambiguous_leaf_name_refused() {
    std::printf("#883: a kind-less SPEC matching two declarations is refused, not guessed:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    // ONE module, two kinds — the shape that leaves a kind-less SPEC nothing to choose by.
    declare_stub_kind(net, "mod", "stub-a", conn_role_t::DIAL);
    declare_stub_kind(net, "mod", "stub-b", conn_role_t::DIAL);

    stub_link_t link_a;
    net.provide_link("mod", "x", link_a);
    g_stub_built = 0;

    const auto w = node.write(path_t("/net/mod/conn"), conn_spec("x", 0));
    check(!w.has_value() && w.error() == status_t::TYPE_MISMATCH,
          "an ambiguous kind-less SPEC answers TYPE_MISMATCH");
    check(!node.find(path_t::parse("/net/mod/x")->key()).has_value(),
          "and the module gained no connection");
    check(router.registry().live_size() == 0, "nothing entered the router registry");

    // Total refusal: the staging survives, and the disambiguating SPEC still binds it.
    const auto wa = node.write(path_t("/net/mod/conn"), conn_spec("x", 0, "stub-a"));
    const auto wb = node.write(path_t("/net/mod/conn"), conn_spec("y", 0, "stub-b"));
    check(wa.has_value() && wb.has_value(), "naming the kind afterwards creates on both kinds");
    check(router.registry().by_name("net/mod/x") == &link_a,
          "so the refusal consumed the staging it could not choose");
    check(g_stub_built == 1, "and the OTHER kind's factory ran for the connection with no staging");
}

/**
 * @brief #883 — the endpoint's module decides; an unrelated staging cannot capture it.
 *
 * The staged scan used to run BEFORE the (kind, role) → module declaration, so a link staged
 * under `mod-a` swallowed a SPEC meant for `mod-b` — the kind's factory never ran and the
 * connection mounted in the wrong place. Since RFC-0014 S7 the module is the endpoint path
 * rather than a derived lookup, and the staging is still not merely outvoted: it is
 * untouched, and still binds its own module afterwards.
 */
void test_kind_module_beats_unrelated_staging() {
    std::printf("#883: an endpoint SPEC is not captured by a staging under another module:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_stub_kind(net, "mod-a", "stub-a", conn_role_t::DIAL);
    declare_stub_kind(net, "mod-b", "stub-b", conn_role_t::DIAL);

    stub_link_t staged;
    net.provide_link("mod-a", "x", staged);  // staged under mod-a; the SPEC below goes to mod-b
    g_stub_built = 0;

    const auto w = node.write(path_t("/net/mod-b/conn"), conn_spec("x", 0, "stub-b"));
    check(w.has_value(), "SPEC{name=x} to mod-b's endpoint creates");
    check(node.find(path_t::parse("/net/mod-b/x")->key()).has_value(),
          "it mounts under mod-b — the module whose endpoint took the SPEC");
    check(g_stub_built == 1, "the kind's FACTORY ran (the staging did not capture the create)");
    check(router.registry().by_name("net/mod-b/x") != nullptr &&
              router.registry().by_name("net/mod-b/x") != &staged,
          "and the wired link is the constructed one, not the staged one");
    check(!node.find(path_t::parse("/net/mod-a/x")->key()).has_value(),
          "nothing was mounted under the staging's own module");

    // Untouched: a kind-less SPEC to mod-a's OWN endpoint still binds the staging.
    const auto w2 = node.write(path_t("/net/mod-a/conn"), conn_spec("x", 0));
    check(w2.has_value() && router.registry().by_name("net/mod-a/x") == &staged,
          "the staging survived and still binds mod-a/x");
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
 * @brief #1025 — `make_connection_locked` arms the link only AFTER it is fully wired.
 *
 * A DIAL transport that starts its receive thread inside its own constructor is already
 * decoding while the creation path is still registering the vertex and running
 * `fwd_router_t::add_child` — so a message the peer pushes on connect lands in an empty
 * receiver slot and is dropped silently. The fix hands the ordering to the owner: the
 * factory builds the socket without a recv thread, and creation arms it here. This pins the
 * ORDER — the arm must come after the receiver install, not merely happen.
 */
void test_link_is_armed_after_wiring() {
    std::printf("creation arms the link only after add_child wired it (#1025):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);

    arm_probe_link_t link;
    check(link.starts.load() == 0, "a freshly constructed link has not been armed");
    net.provide_link("ws-client", "up", link);

    const auto w = node.write(path_t("/net/ws-client/conn"), conn_spec("up", 8080));
    check(w.has_value(), "the SPEC created the connection over the staged link");
    check(link.starts.load() > 0, "creation armed the link");
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
    std::vector<std::byte> payload;
    tr::wire::emit_tlv(payload, type_t::VALUE, opt_t{}, std::span<const std::byte>(&value, 1));
    return tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE, tr::testing::b_path(dst),
                              tr::testing::b_path({"reply-ep"}), {}, payload);
}

/**
 * @brief #1025 — a SPEC-created `kind=ws` DIAL delivers the message its peer pushed on connect.
 *
 * The other two guards each hold one end and neither covers the middle. The raw-peer case in
 * `ws_transport_test` constructs `transport_ws_client` DIRECTLY, so the factory's argument list
 * is never read; `test_link_is_armed_after_wiring` above goes in through `provide_link`, which
 * takes the staged-link branch and never calls a factory at all. The `defer_recv` argument the
 * built-in `ws` factory passes (`core/src/builtin_transport_ws.cpp`) sits between them: drop it
 * and the recv thread is spawned inside the constructor, racing whoever installs the receiver —
 * silently, because a decode into an empty `receiver_slot_t` moves no counter at all.
 *
 * So this drives the raw-peer harness through the PRODUCTION creation path — a graph write of
 * SPEC{kind=ws, addr, port} to the ws DIAL module's creator endpoint — and observes DOWNSTREAM
 * of the link, where a drop shows.
 * The peer writes its `101` and a COMPLETE BINARY message carrying `FWD{WRITE dst=/temp}` in
 * ONE `::send`, so the whole message is off the wire and in the client's handshake carry-over
 * before the constructor returns; two writes would leave the client parked in `recv` and
 * reproduce nothing. A DELIVERED frame reaches the terminus and lands in the LKV — `/temp`
 * takes a write it had not taken before. A frame decoded before the wiring reaches nothing,
 * and `/temp` stays as it was.
 *
 * **Reshaped by the #1548 S5 flip.** The `ws` kind's DIAL connections are now engine-managed,
 * which changes both halves of this case:
 *
 * - The dial no longer happens at CREATION. The SPEC mints the vertex `DORMANT` with no
 *   socket — asserted below, along with the peer NOT being dialed — and the standing binding
 *   (`acquire_link`) is what demands the link. That is the flip's own behaviour change, so it
 *   is checked here rather than merely tolerated.
 * - The window this case used to hold open with a map-lock gate is GONE by construction, and
 *   the gate went with it. `fwd_router_t::add_child` installs the receiver on the ENGINE — a
 *   stable routing identity that exists before any socket does — and every socket the engine
 *   later constructs is wired by `self_heal_link_t::wire_socket`, which installs the sinks and
 *   only THEN calls `start_receiving()`. The span the gate used to stretch (factory returns →
 *   receiver wired) no longer exists on this path; the ordering is a straight line inside the
 *   engine. `defer_recv` is still load-bearing, and for that ordering: without it the socket's
 *   recv thread starts inside its own constructor, before `wire_socket` has installed anything.
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
    // How long the test waits to conclude that NOTHING dialed the peer at creation. Three
    // orders of magnitude more than a loopback connect + handshake needs, so an eager factory
    // (the pre-#1548 shape) fails the dormancy check here deterministically, not flakily.
    constexpr auto kNoDialWindow = 200ms;

    std::promise<bool> one_write_done;  // the 101 and the message went out as ONE send
    std::promise<void> peer_answered;   // ...and it is on the wire NOW
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

    // Declared before the graph they serve: the counter and its callable outlive the vertex
    // they are subscribed on.
    std::atomic<int> temp_writes{0};
    auto on_temp = [&temp_writes](const tr::view::rope_t&) {
        temp_writes.fetch_add(1, std::memory_order_relaxed);
    };

    graph_t node;
    fwd_router_t router(node);
    (void)node.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
    (void)node.subscribe(path_t("/temp"), on_temp);
    const int before = temp_writes.load(std::memory_order_relaxed);

    {
        // The FULL ctor — the one that installs the built-in `kind` catalog, so `kind=ws`
        // below is served by core/src/builtin_transport_ws.cpp and by nothing else. Declared
        // in its own scope so the socket (and its recv thread) is torn down before the router
        // and graph it delivers into.
        transport_vertex_t net(node, router);
        declare_builtin_modules(net);  // ADR-0073 §4: the application mints the module names

        const auto w = node.write(path_t("/net/ws-client/conn"),
                                  conn_spec("up", ntohs(bound.sin_port), "ws", "127.0.0.1"));

        check(w.has_value(), "the SPEC created the connection through the built-in ws factory");
        check(net.link_of("net/ws-client/up") != nullptr,
              "the link is a CONSTRUCTED one (no provide_link staged anything here)");
        if constexpr (kBuiltinDialIsEngineManaged) {
            // The #1548 flip, observed from the peer's side: creation dialled NOTHING. The raw
            // listener never accepted, so the pusher thread is still parked in `accept`.
            check(read_link_state_byte(node, "/net/ws-client/up") == 0,
                  "the engine-managed DIAL is minted DORMANT (0x00) — no socket at creation");
            check(answered_fut.wait_for(kNoDialWindow) == std::future_status::timeout,
                  "and the peer was never dialled — the engine waits for demand (#1548)");
        }
        // The standing binding IS the demand: the engine dials, handshakes, and the peer's
        // push-on-connect rides the very first read the socket ever does. On a closed-out
        // build the socket is already up and this is the documented no-op.
        check(net.acquire_link("net/ws-client/up").has_value(),
              "acquire_link takes the standing hold and kicks the dial");
        check(one_write_fut.wait_for(5s) == std::future_status::ready && one_write_fut.get(),
              "the peer put the 101 and a COMPLETE pushed message in ONE write");

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

/** @brief One length-prefixed record: u32-LE len ++ payload (the M6 tcp transport framing). */
std::vector<std::byte> tcp_record(std::span<const std::byte> payload) {
    std::vector<std::byte> out(4);
    tr::detail::store_le(out, static_cast<std::uint32_t>(payload.size()), 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/**
 * @brief #1045 — a SPEC-created `kind=tcp` DIAL delivers the frame its peer pushed on connect.
 *
 * The `ws` twin above, for the transport this issue is scoped to. The same argument applies
 * unchanged: `tcp_test`'s raw-peer case constructs `tcp_transport_t` DIRECTLY, so the
 * factory's argument list is never read, and `test_link_is_armed_after_wiring` goes in through
 * `provide_link`, which takes the staged-link branch and calls no factory at all. The
 * `defer_recv` argument the built-in `tcp` factory passes
 * (`core/src/builtin_transport_tcp.cpp`) sits between them. Drop it and the recv thread is
 * spawned inside the constructor, racing the receiver install — silently, because a decode
 * into an empty `receiver_slot_t` moves no counter at all.
 *
 * So this drives a raw-peer harness through the PRODUCTION creation path — a graph write of
 * SPEC{kind=tcp, addr, port} to the tcp DIAL module's creator endpoint — and observes
 * DOWNSTREAM of the link, where a drop
 * shows. tcp has no handshake, so the peer simply writes ONE complete length-prefixed record
 * carrying `FWD{WRITE dst=/temp}` the moment it accepts, and then goes quiet. The factory
 * wires the router as the receiver, so a DELIVERED frame reaches the terminus and lands in
 * the LKV — `/temp` takes a write it had not taken before. A frame decoded before the wiring
 * reaches nothing, and `/temp` stays as it was.
 *
 * **Reshaped by the #1548 S5 flip**, exactly as the `ws` twin above and for the same two
 * reasons: creation is dormant (asserted here from the peer's side — nothing accepts until
 * `acquire_link` demands the link), and the map-lock gate that used to stretch the
 * factory-returns → receiver-wired span is retired because that span no longer exists on this
 * path. See the twin's contract note for the ordering the engine puts in its place.
 */
void test_factory_built_tcp_dial_delivers_push_on_connect() {
    std::printf("a SPEC-created kind=tcp DIAL delivers the peer's push-on-connect (#1045):\n");

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

    // The pushed frame: a remote WRITE of one byte into this node's own /temp.
    const std::byte kPushed{0x6B};
    const std::vector<std::byte> record = tcp_record(fwd_write({"temp"}, kPushed));
    // How long the test waits to conclude that NOTHING dialled the peer at creation — three
    // orders of magnitude more than a loopback connect needs, so an eager factory fails the
    // dormancy check deterministically rather than flakily.
    constexpr auto kNoDialWindow = 200ms;

    std::promise<bool> one_write_done;  // the whole record went out as ONE send
    std::promise<void> peer_answered;   // ...and it is on the wire NOW
    std::promise<void> test_done;       // safe to close the peer socket
    auto one_write_fut = one_write_done.get_future();
    auto answered_fut = peer_answered.get_future();
    auto done_fut = test_done.get_future();

    std::thread pusher([&] {
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            one_write_done.set_value(false);
            peer_answered.set_value();
            return;
        }
        // ONE syscall the instant the connection is accepted — the push-on-connect shape,
        // and what makes the window real.
        const ssize_t sent = ::send(cfd, record.data(), record.size(), 0);
        one_write_done.set_value(sent == static_cast<ssize_t>(record.size()));
        peer_answered.set_value();

        done_fut.wait();
        ::close(cfd);
    });

    // Declared before the graph they serve: the counter and its callable outlive the vertex
    // they are subscribed on.
    std::atomic<int> temp_writes{0};
    auto on_temp = [&temp_writes](const tr::view::rope_t&) {
        temp_writes.fetch_add(1, std::memory_order_relaxed);
    };

    graph_t node;
    fwd_router_t router(node);
    (void)node.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
    (void)node.subscribe(path_t("/temp"), on_temp);
    const int before = temp_writes.load(std::memory_order_relaxed);

    {
        // The FULL ctor — the one that installs the built-in `kind` catalog, so `kind=tcp`
        // below is served by core/src/builtin_transport_tcp.cpp and by nothing else. Declared
        // in its own scope so the socket (and its recv thread) is torn down before the router
        // and graph it delivers into.
        transport_vertex_t net(node, router);
        // ADR-0073 §4: the application mints the module name. `declare_builtin_modules` above
        // covers udp/ws; the tcp DIAL module is declared here, where it is used.
        check(net.register_module(std::string(tr::net::kTcpClientSuggestedModule), "tcp",
                                  conn_role_t::DIAL)
                  .has_value(),
              "the tcp DIAL module is declared");

        const auto w = node.write(path_t("/net/tcp-client/conn"),
                                  conn_spec("up", ntohs(bound.sin_port), "tcp", "127.0.0.1"));

        check(w.has_value(), "the SPEC created the connection through the built-in tcp factory");
        check(net.link_of("net/tcp-client/up") != nullptr,
              "the link is a CONSTRUCTED one (no provide_link staged anything here)");
        if constexpr (kBuiltinDialIsEngineManaged) {
            // The #1548 flip, observed from the peer's side: creation dialled NOTHING.
            check(read_link_state_byte(node, "/net/tcp-client/up") == 0,
                  "the engine-managed DIAL is minted DORMANT (0x00) — no socket at creation");
            check(answered_fut.wait_for(kNoDialWindow) == std::future_status::timeout,
                  "and the peer was never dialled — the engine waits for demand (#1548)");
        }
        // The standing binding IS the demand (a no-op on a closed-out, eager build).
        check(net.acquire_link("net/tcp-client/up").has_value(),
              "acquire_link takes the standing hold and kicks the dial");
        check(one_write_fut.wait_for(5s) == std::future_status::ready && one_write_fut.get(),
              "the peer put a COMPLETE pushed record on the wire in ONE write");

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
              "the pushed frame reached the receiver the factory-built link was wired to: "
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

    // A kind the endpoint's MODULE does not declare => SCHEMA_NOT_FOUND at the
    // declaration_for gate (ADR-0073 §4), no vertex. `udp-client` declares `udp` and nothing
    // else, so this exercises the declared-only gate — the factory lookup is never reached.
    const auto w1 = node.write(path_t("/net/udp-client/conn"),
                               conn_spec("x", kNeverBound, "pigeon", "127.0.0.1"));
    check(!w1.has_value() && w1.error() == status_t::SCHEMA_NOT_FOUND,
          "a kind the module does not declare => SCHEMA_NOT_FOUND");
    check(!node.find(path_t::parse("/net/udp-client/x")->key()).has_value(),
          "no /net/udp-client/x vertex was created");

    // The DISCRIMINATING twin: a DECLARED module whose kind has NO registered factory —
    // the creation now passes declaration_for and must fail at the factory-lookup gate with
    // the same status. Without this case the factory-missing branch is exercised by no
    // test (the pre-ADR-0073 "pigeon" case used to cover it).
    check(net.register_module("pigeon-x", "pigeon", conn_role_t::DIAL).has_value(),
          "a module can be declared for a kind with no factory (declaration is naming)");
    const auto w1b = node.write(path_t("/net/pigeon-x/conn"),
                                conn_spec("x2", kNeverBound, "pigeon", "127.0.0.1"));
    check(!w1b.has_value() && w1b.error() == status_t::SCHEMA_NOT_FOUND,
          "declared module, missing factory => SCHEMA_NOT_FOUND at the factory gate");
    check(!node.find(path_t::parse("/net/pigeon-x/x2")->key()).has_value(),
          "no vertex was created for the factory-less kind");

    // A udp DIAL without addr (and a LISTEN without a `port` KEY) => TYPE_MISMATCH, no vertex.
    const auto w2 = node.write(path_t("/net/udp-client/conn"), conn_spec("y", kNeverBound, "udp"));
    check(!w2.has_value() && w2.error() == status_t::TYPE_MISMATCH,
          "udp client without addr => TYPE_MISMATCH");
    // Built member-by-member because the `conn_spec` one-liner always emits `port` — the
    // key must be ABSENT for this case. Since #1362 an explicit `port = 0` is the EPHEMERAL
    // request (covered below), so "missing key" is the only remaining config error here.
    const auto w3 =
        node.write(path_t("/net/udp-server/conn"), tr::net::conn_spec_t("z").kind("udp").view());
    check(!w3.has_value() && w3.error() == status_t::TYPE_MISMATCH,
          "udp listener with NO port key => TYPE_MISMATCH");
    check(!node.find(path_t::parse("/net/udp-client/y")->key()).has_value() &&
              !node.find(path_t::parse("/net/udp-server/z")->key()).has_value(),
          "no vertices were created for the failed configs");

    // ...and the positive half of the same contract (#1362): an explicit `port = 0` is a
    // REQUEST, not an omission — the listener comes up on an OS-granted port, and the
    // grant is readable off the constructed link.
    const auto w6 = node.write(path_t("/net/udp-server/conn"), conn_spec("eph", 0, "udp"));
    check(w6.has_value(), "udp listener with port = 0 => EPHEMERAL, creation succeeds");
    auto* const eph = dynamic_cast<tr::net::udp_transport_t*>(net.link_of("net/udp-server/eph"));
    check(eph != nullptr && eph->ok() && eph->local_port() != 0,
          "the OS-granted bind port is readable back off the link");

    // No kind and no staged link => TYPE_MISMATCH on BOTH roles (#1062): the config is
    // missing a required field (the addr/port precedent), it is not an address to a
    // missing thing — NOT_FOUND would reach the peer as `tr::path::not_found`, RFC-0014's
    // reserved "no such creator endpoint" probe answer.
    //
    // Since S7 an endpoint SPEC that names no kind inherits its MODULE's declared kind, so
    // the kind-less shape is reached through a module declared with an EMPTY kind — the
    // `provide_link`-only spelling, with nothing staged under it here.
    check(net.register_module("bare-dial", "", conn_role_t::DIAL).has_value() &&
              net.register_module("bare-listen", "", conn_role_t::LISTEN).has_value(),
          "a module may be declared with no kind at all (the staged-link spelling)");
    const auto w4 = node.write(path_t("/net/bare-dial/conn"), conn_spec("w", 8080));
    check(!w4.has_value() && w4.error() == status_t::TYPE_MISMATCH,
          "no kind + no provide_link (DIAL) => TYPE_MISMATCH, not NOT_FOUND");
    const auto w5 = node.write(path_t("/net/bare-listen/conn"), conn_spec("v", 8080));
    check(!w5.has_value() && w5.error() == status_t::TYPE_MISMATCH,
          "no kind + no provide_link (LISTEN) => TYPE_MISMATCH, not NOT_FOUND");
    check(!node.find(path_t::parse("/net/bare-dial/w")->key()).has_value() &&
              !node.find(path_t::parse("/net/bare-listen/v")->key()).has_value(),
          "no vertices were created for the kind-less configs");
}

void test_link_of_accessor() {
    std::printf("link_of reaches a SPEC-constructed owned transport (#374):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_builtin_modules(net);  // ADR-0073 §4: the application mints the module names

    // A ws LISTENER built purely from config — the owned server socket is otherwise
    // unreachable by the app (no accessor existed before #374).
    const auto w = node.write(path_t("/net/ws-server/conn"), conn_spec("srv", kEphemeral, "ws"));
    check(w.has_value(), "a SPEC{kind=ws} at the LISTEN endpoint constructs the owned server");

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
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);
    // A first-level vertex the link would shadow.
    (void)node.register_vertex(path_t("/system"), role_t::STORED_VALUE);

    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "system", channel.a());  // stage a link named exactly "system"
    // #373 existed because a connection NAME was the FIRST dst segment, so a link sharing a
    // name with a first-level vertex black-holed every /system/... read onto the transport.
    // A connection is now addressed /net/<module>/<name> (RFC-0014, ADR-0061), so the two
    // names live in different places and cannot collide — the guard is not just unnecessary,
    // keeping it would wrongly reject a connection merely NAMED AFTER a local vertex.
    const auto w = node.write(path_t("/net/ws-client/conn"), conn_spec("system", 0));
    check(w.has_value(), "a link may now be named after a first-level vertex");
    check(node.find(path_t::parse("/net/ws-client/system")->key()).has_value(),
          "it mounts at /net/ws-client/system, nowhere near /system");
    check(node.find(path_t::parse("/system")->key()).has_value(),
          "the unrelated first-level /system vertex is untouched");
    check(router.registry().by_name("net/ws-client/system") == &channel.a(),
          "and it IS wired into the router under its mount path");

    // Control: a non-colliding name still works end-to-end.
    net.provide_link("ws-client", "uplink", channel.a());
    const auto w2 = node.write(path_t("/net/ws-client/conn"), conn_spec("uplink", 0));
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
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);
    // Only /system/mode is registered, so /system stays a structural placeholder. Under the
    // old flat routing the router shadowed that first segment; under mount addressing there
    // is nothing to shadow.
    (void)node.register_vertex(path_t("/system/mode"), role_t::STORED_VALUE);
    check(!node.find(path_t::parse("/system")->key()).has_value(),
          "/system is a placeholder (find sees no registered vertex there)");

    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "system", channel.a());
    const auto w = node.write(path_t("/net/ws-client/conn"), conn_spec("system", 0));
    check(w.has_value(), "a link name matching a placeholder first-level parent is accepted");
    check(router.registry().by_name("net/ws-client/system") == &channel.a(),
          "and it is wired under its mount path, leaving /system/mode reachable");
    channel.shutdown();
}

/**
 * @brief A ws LISTENER spec carrying the ws-private `peer_named` / `max_peers` keys.
 *
 * SPEC{ NAME "name" <name>, SETTINGS "config"{ NAME "port" VALUE u16, NAME "kind" NAME "ws",
 *       NAME "peer_named" VALUE u8, NAME "max_peers" VALUE u32 } } — written to the ws LISTEN
 * module's creator endpoint, which is what makes the role LISTEN (RFC-0014 §1).
 */
view_t ws_listener_spec(std::string_view name, std::uint16_t port, bool peer_named,
                        std::uint32_t max_peers) {
    return conn_spec_t(name)
        .port(port)
        .kind("ws")
        .flag("peer_named", peer_named)
        .u32("max_peers", max_peers)
        .view();
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
 * @brief An OS-granted port that nothing is listening on (bind 0, read it back, release).
 *
 * For the DIAL-side cases only — a connect that must be REFUSED, and the name-validation
 * SPECs that are rejected before any socket is dialled. A DIAL has to name a concrete port,
 * so it cannot ask for `0`; the LISTEN side does exactly that instead (#1362) and no longer
 * calls this. The tiny close-to-bind race is acceptable for a port whose whole job is to be
 * unserved.
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
    const auto plain =
        node.write(path_t("/net/udp-server/conn"), conn_spec("plain", kEphemeral, "udp"));
    check(plain.has_value(), "SPEC{kind=udp} with no max_frame constructs the socket");
    auto* const plain_link =
        dynamic_cast<tr::net::udp_transport_t*>(net.link_of("net/udp-server/plain"));
    check(plain_link != nullptr &&
              plain_link->effective_max_frame() == tr::net::udp_transport_t::kMaxDatagram,
          "without the key the connection carries the full datagram ceiling");

    // The SUBJECT: the same write plus `max_frame = 4096`.
    const auto capped =
        node.write(path_t("/net/udp-server/conn"),
                   conn_spec_t("capped").port(kEphemeral).kind("udp").max_frame(4096).view());
    check(capped.has_value(), "SPEC{kind=udp, max_frame=4096} constructs the socket");
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
 * its `bus()` was null, creation never installed `on_children`, and ADR-0044's peer
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
        node.write(path_t("/net/ws-server/conn"), conn_spec("plain", kEphemeral, "ws"));
    check(plain.has_value(), "SPEC{kind=ws} with no peer_named constructs the server");
    auto* const plain_srv =
        dynamic_cast<tr::net::transport_ws_server*>(net.link_of("net/ws-server/plain"));
    check(plain_srv != nullptr && plain_srv->bus() == nullptr,
          "without the key the SPEC-created server has a NULL bus (no ADR-0044 facet)");

    // ----- the SUBJECT: peer_named=1 => the bus facet, hence the synthesized listing. -----
    const auto w =
        node.write(path_t("/net/ws-server/conn"),
                   ws_listener_spec("bus", kEphemeral, /*peer_named=*/true, /*max_peers=*/8));
    check(w.has_value(), "SPEC{kind=ws, peer_named=1} constructs the owned server");
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
    //
    // Every DECLARED module is listed, not only the ones carrying a connection: since S2b a
    // declaration mints the module vertex and its `conn` endpoint eagerly, which is what
    // makes an empty module DISCOVERABLE — a creator has to find the endpoint before it can
    // write the first SPEC to it.
    check(enumerate_peers(node, "/net:children[]") ==
              std::set<std::string>{"udp-client", "udp-server", "ws-client", "ws-server"},
          "/net:children[] lists every declared module");
    // The `conn` endpoint is NOT listed (RFC-0014 §3, S4): the module's `:children[]` returns
    // its member CONNECTIONS, and the endpoint is the control that creates them.
    check(
        enumerate_peers(node, "/net/ws-server:children[]") == std::set<std::string>{"bus", "plain"},
        "/net/ws-server:children[] lists the two connections — no endpoint, no vertex per peer");
}

/**
 * @brief #929: a dial that could not come up answers TRANSPORT_DOWN, not NOT_FOUND.
 *
 * The disposition is the whole point. `NOT_FOUND` maps to `tr::path::not_found` (0x0020),
 * whose registry disposition is PERMANENT — "don't retry this as-is". A refused connect is
 * `tr::transport::down` (0x0060), TRANSIENT — "retry may succeed". Before #929 every
 * built-in factory's `!ok()` collapsed to `NOT_FOUND`, so a peer that reads the disposition
 * off the code stopped retrying a link that would have come back, and a genuinely wrong
 * address was indistinguishable from a link that was momentarily down.
 *
 * The dial is a real loopback connect to a port `free_port()` reserved and released, so
 * nothing is listening and the connect is REFUSED — the same `!ok()` a peer-not-up dial hits.
 */
void test_refused_dial_is_transport_down() {
    std::printf("#929: a refused dial reports TRANSPORT_DOWN (transient), not NOT_FOUND:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    check(net.register_module(std::string(tr::net::kTcpClientSuggestedModule), "tcp",
                              conn_role_t::DIAL)
              .has_value(),
          "the application declares a tcp DIAL module (ADR-0073 §4)");

    const std::uint16_t dead = free_port();
    check(dead != 0, "reserved and released a loopback port — nothing listens on it");
    // The FACTORY's own mapping, at its locus and on every build shape: `make_checked` is the
    // one place `!ok()` becomes a status, and a refused loopback connect is what a peer-not-up
    // dial hits. Asserted directly because since the #1548 S5 flip the engine no longer runs
    // this factory on the CREATION path, so a creating write is no longer where the mapping is
    // observable on a stock build.
    const auto direct = tr::net::make_checked<tr::net::tcp_transport_t>("127.0.0.1", dead);
    check(!direct.has_value() && direct.error() == status_t::TRANSPORT_DOWN,
          "a refused tcp dial => TRANSPORT_DOWN (the pre-#929 answer was NOT_FOUND)");

    const auto w =
        node.write(path_t("/net/tcp-client/conn"), conn_spec("refused", dead, "tcp", "127.0.0.1"));
    if constexpr (kBuiltinDialIsEngineManaged) {
        // The connect is DEFERRED to the engine's first dial (#1548), so creation succeeds and
        // the vertex rests DORMANT. Nothing about #929 changes: the status the factory answers
        // is the same one, it just reaches an op's drop census instead of the creating write.
        check(w.has_value(), "engine build: the creating write does not dial, so it succeeds");
        check(read_link_state_byte(node, "/net/tcp-client/refused") == 0,
              "and the connection rests DORMANT with the peer down");
    } else {
        check(!w.has_value() && w.error() == status_t::TRANSPORT_DOWN,
              "eager build: the refused dial fails the creating write with TRANSPORT_DOWN");
        check(!node.find(path_t::parse("/net/tcp-client/refused")->key()).has_value(),
              "and the refused dial left no connection vertex behind");
    }

    // The wire disposition this status now carries — the consequence the collapse inverted.
    check(tr::wire::err_disposition(tr::wire::err_t::TRANSPORT_DOWN) ==
              tr::wire::err_disposition_t::TRANSIENT,
          "tr::transport::down is TRANSIENT in the registry");
    check(tr::wire::err_disposition(tr::wire::err_t::PATH_NOT_FOUND) ==
              tr::wire::err_disposition_t::PERMANENT,
          "tr::path::not_found is PERMANENT — the disposition the collapse handed a peer");
}

/**
 * @brief ADR-0073 §4 / #621: modules are declared-only — a kind with a factory but no declared
 *        module has NO door at all, rather than silently mounting under a library-derived name.
 *
 * Ablation-verified: before the fix this creation SUCCEEDED and mounted the connection
 * under the derived `/net/udp-client/...`, so both checks below failed.
 *
 * RFC-0014 S7 made the guarantee STRUCTURAL rather than a gate: the only creation door is the
 * module's own `conn` endpoint, and declaring the module is what mints it. There is no config a
 * creator can send that reaches a module nobody declared, because the address it would have to
 * write to does not exist.
 *
 * The two doors answer that absence differently, and both arms are asserted below.
 *  - **The wire** — the one a peer uses — answers `tr::path::not_found`: a remote `FWD{WRITE}`
 *    to an unresolved `dst` does NOT create (RFC-0005 §D amendment 1), and the identity is
 *    pinned on real reply bytes by the `conn/absent-endpoint-not-found` vector in
 *    `test_conformance_vectors`.
 *  - **The in-process host API** takes the ordinary local write-creates rule (`mkdir -p`,
 *    CREATE-gated on the nearest ancestor): the call SUCCEEDS and mints a plain
 *    `role_t::STORED_VALUE` vertex at that address holding the SPEC's bytes as a value. That is
 *    not a creator endpoint and it constructs nothing — which is exactly what this test pins,
 *    because "it looked like it worked" is the failure mode a derived module name had.
 */
void test_unregistered_kind_is_schema_not_found() {
    std::printf("#621: an undeclared module has no creator endpoint (no derived name):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    // Deliberately NO register_module: `udp` has a transport FACTORY (linked built-in),
    // but this application never declared a module for it.
    check(!node.find(path_t::parse("/net/udp-client/conn")->key()).has_value(),
          "the library derived no `udp-client` module, so it minted no endpoint");
    const auto w =
        node.write(path_t("/net/udp-client/conn"), conn_spec("x", kNeverBound, "udp", "127.0.0.1"));
    // The local door's write-creates rule mints a value vertex here. What it does NOT do is
    // the whole claim: no connection, no socket, no route.
    check(w.has_value(), "the LOCAL door write-creates a plain vertex at the absent address");
    check(!node.find(path_t::parse("/net/udp-client/x")->key()).has_value(),
          "nothing mounted under the old derived /net/udp-client name");
    check(net.settings_of("net/udp-client/x") == nullptr,
          "no connection record exists — the SPEC reached no creation path");
    check(router.registry().size() == 0, "and no link entered the router's demux table");
    // Writing the same SPEC a second time now hits a REGISTERED vertex — and still creates
    // nothing, because that vertex is a stored value, not a `role_t::HANDLER` endpoint.
    const auto again =
        node.write(path_t("/net/udp-client/conn"), conn_spec("x", kNeverBound, "udp", "127.0.0.1"));
    check(again.has_value() && router.registry().size() == 0,
          "a second write assigns that value vertex and still constructs nothing");
}

/**
 * @brief RFC-0014 S2b: declaring a module MINTS its `conn` creator endpoint, and a `SPEC`
 *        write to that endpoint MATERIALIZES the connection.
 *
 * The endpoint door, end to end: no `type`, no `role` — the module in the path is both —
 * and the connection lands at `/net/<module>/<name>` with its link in the router's single
 * demux table. Since S7 retired the `:children[]` creation spelling this is the ONLY door.
 */
void test_conn_endpoint_spec_creates() {
    std::printf("RFC-0014 S2b: a SPEC write to /net/<module>/conn materializes a connection:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    check(net.register_module("ws-client", "ws", conn_role_t::DIAL).has_value(),
          "the application declares the module");
    check(node.find(path_t::parse("/net/ws-client/conn")->key()).has_value(),
          "declaring the module minted its creator endpoint at /net/<module>/conn");

    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "up", channel.a());
    const auto w = node.write(path_t("/net/ws-client/conn"), conn_spec_t("up").view());
    check(w.has_value(), "SPEC{name=up} written to the endpoint succeeds");
    check(node.find(path_t::parse("/net/ws-client/up")->key()).has_value(),
          "the connection vertex exists at /net/<module>/<name>");
    check(router.registry().size() == 1 &&
              router.registry().by_name("net/ws-client/up") == &channel.a(),
          "its link is wired into the router's child_registry_t");
    // The role is POSITIONAL: the module declared DIAL, and the endpoint fixed it without
    // the SPEC saying anything about a role.
    const auto* s = net.settings_of("net/ws-client/up");
    check(s != nullptr && s->role == conn_role_t::DIAL,
          "the role came from the module, not from the payload");
    channel.shutdown();
}

/** @brief RFC-0014 S2b: the endpoint dispatches a CONSTRUCTING SPEC too — the module's
 *         declared kind selects the factory and the real socket comes up. */
void test_conn_endpoint_constructs_socket() {
    std::printf("RFC-0014 S2b: an endpoint SPEC drives the transport factory:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    check(net.register_module("udp-server", "udp", conn_role_t::LISTEN).has_value(),
          "declare the listen module");
    const auto w =
        node.write(path_t("/net/udp-server/conn"), conn_spec_t("srv").port(kEphemeral).view());
    check(w.has_value(), "SPEC{name=srv, config{port}} constructs the bound socket");
    check(read_link_state_byte(node, "/net/udp-server/srv") == 4,
          "the constructed LISTEN reports LISTENING (0x04) — the role came from the module");
}

/**
 * @brief RFC-0014 §4 S5 (#1548): the BUILT-IN dial kinds declare `self_heal_dial`
 *        BUILD-CONDITIONED on `kSelfHealLinks` — asserted on BOTH shapes.
 *
 * This is the one case that runs on the `build-test-self-heal-closed` CI leg as well as the
 * stock one, and it asserts a DIFFERENT outcome on each. The subject is a `tcp` DIAL SPEC
 * naming a port nothing is listening on:
 *
 * - **`kSelfHealLinks = true`** (stock): creation SUCCEEDS and mints the vertex `DORMANT`.
 *   The factory did not run; the engine will dial on demand. That is the #1548 flip.
 * - **`kSelfHealLinks = false`** (the #1470 module gate): the built-ins consult the knob at
 *   their own registration site and declare `self_heal_dial = false`, so creation runs the
 *   factory eagerly and a refused connect answers `TRANSPORT_DOWN` — today's behaviour,
 *   unchanged. The LOUD `register_transport_type` refusal is reserved for a THIRD-PARTY kind
 *   that *claims* the engine on a build that excluded it; a built-in never claims one it
 *   cannot have, so it is not downgraded — it never asked. (Maintainer ruling 2026-08-25,
 *   option (a).) The observable proof that no refusal fired: the kind is still CATALOGUED —
 *   the write's answer is `TRANSPORT_DOWN`, the factory's own, not the `SCHEMA_NOT_FOUND` an
 *   uncatalogued kind gives.
 *
 * The engine's own behaviour is `link_liveness_test`'s (it skips itself on the closed build);
 * what is pinned here is only that the built-in kinds' DECLARATION tracks the knob.
 */
void test_builtin_dial_traits_track_the_build_knob() {
    std::printf("RFC-0014 S5 (#1548): the built-in tcp DIAL kind follows kSelfHealLinks:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    check(net.register_module("tcp-client", "tcp", conn_role_t::DIAL).has_value(),
          "declare the tcp dial module");

    // A port with nothing behind it: bind an ephemeral listener and drop it, so a connect
    // there is refused. This is the condition the two shapes answer differently.
    std::uint16_t dead_port = 0;
    {
        const tr::net::transport_tcp_server probe(0);
        check(probe.ok(), "an ephemeral probe listener bound");
        dead_port = probe.local_port();
    }
    const auto w =
        node.write(path_t("/net/tcp-client/conn"),
                   conn_spec_t("a").kind("tcp").addr("127.0.0.1").port(dead_port).view());

    if constexpr (kBuiltinDialIsEngineManaged) {
        check(w.has_value(), "engine build: creation with the peer DOWN succeeds (#1548)");
        check(read_link_state_byte(node, "/net/tcp-client/a") == 0,
              "... and the vertex is minted DORMANT (0x00) — no socket at creation");
    } else {
        check(!w.has_value() && w.error() == status_t::TRANSPORT_DOWN,
              "closed-out build: the built-in declares eager, so a dead peer fails at creation");
        check(!node.find(path_t::parse("/net/tcp-client/a")->key()).has_value(),
              "... and nothing landed");
    }
}

/**
 * @brief RFC-0014 S2b: a malformed endpoint write is REFUSED and leaves NO side effect.
 *
 * Every refusal below is checked twice — the status, and that the graph and the router's
 * demux table are exactly as they were. A creation that half-lands (a vertex with no route,
 * a socket with no vertex) is the failure mode the SPEC-atomicity clause exists to forbid.
 */
void test_conn_endpoint_malformed_refuses() {
    std::printf("RFC-0014 S2b: a malformed SPEC refuses with no side effects:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    (void)net.register_module("udp-client", "udp", conn_role_t::DIAL);
    const auto endpoint = path_t("/net/udp-client/conn");

    // 1. A payload that is neither SPEC nor NAME never falls through to an ordinary assign.
    std::vector<std::byte> value_tlv;
    tr::wire::emit_tlv(value_tlv, type_t::VALUE, opt_t{}, std::span<const std::byte>{});
    const auto bad_type = node.write(endpoint, owned(value_tlv));
    check(!bad_type.has_value() && bad_type.error() == status_t::TYPE_MISMATCH,
          "a VALUE payload => TYPE_MISMATCH (the endpoint never assigns)");

    // 2. `name` is REQUIRED and stays required (ADR-0073 §5) — no node-assigned fallback.
    const auto no_name = node.write(endpoint, conn_spec_t("").port(1).view());
    check(!no_name.has_value() && no_name.error() == status_t::TYPE_MISMATCH,
          "SPEC with an empty name => TYPE_MISMATCH (no node-assigned name)");

    // 3. The wire minting boundary runs the shared segment predicate (ADR-0073 §1).
    const auto bad_seg = node.write(endpoint, conn_spec_t("a/b").port(1).view());
    check(!bad_seg.has_value() && bad_seg.error() == status_t::INVALID_PATH,
          "SPEC naming an unaddressable segment => INVALID_PATH");

    // 4. The reserved name, refused create-side BEFORE any socket is built.
    const auto reserved = node.write(endpoint, conn_spec_t("conn").port(1).view());
    check(!reserved.has_value() && reserved.error() == status_t::PATH_IN_USE,
          "SPEC naming the reserved `conn` => PATH_IN_USE");
    check(node.find(path_t::parse("/net/udp-client/conn")->key()).has_value(),
          "the endpoint itself is untouched");

    // 5. A kind this module does not construct is an unsupported catalog entry.
    const auto wrong_kind =
        node.write(endpoint, conn_spec_t("x").kind("ws").addr("127.0.0.1").port(1).view());
    check(!wrong_kind.has_value() && wrong_kind.error() == status_t::SCHEMA_NOT_FOUND,
          "SPEC naming a kind the module does not construct => SCHEMA_NOT_FOUND");

    // 6. A config the kind cannot build from (a DIAL with no addr) fails in the factory.
    const auto no_addr = node.write(endpoint, conn_spec_t("y").port(kNeverBound).view());
    check(!no_addr.has_value() && no_addr.error() == status_t::TYPE_MISMATCH,
          "SPEC whose config is missing a required key => TYPE_MISMATCH");

    // NOTHING landed: no vertex, no route, no connection record.
    for (const char* leaf : {"x", "y"}) {
        std::string p = "/net/udp-client/";
        p += leaf;
        check(!node.find(path_t::parse(p)->key()).has_value(), "no vertex was minted");
    }
    check(router.registry().size() == 0, "no link was wired into the demux table");
    check(net.settings_of("net/udp-client/x") == nullptr &&
              net.settings_of("net/udp-client/y") == nullptr,
          "no connection record was kept");
}

/** @brief RFC-0014 S2b: `SPEC` naming an existing connection is `PATH_IN_USE` — the
 *         reject a retrying orchestrator reads as "already exists" (ADR-0073 §5). */
void test_conn_endpoint_spec_in_use_is_idempotent_safe() {
    std::printf("RFC-0014 S2b: a re-sent SPEC answers PATH_IN_USE (retry-idempotent):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);
    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "up", channel.a());
    check(node.write(path_t("/net/ws-client/conn"), conn_spec_t("up").view()).has_value(),
          "the first create succeeds");
    const auto again = node.write(path_t("/net/ws-client/conn"), conn_spec_t("up").view());
    check(!again.has_value() && again.error() == status_t::PATH_IN_USE,
          "the retry answers PATH_IN_USE — one connection, and the client learns so");
    check(router.registry().size() == 1, "still exactly one link in the demux table");
    channel.shutdown();
}

/** @brief RFC-0014 S2b: a `NAME` write to the same endpoint REMOVES the connection —
 *         absent name is a no-op success, the reserved name is refused. */
void test_conn_endpoint_name_removes() {
    std::printf("RFC-0014 S2b: a NAME write to /net/<module>/conn removes the connection:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);
    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "up", channel.a());
    (void)node.write(path_t("/net/ws-client/conn"), conn_spec_t("up").view());
    const auto endpoint = path_t("/net/ws-client/conn");

    // The endpoint cannot self-destruct (RFC-0014 §2/§3 remove-side).
    const auto self = node.write(endpoint, tr::net::conn_remove("conn"));
    check(!self.has_value() && self.error() == status_t::PERMISSION_DENIED,
          "NAME{conn} => PERMISSION_DENIED (the endpoint never routes to retire)");
    check(node.find(path_t::parse("/net/ws-client/conn")->key()).has_value(),
          "the endpoint still resolves");

    // An unresolvable name is a NO-OP SUCCESS — the retried-remove leg.
    check(node.write(endpoint, tr::net::conn_remove("never")).has_value(),
          "NAME naming no connection is a no-op success");

    check(node.write(endpoint, tr::net::conn_remove("up")).has_value(), "NAME{up} removes it");
    check(!node.find(path_t::parse("/net/ws-client/up")->key()).has_value(),
          "the connection vertex is retired");
    check(router.registry().by_name("net/ws-client/up") == nullptr,
          "and it is un-routed: the NAME no longer resolves to a link");
    // Removing it again is the same no-op success, so a retried teardown is safe too.
    check(node.write(endpoint, tr::net::conn_remove("up")).has_value(),
          "a repeated remove is a no-op success");
    channel.shutdown();
}

/**
 * @brief RFC-0014 §3 (S4): `conn` is HIDDEN from `/net/<module>:children[]`, and hiding it
 *        costs nothing else — the endpoint stays addressable, writable, and probe-able.
 *
 * The two halves are inseparable and that is the point of testing them together. §3 says the
 * module's listing returns its member CONNECTIONS, so the endpoint must not appear; §6 makes
 * `read <module>/conn:schema` the sanctioned creatability probe, so the endpoint must still
 * resolve. "Hidden" that also meant "unreachable" would break the discovery contract that
 * exists BECAUSE it is hidden.
 */
void test_conn_endpoint_is_hidden_from_enumeration() {
    std::printf("RFC-0014 §3 (S4): the conn endpoint is unlisted but still addressable:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);

    // A declared module with no connection yet lists NOTHING — not even its own endpoint.
    check(enumerate_peers(node, "/net/ws-client:children[]").empty(),
          "an empty module's :children[] is empty — the endpoint is not a member");
    // Not vacuous: the module itself IS discoverable one level up, which is how a creator
    // finds the endpoint it cannot see in the module's own listing.
    check(enumerate_peers(node, "/net:children[]") == std::set<std::string>{"ws-client"},
          "/net:children[] still lists the declared module");

    // Unlisted is not unreachable — the endpoint resolves, and the SPEC write still executes.
    const auto endpoint = path_t("/net/ws-client/conn");
    check(node.find(path_t::parse("/net/ws-client/conn")->key()).has_value(),
          "the hidden endpoint still RESOLVES (RFC-0014 §6's creatability probe needs it to)");
    check(node.read(path_t("/net/ws-client/conn:schema")).has_value(),
          "and its :schema facet still reads (the probe's own verb)");

    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "up", channel.a());
    check(node.write(endpoint, conn_spec_t("up").view()).has_value(),
          "a SPEC written to the hidden endpoint still creates the connection");

    // Now the listing holds exactly the member connection — the endpoint stays out of it.
    check(enumerate_peers(node, "/net/ws-client:children[]") == std::set<std::string>{"up"},
          "/net/ws-client:children[] lists the connection and only the connection");
    channel.shutdown();
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
    const auto wb = node_b.write(path_t("/io/l/conn"), conn_spec("a", kEphemeral, "udp"));
    check(wb.has_value(), "B: a SPEC to /io/l/conn creates under the app-chosen /io/l");
    check(node_b.find(path_t::parse("/io/l/a")->key()).has_value(),
          "B: the connection vertex is /io/l/a — the app's shape, not the library's");
    check(router_b.registry().by_name("io/l/a") != nullptr, "B: the socket is routed under io/l/a");
    auto* const b_link = dynamic_cast<tr::net::udp_transport_t*>(net_b.link_of("io/l/a"));
    const std::uint16_t b_port = b_link != nullptr ? b_link->local_port() : 0;
    check(b_port != 0, "B: the ephemeral bind port is readable back off the app-planed link");

    const auto wa = node_a.write(path_t("/io/w/conn"), conn_spec("b", b_port, "udp", "127.0.0.1"));
    check(wa.has_value(), "A: a SPEC to /io/w/conn creates under the app-chosen /io/w");
    check(node_a.find(path_t::parse("/io/w/b")->key()).has_value(),
          "A: the connection vertex is /io/w/b");
    const auto* s = net_a.settings_of("io/w/b");
    check(s != nullptr && s->kind == "udp" && s->port == b_port,
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
 * @brief #1096 — an embedder walking `for_each_vertex` can partition this net plane's
 *        STRUCTURAL vertices from everything else.
 *
 * The exact partition the issue asks for, taken from ONE walk of the live graph: `/net` and
 * `/net/<module>` are structural; the connection leaf `/net/<module>/<name>` is not; and an
 * application's own structural vertex `/zone` — registered `STORED_VALUE`, holding nothing but
 * a child, visited identically — is not either. The `/zone` arm is the load-bearing one: it
 * is what stops the predicate degenerating into "two segments under the root", which is the
 * path-shape workaround this issue exists to retire.
 */
void test_structural_vertex_partition() {
    std::printf("#1096: for_each_vertex partitions structural vertices from value vertices:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);

    tr::net::loopback_channel_t channel;
    net.provide_link("ws-client", "up", channel.a());
    const auto w = node.write(path_t("/net/ws-client/conn"), conn_spec("up", 8080));
    check(w.has_value(), "the connection /net/ws-client/up is created");

    // The application's OWN structural vertex — same role, same shape, same walk. Nothing in
    // the graph distinguishes it from /net/ws-client, which is exactly why graph_t must not
    // be the one answering (the doctrine ruling; docs/reference/11 §structural vertices).
    (void)node.register_vertex(path_t("/zone"), role_t::STORED_VALUE);
    (void)node.register_vertex(path_t("/zone/temp"), role_t::STORED_VALUE);

    std::set<std::string> structural;
    std::set<std::string> ordinary;
    node.for_each_vertex([&](tr::wire::key_view_t key, tr::graph::vertex_handle_t) {
        // Spell the key back as a `/`-path for the assertions below.
        std::string spelled;
        std::span<const std::byte> rest = key.bytes();
        while (!rest.empty()) {
            // Packed records (RFC-0018): `[u8 len][bytes]`.
            const auto len = static_cast<std::size_t>(static_cast<std::uint8_t>(rest[0]));
            if (len == 0 || rest.size() < 1 + len) break;
            spelled.push_back('/');
            spelled += tr::detail::as_string_view(rest.subspan(1, len));
            rest = rest.subspan(1 + len);
        }
        (net.is_structural(key) ? structural : ordinary).insert(spelled);
    });

    check(structural.contains("/net"), "the net root /net is structural");
    check(structural.contains("/net/ws-client"), "the module segment /net/ws-client is structural");
    check(!structural.contains("/net/ws-client/up") && ordinary.contains("/net/ws-client/up"),
          "the connection leaf /net/ws-client/up is NOT structural");
    check(!structural.contains("/zone") && ordinary.contains("/zone"),
          "an app's OWN structural vertex /zone is not one of the net plane's (not a path shape)");
    check(!structural.contains("/zone/temp") && ordinary.contains("/zone/temp"),
          "an app leaf /zone/temp is NOT structural");
    check(structural.size() == 2, "exactly two structural vertices — the root and the module");
    channel.shutdown();
}

/**
 * @brief #1096 — the edges of the predicate: an app-chosen root, an undeclared module name,
 *        the graph root, and the `/net/<module>/conn` creator endpoint.
 */
void test_structural_vertex_edges() {
    std::printf("#1096: structural-vertex edges (custom root, undeclared module, conn):\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router, "/io");
    tr::net::loopback_channel_t channel;
    // The KIND-LESS spelling: the module is declared for no kind at all, so nothing can be
    // constructed under it and the staged link is the only thing that can carry the bytes.
    (void)net.register_module("w", "", conn_role_t::DIAL);
    net.provide_link("w", "b", channel.a());
    const auto w = node.write(path_t("/io/w/conn"), conn_spec("b", 9000));
    check(w.has_value(), "the connection /io/w/b is created under the app-chosen root");

    // The `path_t` must OUTLIVE the call — `key()` is a span into it, not an owned copy.
    const auto structural_at = [&](std::string_view p) {
        const auto path = path_t::parse(p);
        return net.is_structural(tr::wire::key_view_t(path->key()));
    };
    check(structural_at("/io"), "the app-chosen net root /io is structural");
    check(structural_at("/io/w"),
          "a module declared for no kind (the provide_link spelling) is structural too");
    check(!structural_at("/io/w/b"), "the connection leaf /io/w/b is not");
    check(!structural_at("/io/nope"), "a module name of no connection and no declaration is not");
    check(!structural_at("/net"),
          "another plane's default root is not structural for a net plane rooted at /io");
    check(!net.is_structural(tr::wire::key_view_t{}),
          "the graph root is nobody's structural vertex");
    // RFC-0014's per-module creator endpoint is an addressable control surface with its own
    // :schema catalog — not a grouping segment.
    check(!structural_at("/io/w/conn"),
          "the /net/<module>/conn creator endpoint is not structural");
    channel.shutdown();
}

/**
 * @brief #688 open trace — a peer-created child DOES reach `fwd_router_t::add_child`, and
 *        THE segment predicate runs strictly before it.
 *
 * The trace the ruling left open, executed against the PRODUCTION wiring rather than a
 * hand-built harness (the RFC-0014 lesson: two silent misroutes shipped because no test
 * drove the real path). Since S7 retired the `:children[]` creation door the reachable chain
 * is `graph_t::write(/net/<module>/conn) -> transport_vertex_t::on_write ->
 * endpoint_write -> endpoint_create_locked -> make_connection_locked ->
 * fwd_router_t::add_child`, so a peer-supplied `SPEC` name still becomes the last segment of
 * a routable MOUNT name — which is exactly why the predicate must gate it, and why gating it
 * anywhere later would be too late.
 *
 * The positive control proves the chain is real (a legal name lands in the router's
 * registry); each reserved-character case proves the door shuts first — `INVALID_PATH`
 * from `endpoint_create_locked`, before the declaration lookup, before key composition,
 * before the factory, hence before `add_child`. Ablation-verified: delete the
 * `valid_segment` call in `transport_vertex_t::endpoint_create_locked` and every negative
 * case below registers a mount named `net/udp-client/a:b` (etc.) that no conforming `dst`
 * can address.
 *
 * `routable_mount_name` (fwd_router.cpp, #523) is a SIBLING of the predicate, not a
 * second copy of it: it bounds a whole multi-segment mount PATH, where `/` is legal and
 * `kMaxSegments` is the bound; `valid_segment` bounds ONE segment, where `/` is not. It
 * accepts every name below, so it is not, and must not be mistaken for, this gate.
 */
void test_wire_name_reaches_add_child() {
    std::printf("#688 trace: endpoint_write -> make_connection -> add_child, predicate first:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_builtin_modules(net);  // ADR-0073 §4: the application mints the module names

    // ----- POSITIVE CONTROL: the chain is real. A legal peer name becomes a mount. -----
    const auto endpoint = path_t("/net/udp-client/conn");
    const auto ok = node.write(endpoint, conn_spec("ok-name", free_port(), "udp", "127.0.0.1"));
    check(ok.has_value(), "a legal peer-supplied name creates the connection");
    check(router.registry().by_name("net/udp-client/ok-name") != nullptr,
          "and it REACHED fwd_router_t::add_child — the peer's name is now a mount segment");
    const std::size_t mounts_after_control = router.registry().size();
    check(mounts_after_control == 1, "exactly one mount is registered so far");

    // ----- SUBJECT: one reserved character per case, same production door. -----
    for (const std::string_view bad : {"a/b", "a:b", "a.b", "a*b", "a?b"}) {
        const auto w = node.write(endpoint, conn_spec(bad, free_port(), "udp", "127.0.0.1"));
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

/** @brief Lowercase hex of a byte buffer, for the golden-SPEC comparisons below. */
std::string to_hex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::byte b : bytes) {
        const auto v = std::to_integer<std::uint8_t>(b);
        out.push_back(kDigits[v >> 4]);
        out.push_back(kDigits[v & 0x0F]);
    }
    return out;
}

/**
 * @brief The public builder's bytes, PINNED — the creator-endpoint SPEC, byte for byte.
 *
 * The goldens below are the pre-#902 hand-emit's, with the `type` pair and the `role` pair
 * REMOVED and every enclosing length restated — which is exactly the wire delta RFC-0014 S7
 * makes, and the reason they are still worth pinning as literals rather than re-encoding the
 * builder's own output: a future edit that changes a byte fails HERE rather than in whichever
 * transport test happens to notice. They also pin the two structural choices a reader would
 * otherwise have to infer: an untouched builder emits NO `config` (the `provide_link`
 * spelling), and setters append in CALL order.
 *
 * The `type` and `role` keys have no spelling left to pin: `conn_spec_t` cannot emit either,
 * and `test_conformance_vectors` asserts their ABSENCE from the committed `conn/` vectors.
 */
void test_conn_spec_bytes_pinned() {
    std::printf("conn_spec_t bytes (#902): pinned against the endpoint SPEC's wire:\n");
    check(to_hex(conn_spec_t("up").bytes()) == "0e400e00020004006e616d65020002007570",
          "no setter ran => SPEC{name} with no config at all");
    check(to_hex(conn_spec_t("up").port(8080).bytes()) ==
              "0e402a00020004006e616d6502000200757002000600636f6e6669670b400e0002000400706f7274"
              "01000200901f",
          "port=8080");
    check(to_hex(conn_spec_t("srv").port(47131).kind("udp").bytes()) ==
              "0e403a00020004006e616d650200030073727602000600636f6e6669670b401d0002000400706f7274"
              "010002001bb8020004006b696e6402000300756470",
          "port + kind");
    check(to_hex(conn_spec_t("cli").port(47131).kind("udp").addr("127.0.0.1").bytes()) ==
              "0e404f00020004006e616d6502000300636c6902000600636f6e6669670b40320002000400706f7274"
              "010002001bb8020004006b696e64020003007564700200040061646472020009003132372e302e302e"
              "31",
          "port + kind + addr");
    check(to_hex(conn_spec_t("c")
                     .port(0)
                     .keepalive_ms(1)
                     .max_frame(2)
                     .backoff_ms(3)
                     .connect_timeout_ms(4)
                     .bytes()) ==
              "0e408100020004006e616d65020001006302000600636f6e6669670b40660002000400706f72740100"
              "0200000002000900"
              "6b656570616c6976650100040001000000020009006d61785f6672616d6501"
              "00040002000000020007006261636b6f6666010004000300000002000f00636f6e6e6563745f7469"
              "6d656f75740100040004000000",
          "all four u32 keys, in call order");
    // The one-call sugar is the same encoder, not a second one. Its pair order is `kind`,
    // `addr`, `port` — the order the `conn/create-via-spec` conformance vector carries and the
    // TypeScript `encodeConnSpec` emits, so the one vector pins all three cores. The chain form
    // still appends in CALL order, which is why the equality below spells the same order.
    check(to_hex(conn_spec("cli", 47131, "udp", "127.0.0.1").bytes()) ==
              to_hex(conn_spec_t("cli").kind("udp").addr("127.0.0.1").port(47131).bytes()),
          "conn_spec(...) delegates to conn_spec_t — same bytes");
}

/**
 * @brief Every field combination decodes back through the CONSUMER's own reader.
 *
 * `transport_vertex_t::parse_config` is a `config_reader_t` walk over the SPEC's `config`
 * SETTINGS, so running that reader over the builder's output is the encoder↔decoder
 * round-trip for the whole universal-key vocabulary — including the combinations no
 * transport test happens to create (a kind-less SPEC carrying an `addr`, and one carrying
 * neither). The creation path proper is covered by the config-constructed tests above, which
 * now write these very bytes.
 *
 * The `role` dimension this loop used to carry is GONE, not dropped for brevity: S7 retired
 * the `role` config pair with the `:children[]` door it belonged to, `conn_spec_t` cannot
 * emit one, and `parse_config` no longer reads one. The role is the module's, so it never
 * crosses this encode↔decode seam at all.
 */
void test_conn_spec_round_trips_through_the_reader() {
    std::printf("conn_spec_t (#902): round-trips through config_reader_t, all combinations:\n");
    for (const std::string_view kind : {std::string_view{}, std::string_view{"udp"}}) {
        for (const std::string_view addr : {std::string_view{}, std::string_view{"127.0.0.1"}}) {
            const view_t spec = conn_spec("x", 47000, kind, addr);
            const auto decoded = tr::wire::decode(spec);
            if (!decoded) {
                check(false, "the built SPEC decodes");
                continue;
            }
            const tr::wire::tlv_t* config = nullptr;
            for (const tr::wire::tlv_t& child : decoded->children) {
                if (child.type == type_t::SETTINGS) config = &child;
            }
            const tr::net::config_reader_t cfg(config);
            const bool ok = config != nullptr && !cfg.u8("role") && cfg.u16("port") &&
                            *cfg.u16("port") == 47000 &&
                            cfg.name("kind").value_or(std::string_view{}) == kind &&
                            cfg.name("addr").value_or(std::string_view{}) == addr;
            check(ok, "port/kind/addr survive the encode→decode round trip, and no `role` does");
        }
    }
}

/** @brief The raw bytes of a conformance vector's `input.bin`. */
std::vector<std::byte> vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

/**
 * @brief Arena-decode a FWD and resolve it against @p resolver — `fwd_router_t`'s own terminus
 *        wiring (ADR-0041), which is the door a probe arriving over the network comes through.
 */
tr::graph::result_t<tr::view::rope_t> resolve_bytes(tr::graph::op_resolver_t& resolver,
                                                    std::span<const std::byte> fwd) {
    const auto arena = tr::wire::decode_into(fwd, tr::mem::heap_source());
    if (!arena) return std::unexpected(status_t::INVALID_PATH);
    return resolver.resolve(*arena, {});
}

/**
 * @brief The registered u16 error identity of a `STATUS{ ERROR{ VALUE u16 LE } }` reply payload
 *        (RFC-0002 §C) — `0` when the shape does not match, which no registered code is.
 */
std::uint16_t status_error_code(const tr::wire::tlv_t& status) {
    if (status.type != type_t::STATUS || status.children.size() != 1) return 0;
    const tr::wire::tlv_t& err = status.children[0];
    if (err.type != type_t::ERROR || !err.opt.pl || err.children.empty()) return 0;
    const tr::wire::tlv_t& id = err.children[0];
    if (id.type != type_t::VALUE || id.payload.size() != 2) return 0;
    return tr::detail::load_le<std::uint16_t>(id.payload);
}

/** @brief The `kind` byte and the `STATUS`/payload child of a decoded FWD{REPLY}. */
struct reply_parts_t {
    tr::view::view_t flat;       /**< @brief The flattened reply the tlv below borrows. */
    tr::wire::tlv_t tlv;         /**< @brief The decoded FWD{REPLY}. */
    std::uint8_t kind = 0xFF;    /**< @brief `reply_kind_t` — RESULT or ERROR. */
    std::uint16_t code = 0xFFFF; /**< @brief The registered error identity, when kind is ERROR. */
};

/** @brief Flatten, decode and split a reply rope into @ref reply_parts_t. */
reply_parts_t reply_parts(const tr::view::rope_t& reply) {
    reply_parts_t out;
    out.flat = reply.flatten();
    const auto dec = tr::wire::decode(out.flat.bytes());
    if (!dec) return out;
    out.tlv = *dec;
    // `op, dst, src, kind [, payload]` — a RESULT that carries nothing (an executed WRITE) has
    // four children, an ERROR always has the STATUS as its fifth.
    if (out.tlv.children.size() < 4) return out;
    if (out.tlv.children[3].type == type_t::VALUE && !out.tlv.children[3].payload.empty())
        out.kind = tr::detail::load_le<std::uint8_t>(out.tlv.children[3].payload);
    out.code = out.tlv.children.size() >= 5 ? status_error_code(out.tlv.children[4]) : 0;
    return out;
}

/**
 * @brief RFC-0014 S7-A — the seven `conn/` control-plane vectors, bound to the live endpoint.
 *
 * The harness that scores those vectors decodes and re-encodes `input.bin` and stops there
 * (HARNESS.md §"What a vector gates"), so every behavioural claim their `description.md` files
 * make is, on the conformance surface alone, unfalsifiable: delete the creator endpoint
 * outright and all seven still score `ok`. This is where they can be false.
 *
 * Two doors are exercised on purpose, because RFC-0014 pins two different things:
 *
 * - **The local API door** (`graph_t::write` on the endpoint path) carries the EFFECT clauses —
 *   what exists afterwards, what is routed, what the config parsed to, what did NOT move.
 * - **The wire door** (`op_resolver_t` over a real `FWD`) carries the ERROR-IDENTITY clauses.
 *   §Compatibility's error identities are wire codes, and a `status_t` enumerator is not one;
 *   the mapping from status to registered code lives in `core/src/fwd_reply.cpp` and is only
 *   observable in a reply. Asserting `status_t::PERMISSION_DENIED` would leave `0x0050` free to
 *   drift, which is exactly the class of gap the vectors exist to close.
 *
 * Every refusal is checked twice — the answer, and that nothing moved — and every vector that
 * asserts an absence is paired with the presence that makes the absence meaningful.
 */
void test_conformance_vectors() {
    std::printf("RFC-0014 S7-A: the conn/ vectors are the bytes the creator endpoint obeys:\n");

    const auto endpoint = path_t("/net/ws-client/conn");
    const std::vector<std::byte> v_create = vector_bytes("conn/create-via-spec");
    const std::vector<std::byte> v_in_use = vector_bytes("conn/spec-name-in-use");
    const std::vector<std::byte> v_remove = vector_bytes("conn/remove-via-name");
    const std::vector<std::byte> v_noop = vector_bytes("conn/remove-nonexistent-noop");
    const std::vector<std::byte> v_reserved = vector_bytes("conn/remove-reserved-rejected");
    const std::vector<std::byte> v_bad_type = vector_bytes("conn/bad-payload-type");
    const std::vector<std::byte> v_probe = vector_bytes("conn/absent-endpoint-not-found");

    // --- the bytes ARE the production builders' output ----------------------------------
    // Pinned first, so a builder edit that changes a byte fails here rather than leaving the
    // committed vector and the shipped encoder quietly disagreeing.
    check(to_hex(v_create) == to_hex(conn_spec_t("up").addr("127.0.0.1").port(8080).bytes()),
          "conn/create-via-spec == conn_spec_t's endpoint spelling (byte-exact)");
    check(to_hex(v_in_use) == to_hex(conn_spec_t("up").addr("10.0.0.9").port(9000).bytes()),
          "conn/spec-name-in-use == the same builder, same name, different config");
    check(to_hex(v_remove) == to_hex(tr::net::conn_remove("up").bytes()),
          "conn/remove-via-name == tr::net::conn_remove(\"up\")");
    check(to_hex(v_noop) == to_hex(tr::net::conn_remove("never").bytes()),
          "conn/remove-nonexistent-noop == conn_remove of a name that resolves to nothing");
    check(to_hex(v_reserved) == to_hex(tr::net::conn_remove("conn").bytes()),
          "conn/remove-reserved-rejected == conn_remove of the reserved endpoint name");
    // bad-payload-type IS the create vector's own `config` child, envelope stripped — the
    // claim its description makes about why it is refused, checked on the bytes themselves.
    const std::string create_hex = to_hex(v_create);
    check(create_hex.find(to_hex(v_bad_type)) != std::string::npos,
          "conn/bad-payload-type IS conn/create-via-spec's config child, SPEC envelope removed");
    // The positional-role claim, read off the wire: the endpoint SPEC carries neither a
    // `role` pair nor a `type` pair for the endpoint to obey (RFC-0014 §1).
    std::vector<std::byte> role_key;
    tr::wire::emit_name(role_key, "role");
    std::vector<std::byte> type_key;
    tr::wire::emit_name(type_key, "type");
    check(create_hex.find(to_hex(role_key)) == std::string::npos,
          "the endpoint SPEC carries no `role` key — the role is the module's (RFC-0014 §1)");
    check(create_hex.find(to_hex(type_key)) == std::string::npos,
          "... and no `type` key — the module in the path is the transport");

    // --- the effects, through the live endpoint ------------------------------------------
    {
        graph_t node;
        fwd_router_t router(node);
        transport_vertex_t net(node, router);
        check(net.register_module("ws-client", "ws", conn_role_t::DIAL).has_value(),
              "the application declares the module that mints the endpoint");

        // conn/bad-payload-type runs FIRST, on an empty module, so "nothing was created" is a
        // statement about an emptiness this write could have broken.
        const auto bad = node.write(endpoint, owned(v_bad_type));
        check(!bad.has_value() && bad.error() == status_t::TYPE_MISMATCH,
              "conn/bad-payload-type => TYPE_MISMATCH (a SETTINGS names no operation)");
        // The RFC names the EMPTY payload in the same clause; same door, no bytes to pin.
        const auto empty = node.write(endpoint, view_t{});
        check(!empty.has_value() && empty.error() == status_t::TYPE_MISMATCH,
              "... and so is an empty payload (the clause's other arm)");
        check(enumerate_peers(node, "/net/ws-client:children[]").empty() &&
                  router.registry().size() == 0,
              "neither refusal created a connection or wired a link");

        tr::net::loopback_channel_t channel;
        net.provide_link("ws-client", "up", channel.a());

        const auto created = node.write(endpoint, owned(v_create));
        check(created.has_value(), "conn/create-via-spec is ACCEPTED by the creator endpoint");
        check(node.find(path_t::parse("/net/ws-client/up")->key()).has_value(),
              "... and the connection vertex /net/ws-client/up exists");
        check(router.registry().by_name("net/ws-client/up") == &channel.a(),
              "... with its link in the router's single demux table");
        const auto* s = net.settings_of("net/ws-client/up");
        check(s != nullptr && s->addr == "127.0.0.1" && s->port == 8080,
              "... and the vector's config reached conn_settings_t (addr/port)");
        check(s != nullptr && s->role == conn_role_t::DIAL,
              "... and the role came from the MODULE — nothing on the wire said DIAL");

        // conn/spec-name-in-use: refused, and it changed nothing it named.
        const auto again = node.write(endpoint, owned(v_in_use));
        check(!again.has_value() && again.error() == status_t::PATH_IN_USE,
              "conn/spec-name-in-use => PATH_IN_USE (retry-idempotent, never a second link)");
        check(router.registry().size() == 1, "... still exactly one link in the demux table");
        const auto* s2 = net.settings_of("net/ws-client/up");
        check(s2 != nullptr && s2->addr == "127.0.0.1" && s2->port == 8080,
              "... and the LIVE config is untouched: re-SPEC is not a reconfiguration door");

        // conn/remove-nonexistent-noop: success, and the live connection survives it.
        check(node.write(endpoint, owned(v_noop)).has_value(),
              "conn/remove-nonexistent-noop => success (a retried teardown is safe)");
        check(node.find(path_t::parse("/net/ws-client/up")->key()).has_value() &&
                  router.registry().size() == 1,
              "... and it removed NOTHING — the live connection is still there and routed");

        // conn/remove-reserved-rejected: the endpoint cannot self-destruct.
        const auto reserved = node.write(endpoint, owned(v_reserved));
        check(!reserved.has_value() && reserved.error() == status_t::PERMISSION_DENIED,
              "conn/remove-reserved-rejected => PERMISSION_DENIED (never routes to retire)");
        check(node.find(path_t::parse("/net/ws-client/conn")->key()).has_value(),
              "... the endpoint still resolves");

        // conn/remove-via-name: the retire, and the un-route with it.
        check(node.write(endpoint, owned(v_remove)).has_value(), "conn/remove-via-name => success");
        check(!node.find(path_t::parse("/net/ws-client/up")->key()).has_value(),
              "... the connection vertex is retired");
        check(router.registry().by_name("net/ws-client/up") == nullptr,
              "... and un-routed: the NAME no longer resolves to a link");
        // The door the reserved-name refusal had to leave working: it still creates.
        net.provide_link("ws-client", "up", channel.a());
        check(node.write(endpoint, owned(v_create)).has_value(),
              "... and the endpoint still creates afterwards (the refusals wrecked nothing)");
        channel.shutdown();
    }

    // --- the error IDENTITIES, off a real reply ------------------------------------------
    // The four refusals again, as FWD writes through the production terminus, so the u16 a
    // peer actually reads is the thing asserted. RFC-0014 §2 spells the reserved-name refusal
    // `tr::acl::permission_denied`, which is not a registered identity in RFC-0002 §D's table;
    // the shipped mapping answers `tr::access::denied` (0x0050), and per §Discussion's
    // clause-kind rule the identity is what code plus this vector pin. Raised on #492.
    {
        graph_t node;
        fwd_router_t router(node);
        transport_vertex_t net(node, router);
        tr::graph::op_resolver_t resolver(node);
        (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);
        tr::net::loopback_channel_t channel;
        net.provide_link("ws-client", "up", channel.a());

        const auto to_endpoint = [&](std::span<const std::byte> payload) {
            return tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE,
                                      tr::testing::b_path({"net", "ws-client", "conn"}),
                                      tr::testing::b_path({"reply-ep"}), {}, payload);
        };
        const auto answer = [&](std::span<const std::byte> payload) {
            auto reply = resolve_bytes(resolver, to_endpoint(payload));
            check(reply.has_value(), "the endpoint write produced a reply");
            return reply.has_value() ? reply_parts(*reply) : reply_parts_t{};
        };

        const auto created = answer(v_create);
        check(created.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::RESULT),
              "a SPEC over the wire answers RESULT — the control arm for the four below");

        const auto in_use = answer(v_in_use);
        check(in_use.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::ERROR) &&
                  in_use.code == 0x0022,
              "conn/spec-name-in-use answers ERROR{ tr::path::in_use 0x0022 }");
        const auto bad = answer(v_bad_type);
        check(bad.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::ERROR) &&
                  bad.code == 0x0030,
              "conn/bad-payload-type answers ERROR{ tr::schema::type_mismatch 0x0030 }");
        const auto reserved = answer(v_reserved);
        check(reserved.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::ERROR) &&
                  reserved.code == 0x0050,
              "conn/remove-reserved-rejected answers ERROR{ tr::access::denied 0x0050 } — the "
              "REGISTERED identity, not RFC-0014 §2's unregistered `tr::acl::permission_denied`");
        const auto noop = answer(v_noop);
        check(noop.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::RESULT),
              "conn/remove-nonexistent-noop answers RESULT — a no-op SUCCESS, not an error");
        const auto removed = answer(v_remove);
        check(removed.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::RESULT),
              "conn/remove-via-name answers RESULT");
        channel.shutdown();
    }

    // --- conn/absent-endpoint-not-found: RFC-0014 §6's creatability probe ------------------
    // The same 53 bytes at two nodes. Nothing in the frame is inherently an error; the answer
    // is a property of the receiving node's module declarations, which is what the probe is
    // for — so the declared node is the ablation, not a courtesy.
    {
        graph_t node;
        fwd_router_t router(node);
        transport_vertex_t net(node, router);
        tr::graph::op_resolver_t resolver(node);
        (void)net.register_module("ws-client", "ws", conn_role_t::DIAL);  // some OTHER module

        const auto absent = resolve_bytes(resolver, v_probe);
        check(absent.has_value(), "the probe resolved to a reply");
        const auto a = absent.has_value() ? reply_parts(*absent) : reply_parts_t{};
        check(
            a.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::ERROR) && a.code == 0x0020,
            "no `can` module => ERROR{ tr::path::not_found 0x0020 } — \"not creatable\"");
        check(a.code != 0x0031,
              "... and NOT tr::schema::not_found, which §Compatibility reserves for a PRESENT "
              "endpoint refusing an unknown config type");
    }
    {
        graph_t node;
        fwd_router_t router(node);
        transport_vertex_t net(node, router);
        tr::graph::op_resolver_t resolver(node);
        check(net.register_module("can", "can", conn_role_t::DIAL).has_value(),
              "the ablation: the same node, with the `can` module declared");
        const auto present = resolve_bytes(resolver, v_probe);
        check(present.has_value(), "the probe resolved to a reply");
        const auto p = present.has_value() ? reply_parts(*present) : reply_parts_t{};
        check(p.kind == static_cast<std::uint8_t>(tr::graph::reply_kind_t::RESULT),
              "the IDENTICAL bytes now answer RESULT — the frame carries no error of its own");
    }
}

}  // namespace

int main() {
    test_conformance_vectors();
    test_conn_spec_bytes_pinned();
    test_conn_spec_round_trips_through_the_reader();
    test_create_connection_vertex();
    test_await_link_state();
    test_liveness_enum_value();
    test_constructed_link_reports_role_state();
    test_backoff_connect_timeout_parsed();
    test_fwd_still_routes();
    test_local_path_untouched();
    test_slim_net_reports_its_injected_egress_store();
    test_config_constructed_udp();
    test_provide_link_wins();
    test_same_leaf_name_under_two_modules();
    test_ambiguous_leaf_name_refused();
    test_kind_module_beats_unrelated_staging();
    test_link_is_armed_after_wiring();
    test_factory_built_ws_dial_delivers_push_on_connect();
    test_factory_built_tcp_dial_delivers_push_on_connect();
    test_creation_errors();
    test_link_of_accessor();
    test_link_name_collision_rejected();
    test_link_name_collision_placeholder_parent();
    test_udp_max_frame_reaches_the_transport();
    test_ws_peer_named_config();
    test_refused_dial_is_transport_down();
    test_unregistered_kind_is_schema_not_found();
    test_register_module_rejects_reserved_chars();
    test_conn_endpoint_spec_creates();
    test_conn_endpoint_constructs_socket();
    test_builtin_dial_traits_track_the_build_knob();
    test_conn_endpoint_malformed_refuses();
    test_conn_endpoint_spec_in_use_is_idempotent_safe();
    test_conn_endpoint_name_removes();
    test_conn_endpoint_is_hidden_from_enumeration();
    test_wire_name_reaches_add_child();
    test_app_chosen_root_and_module();
    test_structural_vertex_partition();
    test_structural_vertex_edges();

    return tr::testing::summary("transport_vertex");
}
