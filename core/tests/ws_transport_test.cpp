/**
 * @file
 * @brief #54 — transport_ws SERVER socket-layer tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A transport_ws_server binds an
 * ephemeral localhost port; the test drives it with a raw TCP client: send a
 * correct RFC 6455 Upgrade request, verify the 101 response carries the right
 * Sec-WebSocket-Accept (cross-checked with ws::accept_key), then (a) send a
 * MASKED client BINARY frame and assert the transport_t receiver got exactly the
 * payload, and (b) call server.send() and assert the client reads back a server
 * BINARY frame ws::decode_frame()s to those bytes. Built under TSan (recv thread
 * + receiver handoff) and ASan+UBSan.
 *
 * A second test wires a transport_ws_client into a transport_ws_server and
 * asserts a full round trip (client.send → server receiver, server.send → client
 * receiver) — the real integration test for the dial-out (client) half (#54).
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/rope.hpp"
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

/**
 * @brief Connect a raw TCP client to 127.0.0.1:port.
 *
 * Returns the fd, or -1.
 */
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

/**
 * @brief Read up to `cap` bytes with a per-read poll timeout; stops when `done(buf)` is true or the
 *        deadline passes.
 *
 * Keeps the test deterministic.
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

/**
 * @brief Build a MASKED client→server frame (RFC 6455 §5.3): FIN=1, given opcode, the MASK bit set
 *        with a 4-byte key, payload XOR-masked.
 *
 * Small payloads only (<126).
 */
std::vector<std::byte> masked_client_frame(ws::opcode_t op, std::span<const std::byte> payload,
                                           std::array<std::uint8_t, 4> mask, bool fin = true) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>((fin ? 0x80u : 0x00u) | static_cast<std::uint8_t>(op)));
    out.push_back(
        static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(payload.size())));  // MASK=1
    for (std::uint8_t m : mask) out.push_back(static_cast<std::byte>(m));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out.push_back(
            static_cast<std::byte>(std::to_integer<std::uint8_t>(payload[i]) ^ mask[i % 4]));
    }
    return out;
}

/**
 * @brief Drive the RFC 6455 opening handshake on a raw client fd.
 *
 * Returns true on 101.
 */
bool raw_handshake(int cfd) {
    const std::string client_key = "dGhlIHNhbXBsZSBub25jZQ==";
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
    return resp.find("101 Switching Protocols") != std::string::npos;
}

/**
 * @brief A fragmented message reaches the OWNING sink as the rope its fragments already are — one
 *        owning link per fragment, chained, never memcpy'd flat (ADR-0053 §5) — with an interleaved
 *        control frame (PING) handled mid-message per RFC 6455.
 */
void test_fragmented_message_rope() {
    std::printf("transport_ws server — fragmented message -> rope (ADR-0053):\n");

    // The promise + named receiver lambda live BEFORE the transport: the slot
    // binds the callable by address, and the server dtor joins the recv thread.
    std::promise<tr::view::rope_t> got;
    auto fut = got.get_future();
    auto rope_rx = [&](tr::view::rope_t msg) { got.set_value(std::move(msg)); };
    tr::net::transport_ws_server server(0);
    check(server.ok() && server.delivers_ropes(), "server up; delivers_ropes() is true");

    server.set_rope_receiver(rope_rx);

    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client connected + 101 handshake");

    const std::array<std::uint8_t, 4> mask{0x37, 0xFA, 0x21, 0x3D};
    const auto part = [](const char* t) {
        std::vector<std::byte> v(std::strlen(t));
        std::memcpy(v.data(), t, v.size());
        return v;
    };
    const auto p1 = part("AAAA"), p2 = part("BBB"), p3 = part("CC");
    write_bytes(cfd, masked_client_frame(ws::opcode_t::BINARY, p1, mask, /*fin=*/false));
    write_bytes(cfd, masked_client_frame(ws::opcode_t::CONT, p2, mask, /*fin=*/false));
    // A control frame MAY be injected in the middle of a fragmented message
    // (RFC 6455 §5.4) — it must not disturb the assembly.
    write_bytes(cfd, masked_client_frame(ws::opcode_t::PING, p3, mask));
    write_bytes(cfd, masked_client_frame(ws::opcode_t::CONT, p3, mask, /*fin=*/true));

    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "one completed message delivered to the rope sink");
    if (arrived) {
        const tr::view::rope_t msg = fut.get();
        check(msg.link_count() == 3, "one owning link per fragment (chained, not flattened)");
        check(msg.total_length() == 9, "total length == sum of fragments");
        std::vector<std::byte> flat;
        msg.walk(
            [&](std::span<const std::byte> sp) { flat.insert(flat.end(), sp.begin(), sp.end()); });
        check(flat.size() == 9 && std::memcmp(flat.data(), "AAAABBBCC", 9) == 0,
              "reassembled bytes are byte-exact");
    }
    ::close(cfd);
}

/**
 * @brief The same fragmented message on the SPAN tier: delivered once, byte-exact (the borrowed
 *        tier pays the single flatten inside the transport).
 */
void test_fragmented_message_span() {
    std::printf("transport_ws server — fragmented message -> span tier:\n");

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    auto rx = [&](std::span<const std::byte> f) {
        got.set_value(std::vector<std::byte>(f.begin(), f.end()));
    };
    tr::net::transport_ws_server server(0);
    check(server.ok(), "server up");

    server.set_receiver(rx);

    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client connected + 101 handshake");

    const std::array<std::uint8_t, 4> mask{0x11, 0x22, 0x33, 0x44};
    const std::array<std::byte, 3> a{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 2> b{std::byte{4}, std::byte{5}};
    write_bytes(cfd, masked_client_frame(ws::opcode_t::BINARY, a, mask, /*fin=*/false));
    write_bytes(cfd, masked_client_frame(ws::opcode_t::CONT, b, mask, /*fin=*/true));

    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "one completed message delivered to the span sink");
    if (arrived) {
        const auto r = fut.get();
        const std::array<std::byte, 5> want{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                            std::byte{5}};
        check(r.size() == 5 && std::memcmp(r.data(), want.data(), 5) == 0,
              "span delivery is byte-exact (single flatten inside the transport)");
    }
    ::close(cfd);
}

void test_handshake_and_frames() {
    std::printf("transport_ws server — handshake + masked recv + server send:\n");

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    auto rx = [&](std::span<const std::byte> f) {
        got.set_value(std::vector<std::byte>(f.begin(), f.end()));
    };
    tr::net::transport_ws_server server(0);
    check(server.ok(), "listen socket bound");
    const std::uint16_t port = server.local_port();
    check(port != 0, "ephemeral port resolved");

    server.set_receiver(rx);

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

/**
 * @brief Two real transport_t endpoints over a live WS connection: a transport_ws_server and a
 *        transport_ws_client dialing into it.
 *
 * Asserts a FULL round trip — the
 * client's MASKED BINARY frame surfaces at the server's receiver as exact bytes,
 * and the server's UNMASKED BINARY frame surfaces at the client's receiver as
 * exact bytes. Deterministic via futures with a deadline. This is the real
 * integration test for the dial-out (client) half (#54).
 */
void test_client_server_roundtrip() {
    std::printf("transport_ws client <-> server — full round trip:\n");

    std::promise<std::vector<std::byte>> srv_got;
    auto srv_fut = srv_got.get_future();
    auto srv_rx = [&](std::span<const std::byte> f) {
        srv_got.set_value(std::vector<std::byte>(f.begin(), f.end()));
    };
    std::promise<std::vector<std::byte>> cli_got;
    auto cli_fut = cli_got.get_future();
    auto cli_rx = [&](std::span<const std::byte> f) {
        cli_got.set_value(std::vector<std::byte>(f.begin(), f.end()));
    };

    tr::net::transport_ws_server server(0);
    check(server.ok(), "server listen socket bound");
    const std::uint16_t port = server.local_port();
    check(port != 0, "ephemeral port resolved");

    server.set_receiver(srv_rx);

    tr::net::transport_ws_client client("127.0.0.1", port);
    check(client.ok(), "client connected + 101 Sec-WebSocket-Accept verified");

    client.set_receiver(cli_rx);

    // --- client → server: client.send() emits a MASKED BINARY frame ---
    const std::array<std::byte, 5> c2s{std::byte{0x01}, std::byte{0x07}, std::byte{0xDE},
                                       std::byte{0xAD}, std::byte{0xBE}};
    client.send(c2s);
    const bool got_at_server = srv_fut.wait_for(2s) == std::future_status::ready;
    check(got_at_server, "server receiver fired on client.send()");
    if (got_at_server) {
        const auto r = srv_fut.get();
        check(r.size() == c2s.size() && std::memcmp(r.data(), c2s.data(), c2s.size()) == 0,
              "server got the exact bytes client.send() emitted");
    }

    // --- server → client: server.send() emits an UNMASKED BINARY frame ---
    const std::array<std::byte, 4> s2c{std::byte{0x01}, std::byte{0x02}, std::byte{0xCA},
                                       std::byte{0xFE}};
    server.send(s2c);
    const bool got_at_client = cli_fut.wait_for(2s) == std::future_status::ready;
    check(got_at_client, "client receiver fired on server.send()");
    if (got_at_client) {
        const auto r = cli_fut.get();
        check(r.size() == s2c.size() && std::memcmp(r.data(), s2c.data(), s2c.size()) == 0,
              "client got the exact bytes server.send() emitted");
    }
}

/** @brief A collecting span sink with a deadline wait (multi-shot, unlike a promise). */
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
};

/** @brief The peer-named twin of @ref frame_sink_t (the bus_link_t sink shape). */
struct peer_sink_t {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::vector<std::byte>>> frames;

    /** @brief The peer-named receiver callable. */
    void operator()(std::string_view peer, std::span<const std::byte> f) {
        {
            const std::lock_guard lock(m);
            frames.emplace_back(std::string(peer), std::vector<std::byte>(f.begin(), f.end()));
        }
        cv.notify_all();
    }
    /** @brief True once at least @p n frames arrived before @p timeout. */
    bool wait_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
};

/**
 * @brief #362 — the multi-peer server: two concurrent clients, peer-named inbound
 *        delivery through the bus_link_t facet, broadcast send, a DIRECTED
 *        peer_link send reaching exactly one peer, and live peer enumeration
 *        tracking a departure.
 */
void test_multi_peer_bus() {
    std::printf("transport_ws server — multi-peer (#362, bus facet):\n");

    // Sinks live BEFORE the transports (the file's destruction-order idiom):
    // every transport joins its recv thread before the sink it delivers to dies.
    peer_sink_t srv_sink;
    frame_sink_t a_sink;
    frame_sink_t b_sink;

    tr::net::transport_ws_server server(0, /*max_peers=*/0, /*peer_named=*/true);
    check(server.ok(), "listen socket bound");
    const std::uint16_t port = server.local_port();
    check(server.bus() != nullptr, "peer_named server exposes the bus_link_t facet (ADR-0044)");
    server.bus()->set_peer_receiver(srv_sink);

    tr::net::transport_ws_client a("127.0.0.1", port);
    a.set_receiver(a_sink);
    std::optional<tr::net::transport_ws_client> b;
    b.emplace("127.0.0.1", port);
    b->set_receiver(b_sink);
    check(a.ok() && b->ok(), "TWO clients connected concurrently (listen(fd,1) era over)");

    // --- inbound: each client's frame arrives tagged with a DISTINCT peer name ---
    const std::array<std::byte, 2> pa{std::byte{0x01}, std::byte{0xA1}};
    const std::array<std::byte, 2> pb{std::byte{0x01}, std::byte{0xB1}};
    a.send(pa);
    b->send(pb);
    check(srv_sink.wait_count(2, 2s), "server got both clients' frames");
    std::string name_a;
    {
        const std::lock_guard lock(srv_sink.m);
        check(srv_sink.frames[0].first != srv_sink.frames[1].first,
              "the two deliveries carry two distinct peer names");
        for (const auto& [peer, bytes] : srv_sink.frames)
            if (bytes == std::vector<std::byte>(pa.begin(), pa.end())) name_a = peer;
    }
    check(!name_a.empty(), "client a's frame is identifiable by its peer tag");

    // --- enumeration: both peers audible ---
    std::size_t n_peers = 0;
    server.bus()->enumerate_peers([&](std::string_view) { ++n_peers; });
    check(n_peers == 2, "enumerate_peers lists both open peers");

    // --- broadcast: the flat send() reaches every open peer ---
    const std::array<std::byte, 2> bc{std::byte{0x01}, std::byte{0xCC}};
    server.send(bc);
    check(a_sink.wait_count(1, 2s) && b_sink.wait_count(1, 2s),
          "flat server.send() broadcast to both clients");

    // --- directed: peer_link(name)->send() reaches exactly that peer ---
    tr::net::transport_t* const link_a = server.bus()->peer_link(name_a);
    check(link_a != nullptr, "peer_link resolves client a's name");
    const std::array<std::byte, 2> da{std::byte{0x01}, std::byte{0xDA}};
    if (link_a != nullptr) link_a->send(da);
    check(a_sink.wait_count(2, 2s), "directed send reached client a");
    check(!b_sink.wait_count(2, std::chrono::milliseconds(300)),
          "directed send did NOT reach client b");

    // --- departure: closing b frees its slot; enumeration tracks it ---
    b.reset();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::size_t live = 2;
    while (std::chrono::steady_clock::now() < deadline) {
        live = 0;
        server.bus()->enumerate_peers([&](std::string_view) { ++live; });
        if (live == 1) break;
        std::this_thread::sleep_for(20ms);
    }
    check(live == 1, "departed peer left enumeration (slot recycled)");
    check(server.bus()->peer_link(name_a) != nullptr, "surviving peer still resolves");
}

/** @brief #362 — the max_peers deployment cap: a peer beyond it is refused cleanly,
 *         and a departure frees the slot for the next connection. */
void test_max_peers_cap() {
    std::printf("transport_ws server — max_peers admission cap (#362):\n");

    tr::net::transport_ws_server server(0, /*max_peers=*/1);
    check(server.ok(), "capped server bound");
    const std::uint16_t port = server.local_port();

    std::optional<tr::net::transport_ws_client> a;
    a.emplace("127.0.0.1", port);
    check(a->ok(), "first client admitted");

    const tr::net::transport_ws_client b("127.0.0.1", port);
    check(!b.ok(), "second client refused cleanly at the cap (handshake never completes)");

    a.reset();  // departure frees the slot...
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool readmitted = false;
    while (!readmitted && std::chrono::steady_clock::now() < deadline) {
        const tr::net::transport_ws_client c("127.0.0.1", port);
        readmitted = c.ok();
        if (!readmitted) std::this_thread::sleep_for(50ms);
    }
    check(readmitted, "...and the next client is admitted into the recycled slot");
}

}  // namespace

int main() {
    test_handshake_and_frames();
    test_fragmented_message_rope();
    test_fragmented_message_span();
    test_client_server_roundtrip();
    test_multi_peer_bus();
    test_max_peers_cap();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
