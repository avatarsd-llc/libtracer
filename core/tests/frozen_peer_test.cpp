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
 * it calls `transport_t::close_frozen_peers(now, deadline)` with `now` from its own clock,
 * and the vertex closes each peer that owed the wire progress (a frame held on it, or a
 * frame or handshake the peer began) and made none since its record fell below `deadline`.
 * libtracer reads no clock, so the test drives `now` as plain numbers.
 *
 * Per transport, three arms: a frozen peer is closed past the deadline and counted, never
 * before it; a draining peer is not; an idle peer, owing nothing, is not.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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
 *        stuck peer is still kept while its record is not older than the deadline; past it,
 *        a frozen one is closed.
 * @return What the final check closed.
 */
std::size_t judge(tr::net::transport_t& link) {
    std::size_t closed = link.close_frozen_peers(1, 0);  // stamps the record (moved since)
    closed += link.close_frozen_peers(2, 1);             // stuck, record 1 >= 1: kept
    check(closed == 0, "  nothing is closed while its record is within the deadline");
    return link.close_frozen_peers(10, 5);  // stuck, record 1 < 5: frozen
}

const std::vector<std::byte> kBig(8 * 1024 * 1024, std::byte{0x5a});
/** @brief A record a draining peer takes in a moment, well inside every receive cap. */
const std::vector<std::byte> kSmall(256 * 1024, std::byte{0x5b});

void test_tcp_dialer() {
    std::printf("tcp dialer:\n");
    {
        listener_t l;
        tr::net::tcp_transport_t link("127.0.0.1", l.port,
                                      {.liveness_window_ms = tr::net::kBoundedWaitMs});
        link.start_receiving();
        const int peer = ::accept(l.fd, nullptr, nullptr);
        check(link.ok() && peer >= 0, "dialed, and the peer never reads");
        check(link.close_frozen_peers(100, 50) == 0, "an idle peer owes nothing: kept");
        link.send(std::span(kBig));  // cut short: the rest is held on the wire
        check(judge(link) == 1, "a frozen peer is closed past the deadline");
        check(link.frozen_closed() == 1, "and the close is counted");
        check(sees_close(peer), "the peer sees the connection go");
        ::close(peer);
    }
    {
        listener_t l;
        tr::net::tcp_transport_t link("127.0.0.1", l.port,
                                      {.liveness_window_ms = tr::net::kBoundedWaitMs});
        link.start_receiving();
        const int peer = ::accept(l.fd, nullptr, nullptr);
        std::atomic<bool> stop{false};
        std::thread reader([&] { drain(peer, stop); });
        std::size_t closed = 0;
        for (std::uint64_t now = 1; now <= 20; ++now) {
            link.send(std::span(kSmall));
            std::this_thread::sleep_for(20ms);
            closed += link.close_frozen_peers(now * 10, now * 10 - 5);
        }
        stop = true;
        reader.join();
        check(closed == 0 && link.frozen_closed() == 0 && link.link_up(),
              "a draining peer is never closed");
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
    check(server.close_frozen_peers(100, 50) == 0, "an idle peer owes nothing: kept");
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
    std::size_t closed = 0;
    for (std::uint64_t now = 1; now <= 20; ++now) {
        server.send(std::span(kSmall));
        std::this_thread::sleep_for(20ms);
        closed += server.close_frozen_peers(now * 10, now * 10 - 5);
    }
    stop = true;
    reader.join();
    check(closed == 0 && server.frozen_closed() == 2, "a draining peer is never closed");
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
    check(server.close_frozen_peers(100, 50) == 0 && server.close_frozen_peers(200, 150) == 0,
          "an idle, open peer owes nothing: kept");
    std::size_t closed = 0;
    for (std::uint64_t now = 1; now <= 10; ++now) {
        server.send(std::span(kSmall));
        std::this_thread::sleep_for(30ms);
        closed += server.close_frozen_peers(1000 + now * 10, 1000 + now * 10 - 5);
    }
    check(closed == 0 && client.link_up(), "a draining peer is never closed");
    check(server.frozen_closed() == 1, "the one close is counted");
}

void test_ws_client() {
    std::printf("ws client:\n");
    listener_t l;
    // A server that answers the handshake and then never reads.
    int peer = -1;
    std::thread srv([&] {
        peer = ::accept(l.fd, nullptr, nullptr);
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
        (void)::send(peer, head.data(), head.size(), MSG_NOSIGNAL);
    });
    tr::net::ws_client_transport_t client("127.0.0.1", l.port,
                                          {.liveness_window_ms = tr::net::kBoundedWaitMs});
    client.start_receiving();
    srv.join();
    check(client.ok() && peer >= 0, "the handshake completed, and the server never reads");
    check(client.close_frozen_peers(100, 50) == 0, "an idle peer owes nothing: kept");
    client.send(std::span(kBig));
    check(judge(client) == 1, "a frozen server is closed past the deadline");
    check(client.frozen_closed() == 1, "and the close is counted");
    check(sees_close(peer), "the server sees the connection go");
    ::close(peer);
}

}  // namespace

int main() {
    std::printf("== #2198: the application's liveness check closes frozen peers ==\n");
    test_tcp_dialer();
    test_tcp_server();
    test_ws_server();
    test_ws_client();
    return tr::testing::summary("frozen_peer_test");
}
