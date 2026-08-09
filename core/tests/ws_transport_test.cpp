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
 *
 * The last group inverts the roles: a raw socket acts as a HOSTILE SERVER and our
 * transport_ws_client dials it, which is the only way to reach the client's own control-frame
 * reply and teardown paths over a socket (#1010).
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
#include <utility>
#include <vector>

#include "libtracer/path.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
namespace ws = tr::net::ws;

using tr::testing::check;

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

/**
 * @brief Write every byte of @p b to @p fd.
 *
 * `MSG_NOSIGNAL` is load-bearing here, not hygiene: the §5.5 client vectors below make the
 * transport under test FAIL the connection mid-write, so the peer really does vanish under
 * this loop, and the default `send` would take the process down with SIGPIPE.
 */
void write_bytes(int fd, std::span<const std::byte> b) {
    std::size_t off = 0;
    while (off < b.size()) {
        const ssize_t n = ::send(fd, b.data() + off, b.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += static_cast<std::size_t>(n);
    }
}

/**
 * @brief Read up to `cap` bytes with a per-read poll timeout; stops when `done(buf)` is true or the
 *        deadline passes.
 *
 * Keeps the test deterministic. @p closed (when given) reports that the PEER went away
 * during the window — an orderly FIN (`recv` == 0) or a reset (`recv` < 0), which are the
 * two shapes a `teardown_peer` on the other end can take depending on whether bytes were
 * still queued for it. A budget that simply expires leaves it untouched.
 */
template <typename Done>
std::vector<std::byte> read_until(int fd, Done done, std::chrono::milliseconds budget,
                                  bool* closed = nullptr) {
    std::vector<std::byte> buf;
    std::array<std::byte, 1024> chunk;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done(buf) && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, 50) <= 0) continue;
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) {
            if (closed != nullptr) *closed = true;
            break;
        }
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
 * @brief A peer is OPEN to senders the instant its `101` is on the wire — the handshake
 *        window is closed, proven by holding it open rather than by racing for it.
 *
 * `transport_ws_server::on_readable` writes the `101 Switching Protocols` response and
 * publishes the slot
 * (`open = true`) inside ONE `write_m_` critical section. Store `open` after that lock is
 * released and there is a window in which the peer has already read the response — so it
 * believes the connection is up — while every `send` still skips the slot as not-open and
 * the frame goes nowhere. In production that window is a few instructions and shows up only
 * as an occasional lost first frame (it went from 0/40 to 1/40 the moment #848 made the
 * server egress fast enough to reach `send` inside it), which is exactly the kind of fix a
 * racing test cannot pin.
 *
 * So this test does not race: `detail::ws_peer_published_hook` PARKS the server's poll
 * thread at the instant after the response write, the test does its whole `send` inside that
 * parked window, and only then releases it. With the store inside the lock the send lands
 * and the client decodes the frame; with the store moved back out, the identical sequence
 * reads an empty socket until the budget expires. Deterministic in both directions.
 */
void test_peer_open_before_response_is_readable() {
    std::printf("transport_ws server — a peer is open to senders the instant its 101 lands:\n");

    tr::net::transport_ws_server server(0);
    check(server.ok(), "listen socket bound");

    // The parked window. `reached` is set on the server's poll thread; `released` is set by
    // this thread once its send has been made.
    std::mutex m;
    std::condition_variable cv;
    bool reached = false;
    bool released = false;
    bool armed = true;  // one-shot: later peers (none here) must not park
    struct hook_state_t {
        std::mutex* m;
        std::condition_variable* cv;
        bool* reached;
        bool* released;
        bool* armed;
    };
    static hook_state_t s_state{};  // the hook is a plain function pointer: no capture
    s_state = hook_state_t{&m, &cv, &reached, &released, &armed};

    /** @brief Clears the seam however this test leaves. */
    struct hook_guard_t {
        ~hook_guard_t() { tr::net::detail::ws_peer_published_hook = nullptr; }
    } const guard;

    tr::net::detail::ws_peer_published_hook = [] {
        std::unique_lock lock(*s_state.m);
        if (!*s_state.armed) return;
        *s_state.armed = false;
        *s_state.reached = true;
        s_state.cv->notify_all();
        // Park the handshake here until the test has sent. Bounded so a broken build fails
        // the assertions rather than hanging the suite.
        s_state.cv->wait_for(lock, 5s, [] { return *s_state.released; });
    };

    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client connected + 101 handshake");

    {
        std::unique_lock lock(m);
        check(cv.wait_for(lock, 2s, [&] { return reached; }),
              "the server reached the instant just past the 101 write");
    }

    // The whole send happens INSIDE the parked window: nothing but the publish-under-lock
    // can make this frame reach the peer.
    const std::array<std::byte, 4> payload{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                           std::byte{0xEF}};
    server.send(std::span<const std::byte>(payload));

    {
        const std::lock_guard lock(m);
        released = true;
    }
    cv.notify_all();

    const auto got = read_until(
        cfd, [](const std::vector<std::byte>& b) { return ws::decode_frame(b).has_value(); }, 2s);
    const auto dec = ws::decode_frame(got);
    check(dec.has_value(), "the frame sent in that instant REACHED the peer");
    if (dec)
        check(dec->first.payload.size() == payload.size() &&
                  std::memcmp(dec->first.payload.data(), payload.data(), payload.size()) == 0,
              "and it is byte-identical to what was sent");

    ::close(cfd);
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

/**
 * @brief #1060 — a RESERVED data frame injected BETWEEN two fragments FAILS the connection
 *        instead of being stepped over while the assembler stitches around it.
 *
 * The consequence the transports' `default:` arm hid. Reassembly state is touched only in the
 * `BINARY`/`CONT` arm, so a reserved frame — sorted as DATA by `is_control_opcode`, hence
 * subject to no decoder rule at all before this — was dropped with `assembling` untouched and
 * the two halves were joined as though nothing had arrived between them. RFC 6455 §5.4 permits
 * exactly one thing to be interleaved into a fragmented message, a CONTROL frame, and
 * `test_fragmented_message_rope` above pins that a PING still may be.
 *
 * The second assertion is what makes this vector about FRAGMENTATION rather than a third copy
 * of the plain reserved-opcode case: with the §5.2 clause ablated the connection stays up AND
 * `12` + `345` is delivered as one 5-byte message, so both assertions redden together.
 */
void test_reserved_frame_between_fragments_fails_the_connection() {
    std::printf("transport_ws server — a reserved frame between two fragments fails it (#1060):\n");

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
    const std::array<std::byte, 2> head{std::byte{1}, std::byte{2}};
    const std::array<std::byte, 3> tail{std::byte{3}, std::byte{4}, std::byte{5}};
    const std::array<std::byte, 2> junk{std::byte{0x5A}, std::byte{0x5A}};
    write_bytes(cfd, masked_client_frame(ws::opcode_t::BINARY, head, mask, /*fin=*/false));
    // Opcode 0x3: reserved, non-control, and otherwise IMPECCABLE — FIN set, masked, two
    // payload bytes, far inside both §5.5's bound and the receive cap.
    write_bytes(cfd, masked_client_frame(static_cast<ws::opcode_t>(0x3), junk, mask));
    write_bytes(cfd, masked_client_frame(ws::opcode_t::CONT, tail, mask, /*fin=*/true));

    bool closed = false;
    const auto back =
        read_until(cfd, [](const std::vector<std::byte>& b) { return !b.empty(); }, 2s, &closed);
    check(back.empty() && closed, "nothing came back, and the server FAILED the connection");
    check(fut.wait_for(500ms) != std::future_status::ready,
          "the fragments were NOT stitched around the reserved frame");
    check(server.malformed_rx() == 1, "the reserved frame was COUNTED as malformed, exactly once");
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
 * @brief Zero-copy scatter-gather egress: server.send(iov) with the payload split into N spans
 *        round-trips to a connected client identically to a flat server.send(span).
 *
 * The server→client path is UNMASKED (RFC 6455 §5.1), so the override rides the frame header ahead
 * of the payload spans with no flatten copy. The client decodes an ordinary unmasked BINARY frame;
 * the reassembled payload must be byte-identical whether the server gathered it from spans or wrote
 * it flat. A 200-byte payload forces the 126 + u16-BE extended-length header, so the shared
 * encode_frame_header helper is exercised on both paths.
 */
void test_scatter_gather_send() {
    std::printf("transport_ws server — scatter-gather send(iov) zero-copy egress:\n");

    tr::net::transport_ws_server server(0);
    check(server.ok(), "listen socket bound");
    const std::uint16_t port = server.local_port();

    const int cfd = tcp_connect(port);
    check(cfd >= 0 && raw_handshake(cfd), "raw client connected + 101 handshake");

    // A payload deliberately > 125 bytes so the frame header takes the 126 + u16-BE
    // extended-length form; split into four uneven spans — the "rope" a router gathers.
    std::vector<std::byte> payload(200);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::byte>((i * 7 + 1) & 0xFF);
    const std::array<std::size_t, 4> cut{40, 55, 5, 100};  // sums to 200
    std::vector<std::span<const std::byte>> spans;
    std::size_t off = 0;
    for (std::size_t c : cut) {
        spans.emplace_back(payload.data() + off, c);
        off += c;
    }

    // --- (a) scatter-gather send: the four spans emit as ONE server frame ---
    server.send(std::span<const std::span<const std::byte>>(spans));
    const auto sg_bytes = read_until(
        cfd, [](const std::vector<std::byte>& b) { return ws::decode_frame(b).has_value(); }, 2s);
    auto sg = ws::decode_frame(sg_bytes);
    check(sg.has_value(), "client decoded the scatter-gather server frame");
    if (sg) {
        check(sg->first.op == ws::opcode_t::BINARY, "scatter-gather frame is BINARY");
        check(sg->first.payload.size() == payload.size() &&
                  std::memcmp(sg->first.payload.data(), payload.data(), payload.size()) == 0,
              "scatter-gather payload == the gathered spans concatenated");
    }

    // --- (b) flat send of the SAME payload: byte-identical reassembly ---
    server.send(std::span<const std::byte>(payload));
    const auto flat_bytes = read_until(
        cfd, [](const std::vector<std::byte>& b) { return ws::decode_frame(b).has_value(); }, 2s);
    auto flat = ws::decode_frame(flat_bytes);
    check(flat.has_value(), "client decoded the flat server frame");
    if (sg && flat)
        check(flat->first.payload == sg->first.payload,
              "flat send and scatter-gather send reassemble to identical bytes");

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

    tr::net::transport_ws_server server(0, &tr::mem::heap_backend(), /*max_frame=*/0,
                                        /*max_peers=*/0, /*peer_named=*/true);
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

    // --- #426 / ADR-0073 §2: peer names are the routable `p<slot>` fallback ---
    // The old `<ip>:<port>` name contained two reserved characters, so a peer was
    // enumerable but unaddressable, and the delivery tag TAINTED the accumulated
    // return route. Every name must now pass THE segment predicate (fails before
    // the rename), and be exactly the slot spelling.
    {
        bool all_legal = true;
        std::vector<std::string> names;
        server.bus()->enumerate_peers([&](std::string_view p) {
            names.emplace_back(p);
            if (!tr::graph::valid_segment(p)) all_legal = false;
        });
        check(all_legal, "every enumerated peer name is a legal path segment (#426)");
        check(!names.empty() && names[0].size() >= 2 && names[0][0] == 'p',
              "the fallback name is the p<slot> spelling");
        check(name_a == "p0" || name_a == "p1",
              "the DELIVERY TAG is the p<slot> name (the return route is addressable)");
    }

    // --- slot recycling: a new session lands in the freed slot and REUSES its name ---
    std::optional<tr::net::transport_ws_client> c;
    c.emplace("127.0.0.1", port);
    frame_sink_t c_sink;
    c->set_receiver(c_sink);
    check(c->ok(), "third client connected after the departure");
    const std::array<std::byte, 2> pc{std::byte{0x01}, std::byte{0xC1}};
    c->send(pc);
    check(srv_sink.wait_count(3, 2s), "server got the third client's frame");
    {
        const std::lock_guard lock(srv_sink.m);
        const auto& [peer, bytes] = srv_sink.frames.back();
        const bool p_form = peer.size() >= 2 && peer[0] == 'p' &&
                            peer.find_first_not_of("0123456789", 1) == std::string::npos;
        check(bytes == std::vector<std::byte>(pc.begin(), pc.end()) && p_form,
              "the new session delivers under a p<slot> name");
        check(peer != name_a, "…and it is not the surviving peer's name (directedness held)");
    }
}

/** @brief #362 — the max_peers deployment cap: a peer beyond it is refused cleanly,
 *         and a departure frees the slot for the next connection. */
void test_max_peers_cap() {
    std::printf("transport_ws server — max_peers admission cap (#362):\n");

    tr::net::transport_ws_server server(0, &tr::mem::heap_backend(), /*max_frame=*/0,
                                        /*max_peers=*/1);
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

/** @brief #374 — close_peer tears down exactly one named peer, recycling its slot
 *         precisely as a remote FIN would: the survivor stays resolvable, an unknown
 *         name is refused, and a fresh client is admitted into the freed slot. */
void test_close_peer() {
    std::printf("transport_ws server — single-peer close_peer (#374):\n");

    peer_sink_t srv_sink;
    frame_sink_t a_sink;
    frame_sink_t b_sink;

    tr::net::transport_ws_server server(0, &tr::mem::heap_backend(), /*max_frame=*/0,
                                        /*max_peers=*/2, /*peer_named=*/true);
    check(server.ok(), "listen socket bound");
    const std::uint16_t port = server.local_port();
    server.bus()->set_peer_receiver(srv_sink);

    tr::net::transport_ws_client a("127.0.0.1", port);
    a.set_receiver(a_sink);
    std::optional<tr::net::transport_ws_client> b;
    b.emplace("127.0.0.1", port);
    b->set_receiver(b_sink);
    check(a.ok() && b->ok(), "two clients connected");

    // Drive one frame each so the server learns both peer names.
    const std::array<std::byte, 2> pa{std::byte{0x01}, std::byte{0xA1}};
    const std::array<std::byte, 2> pb{std::byte{0x01}, std::byte{0xB1}};
    a.send(pa);
    b->send(pb);
    check(srv_sink.wait_count(2, 2s), "server got both clients' frames");
    std::string name_a;
    std::string name_b;
    {
        const std::lock_guard lock(srv_sink.m);
        for (const auto& [peer, bytes] : srv_sink.frames) {
            if (bytes == std::vector<std::byte>(pa.begin(), pa.end())) name_a = peer;
            if (bytes == std::vector<std::byte>(pb.begin(), pb.end())) name_b = peer;
        }
    }
    check(!name_a.empty() && !name_b.empty(), "both peers identifiable by tag");

    std::size_t n_peers = 0;
    server.bus()->enumerate_peers([&](std::string_view) { ++n_peers; });
    check(n_peers == 2, "enumerate_peers lists both open peers");

    // Close b via the bus facet; its slot recycles asynchronously (one poll bound).
    check(server.bus()->close_peer(name_b), "close_peer(name_b) reports the peer was closed");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::size_t live = 2;
    while (std::chrono::steady_clock::now() < deadline) {
        live = 0;
        server.bus()->enumerate_peers([&](std::string_view) { ++live; });
        if (live == 1) break;
        std::this_thread::sleep_for(20ms);
    }
    check(live == 1, "close_peer recycled the slot (departed peer left enumeration)");
    check(server.bus()->peer_link(name_a) != nullptr, "surviving peer still resolves");
    check(server.bus()->peer_link(name_b) == nullptr, "closed peer no longer resolves");
    check(!server.bus()->close_peer("0.0.0.0:1"), "close_peer of an unknown name returns false");

    // A fresh client is admitted into the recycled slot (proves it freed like a FIN,
    // under max_peers=2 with one survivor still occupying a slot).
    const auto rd = std::chrono::steady_clock::now() + 2s;
    bool readmitted = false;
    while (!readmitted && std::chrono::steady_clock::now() < rd) {
        const tr::net::transport_ws_client c("127.0.0.1", port);
        readmitted = c.ok();
        if (!readmitted) std::this_thread::sleep_for(50ms);
    }
    check(readmitted, "a new client is admitted into the recycled slot");

    b.reset();
}

/** @brief Append one UNMASKED server→client frame (payload < 126) to @p out. */
void append_server_frame(std::vector<std::byte>& out, ws::opcode_t op,
                         std::span<const std::byte> payload, bool fin = true) {
    out.push_back(static_cast<std::byte>((fin ? 0x80u : 0x00u) | static_cast<std::uint8_t>(op)));
    out.push_back(static_cast<std::byte>(payload.size()));  // MASK=0: a server frame
    out.insert(out.end(), payload.begin(), payload.end());
}

/**
 * @brief #1020 — bytes the server pipelines behind its `101` reach the frame stream.
 *
 * `transport_ws_client::handshake` reads the response into a buffer of its own until
 * CRLFCRLF, and a single `recv` routinely returns the `101` AND whatever the server sent
 * straight after it — which is what a server that pushes state on connect does. Those
 * bytes are off the socket; if the handshake drops them nothing can ever read them back,
 * and the frame vanishes with no counter moving. The accept side has carried them over
 * since it grew a second peer (`transport_ws_server::on_readable`); this is the DIAL side
 * of that rule.
 *
 * The peer here writes the `101`, a complete PING, and the FIRST fragment of a BINARY
 * message in ONE `::send`, so all three land in one `recv` on the client — the shape the
 * defect needs. Then it goes quiet. Two independent observables follow:
 *
 *  - the PONG. A complete pipelined control frame must be answered even though NOTHING
 *    more arrives on the socket — that fails both if the handshake discards the bytes and
 *    if the recv loop polls before draining what it was handed.
 *  - the message BYTES. The closing CONT is only written after the test has installed its
 *    receiver, so the delivery is not racing the recv thread's start; a dropped first
 *    fragment leaves that CONT stray (the assembler drops it) and NO message arrives.
 */
void test_frame_pipelined_behind_the_101() {
    std::printf("transport_ws client — a frame pipelined behind the 101 is not dropped (#1020):\n");

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

    const std::array<std::byte, 4> ping_payload{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                                std::byte{0xEF}};
    const std::array<std::byte, 4> head_bytes{std::byte{0x01}, std::byte{0x02}, std::byte{0xF0},
                                              std::byte{0x0D}};
    const std::array<std::byte, 2> tail_bytes{std::byte{0xC0}, std::byte{0xDE}};

    std::promise<bool> one_write_done;  // the combined blob went out as ONE send
    std::promise<bool> pong_seen;       // the pipelined PING was answered
    std::promise<void> receiver_ready;  // the test has installed its sink
    std::promise<void> test_done;       // safe to close the peer socket
    auto one_write_fut = one_write_done.get_future();
    auto pong_fut = pong_seen.get_future();
    auto ready_fut = receiver_ready.get_future();
    auto done_fut = test_done.get_future();

    std::thread pusher([&] {
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            one_write_done.set_value(false);
            pong_seen.set_value(false);
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
            pong_seen.set_value(false);
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
        append_server_frame(blob, ws::opcode_t::PING, ping_payload);
        append_server_frame(blob, ws::opcode_t::BINARY, head_bytes, /*fin=*/false);
        // ONE syscall: the handshake reply and the frames behind it coalesce into a single
        // segment, so the client's first `recv` returns all of them together. A test that
        // paused here would prove nothing — that pause is the mask this case removes.
        const ssize_t sent = ::send(cfd, blob.data(), blob.size(), 0);
        one_write_done.set_value(sent == static_cast<ssize_t>(blob.size()));

        // Nothing else is written until the PONG comes back: the client must decode the
        // pipelined PING with no further bytes arriving on the socket.
        const auto pong = read_until(
            cfd, [](const std::vector<std::byte>& b) { return ws::decode_frame(b).has_value(); },
            3s);
        const auto dec = ws::decode_frame(pong);
        pong_seen.set_value(
            dec.has_value() && dec->first.op == ws::opcode_t::PONG &&
            dec->first.payload.size() == ping_payload.size() &&
            std::memcmp(dec->first.payload.data(), ping_payload.data(), ping_payload.size()) == 0);

        ready_fut.wait();
        std::vector<std::byte> cont;
        append_server_frame(cont, ws::opcode_t::CONT, tail_bytes, /*fin=*/true);
        write_bytes(cfd, cont);
        done_fut.wait();
        ::close(cfd);
    });

    // The sink outlives the transport that delivers to it (this file's destruction idiom).
    frame_sink_t sink;
    {
        tr::net::transport_ws_client client("127.0.0.1", ntohs(bound.sin_port));
        check(client.ok(), "the client completed its opening handshake");
        client.set_receiver(sink);
        receiver_ready.set_value();

        check(one_write_fut.wait_for(3s) == std::future_status::ready && one_write_fut.get(),
              "the peer put the 101 and the frames behind it in ONE write");
        check(pong_fut.wait_for(4s) == std::future_status::ready && pong_fut.get(),
              "the PING pipelined behind the 101 was answered with a matching PONG");
        check(sink.wait_count(1, 4s), "the pipelined BINARY fragment completed into a message");
        std::vector<std::byte> got;
        {
            const std::lock_guard lock(sink.m);
            if (!sink.frames.empty()) got = sink.frames.front();
        }
        std::vector<std::byte> want(head_bytes.begin(), head_bytes.end());
        want.insert(want.end(), tail_bytes.begin(), tail_bytes.end());
        check(got == want, "and it carried the PIPELINED fragment's bytes, not just the CONT's");
        test_done.set_value();
    }
    pusher.join();
    ::close(lfd);
}

/**
 * @brief #1025 — a `defer_recv` client decodes NOTHING until start_receiving(), so a message
 *        the server pushes ON CONNECT cannot be dropped into an empty sink.
 *
 * The one-phase constructor dials, handshakes AND spawns the recv thread before it returns,
 * so `set_receiver` can only ever run afterwards. A server that pushes its state the instant
 * the handshake completes has that message in flight before the constructor returns, and
 * whether it beats the caller's next statement is decided by nothing but scheduling: the
 * recv thread reaching its first drain versus the calling thread performing one store. Lose
 * that race and `receiver_slot_t`'s empty slot drops the message — no counter, no queue, a
 * healthy-looking connection.
 *
 * The guard does not race it. The peer writes the `101`, a PING and a COMPLETE BINARY
 * message in ONE `::send` (so all of it is in the client's first `recv`) and then goes
 * quiet, and the client is constructed with `defer_recv`. Three observables, in order:
 *
 *  - SILENCE. For a generous window after the handshake the client sends back NOTHING — the
 *    pipelined PING is not answered, because nothing has been decoded at all. A client that
 *    started its recv thread in the constructor answers it well inside that window.
 *  - the PONG, once `start_receiving()` runs — the positive control that keeps the silence
 *    above from being vacuous: the bytes were really there, and really decodable.
 *  - the MESSAGE, delivered to a sink installed while the thread was still held. In the
 *    one-phase shape this is exactly the frame that vanishes.
 */
void test_push_on_connect_waits_for_start_receiving() {
    std::printf("transport_ws client — a push-on-connect message waits for the sink (#1025):\n");

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

    const std::array<std::byte, 3> ping_payload{std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}};
    const std::array<std::byte, 5> pushed{std::byte{0x50}, std::byte{0x55}, std::byte{0x53},
                                          std::byte{0x48}, std::byte{0x21}};

    std::promise<bool> one_write_done;  // the combined blob went out as ONE send
    std::promise<bool> stayed_quiet;    // nothing came back while the recv thread was held
    std::promise<bool> pong_seen;       // ...and the PING was answered once it was released
    std::promise<void> armed;           // sink installed + start_receiving() called
    std::promise<void> test_done;       // safe to close the peer socket
    auto one_write_fut = one_write_done.get_future();
    auto quiet_fut = stayed_quiet.get_future();
    auto pong_fut = pong_seen.get_future();
    auto armed_fut = armed.get_future();
    auto done_fut = test_done.get_future();

    std::thread pusher([&] {
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            one_write_done.set_value(false);
            stayed_quiet.set_value(false);
            pong_seen.set_value(false);
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
            stayed_quiet.set_value(false);
            pong_seen.set_value(false);
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
        append_server_frame(blob, ws::opcode_t::PING, ping_payload);
        append_server_frame(blob, ws::opcode_t::BINARY, pushed);  // COMPLETE, FIN=1
        // ONE syscall: the 101 and the push behind it coalesce into a single segment, so the
        // client's first `recv` holds the whole message. This is the push-on-connect shape.
        const ssize_t sent = ::send(cfd, blob.data(), blob.size(), 0);
        one_write_done.set_value(sent == static_cast<ssize_t>(blob.size()));

        // The held window. `read_until` with a predicate that never fires burns the whole
        // budget, so this is a full 500 ms of listening — orders of magnitude more than the
        // recv thread needs to drain what it was handed and reply, which is why a client
        // that spawned that thread in its constructor fails here deterministically rather
        // than flakily.
        auto early = read_until(cfd, [](const std::vector<std::byte>&) { return false; }, 500ms);
        stayed_quiet.set_value(early.empty());

        armed_fut.wait();
        auto back = read_until(
            cfd, [](const std::vector<std::byte>& b) { return ws::decode_frame(b).has_value(); },
            3s);
        // Anything that leaked into the held window still counts as sent: the PONG check must
        // stay a POSITIVE control (the bytes survived and decode) in both states, so that the
        // silence assertion above is the only thing the deferral is on the hook for.
        back.insert(back.begin(), early.begin(), early.end());
        const auto dec = ws::decode_frame(back);
        pong_seen.set_value(
            dec.has_value() && dec->first.op == ws::opcode_t::PONG &&
            dec->first.payload.size() == ping_payload.size() &&
            std::memcmp(dec->first.payload.data(), ping_payload.data(), ping_payload.size()) == 0);

        done_fut.wait();
        ::close(cfd);
    });

    // The sink outlives the transport that delivers to it (this file's destruction idiom).
    frame_sink_t sink;
    {
        tr::net::transport_ws_client client("127.0.0.1", ntohs(bound.sin_port),
                                            &tr::mem::heap_backend(), /*max_frame=*/0,
                                            /*recv_stack=*/0, /*defer_recv=*/true);
        check(client.ok(), "the deferred client completed its opening handshake");
        check(one_write_fut.wait_for(3s) == std::future_status::ready && one_write_fut.get(),
              "the peer put the 101 and a COMPLETE pushed message in ONE write");
        check(quiet_fut.wait_for(3s) == std::future_status::ready && quiet_fut.get(),
              "the client decoded NOTHING before start_receiving(): the pipelined PING went "
              "unanswered for the whole held window");

        // The install the one-phase shape cannot make in time.
        client.set_receiver(sink);
        client.start_receiving();
        armed.set_value();

        check(pong_fut.wait_for(4s) == std::future_status::ready && pong_fut.get(),
              "after start_receiving() the pipelined PING was answered with a matching PONG");
        check(sink.wait_count(1, 4s),
              "and the message the server pushed on connect was DELIVERED, not dropped");
        std::vector<std::byte> got;
        {
            const std::lock_guard lock(sink.m);
            if (!sink.frames.empty()) got = sink.frames.front();
        }
        check(got == std::vector<std::byte>(pushed.begin(), pushed.end()),
              "carrying the pushed message's own bytes");
        test_done.set_value();
    }
    pusher.join();
    ::close(lfd);
}

// ---------------------------------------------------------------------------
// #1010 — RFC 6455 §5.5 at the CLIENT: a hostile SERVER driven at our dial half.
//
// `transport_alloc_softfail_test.cpp` drives the same three vectors at the SERVER from a raw
// socket. Both halves route through the one `decode_frame_checked`, but they do NOT share
// what comes after it: the server answers out of `kMaxServerControlFrame` and fails a
// connection through `teardown_slot`, the client answers out of `kMaxClientControlFrame`
// (four bytes wider, because a client control frame is MASKED) and fails through
// `teardown_peer` + the departure seam. A regression in either of the client's two would
// leave the three server-side vectors green, which is the gap these three close. The
// deployment shape is ordinary: a libtracer node that dials out to a peer is the client, and
// that peer is exactly as untrusted as an inbound one.
// ---------------------------------------------------------------------------

/**
 * @brief Bind and listen on an ephemeral loopback port.
 *
 * @param port Out: the port the kernel chose, host byte order.
 * @retval -1 The listener could not be brought up.
 */
int bind_ephemeral_listener(std::uint16_t& port) {
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    const int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
        ::listen(lfd, 1) != 0) {
        ::close(lfd);
        return -1;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(lfd, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
        ::close(lfd);
        return -1;
    }
    port = ntohs(bound.sin_port);
    return lfd;
}

/**
 * @brief Read our client's opening handshake off @p cfd and answer a valid `101`.
 *
 * The reply is legitimate — these cases are about what the peer does AFTER the upgrade, so
 * the handshake must not be the thing that fails.
 */
bool answer_opening_handshake(int cfd) {
    const auto req = read_until(
        cfd,
        [](const std::vector<std::byte>& b) {
            return std::string_view(reinterpret_cast<const char*>(b.data()), b.size())
                       .find("\r\n\r\n") != std::string_view::npos;
        },
        2s);
    const std::string_view text(reinterpret_cast<const char*>(req.data()), req.size());
    const std::size_t kpos = text.find("Sec-WebSocket-Key: ");
    if (kpos == std::string_view::npos) return false;
    const std::size_t vstart = kpos + std::string_view("Sec-WebSocket-Key: ").size();
    const std::string key(text.substr(vstart, text.find("\r\n", vstart) - vstart));

    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    resp += ws::accept_key(key);
    resp += "\r\n\r\n";
    write_str(cfd, resp);
    return true;
}

/**
 * @brief An UNMASKED server→client CONTROL frame DECLARING @p declared_len payload bytes —
 *        past the §5.5 bound — in the 16-bit length form, with that many bytes behind it.
 *
 * Hand-rolled on purpose: `ws::encode_server_control` refuses a payload past the §5.5 bound
 * (what `test_control_encoders_are_byte_identical` pins), so this shape has to be written
 * out byte by byte — which is the point, only a hostile peer emits it.
 */
std::vector<std::byte> oversize_server_control(ws::opcode_t op, std::size_t declared_len) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(op)));
    out.push_back(static_cast<std::byte>(126u));  // MASK=0, 16-bit extended length
    out.push_back(static_cast<std::byte>((declared_len >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(declared_len & 0xFFu));
    out.insert(out.end(), declared_len, std::byte{0x77});
    return out;
}

/**
 * @brief The link-down seam, latched.
 *
 * `%transport_t::set_down_notifier` is how the routing plane learns a point-to-point link
 * died (RFC-0009 §D extended to peer departure). It is the client's OWN report that it
 * failed the connection, which is a strictly different observable from the peer's socket
 * going away: a client that dropped the fd without taking the teardown path would satisfy
 * the second and not the first.
 */
struct down_latch_t {
    std::mutex m;
    std::condition_variable cv;
    bool fired = false;

    /** @brief The `%transport_t::down_fn_t` entry point; @p ctx is the latch. */
    static void notify(void* ctx) {
        auto* self = static_cast<down_latch_t*>(ctx);
        {
            const std::lock_guard lock(self->m);
            self->fired = true;
        }
        self->cv.notify_all();
    }
    /** @brief True once the notifier fired, waiting up to @p timeout for it. */
    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return fired; });
    }
    /** @brief Whether it has fired already, without waiting. */
    bool fired_now() {
        const std::lock_guard lock(m);
        return fired;
    }
};

/** @brief What a hostile server's illegal control frame drew out of our client. */
struct control_breach_outcome_t {
    bool handshaken = false;            /**< @brief The client completed its opening handshake. */
    std::vector<std::byte> reply;       /**< @brief Every byte the client wrote back after it. */
    bool peer_closed = false;           /**< @brief The client's end of the socket went away. */
    bool link_down = false;             /**< @brief The departure seam fired. */
    std::uint64_t malformed_before = 0; /**< @brief `malformed_rx()` before the breach. */
    std::uint64_t malformed_after = 0;  /**< @brief ...and after it. */
};

/**
 * @brief Dial a `%transport_ws_client` at a raw socket acting as a HOSTILE SERVER, feed it
 *        @p breach, and collect what the client did about it.
 *
 * `defer_recv` is what makes this deterministic rather than a race: the breach is written
 * while the recv thread is still parked, so every byte of it is queued before the client is
 * allowed to look, and the observation window opens only once `start_receiving()` has run.
 * The window uses a predicate that never fires, so it burns its whole budget unless the
 * peer disappears — which is one of the two things being observed.
 *
 * The hostile end holds its socket OPEN until the caller says it is done. That is not
 * tidiness: closing it would make the client's recv return 0 and take the ordinary
 * remote-hangup teardown, which fires the departure seam too — so `link_down` would be
 * satisfied by the harness itself and assert nothing about §5.5. Ablating the gate is what
 * caught that: with the peer closing at the end of the window, `link_down` stayed green in
 * BOTH states.
 */
control_breach_outcome_t drive_control_breach(std::span<const std::byte> breach) {
    control_breach_outcome_t out;
    std::uint16_t port = 0;
    const int lfd = bind_ephemeral_listener(port);
    if (lfd < 0) return out;

    const std::vector<std::byte> frame(breach.begin(), breach.end());
    std::promise<void> armed;                                   // start_receiving() has run
    std::promise<std::pair<std::vector<std::byte>, bool>> obs;  // (bytes back, peer closed)
    std::promise<void> test_done;                               // safe to close the peer socket
    auto armed_fut = armed.get_future();
    auto obs_fut = obs.get_future();
    auto done_fut = test_done.get_future();

    std::thread hostile([&] {
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            obs.set_value({{}, false});
            return;
        }
        if (!answer_opening_handshake(cfd)) {
            obs.set_value({{}, false});
            ::close(cfd);
            return;
        }
        write_bytes(cfd, frame);
        armed_fut.wait();
        bool closed = false;
        auto got =
            read_until(cfd, [](const std::vector<std::byte>&) { return false; }, 3s, &closed);
        obs.set_value({std::move(got), closed});
        done_fut.wait();
        ::close(cfd);
    });

    // The latch is the notifier's ctx and the sink is the receiver: both must outlive the
    // transport that calls into them (this file's destruction idiom).
    down_latch_t latch;
    frame_sink_t sink;
    {
        tr::net::transport_ws_client client("127.0.0.1", port, &tr::mem::heap_backend(),
                                            /*max_frame=*/0, /*recv_stack=*/0,
                                            /*defer_recv=*/true);
        out.handshaken = client.ok();
        out.malformed_before = client.malformed_rx();
        client.set_receiver(sink);
        client.set_down_notifier(&down_latch_t::notify, &latch);
        client.start_receiving();
        armed.set_value();

        if (obs_fut.wait_for(6s) == std::future_status::ready) {
            auto observed = obs_fut.get();
            out.reply = std::move(observed.first);
            out.peer_closed = observed.second;
        }
        // Read BEFORE releasing the hostile end: past `test_done` its `close` would fire the
        // departure seam by itself and both of these would stop measuring the §5.5 gate.
        out.link_down = latch.wait(2s);
        out.malformed_after = client.malformed_rx();
        test_done.set_value();
    }
    hostile.join();
    ::close(lfd);
    return out;
}

/**
 * @brief #1010 — a 125-byte PING from the server we dialled is answered with a PONG that is
 *        byte-exact AND masked, and the link survives it.
 *
 * The mirror of the server's `test_ws_ping_at_the_bound_is_answered`, plus the thing only
 * the client half can get wrong: RFC 6455 §5.3 says every client→server frame is masked, so
 * the reply is four bytes wider than the server's and its payload is XOR-transformed. The two
 * existing client PING cases in this file (#1020, #1025) assert only that `ws::decode_frame`
 * recovers the payload — and decode UNMASKS, so an unmasked PONG passes both of them. This
 * one reads the wire bytes: the MASK bit, the frame's own key, and the XOR of every payload
 * byte under it.
 *
 * The liveness probe at the end is a POSITIVE control for "the legal frame did not fail the
 * connection" — a message sent after the PONG still arrives — rather than the vacuous
 * negative of merely observing no teardown.
 */
void test_client_answers_a_control_frame_at_the_bound_masked() {
    std::printf("transport_ws client — a 125-byte PING draws a MASKED byte-exact PONG (#1010):\n");

    std::uint16_t port = 0;
    const int lfd = bind_ephemeral_listener(port);
    check(lfd >= 0, "raw listener bound on an ephemeral loopback port");
    if (lfd < 0) return;

    std::vector<std::byte> ping_payload(ws::kMaxControlPayload);
    for (std::size_t i = 0; i < ping_payload.size(); ++i)
        ping_payload[i] = static_cast<std::byte>(i * 7u + 1u);
    const std::array<std::byte, 4> liveness{std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
                                            std::byte{0x40}};

    std::promise<void> armed;                   // start_receiving() has run
    std::promise<std::vector<std::byte>> pong;  // what came back
    std::promise<void> test_done;               // safe to close the peer socket
    auto armed_fut = armed.get_future();
    auto pong_fut = pong.get_future();
    auto done_fut = test_done.get_future();

    std::thread hostile([&] {
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            pong.set_value({});
            return;
        }
        if (!answer_opening_handshake(cfd)) {
            pong.set_value({});
            ::close(cfd);
            return;
        }
        std::vector<std::byte> ping;
        append_server_frame(ping, ws::opcode_t::PING, ping_payload);
        write_bytes(cfd, ping);
        armed_fut.wait();
        pong.set_value(read_until(
            cfd,
            [](const std::vector<std::byte>& b) { return b.size() >= ws::kMaxClientControlFrame; },
            4s));
        // The liveness probe, written only once the PONG has been read: a LEGAL control
        // frame must leave the connection carrying traffic.
        std::vector<std::byte> msg;
        append_server_frame(msg, ws::opcode_t::BINARY, liveness);
        write_bytes(cfd, msg);
        done_fut.wait();
        ::close(cfd);
    });

    down_latch_t latch;
    frame_sink_t sink;
    {
        tr::net::transport_ws_client client("127.0.0.1", port, &tr::mem::heap_backend(),
                                            /*max_frame=*/0, /*recv_stack=*/0,
                                            /*defer_recv=*/true);
        check(client.ok(), "the client completed its opening handshake");
        client.set_receiver(sink);
        client.set_down_notifier(&down_latch_t::notify, &latch);
        client.start_receiving();
        armed.set_value();

        std::vector<std::byte> got;
        if (pong_fut.wait_for(6s) == std::future_status::ready) got = pong_fut.get();

        // Every verdict below is computed with its own size guard rather than nested under
        // one: a shape change must REPORT on each line it breaks, not make the lines
        // disappear. (An `if (size == …) { … }` wrapper hid the mask-bit check under the
        // ablation that flattened the reply to the server shape.)
        const bool whole = got.size() == ws::kMaxClientControlFrame;
        check(whole,
              "the client wrote back exactly one client control frame: 2 header + 4 mask key + "
              "125 payload bytes");
        check(got.size() >= 2 && got[0] == static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(
                                                                              ws::opcode_t::PONG)),
              "FIN=1 and the opcode is PONG");
        check(got.size() >= 2 && got[1] == static_cast<std::byte>(0x80u | ws::kMaxControlPayload),
              "the MASK bit is SET with the 125-byte length inline — a client control frame "
              "is always masked (RFC 6455 §5.3)");
        bool xor_exact = whole;
        for (std::size_t i = 0; xor_exact && i < ws::kMaxControlPayload; ++i) {
            const std::uint8_t key_byte = std::to_integer<std::uint8_t>(got[2 + (i % 4)]);
            xor_exact =
                got[6 + i] ==
                static_cast<std::byte>(std::to_integer<std::uint8_t>(ping_payload[i]) ^ key_byte);
        }
        check(xor_exact,
              "every payload byte on the wire is the PING's byte XOR the frame's OWN key — "
              "byte-exact and actually masked");
        const auto dec = ws::decode_frame(got);
        check(dec.has_value() && dec->first.op == ws::opcode_t::PONG &&
                  dec->first.payload == ping_payload,
              "and unmasking the frame recovers the 125-byte payload exactly");

        check(sink.wait_count(1, 4s),
              "the connection SURVIVED the legal control frame: a message sent after the PONG "
              "was delivered");
        std::vector<std::byte> after;
        {
            const std::lock_guard lock(sink.m);
            if (!sink.frames.empty()) after = sink.frames.front();
        }
        check(after == std::vector<std::byte>(liveness.begin(), liveness.end()),
              "carrying that message's own bytes");
        check(!latch.fired_now(), "and the link was never reported down");
        check(client.malformed_rx() == 0,
              "a control frame exactly AT the §5.5 bound is not a breach");
        test_done.set_value();
    }
    hostile.join();
    ::close(lfd);
}

/**
 * @brief #1010 — an OVERSIZED control frame from the server we dialled FAILS the connection
 *        (RFC 6455 §5.5 / §7.1.7) instead of being echoed.
 *
 * The mirror of the server's `test_ws_oversized_ping_fails_the_connection`. The declared
 * length is checked off the HEADER, so the 4 KiB never has to be buffered — and it is never
 * reflected, which is what stops a dialled-out link from being an amplifier.
 *
 * Which assertion carries the weight, stated plainly: the "no PONG" line is PASSIVE here.
 * With the §5.5 gate ablated the client decodes the 4 KiB PING as legal and calls
 * `encode_client_control`, which returns 0 for a payload past the bound — so nothing goes out
 * either way and that line stays green in both states. The three that redden are the ones
 * about what the client did INSTEAD of answering: the teardown, the departure seam, and the
 * counter. (In the fragmented case below the same line is load-bearing — an 8-byte payload
 * encodes fine, so a client with no gate really does reply.)
 */
void test_client_fails_the_connection_on_an_oversize_control_frame() {
    std::printf(
        "transport_ws client — a hostile server's 4 KiB PING fails the connection (#1010):\n");
    const auto got = drive_control_breach(oversize_server_control(ws::opcode_t::PING, 4096));
    check(got.handshaken, "the client completed its opening handshake");
    check(got.reply.empty(), "no PONG came back — and so no 4 KiB reflection");
    check(got.peer_closed, "the client FAILED the connection: its end of the socket went away");
    check(got.link_down, "and it reported the link down through the departure seam");
    check(got.malformed_after > got.malformed_before,
          "the §5.5 breach was COUNTED as malformed, not silently tolerated");
}

/**
 * @brief #1010 — a FRAGMENTED control frame from the server we dialled is equally illegal.
 *
 * The mirror of the server's `test_ws_fragmented_control_fails_the_connection`. This one is
 * the sharper of the two on the client: the `PING` arm of the recv loop's switch does not
 * itself look at `fin`, so with the §5.5 gate gone the client happily builds and sends a
 * PONG for a frame it should have torn the connection down over.
 */
void test_client_fails_the_connection_on_a_fragmented_control_frame() {
    std::printf(
        "transport_ws client — a hostile server's non-final PING fails the connection (#1010):\n");
    const std::array<std::byte, 8> payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                           std::byte{0x04}, std::byte{0x05}, std::byte{0x06},
                                           std::byte{0x07}, std::byte{0x08}};
    std::vector<std::byte> frame;
    append_server_frame(frame, ws::opcode_t::PING, payload, /*fin=*/false);

    const auto got = drive_control_breach(frame);
    check(got.handshaken, "the client completed its opening handshake");
    check(got.reply.empty(), "no PONG came back for a non-final control frame");
    check(got.peer_closed, "the client FAILED the connection: its end of the socket went away");
    check(got.link_down, "and it reported the link down through the departure seam");
    check(got.malformed_after > got.malformed_before,
          "the §5.5 breach was COUNTED as malformed, not silently tolerated");
}

/**
 * @brief #1060 — a RESERVED opcode from the server we dialled fails the connection (§5.2).
 *
 * @param op    The raw opcode nibble to send. A `std::uint8_t` on purpose: these are exactly
 *              the values `ws::opcode_t` does not name.
 * @param label How the case prints.
 *
 * The frame is otherwise IMPECCABLE — FIN set, an 8-byte payload well inside both §5.5's
 * bound and the receive cap — so neither existing rule can be what rejects it. Driven for
 * both halves of the reserved space, which reach the rule from opposite sides: `0x3`-`0x7`
 * are sorted as DATA by `is_control_opcode` and so met no opcode-shape rule, while
 * `0xB`-`0xF` are control frames a peer can shape perfectly legally and slide past §5.5.
 */
void drive_client_reserved_opcode(std::uint8_t op, const char* label) {
    std::printf("transport_ws client — a reserved opcode %s fails the connection (#1060):\n",
                label);
    const std::array<std::byte, 8> payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                           std::byte{0x04}, std::byte{0x05}, std::byte{0x06},
                                           std::byte{0x07}, std::byte{0x08}};
    std::vector<std::byte> frame;
    append_server_frame(frame, static_cast<ws::opcode_t>(op), payload);

    const auto got = drive_control_breach(frame);
    check(got.handshaken, "the client completed its opening handshake");
    check(got.reply.empty(), "the client wrote nothing back");
    check(got.peer_closed, "the client FAILED the connection: its end of the socket went away");
    check(got.link_down, "and it reported the link down through the departure seam");
    check(got.malformed_after > got.malformed_before,
          "the §5.2 breach was COUNTED as malformed, not silently ignored");
}

/** @brief #1060 — the reserved NON-CONTROL half (`0x3`), at the client. */
void test_client_fails_the_connection_on_a_reserved_data_opcode() {
    drive_client_reserved_opcode(0x3, "0x3 (reserved non-control)");
}

/** @brief #1060 — the reserved CONTROL half (`0xB`), legal-shaped, at the client. */
void test_client_fails_the_connection_on_a_reserved_control_opcode() {
    drive_client_reserved_opcode(0xB, "0xB, legal-shaped (reserved control)");
}

}  // namespace

int main() {
    test_handshake_and_frames();
    test_peer_open_before_response_is_readable();
    test_fragmented_message_rope();
    test_fragmented_message_span();
    test_reserved_frame_between_fragments_fails_the_connection();
    test_scatter_gather_send();
    test_client_server_roundtrip();
    test_multi_peer_bus();
    test_max_peers_cap();
    test_close_peer();
    test_frame_pipelined_behind_the_101();
    test_push_on_connect_waits_for_start_receiving();
    test_client_answers_a_control_frame_at_the_bound_masked();
    test_client_fails_the_connection_on_an_oversize_control_frame();
    test_client_fails_the_connection_on_a_fragmented_control_frame();
    test_client_fails_the_connection_on_a_reserved_data_opcode();
    test_client_fails_the_connection_on_a_reserved_control_opcode();
    return tr::testing::summary("ws_transport");
}
