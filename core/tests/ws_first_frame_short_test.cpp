/**
 * @file
 * @brief #2781 — a server session's first frame that the socket cuts short is finished on
 *        writability, not left waiting for the next send to that peer.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A WebSocket server writes its `101` response as the session's first frame, through
 * `egress_t::flush`, before any writability event. That flush armed no write interest, so a
 * `101` the socket took only part of stayed held until something else was sent to the peer;
 * a client waiting for the rest of its handshake never got it.
 *
 * The short write is forced, not hoped for: this binary links with `--wrap=send`, and the
 * wrapper lets the server's first write move ONE byte and refuses the retry of the rest
 * (`EAGAIN`, a full socket), so the frame is held. A raw client then sends a legal
 * Upgrade request and reads. Before the fix it reads `H` and then nothing for its whole 3 s
 * budget; with it, the server's driving thread resumes the frame on writability and the
 * client reads the full `101`.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include "libtracer/transport_ws.hpp"
#include "test_support.hpp"

using tr::testing::check;
using namespace std::chrono_literals;

namespace {

std::atomic<std::uint32_t> g_server{0}; /**< @brief The server's port: only ITS write is cut. */
/** @brief The server writes left to cut: at 2 the next moves one byte, at 1 it is refused. */
std::atomic<int> g_cut{0};

/** @brief The local port of socket @p fd, or 0. */
std::uint16_t local_port(int fd) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

}  // namespace

extern "C" {
ssize_t __real_send(int fd, const void* buf, std::size_t len, int flags);

/** @brief The link-time wrap of `send(2)`: the server's first write moves one byte, and the
 *         retry of the rest finds the socket full, as a peer that stopped reading leaves it. */
ssize_t __wrap_send(int fd, const void* buf, std::size_t len, int flags) {
    if (g_cut.load() > 0 && local_port(fd) == g_server.load()) {
        if (g_cut.fetch_sub(1) == 1) {
            errno = EAGAIN;
            return -1;
        }
        len = 1;
    }
    return __real_send(fd, buf, len, flags);
}
}

namespace {

/** @brief The legal RFC 6455 opening-handshake request the raw client sends. */
constexpr std::string_view kUpgradeRequest =
    "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

/** @brief Connect a raw TCP client to `127.0.0.1:port`; -1 on failure. */
int tcp_connect(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/** @brief Read from @p fd until the `101` header block ends, the peer closes, or 3 s pass. */
std::string_view read_101(int fd, std::vector<char>& buf) {
    std::array<char, 512> chunk;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::string_view(buf.data(), buf.size()).find("\r\n\r\n") == std::string_view::npos &&
           std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, 25) <= 0) continue;
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) break;
        buf.insert(buf.end(), chunk.data(), chunk.data() + n);
    }
    return {buf.data(), buf.size()};
}

/** @brief The `101` a socket cuts short after one byte still reaches the client whole. */
void test_cut_short_101_finishes() {
    std::printf("ws server — a 101 the socket cuts short finishes on writability:\n");
    tr::net::ws_server_transport_t server(0);
    g_server.store(server.local_port());
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0, "raw client connected");
    g_cut.store(2);
    check(::send(cfd, kUpgradeRequest.data(), kUpgradeRequest.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(kUpgradeRequest.size()),
          "the Upgrade request is sent");
    std::vector<char> buf;
    const std::string_view got = read_101(cfd, buf);
    check(g_cut.load() == 0, "the server's first write was cut short after one byte");
    check(got.starts_with("HTTP/1.1 101 Switching Protocols\r\n"), "the status line arrives");
    check(got.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n") !=
              std::string_view::npos,
          "and the rest of the 101 follows it");
    ::close(cfd);
}

}  // namespace

int main() {
    test_cut_short_101_finishes();
    return tr::testing::summary("ws_first_frame_short");
}
