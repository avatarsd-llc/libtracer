/**
 * @file
 * @brief #2305 — the WS client's masking-key stream is seeded from the OS entropy source once
 *        per dial, never from a clock.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC 6455 §5.3 masking only needs keys that vary between connections. The seed used to be a
 * `steady_clock` read mixed with the link's address; it is now one `getrandom` read mixed with
 * the address. A raw loopback listener answers the client's Upgrade with a valid 101 and reads
 * the masking key of the first frame the client sends. Two links dialled one after the other,
 * from the same stack frame, usually reuse one address, so the address mix alone cannot tell
 * them apart: different first keys show the entropy read is in the seed.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

/** @brief What one dial showed the raw listener. */
struct seen_t {
    bool ok = false;                   /**< @brief A 101 went out and a frame header came back. */
    std::array<std::uint8_t, 4> key{}; /**< @brief The first frame's masking key. */
    std::uintptr_t self = 0;           /**< @brief The client object's address. */
};

/** @brief Read until @p want bytes arrived, the peer closed, or the receive timeout hit. */
bool read_exact(int fd, std::span<std::uint8_t> want) {
    std::size_t got = 0;
    while (got < want.size()) {
        const ssize_t n = ::recv(fd, want.data() + got, want.size() - got, 0);
        if (n <= 0) return false;
        got += static_cast<std::size_t>(n);
    }
    return true;
}

/** @brief Accept one client, answer its Upgrade, and read its first frame's masking key. */
void serve_one(int lfd, seen_t& out) {
    const int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) return;
    const timeval tv{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string req;
    std::array<char, 512> buf{};
    while (req.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(cfd, buf.data(), buf.size(), 0);
        if (n <= 0) break;
        req.append(buf.data(), static_cast<std::size_t>(n));
    }
    constexpr std::string_view kKey = "Sec-WebSocket-Key: ";
    const std::size_t at = req.find(kKey);
    const std::size_t end = at == std::string::npos ? at : req.find("\r\n", at);
    if (end != std::string::npos) {
        const std::string_view key =
            std::string_view(req).substr(at + kKey.size(), end - at - kKey.size());
        const std::string resp = std::string(
                                     "HTTP/1.1 101 Switching Protocols\r\n"
                                     "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                     "Sec-WebSocket-Accept: ") +
                                 std::string(tr::net::ws::accept_key(key).view()) + "\r\n\r\n";
        std::array<std::uint8_t, 6> head{};  // 2 header bytes + the 4-byte masking key
        out.ok = ::send(cfd, resp.data(), resp.size(), MSG_NOSIGNAL) ==
                     static_cast<ssize_t>(resp.size()) &&
                 read_exact(cfd, head) && (head[1] & 0x80u) != 0;
        for (std::size_t i = 0; i < 4; ++i) out.key[i] = head[2 + i];
    }
    ::close(cfd);
}

/** @brief Dial the listener on @p port once and send one frame; the client dies on return. */
void dial_once(int lfd, std::uint16_t port, seen_t& out) {
    std::thread server([lfd, &out] { serve_one(lfd, out); });
    {
        tr::net::ws_client_transport_t client("127.0.0.1", port);
        out.self = reinterpret_cast<std::uintptr_t>(&client);
        if (client.ok()) {
            const std::byte one[1] = {std::byte{0x01}};
            client.send(std::span<const std::byte>(one));
        }
        server.join();
    }
}

void test_two_dials_get_different_mask_streams() {
    std::printf("ws client — two dials get different masking keys:\n");
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    check(lfd >= 0, "raw listener created");
    const int on = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(::bind(lfd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "listener bound");
    check(::listen(lfd, 2) == 0, "listener listening");
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&bound), &blen);
    const std::uint16_t port = ntohs(bound.sin_port);

    seen_t a;
    seen_t b;
    dial_once(lfd, port, a);
    dial_once(lfd, port, b);
    ::close(lfd);

    std::printf("        client addresses %s\n", a.self == b.self ? "equal" : "differ");
    check(a.ok && b.ok, "both dials completed the handshake and sent a masked frame");
    check(a.key != b.key, "the two links' first masking keys differ");
}

}  // namespace

int main() {
    test_two_dials_get_different_mask_streams();
    return tr::testing::summary("ws_mask_seed");
}
