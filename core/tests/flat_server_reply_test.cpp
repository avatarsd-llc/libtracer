/**
 * @file
 * @brief #2911: a terminus reply on a link that carries several sessions goes to the session
 *        that sent the request.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A flat TCP or WS listener carries every session under one routing identity. A request is a
 * request/response exchange, so its reply belongs to the session that asked, and the other
 * sessions on the same listener do not receive it. Over real loopback sockets:
 *
 * - **READ.** Two sessions on one listener; a READ from A is answered to A, and B receives
 *   nothing;
 * - **deferred AWAIT.** An AWAIT from A, answered by a later write, goes to A; one whose
 *   session has left before the write is not answered to anyone else;
 * - **UDP listener.** A deferred AWAIT is answered to the datagram source that sent it, even
 *   when another source sent a datagram in between; distinct sources park within the link's
 *   one `kMaxAwaitsPerLink` cap, since a source is not a session.
 *
 * Subscription pushes are not replies and keep their fan-out; they are covered elsewhere.
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"
#include "tlv_tree.hpp"  // host-only owning tree (#1829)

#if defined(LIBTRACER_TEST_HAS_TCP)
#include "libtracer/transport_tcp.hpp"
#endif
#if defined(LIBTRACER_TEST_HAS_WS)
#include "libtracer/transport_ws.hpp"
#endif
#if defined(LIBTRACER_TEST_HAS_UDP)
#include "libtracer/transport_udp.hpp"
#endif

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::b_fwd;
using tr::testing::b_path;
using tr::testing::check;
using tr::testing::make_value;

using namespace std::chrono_literals;

/** @brief Records every frame a client receives. */
struct frame_sink_t {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::vector<std::byte>> frames;

    /** @brief The receiver callable (bound by address via set_receiver(F&)). */
    void operator()(std::span<const std::byte> f) {
        {
            const std::lock_guard lock(m);
            frames.emplace_back(f.begin(), f.end());
        }
        cv.notify_all();
    }
    /** @brief True once at least @p n frames arrived before @p timeout. */
    bool wait_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
    /** @brief True if any frame received so far contains @p bytes. */
    bool holds(std::span<const std::byte> bytes) {
        const std::lock_guard lock(m);
        for (const std::vector<std::byte>& f : frames)
            if (!std::ranges::search(f, bytes).empty()) return true;
        return false;
    }
    /** @brief Frames received so far. */
    std::size_t count() {
        const std::lock_guard lock(m);
        return frames.size();
    }
};

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> raw(4);
    tr::detail::store_le<std::uint32_t>(raw, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief `FWD{AWAIT, dst=/<target>, src=/cli, await_timeout=10 s}`. */
std::vector<std::byte> b_await(std::string_view target) {
    std::vector<std::byte> raw(8);
    tr::detail::store_le<std::uint64_t>(raw, 10'000'000'000ULL);
    std::vector<std::byte> timeout;
    tr::wire::emit_tlv(timeout, type_t::VALUE, opt_t{}, raw);
    return b_fwd(fwd_op_t::AWAIT, b_path({target}), b_path({"cli"}), {}, timeout);
}

/** @brief `FWD{READ, dst=/<target>, src=/cli}`. */
std::vector<std::byte> b_read(std::string_view target) {
    return b_fwd(fwd_op_t::READ, b_path({target}), b_path({"cli"}));
}

/** @brief Poll until @p pred holds or @p limit passes. */
template <class Pred>
bool eventually(Pred pred, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

/** @brief One node: `/sink` and `/other`, and its router. */
struct node_t {
    graph_t graph;
    vertex_handle_t sink;
    vertex_handle_t other;
    fwd_router_t router;

    node_t()
        : sink(graph.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE)),
          other(graph.register_vertex(*path_t::parse("/other"), role_t::STORED_VALUE)),
          router(graph) {
        (void)graph.write(sink, make_value(b_value_u32(1)));
        (void)graph.write(other, make_value(b_value_u32(2)));
    }
};

/**
 * @brief Two sessions on one flat listener of kind @p Server, dialled by @p Client: replies
 *        go to the session that asked.
 */
template <class Server, class Client>
void replies_go_to_the_asking_session(const char* kind) {
    std::printf("%s listener, two sessions: each reply goes to the session that asked:\n", kind);
    node_t n;
    Server server(0);
    check(server.ok(), "  the listener is up");
    (void)n.router.attach_link("cli", server);
    const std::uint16_t port = server.local_port();

    frame_sink_t rx_a;
    frame_sink_t rx_b;
    auto a = std::make_unique<Client>("127.0.0.1", port);
    Client b("127.0.0.1", port);
    a->set_receiver(rx_a);
    b.set_receiver(rx_b);
    (void)a->start_receiving();
    (void)b.start_receiving();

    // Each session asks once first, so both are open before the cases below.
    check(eventually(
              [&] {
                  b.send(b_read("other"));
                  return rx_b.wait_count(1, 50ms);
              },
              3s),
          "  session B is open and answered");
    check(eventually(
              [&] {
                  a->send(b_read("other"));
                  return rx_a.wait_count(1, 50ms);
              },
              3s),
          "  session A is open and answered");
    std::this_thread::sleep_for(200ms);
    const std::size_t a0 = rx_a.count();
    const std::size_t b0 = rx_b.count();

    a->send(b_read("sink"));
    check(rx_a.wait_count(a0 + 1, 2s), "  A's READ is answered to A");
    std::this_thread::sleep_for(200ms);
    check(rx_b.count() == b0, "  and B receives nothing");

    a->send(b_await("sink"));
    check(eventually([&] { return n.router.pending_awaits() == 1; }, 2s), "  A's AWAIT parks");
    (void)n.graph.write(n.sink, make_value(b_value_u32(3)));
    check(rx_a.wait_count(a0 + 2, 2s), "  a write answers A's AWAIT to A");
    std::this_thread::sleep_for(200ms);
    check(rx_b.count() == b0, "  and B receives nothing");

    a->send(b_await("sink"));
    check(eventually([&] { return n.router.pending_awaits() == 1; }, 2s),
          "  A parks another AWAIT");
    a.reset();
    std::this_thread::sleep_for(300ms);  // the listener observes A's close
    (void)n.graph.write(n.sink, make_value(b_value_u32(4)));
    std::this_thread::sleep_for(300ms);
    check(rx_b.count() == b0, "  after A has left, the write answers nobody else");
}

#if defined(LIBTRACER_TEST_HAS_TCP)
/** @brief What the directed-send seam does once: let A leave and C take A's slot. */
struct slot_reuse_t {
    std::unique_ptr<tr::net::tcp_transport_t>* a = nullptr;
    std::unique_ptr<tr::net::tcp_transport_t>* c = nullptr;
    frame_sink_t* rx_c = nullptr;
    tr::net::tcp_server_transport_t* server = nullptr;
    std::uint16_t port = 0;
    bool fired = false;
    bool reused = false;
};
slot_reuse_t g_reuse;

/**
 * @brief A deferred AWAIT whose session leaves, and whose slot a new session takes, between the
 *        reply's resolve and its send: the new session never receives the reply.
 */
void slot_reuse_between_resolve_and_send(bool peer_named) {
    std::printf("tcp listener (%s): a slot reused between a reply's resolve and its send:\n",
                peer_named ? "peer-named" : "flat");
    if constexpr (!tr::graph::kFaultInjection) {
        std::printf("  SKIP: this build binds no fault-injection seams\n");
        return;
    } else {
        node_t n;
        tr::net::tcp_server_transport_t server(
            0, tr::net::tcp_server_config_t{.max_peers = 4, .peer_named = peer_named});
        (void)n.router.attach_link("cli", server);
        const std::uint16_t port = server.local_port();
        frame_sink_t rx_a;
        frame_sink_t rx_c;
        auto a = std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", port);
        std::unique_ptr<tr::net::tcp_transport_t> c;
        a->set_receiver(rx_a);
        (void)a->start_receiving();
        check(eventually(
                  [&] {
                      a->send(b_read("other"));
                      return rx_a.wait_count(1, 50ms);
                  },
                  3s),
              "  session A is open and answered");
        a->send(b_await("sink"));
        check(eventually([&] { return n.router.pending_awaits() == 1; }, 2s), "  A's AWAIT parks");

        g_reuse = slot_reuse_t{.a = &a, .c = &c, .rx_c = &rx_c, .server = &server, .port = port};
        tr::net::detail::directed_send_hook = [] {
            if (std::exchange(g_reuse.fired, true)) return;
            g_reuse.a->reset();  // A leaves ...
            *g_reuse.c = std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", g_reuse.port);
            (*g_reuse.c)->set_receiver(*g_reuse.rx_c);
            (void)(*g_reuse.c)->start_receiving();
            // ... and C, answered once, holds A's slot in the next tenancy.
            const tr::net::peer_handle_t next{tr::wire::pair_t{.index = 0, .generation = 2}};
            g_reuse.reused = eventually(
                [&] {
                    (*g_reuse.c)->send(b_read("other"));
                    return g_reuse.rx_c->wait_count(1, 50ms) &&
                           g_reuse.server->peer_link_of(next) != nullptr;
                },
                3s);
        };
        const std::uint64_t dropped0 = server.drop_stats().dropped_tx;
        (void)n.graph.write(n.sink, make_value(b_value_u32(3)));
        tr::net::detail::directed_send_hook = nullptr;
        check(g_reuse.fired && g_reuse.reused, "  C took A's slot while the reply was in flight");
        std::this_thread::sleep_for(300ms);
        // A's reply carries the written value; C's own READs of `/other` never do.
        check(!rx_c.holds(b_value_u32(3)), "  and C never receives A's reply");
        check(server.drop_stats().dropped_tx > dropped0, "  the reply is dropped and counted");
    }
}
#endif

#if defined(LIBTRACER_TEST_HAS_UDP)
/**
 * @brief A UDP listener answers a deferred AWAIT to the source that sent it, even when another
 *        source sent a datagram in between.
 */
void udp_answers_the_awaits_sender() {
    std::printf("udp listener: a deferred AWAIT is answered to its own sender:\n");
    node_t n;
    tr::net::udp_transport_t server(0, "", 0);
    check(server.ok(), "  the listener is up");
    (void)n.router.attach_link("cli", server);
    const std::uint16_t port = server.local_port();

    frame_sink_t rx_a;
    frame_sink_t rx_c;
    tr::net::udp_transport_t a(0, "127.0.0.1", port);
    tr::net::udp_transport_t c(0, "127.0.0.1", port);
    a.set_receiver(rx_a);
    c.set_receiver(rx_c);
    (void)a.start_receiving();
    (void)c.start_receiving();

    a.send(b_await("sink"));
    check(eventually([&] { return n.router.pending_awaits() == 1; }, 2s), "  A's AWAIT parks");
    c.send(b_read("other"));
    check(rx_c.wait_count(1, 2s), "  another source's READ in between is answered to it");
    (void)n.graph.write(n.sink, make_value(b_value_u32(3)));
    check(rx_a.wait_count(1, 2s), "  the write answers the AWAIT to A");
    std::this_thread::sleep_for(200ms);
    check(rx_c.count() == 1, "  and the other source receives only its own reply");
}

/** @brief Distinct UDP sources share the listener's one cap: a source is not a session. */
void udp_sources_share_one_link_cap() {
    std::printf("udp listener: distinct sources park within one link cap:\n");
    node_t n;
    tr::net::udp_transport_t server(0, "", 0);
    (void)n.router.attach_link("cli", server);
    const std::uint16_t port = server.local_port();
    constexpr std::size_t kCap = tr::graph::config_t::kMaxAwaitsPerLink;
    std::vector<std::unique_ptr<tr::net::udp_transport_t>> sources;
    for (std::size_t i = 0; i <= kCap; ++i) {
        sources.push_back(std::make_unique<tr::net::udp_transport_t>(0, "127.0.0.1", port));
        sources.back()->send(b_await("sink"));
        (void)eventually(
            [&] { return n.router.pending_awaits() + n.router.await_refusals() == i + 1; }, 2s);
    }
    check(n.router.pending_awaits() == kCap, "  the cap's worth of AWAITs park");
    check(n.router.await_refusals() == 1, "  and one more source's AWAIT is refused");
}
#endif

}  // namespace

int main() {
#if defined(LIBTRACER_TEST_HAS_TCP)
    replies_go_to_the_asking_session<tr::net::tcp_server_transport_t, tr::net::tcp_transport_t>(
        "tcp");
#endif
#if defined(LIBTRACER_TEST_HAS_WS)
    replies_go_to_the_asking_session<tr::net::ws_server_transport_t,
                                     tr::net::ws_client_transport_t>("ws");
#endif
#if defined(LIBTRACER_TEST_HAS_TCP)
    slot_reuse_between_resolve_and_send(false);
    if constexpr (tr::net::kBusLinks) slot_reuse_between_resolve_and_send(true);
#endif
#if defined(LIBTRACER_TEST_HAS_UDP)
    udp_answers_the_awaits_sender();
    udp_sources_share_one_link_cap();
#endif
    return tr::testing::summary("flat_server_reply");
}
