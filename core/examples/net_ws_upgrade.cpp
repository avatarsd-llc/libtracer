/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a `ws` link carries no frame until an HTTP/1.1 Upgrade has
 *        completed, and the `101` is COMPUTED from the client's own nonce, so the
 *        handshake is a real exchange rather than a greeting — which is why `ok()` on a WS
 *        transport is the handshake's verdict and not the socket's.
 *
 * `ws` is the browser-reachable kind: a page cannot open a raw TCP socket, so libtracer
 * frames reach it inside RFC 6455 messages. The price is a phase no other stream kind has.
 * Before the first byte of a frame:
 *
 *  - the client sends `GET / HTTP/1.1` with `Upgrade: websocket` and a fresh 16-byte nonce
 *    base64'd into `Sec-WebSocket-Key`;
 *  - the server answers `101 Switching Protocols` with `Sec-WebSocket-Accept` set to
 *    `base64(sha1(key ++ RFC-6455-GUID))` — `ws::accept_key`, which this example calls
 *    itself to check the server's answer against;
 *  - only then does either side write a frame, each libtracer frame being exactly one
 *    BINARY message (client→server masked per §5.1, server→client unmasked).
 *
 * That phase is also an attack surface unique to this kind: the peer is unauthenticated and
 * is making this node accumulate a header block. Hence `max_handshake`, a PRE-AUTH budget
 * that is TIGHTEN-ONLY (#934) — a config-writable key may narrow what an anonymous peer can
 * cost the node and may never widen it.
 *
 * The handshake is driven from a raw POSIX socket so it is visible on the wire; the shipped
 * `transport_ws_client` then does the same thing behind `ok()`.
 *
 * Needs the WS transport (`LIBTRACER_TRANSPORT_WS`, on by default). Runs under ctest as
 * `example_net_ws_upgrade`; returns non-zero on any failed check.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"

namespace {

using namespace std::chrono_literals;
namespace ws = tr::net::ws;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief A thread-safe borrowed-span sink: the recv thread pushes, `main` waits. */
class sink_t {
   public:
    /** @brief The receiver callback — copies the span, which dies when it returns. */
    void operator()(std::span<const std::byte> frame) {
        {
            const std::lock_guard lock(m_);
            frames_.emplace_back(frame.begin(), frame.end());
        }
        cv_.notify_all();
    }

    /** @brief Wait until at least @p n frames have landed, or @p budget expires. */
    [[nodiscard]] bool wait_for(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, budget, [&] { return frames_.size() >= n; });
    }

    /** @brief How many frames have landed so far. */
    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(m_);
        return frames_.size();
    }

    /** @brief Frame @p i, by value. */
    [[nodiscard]] std::vector<std::byte> at(std::size_t i) const {
        const std::lock_guard lock(m_);
        return frames_.at(i);
    }

   private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::vector<std::byte>> frames_;
};

/** @brief A raw POSIX TCP client — the hand that types the HTTP request by hand. */
class raw_client_t {
   public:
    /** @brief Connect to `127.0.0.1:@p port`; @ref ok reports whether it succeeded. */
    explicit raw_client_t(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~raw_client_t() {
        if (fd_ >= 0) ::close(fd_);
    }

    raw_client_t(const raw_client_t&) = delete;
    raw_client_t& operator=(const raw_client_t&) = delete;

    /** @brief True iff the connect succeeded. */
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    /** @brief Push @p bytes, resuming partial writes. */
    void write(std::span<const std::byte> bytes) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::send(fd_, bytes.data() + off, bytes.size() - off, 0);
            if (n <= 0) return;
            off += static_cast<std::size_t>(n);
        }
    }

    /** @brief Push @p text as bytes. */
    void write_text(std::string_view text) {
        write(std::as_bytes(std::span(text.data(), text.size())));
    }

    /** @brief Read until `\r\n\r\n` is in hand, or @p budget expires — the header block. */
    [[nodiscard]] std::string read_headers(std::chrono::milliseconds budget) {
        std::string got;
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (got.find("\r\n\r\n") == std::string::npos) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) break;
            char buf[256];
            const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.append(buf, static_cast<std::size_t>(n));
        }
        return got;
    }

   private:
    int fd_ = -1;
};

/** @brief @p n bytes counting up from @p seed — a stand-in for an encoded frame. */
std::vector<std::byte> frame_of(std::size_t n, unsigned seed) {
    std::vector<std::byte> f(n);
    for (std::size_t i = 0; i < n; ++i) f[i] = static_cast<std::byte>(seed + i);
    return f;
}

}  // namespace

int main() {
    bool ok = true;

    sink_t at_server;
    tr::net::transport_ws_server server(std::uint16_t{0});
    server.set_receiver(at_server);
    check(ok, server.ok(), "the WS listener bound an ephemeral port");

    // --- The Upgrade, typed by hand -----------------------------------------------------
    std::printf("the opening handshake, on the wire:\n");
    raw_client_t raw(server.local_port());
    check(ok, raw.ok(), "a raw TCP client reached the listener's port");

    // The RFC 6455 §1.3 example nonce, so the expected accept value is reproducible; a real
    // client mints a fresh random one per connection, which is what makes the reply a proof
    // that the server actually ran the computation.
    const std::string client_key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string upgrade =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: ";
    upgrade += client_key;
    upgrade += "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    raw.write_text(upgrade);

    const std::string response = raw.read_headers(2s);
    check(ok, response.find("101 Switching Protocols") != std::string::npos,
          "the server answered 101 Switching Protocols");
    check(ok,
          response.find("Sec-WebSocket-Accept: " + ws::accept_key(client_key)) != std::string::npos,
          "…with Sec-WebSocket-Accept derived from OUR key, not a constant");

    // --- One libtracer frame is one BINARY message --------------------------------------
    std::printf("a frame, once the upgrade is done:\n");
    const auto payload = frame_of(9, 0x10);
    raw.write(ws::encode_client_frame(ws::opcode_t::BINARY, payload, 0x37FA213Du));
    check(ok, at_server.wait_for(1, 2s), "the masked BINARY message reached the receiver");
    check(ok, at_server.at(0) == payload,
          "…unmasked and stripped: the sink sees the frame, never the WS header");

    // --- The same handshake, behind ok() ------------------------------------------------
    std::printf("the shipped dialer does exactly that:\n");
    sink_t at_client;
    tr::net::transport_ws_client client("127.0.0.1", server.local_port());
    client.set_receiver(at_client);
    check(ok, client.ok(), "ok() on a WS client is the HANDSHAKE's verdict, not the socket's");

    const auto up = frame_of(6, 0x20);
    client.send(up);
    check(ok, at_server.wait_for(2, 2s), "the dialer's frame arrived too");
    check(ok, at_server.at(1) == up, "…byte-identical");

    // Server→client messages are unmasked (§5.1 masks only the client direction), and the
    // sink on either side is handed the same thing: the frame.
    const auto down = frame_of(4, 0x30);
    server.send(down);
    check(ok, at_client.wait_for(1, 2s), "and the server's BINARY message came back down");
    check(ok, at_client.at(0) == down, "…byte-identical, unmasked direction");

    // --- The budget the handshake makes necessary ---------------------------------------
    check(ok, server.effective_max_handshake() > 0,
          "the pre-auth handshake budget is a real, positive bound (#934)");
    tr::net::transport_ws_server tight(std::uint16_t{0}, &tr::mem::heap_backend(), 0, 0, false, 0,
                                       0, /*max_handshake=*/256);
    check(ok, tight.effective_max_handshake() == 256, "a smaller budget is honoured…");
    tr::net::transport_ws_server loose(std::uint16_t{0}, &tr::mem::heap_backend(), 0, 0, false, 0,
                                       0, /*max_handshake=*/1u << 30);
    check(ok, loose.effective_max_handshake() == server.effective_max_handshake(),
          "…and a LARGER one is clamped back — tighten-only, because the peer is anonymous");

    std::printf("ws: 1 upgrade, %zu frames up, %zu down, handshake budget %zu bytes\n",
                at_server.count(), at_client.count(), server.effective_max_handshake());
    return ok ? 0 : 1;
}
