/**
 * @file
 * @brief #2481 — the node's one door for the application's clock: `transport_vertex_t::sweep`
 *        hands `now` and the deadline to every connection's link and answers the earliest
 *        next-due time.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One node carries a TCP dialer whose peer never reads, a TCP server with a peer that drains
 * every frame, a stand-in for a CAN link (the silence sweep of #2151 answers the due time the
 * test sets), and, on a build with a Noise backend, a Noise initiator over a carrier that only
 * counts its datagrams. Every link is driven only through the door, from a fake clock: plain
 * numbers, since libtracer reads no clock. The arms:
 *
 *   - with no calls, nothing changes;
 *   - the door answers the minimum next-due over its links, and hands each the app's numbers;
 *   - a frozen peer is closed and counted, a draining one is kept;
 *   - Noise retransmits at its retry deadline, not before;
 *   - a link whose connection is removed during the walk is freed only by a collect after the
 *     door returns, and the next door call does not reach it.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/conn_spec.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_tcp.hpp"
#include "test_support.hpp"

#if defined(LIBTRACER_NOW_DOOR_NOISE)
#include <cstring>

#include "libtracer/transport_noise.hpp"
#endif

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::testing::check;

/** @brief A loopback listener on an ephemeral port with a small receive buffer. */
struct listener_t {
    int fd = -1;            /**< @brief The listening socket. */
    std::uint16_t port = 0; /**< @brief Its bound port. */
    listener_t() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int small = 4096;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        (void)::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        (void)::listen(fd, 4);
        socklen_t len = sizeof(a);
        (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
    }
    ~listener_t() {
        if (fd >= 0) ::close(fd);
    }
    listener_t(const listener_t&) = delete;
    listener_t& operator=(const listener_t&) = delete;
};

/** @brief A raw client socket connected to 127.0.0.1:@p port. */
int dial_raw(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/** @brief Read and discard everything on @p fd until @p stop. */
void drain(int fd, const std::atomic<bool>& stop) {
    std::vector<std::byte> buf(64 * 1024);
    const timeval tv{.tv_sec = 0, .tv_usec = 20000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (!stop.load()) (void)::recv(fd, buf.data(), buf.size(), 0);
}

/** @brief How many open peers @p server carries, after waiting (bounded) for @p n. */
std::size_t peers_of(tr::net::tcp_server_transport_t& server, std::size_t n) {
    std::size_t open = 0;
    for (int i = 0; i < 200; ++i) {
        open = 0;
        server.enumerate_peers([&](std::string_view) { ++open; });
        if (open == n) break;
        std::this_thread::sleep_for(10ms);
    }
    return open;
}

/** @brief The stand-in for a CAN link: its silence sweep answers @ref due and records what it
 *         was handed. */
struct fake_can_t final : tr::net::transport_t {
    std::uint64_t due = 0;           /**< @brief What @ref sweep answers. */
    std::uint64_t last_now = 0;      /**< @brief The last `now` it was handed. */
    std::uint64_t last_deadline = 0; /**< @brief The last `deadline` it was handed. */
    int sweeps = 0;                  /**< @brief How many times the door reached it. */

    /** @brief The wire is not under test. */
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}

    /** @brief Record the app's numbers and answer the due time the test set. */
    std::uint64_t sweep(std::uint64_t now, std::uint64_t deadline) override {
        ++sweeps;
        last_now = now;
        last_deadline = deadline;
        return due;
    }
};

#if defined(LIBTRACER_NOW_DOOR_NOISE)
namespace noise = tr::net::noise;

/** @brief A key store with one PSK and a deterministic (NOT random) ephemeral source. */
struct door_host_t {
    std::uint64_t state = 0x9E3779B97F4A7C15; /**< @brief The test generator's state. */
    std::uint64_t counter = 0;                /**< @brief The last counter handed out. */
    /** @brief The key store: one all-0x42 key named `door-psk`. */
    bool resolve_psk(std::string_view ref, noise::key32_t& out) {
        out.fill(std::byte{0x42});
        return ref == "door-psk";
    }
    /** @brief The ephemeral source. */
    bool ephemeral(noise::key32_t& out) {
        for (std::size_t i = 0; i < out.size(); i += 8) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            std::memcpy(out.data() + i, &state, 8);
        }
        return true;
    }
    /** @brief The initiator counter. */
    std::uint64_t next_counter() { return ++counter; }
};

/** @brief A datagram carrier that only counts and keeps what it is asked to send. */
struct count_carrier_t final : tr::net::transport_t {
    std::vector<std::vector<std::byte>>* log; /**< @brief Every datagram sent, in order. */
    /** @brief Log into @p into. */
    explicit count_carrier_t(std::vector<std::vector<std::byte>>* into) : log(into) {}
    /** @brief Unused by the Noise link, which always addresses its sends. */
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
    /** @brief Keep one datagram. */
    void send_to(std::uint64_t, std::span<const std::span<const std::byte>> iov) {
        std::vector<std::byte> d;
        for (const auto s : iov) d.insert(d.end(), s.begin(), s.end());
        log->push_back(std::move(d));
    }
    /** @brief Always up. */
    [[nodiscard]] bool ok() const noexcept { return true; }
    /** @brief The datagram bound. */
    [[nodiscard]] std::size_t effective_max_frame() const noexcept { return 1200; }
    /** @brief Nothing ever arrives. */
    [[nodiscard]] std::uint64_t inbound_source() const noexcept { return 0; }
    /** @brief The one peer. */
    [[nodiscard]] std::uint64_t peer_address() const noexcept { return 1; }
};

/** @brief The Noise initiator the node carries. */
using door_noise_t = tr::net::noise_link_t<noise::default_crypto_t, door_host_t, count_carrier_t>;
#endif

/** @brief What the factories build into, and the test reads back. */
struct world_t {
    std::uint16_t dial_port = 0;                       /**< @brief Where the dialer dials. */
    tr::net::tcp_transport_t* dialer = nullptr;        /**< @brief The dialer the node built. */
    tr::net::tcp_server_transport_t* server = nullptr; /**< @brief The server the node built. */
    fake_can_t* can = nullptr;                         /**< @brief The CAN stand-in it built. */
#if defined(LIBTRACER_NOW_DOOR_NOISE)
    door_host_t host;                              /**< @brief The Noise link's host. */
    std::vector<std::vector<std::byte>> datagrams; /**< @brief What the Noise link sent. */
    door_noise_t* noise = nullptr;                 /**< @brief The Noise link it built. */
#endif
};

/** @brief Declare kind @p kind under module @p module, built by @p make from the world. */
template <class Make>
void declare(transport_vertex_t& net, world_t& w, std::string_view kind, std::string_view module,
             conn_role_t role, Make make) {
    world_t* const at = &w;
    net.register_transport_type(
        kind,
        [at, make](const tr::net::conn_settings_t&, const tr::wire::tlv_node_t*,
                   tr::mem::block_source_t& src) -> tr::graph::result_t<tr::net::transport_ptr_t> {
            return make(*at, src);
        });
    (void)net.register_module(module, kind, role);
}

/** @brief Create connection @p name of the kind behind @p module. */
bool create(graph_t& node, std::string_view module, std::string_view kind, std::string_view name) {
    tr::net::conn_spec_t spec(name);
    spec.kind(kind).addr("127.0.0.1").port(1);
    std::string at = "/net/";
    at += module;
    at += "/conn";
    return node.write(path_t(at), spec.view()).has_value();
}

const std::vector<std::byte> kBig(8 * 1024 * 1024, std::byte{0x5a});
const std::vector<std::byte> kSmall(64 * 1024, std::byte{0x5b});

void test_one_door() {
    std::printf("one door drives every link from the app's clock:\n");
    listener_t frozen_peer;  // accepts the dialer, then never reads
    world_t w;
    w.dial_port = frozen_peer.port;
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);

    declare(net, w, "door-dial", "door-dial", conn_role_t::DIAL, [](world_t& at, auto& src) {
        auto l = tr::mem::make_poly<tr::net::tcp_transport_t>(
            src, "127.0.0.1", at.dial_port,
            tr::net::tcp_config_t{.liveness_window_ms = tr::net::kBoundedWaitMs});
        at.dialer = l.get();
        return tr::graph::result_t<tr::net::transport_ptr_t>(std::move(l));
    });
    declare(net, w, "door-listen", "door-listen", conn_role_t::LISTEN, [](world_t& at, auto& src) {
        auto l = tr::mem::make_poly<tr::net::tcp_server_transport_t>(
            src, std::uint16_t{0}, tr::net::tcp_server_config_t{});
        at.server = l.get();
        return tr::graph::result_t<tr::net::transport_ptr_t>(std::move(l));
    });
    declare(net, w, "door-can", "door-can", conn_role_t::DIAL, [](world_t& at, auto& src) {
        auto l = tr::mem::make_poly<fake_can_t>(src);
        at.can = l.get();
        return tr::graph::result_t<tr::net::transport_ptr_t>(std::move(l));
    });
#if defined(LIBTRACER_NOW_DOOR_NOISE)
    declare(net, w, "door-noise", "door-noise", conn_role_t::DIAL, [](world_t& at, auto& src) {
        const tr::net::noise_config_t cfg{
            .psk_ref = "door-psk", .retry = 10, .retry_cap = 40, .attempt = 1000};
        auto l =
            tr::mem::make_poly<door_noise_t>(src, conn_role_t::DIAL, at.host, cfg, &at.datagrams);
        at.noise = l.get();
        return tr::graph::result_t<tr::net::transport_ptr_t>(std::move(l));
    });
#endif
    check(create(node, "door-dial", "door-dial", "d") &&
              create(node, "door-listen", "door-listen", "s") &&
              create(node, "door-can", "door-can", "c"),
          "the node created a TCP dialer, a TCP server and a CAN stand-in");
    check(w.dialer != nullptr && w.server != nullptr && w.can != nullptr, "  and built each");
    const int peer = ::accept(frozen_peer.fd, nullptr, nullptr);
    const int reader = dial_raw(w.server->local_port());
    check(peer >= 0 && reader >= 0 && peers_of(*w.server, 1) == 1,
          "  the dialer's peer and the server's peer are connected");
#if defined(LIBTRACER_NOW_DOOR_NOISE)
    check(create(node, "door-noise", "door-noise", "n") && w.noise != nullptr && w.noise->ok(),
          "  and a Noise initiator over a counting carrier");
    w.noise->dial();  // the link's `now` is 0: the retry is due at 10
    check(w.datagrams.size() == 1, "  its first message is out");
#endif

    // With no calls, nothing changes: a frozen peer stays, nothing is retransmitted.
    w.dialer->send(std::span(kBig));  // cut short: the rest is held on the frozen peer's wire
    std::this_thread::sleep_for(200ms);
    check(w.dialer->frozen_closed() == 0 && w.dialer->link_up() && w.can->sweeps == 0,
          "with no door calls nothing is closed and no link is reached");
#if defined(LIBTRACER_NOW_DOOR_NOISE)
    check(w.datagrams.size() == 1, "  and nothing is retransmitted");
#endif

    std::atomic<bool> stop{false};
    std::thread drainer([&] { drain(reader, stop); });

    // The answer is the minimum over the links: CAN's 2 beats the dialer's 3 (its record 1
    // plus one window of 1, plus one) and Noise's retry at 10.
    w.can->due = 2;
    check(net.sweep(1, 0) == 2, "the door answers the earliest next-due over its links");
    check(w.can->sweeps == 1 && w.can->last_now == 1 && w.can->last_deadline == 0,
          "  each link is reached once, with the app's now and deadline");
    w.can->due = 0;
    check(net.sweep(2, 1) == 3, "  with CAN quiet, the frozen dialer's next check is the earliest");

    // A draining peer, fed while the clock runs, inside the dialer's deadline.
    for (std::uint64_t now = 3; now <= 9; ++now) {
        w.server->send(std::span(kSmall));
        std::this_thread::sleep_for(20ms);
        (void)net.sweep(now, 1);
    }
#if defined(LIBTRACER_NOW_DOOR_NOISE)
    check(w.datagrams.size() == 1, "Noise sends nothing before its retry deadline");
#endif
    const std::uint64_t next = net.sweep(10, 5);
    check(w.dialer->frozen_closed() == 1, "past the deadline the frozen peer is closed, counted");
    check(w.server->frozen_closed() == 0 && peers_of(*w.server, 1) == 1,
          "  and the draining peer is kept");
#if defined(LIBTRACER_NOW_DOOR_NOISE)
    check(w.datagrams.size() == 2 && w.datagrams[1] == w.datagrams[0] &&
              w.noise->count(noise::counter_t::RETRANSMITS) == 1,
          "  Noise retransmits its first message at the retry deadline");
    check(next != 0 && next <= 30, "  and the answer is no later than Noise's next retry, 30");
#else
    (void)next;
#endif
    check(w.can->sweeps == 10, "  the CAN stand-in was reached by every call");
    stop = true;
    drainer.join();
    ::close(reader);
    ::close(peer);
}

/** @brief A link that, swept, removes another connection and collects, as an app might. */
struct remover_t final : tr::net::transport_t {
    graph_t* node = nullptr;                   /**< @brief The graph to collect. */
    transport_vertex_t* net = nullptr;         /**< @brief The vertex to remove from. */
    std::atomic<bool>* victim_alive = nullptr; /**< @brief Cleared by the victim's destructor. */
    bool alive_after_collect = false;          /**< @brief What @p victim_alive read mid-walk. */
    /** @brief The wire is not under test. */
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
    /** @brief Remove the victim and collect, inside the door's walk. */
    std::uint64_t sweep(std::uint64_t, std::uint64_t) override {
        if (net != nullptr && net->remove_connection("net/door-fake/victim").has_value()) {
            node->collect();
            alive_after_collect = victim_alive->load();
        }
        return 0;
    }
};

/** @brief A link that counts the door's calls and says when it is destroyed. */
struct victim_t final : tr::net::transport_t {
    std::atomic<bool>* alive; /**< @brief Cleared on destruction. */
    int* sweeps;              /**< @brief Bumped on each call. */
    victim_t(std::atomic<bool>* a, int* s) : alive(a), sweeps(s) { alive->store(true); }
    ~victim_t() { alive->store(false); }
    victim_t(const victim_t&) = delete;
    victim_t& operator=(const victim_t&) = delete;
    /** @brief The wire is not under test. */
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
    /** @brief Count the call. */
    std::uint64_t sweep(std::uint64_t, std::uint64_t) override {
        ++*sweeps;
        return 0;
    }
};

/** @brief The two links of @ref test_removed_mid_walk. */
struct removal_world_t {
    remover_t* remover = nullptr;          /**< @brief Built first. */
    std::atomic<bool> victim_alive{false}; /**< @brief The victim's liveness. */
    int victim_sweeps = 0;                 /**< @brief Calls the victim saw. */
};

void test_removed_mid_walk() {
    std::printf("a link removed during the walk is freed only after the door returns:\n");
    removal_world_t rw;
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    removal_world_t* const at = &rw;
    net.register_transport_type(
        "door-fake",
        [at](const tr::net::conn_settings_t&, const tr::wire::tlv_node_t*,
             tr::mem::block_source_t& src) -> tr::graph::result_t<tr::net::transport_ptr_t> {
            if (at->remover == nullptr) {
                auto l = tr::mem::make_poly<remover_t>(src);
                at->remover = l.get();
                return l;
            }
            return tr::mem::make_poly<victim_t>(src, &at->victim_alive, &at->victim_sweeps);
        });
    (void)net.register_module("door-fake", "door-fake", conn_role_t::DIAL);
    check(create(node, "door-fake", "door-fake", "remover") &&
              create(node, "door-fake", "door-fake", "victim") && rw.victim_alive.load(),
          "the node carries a remover and its victim");
    rw.remover->node = &node;
    rw.remover->net = &net;
    rw.remover->victim_alive = &rw.victim_alive;

    const int before = rw.victim_sweeps;
    (void)net.sweep(1, 0);
    check(rw.remover->alive_after_collect,
          "a collect inside the walk frees nothing the door still holds");
    check(rw.victim_sweeps - before <= 1, "  the victim is reached at most once");
    node.collect();
    check(!rw.victim_alive.load(), "once the door returns, a collect frees the removed link");
    const int after = rw.victim_sweeps;
    (void)net.sweep(2, 1);
    check(rw.victim_sweeps == after, "  and the next door call does not reach it");
}

}  // namespace

int main() {
    std::printf("== #2481: one node-level door for the application's now ==\n");
    test_one_door();
    test_removed_mid_walk();
    return tr::testing::summary("now_door_test");
}
