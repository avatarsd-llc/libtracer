/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #54 — transport_ws SERVER socket-layer tests. A transport_ws_server binds an
 * ephemeral localhost port; the test drives it with a raw TCP client: send a
 * correct RFC 6455 Upgrade request, verify the 101 response carries the right
 * Sec-WebSocket-Accept (cross-checked with ws::accept_key), then (a) send a
 * MASKED client BINARY frame and assert the transport_t receiver got exactly the
 * payload, and (b) call server.send() and assert the client reads back a server
 * BINARY frame ws::decode_frame()s to those bytes. Built under TSan (recv thread
 * + receiver handoff) and ASan+UBSan. The client role is a later increment.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"

namespace {

using namespace std::chrono_literals;
namespace ws = tr::net::ws;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// Connect a raw TCP client to 127.0.0.1:port. Returns the fd, or -1.
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

void write_str(int fd, std::string_view s) {
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += static_cast<std::size_t>(n);
    }
}

void write_bytes(int fd, std::span<const std::byte> b) {
    std::size_t off = 0;
    while (off < b.size()) {
        const ssize_t n = ::send(fd, b.data() + off, b.size() - off, 0);
        if (n <= 0) return;
        off += static_cast<std::size_t>(n);
    }
}

// Read up to `cap` bytes with a per-read poll timeout; stops when `done(buf)` is
// true or the deadline passes. Keeps the test deterministic.
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

// Build a MASKED client→server frame (RFC 6455 §5.3): FIN=1, given opcode, the
// MASK bit set with a 4-byte key, payload XOR-masked. Small payloads only (<126).
std::vector<std::byte> masked_client_frame(ws::opcode_t op, std::span<const std::byte> payload,
                                           std::array<std::uint8_t, 4> mask) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(op)));  // FIN=1
    out.push_back(
        static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(payload.size())));  // MASK=1
    for (std::uint8_t m : mask) out.push_back(static_cast<std::byte>(m));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out.push_back(
            static_cast<std::byte>(std::to_integer<std::uint8_t>(payload[i]) ^ mask[i % 4]));
    }
    return out;
}

void test_handshake_and_frames() {
    std::printf("transport_ws server — handshake + masked recv + server send:\n");

    tr::net::transport_ws_server server(0);
    check(server.ok(), "listen socket bound");
    const std::uint16_t port = server.local_port();
    check(port != 0, "ephemeral port resolved");

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    server.set_receiver([&](std::span<const std::byte> f) {
        got.set_value(std::vector<std::byte>(f.begin(), f.end()));
    });

    const int cfd = tcp_connect(port);
    check(cfd >= 0, "raw TCP client connected");

    // --- Opening handshake ---
    const std::string client_key = "dGhlIHNhbXBsZSBub25jZQ==";  // RFC 6455 §1.3 example
    std::string upgrade =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: ";
    upgrade += client_key;
    upgrade += "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    write_str(cfd, upgrade);

    const auto resp_bytes = read_until(
        cfd,
        [](const std::vector<std::byte>& b) {
            return std::string_view(reinterpret_cast<const char*>(b.data()), b.size())
                       .find("\r\n\r\n") != std::string_view::npos;
        },
        2s);
    const std::string resp(reinterpret_cast<const char*>(resp_bytes.data()), resp_bytes.size());
    check(resp.find("101 Switching Protocols") != std::string::npos, "got 101 Switching Protocols");
    check(resp.find("Upgrade: websocket") != std::string::npos, "response has Upgrade: websocket");
    check(resp.find("Connection: Upgrade") != std::string::npos,
          "response has Connection: Upgrade");
    const std::string expect_accept = ws::accept_key(client_key);
    check(resp.find("Sec-WebSocket-Accept: " + expect_accept) != std::string::npos,
          "Sec-WebSocket-Accept matches ws::accept_key");

    // --- (a) client → server: a MASKED BINARY frame carrying TLV bytes ---
    const std::array<std::byte, 5> tlv{std::byte{0x01}, std::byte{0x07}, std::byte{0xDE},
                                       std::byte{0xAD}, std::byte{0xBE}};
    const std::array<std::uint8_t, 4> mask{0x37, 0xFA, 0x21, 0x3D};
    const auto frame = masked_client_frame(ws::opcode_t::BINARY, tlv, mask);
    write_bytes(cfd, frame);

    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "server receiver fired on inbound BINARY frame");
    if (arrived) {
        const auto r = fut.get();
        check(r.size() == tlv.size() && std::memcmp(r.data(), tlv.data(), tlv.size()) == 0,
              "unmasked payload matches the sent TLV bytes");
    }

    // --- (b) server → client: server.send() produces an unmasked BINARY frame ---
    const std::array<std::byte, 4> out_tlv{std::byte{0x01}, std::byte{0x02}, std::byte{0xCA},
                                           std::byte{0xFE}};
    server.send(out_tlv);

    const auto srv_bytes = read_until(
        cfd, [](const std::vector<std::byte>& b) { return ws::decode_frame(b).has_value(); }, 2s);
    auto decoded = ws::decode_frame(srv_bytes);
    check(decoded.has_value(), "client decoded a server frame");
    if (decoded) {
        const auto& f = decoded->first;
        check(f.op == ws::opcode_t::BINARY, "server frame is BINARY");
        check(f.payload.size() == out_tlv.size() &&
                  std::memcmp(f.payload.data(), out_tlv.data(), out_tlv.size()) == 0,
              "server BINARY payload matches server.send() bytes");
    }

    ::close(cfd);
}

}  // namespace

int main() {
    test_handshake_and_frames();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
