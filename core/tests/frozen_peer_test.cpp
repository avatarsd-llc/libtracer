/**
 * @file
 * @brief #2198 — the application's liveness check closes a frozen peer on every stream
 *        transport vertex, and leaves a draining or idle one alone.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Since #2174 a SAMPLE lane never strikes, so a peer that keeps its socket open and reads
 * nothing is no longer dropped by the link on its own. The application closes it instead:
 * it calls `transport_t::sweep(now, deadline)` with `now` from its own clock (#2481), and
 * the vertex closes each peer that owed the wire progress (a frame held on it, or a frame or
 * handshake the peer began) and made none since its record fell below `deadline`, and
 * answers when an owing peer is next due. libtracer reads no clock, so the test drives `now`
 * as plain numbers.
 *
 * Per transport, three arms: a frozen peer is closed past the deadline and counted, never
 * before it; a draining peer is not; an idle peer, owing nothing, is not. Each direction is
 * its own row, on the dialers as on the servers (#2812): a server that begins a frame and
 * stops is closed by the dialer, and a self-healing link over it redials.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/self_heal_link.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using namespace std::chrono_literals;

/** @brief A loopback listener on an ephemeral port with a small receive buffer. */
struct listener_t {
    int fd = -1;            /**< @brief The listening socket. */
    std::uint16_t port = 0; /**< @brief Its bound port. */
    listener_t() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
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

/** @brief A raw client socket connected to 127.0.0.1:@p port with a small receive buffer. */
int dial_raw(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    const int small = 4096;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return -1;
    }
    const timeval tv{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/** @brief True once the far side of @p fd has closed or shut it down (EOF or reset). */
bool sees_close(int fd) {
    // Reads what the link had put on the wire before it, a small window at a time; the
    // socket's 5 s receive timeout bounds a link that never closed.
    std::vector<std::byte> buf(64 * 1024);
    for (;;) {
        const ssize_t r = ::recv(fd, buf.data(), buf.size(), 0);
        if (r == 0) return true;
        if (r < 0) return errno != EAGAIN && errno != EWOULDBLOCK;
    }
}

/** @brief Read and discard everything on @p fd until @p stop. */
void drain(int fd, const std::atomic<bool>& stop) {
    std::vector<std::byte> buf(64 * 1024);
    const timeval tv{.tv_sec = 0, .tv_usec = 20000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (!stop.load()) (void)::recv(fd, buf.data(), buf.size(), 0);
}

/** @brief Wait (bounded) until @p server carries @p n open peers. */
template <class Server>
bool wait_peers(Server& server, std::size_t n) {
    for (int i = 0; i < 200; ++i) {
        std::size_t open = 0;
        server.enumerate_peers([&](std::string_view) { ++open; });
        if (open == n) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

/**
 * @brief The verdict sequence every arm shares: one check stamps the record at `now` 1; a
 *        stuck peer is still kept while its record is not older than the deadline, and is
 *        due one window after it; past it, a frozen one is closed.
 * @return How many closes the final check counted.
 */
template <class Link>
std::size_t judge(Link& link) {
    const std::size_t before = link.frozen_closed();
    (void)link.sweep(1, 0);  // stamps the record (moved since)
    check(link.sweep(2, 1) == 3, "  a stuck peer is due one window after its record");
    check(link.frozen_closed() == before,
          "  nothing is closed while its record is within the deadline");
    check(link.sweep(10, 5) == 0, "  a closed peer owes nothing more");  // record 1 < 5
    return link.frozen_closed() - before;
}

/** @brief Wait (bounded) until @p link reports its connection gone. */
template <class Link>
bool goes_down(const Link& link) {
    for (int i = 0; i < 200 && link.link_up(); ++i) std::this_thread::sleep_for(10ms);
    return !link.link_up();
}

/** @brief The u32-LE length prefix of a 16-byte frame. */
constexpr std::array<std::byte, 4> kPrefix16{std::byte{16}, std::byte{0}, std::byte{0},
                                             std::byte{0}};

const std::vector<std::byte> kBig(8 * 1024 * 1024, std::byte{0x5a});
/** @brief A record a draining peer takes in a moment, well inside every receive cap. */
const std::vector<std::byte> kSmall(256 * 1024, std::byte{0x5b});

void test_tcp_dialer() {
    std::printf("tcp dialer:\n");
    {
        listener_t l;
        tr::net::tcp_transport_t link("127.0.0.1", l.port, {});
        link.start_receiving();
        const int peer = ::accept(l.fd, nullptr, nullptr);
        check(link.ok() && peer >= 0, "dialed, and the peer never reads");
        check(link.sweep(100, 50) == 0 && link.frozen_closed() == 0,
              "an idle peer owes nothing: kept, and nothing is due");
        link.send(std::span(kBig));  // cut short: the rest is held on the wire
        check(judge(link) == 1, "a frozen peer is closed past the deadline");
        check(link.frozen_closed() == 1, "and the close is counted");
        check(sees_close(peer), "the peer sees the connection go");
        ::close(peer);
    }
    {
        listener_t l;
        tr::net::tcp_transport_t link("127.0.0.1", l.port, {});
        link.start_receiving();
        const int peer = ::accept(l.fd, nullptr, nullptr);
        std::atomic<bool> stop{false};
        std::thread reader([&] { drain(peer, stop); });
        for (std::uint64_t now = 1; now <= 20; ++now) {
            link.send(std::span(kSmall));
            std::this_thread::sleep_for(20ms);
            (void)link.sweep(now * 10, now * 10 - 5);
        }
        stop = true;
        reader.join();
        check(link.frozen_closed() == 0 && link.link_up(), "a draining peer is never closed");
        ::close(peer);
    }
    // RX owed (#2812): the server begins a frame, part of its prefix or part of its body,
    // and stops, its socket open; the dialer's own TX is idle.
    for (const std::size_t sent : {std::size_t{2}, kPrefix16.size() + 5}) {
        listener_t l;
        tr::net::tcp_transport_t link("127.0.0.1", l.port, {});
        link.start_receiving();
        const int peer = ::accept(l.fd, nullptr, nullptr);
        std::array<std::byte, 9> part{};
        std::copy(kPrefix16.begin(), kPrefix16.end(), part.begin());
        (void)::send(peer, part.data(), sent, MSG_NOSIGNAL);
        std::this_thread::sleep_for(200ms);  // the driving thread reads the partial frame
        check(judge(link) == 1, sent == 2 ? "a server frozen mid-prefix is closed"
                                          : "a server frozen mid-body is closed");
        check(link.frozen_closed() == 1 && goes_down(link), "  counted, and the link goes down");
        check(sees_close(peer), "  the server sees the connection go");
        ::close(peer);
    }
    {
        // A server that sends a frame one byte a window moves bytes in every window: kept.
        listener_t l;
        tr::net::tcp_transport_t link("127.0.0.1", l.port, {});
        link.start_receiving();
        const int peer = ::accept(l.fd, nullptr, nullptr);
        (void)::send(peer, kPrefix16.data(), kPrefix16.size(), MSG_NOSIGNAL);
        for (std::uint64_t now = 1; now <= 12; ++now) {
            const std::byte b{0};
            (void)::send(peer, &b, 1, MSG_NOSIGNAL);
            std::this_thread::sleep_for(20ms);
            (void)link.sweep(now * 10, now * 10 - 5);
        }
        check(link.frozen_closed() == 0 && link.link_up(),
              "a server that keeps sending slowly is never closed");
        ::close(peer);
    }
}

void test_tcp_server() {
    std::printf("tcp server:\n");
    tr::net::tcp_server_transport_t server(0, {});
    check(server.ok(), "the server listens");
    // TX owed: the client never reads, and a broadcast is held on its wire.
    const int tx = dial_raw(server.local_port());
    check(wait_peers(server, 1), "the frozen-TX peer is accepted");
    check(server.sweep(100, 50) == 0 && server.frozen_closed() == 0,
          "an idle peer owes nothing: kept, and nothing is due");
    server.send(std::span(kBig));
    check(judge(server) == 1, "a peer frozen with a frame held on its wire is closed");
    check(sees_close(tx), "it sees the connection go");
    check(wait_peers(server, 0), "and its slot is torn down");
    ::close(tx);
    // RX owed: the client sends two bytes of a length prefix and nothing more.
    const int rx = dial_raw(server.local_port());
    check(wait_peers(server, 1), "the frozen-RX peer is accepted");
    const std::byte half[2] = {std::byte{0x10}, std::byte{0}};
    (void)::send(rx, half, sizeof(half), MSG_NOSIGNAL);
    std::this_thread::sleep_for(200ms);  // the poll thread reads the half prefix
    check(judge(server) == 1, "a peer frozen mid-frame is closed");
    check(sees_close(rx), "it sees the connection go");
    ::close(rx);
    check(server.frozen_closed() == 2, "both closes are counted");
    // Draining: a reader keeps up with every broadcast.
    const int ok = dial_raw(server.local_port());
    check(wait_peers(server, 1), "the draining peer is accepted");
    std::atomic<bool> stop{false};
    std::thread reader([&] { drain(ok, stop); });
    for (std::uint64_t now = 1; now <= 20; ++now) {
        server.send(std::span(kSmall));
        std::this_thread::sleep_for(20ms);
        (void)server.sweep(now * 10, now * 10 - 5);
    }
    stop = true;
    reader.join();
    check(server.frozen_closed() == 2, "a draining peer is never closed");
    ::close(ok);
}

void test_ws_server() {
    std::printf("ws server:\n");
    std::atomic<int> delivered{0};
    auto rx = [&](std::span<const std::byte>) {
        delivered.fetch_add(1, std::memory_order_release);
    };
    tr::net::ws_server_transport_t server(0, {});
    check(server.ok(), "the server listens");
    // A peer that connects and never finishes its opening handshake owes it.
    const int hs = dial_raw(server.local_port());
    const std::string_view part = "GET / HTTP/1.1\r\nHost: x\r\n";
    (void)::send(hs, part.data(), part.size(), MSG_NOSIGNAL);
    std::this_thread::sleep_for(200ms);
    check(judge(server) == 1, "a peer frozen in its handshake is closed");
    check(sees_close(hs), "it sees the connection go");
    ::close(hs);
    // A real client, open and idle, owes nothing; one that drains every broadcast is kept.
    // The server writes the 101 partway through the poll pass that reads the handshake,
    // and records the peer as owing nothing only when that pass returns, so the client can
    // be open while the server still has it mid-handshake. Two checks back to back, with
    // their fake clock already past the deadline, would close it there. One tiny frame the
    // server delivers settles that: its pass starts only after the handshake pass is over.
    server.set_receiver(rx);
    tr::net::ws_client_transport_t client("127.0.0.1", server.local_port(), {});
    check(client.ok() && wait_peers(server, 1), "the client is open");
    client.start_receiving();
    const std::array<std::byte, 4> probe{};
    client.send(std::span<const std::byte>(probe));
    for (int i = 0; i < 200 && delivered.load(std::memory_order_acquire) == 0; ++i)
        std::this_thread::sleep_for(10ms);
    check(delivered.load(std::memory_order_acquire) == 1, "the server has read past the handshake");
    check(server.sweep(100, 50) == 0 && server.sweep(200, 150) == 0 && server.frozen_closed() == 1,
          "an idle, open peer owes nothing: kept, and nothing is due");
    for (std::uint64_t now = 1; now <= 10; ++now) {
        server.send(std::span(kSmall));
        std::this_thread::sleep_for(30ms);
        (void)server.sweep(1000 + now * 10, 1000 + now * 10 - 5);
    }
    check(server.frozen_closed() == 1 && client.link_up(), "a draining peer is never closed");
    check(server.frozen_closed() == 1, "the one close is counted");
}

/** @brief Accept one WS client on @p l, answer its handshake with a 101 followed by
 *         @p tail, and return the server's end; it never reads again. */
int ws_accept(const listener_t& l, std::string_view tail) {
    const int peer = ::accept(l.fd, nullptr, nullptr);
    std::string req;
    char c;
    while (req.find("\r\n\r\n") == std::string::npos && ::recv(peer, &c, 1, 0) == 1) req += c;
    const std::size_t k = req.find("Sec-WebSocket-Key: ") + 19;
    const std::string key = req.substr(k, req.find("\r\n", k) - k);
    std::string head =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    head += tr::net::ws::accept_key(key);
    head += "\r\n\r\n";
    head += tail;
    (void)::send(peer, head.data(), head.size(), MSG_NOSIGNAL);
    return peer;
}

void test_ws_client() {
    std::printf("ws client:\n");
    listener_t l;
    // A server that answers the handshake and then never reads.
    int peer = -1;
    std::thread srv([&] { peer = ws_accept(l, {}); });
    tr::net::ws_client_transport_t client("127.0.0.1", l.port, {});
    client.start_receiving();
    srv.join();
    check(client.ok() && peer >= 0, "the handshake completed, and the server never reads");
    check(client.sweep(100, 50) == 0 && client.frozen_closed() == 0,
          "an idle peer owes nothing: kept, and nothing is due");
    client.send(std::span(kBig));
    check(judge(client) == 1, "a frozen server is closed past the deadline");
    check(client.frozen_closed() == 1, "and the close is counted");
    check(sees_close(peer), "the server sees the connection go");
    ::close(peer);
    // RX owed (#2812): the server sends the first byte of a binary frame behind its 101 and
    // stops, its socket open; the client's own TX is idle.
    listener_t l2;
    std::thread srv2([&] { peer = ws_accept(l2, std::string_view("\x82", 1)); });
    tr::net::ws_client_transport_t stuck("127.0.0.1", l2.port, {});
    stuck.start_receiving();
    srv2.join();
    std::this_thread::sleep_for(200ms);  // the driving thread feeds the partial frame
    check(stuck.ok() && judge(stuck) == 1, "a server frozen mid-frame is closed");
    check(stuck.frozen_closed() == 1 && goes_down(stuck), "  counted, and the link goes down");
    check(sees_close(peer), "  the server sees the connection go");
    ::close(peer);
}

/**
 * @brief A self-healing link forwards the check to its current socket (#2812): a server
 *        frozen mid-frame is closed through it, and the link redials on the app's `now`.
 */
template <bool kOn = tr::net::kSelfHealLinks>
void test_self_heal_redials() {
    if constexpr (kOn) {
        std::printf("self-heal over a tcp dialer:\n");
        listener_t l;
        const timeval tv{.tv_sec = 5, .tv_usec = 0};  // bounds an accept that never comes
        ::setsockopt(l.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        tr::net::conn_settings_t settings;
        settings.addr = "127.0.0.1";
        settings.port = l.port;
        settings.backoff_ms = 1;
        std::atomic<int> ups{0};
        tr::net::self_heal_link_t engine(
            [](const tr::net::conn_settings_t& c, const tr::wire::tlv_node_t*,
               tr::mem::block_source_t& src) -> tr::graph::result_t<tr::net::transport_ptr_t> {
                return tr::mem::make_poly<tr::net::tcp_transport_t>(src, c.addr, c.port,
                                                                    tr::net::tcp_config_t{});
            },
            settings, std::span<const std::byte>{}, false, tr::mem::default_root());
        engine.set_liveness_publisher([&ups](tr::net::link_state_t s) {
            if (s == tr::net::link_state_t::UP) ups.fetch_add(1);
        });
        engine.born();
        engine.acquire();
        const int first = ::accept(l.fd, nullptr, nullptr);
        for (int i = 0; i < 500 && ups.load() == 0; ++i) std::this_thread::sleep_for(10ms);
        check(first >= 0 && ups.load() == 1, "the link dialed and came up");
        (void)::send(first, kPrefix16.data(), 2, MSG_NOSIGNAL);
        std::this_thread::sleep_for(200ms);
        (void)engine.sweep(1, 0);
        (void)engine.sweep(2, 1);
        (void)engine.sweep(10, 5);
        check(sees_close(first), "the frozen server is closed through the self-healing link");
        ::close(first);
        // The heal waits `backoff` on the app's clock: keep calling until it redials.
        listener_t* const lp = &l;
        int second = -1;
        std::thread acc([&] { second = ::accept(lp->fd, nullptr, nullptr); });
        for (std::uint64_t now = 11; now < 600 && ups.load() < 2; ++now) {
            (void)engine.sweep(now, now - 1);
            std::this_thread::sleep_for(10ms);
        }
        acc.join();
        check(ups.load() == 2 && second >= 0, "the link redialed and came up again");
        ::close(second);
        engine.release();
    }
}

}  // namespace

int main() {
    std::printf("== #2198: the application's liveness check closes frozen peers ==\n");
    test_tcp_dialer();
    test_tcp_server();
    test_ws_server();
    test_ws_client();
    test_self_heal_redials();
    return tr::testing::summary("frozen_peer_test");
}
