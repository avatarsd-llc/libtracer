/**
 * @file
 * @brief #2139: the shared link case suite (`link_seam_suite.hpp`) run against the fake link
 *        adapter and against today's stream links where their shape allows.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Five link kinds run the same eight cases:
 *
 * - **fake**: the socket-free adapter of `fake_link.hpp`, deterministic, every case.
 * - **tcp (dial)**: a real `tcp_transport_t` dialled to a raw localhost socket that plays the
 *   peer. Its wire has no fragments, so that case is N/A.
 * - **ws (dial)**: a real `ws_client_transport_t` dialled to a raw socket that answers the
 *   opening handshake and then speaks RFC 6455 server frames, fragments included.
 * - **ws server**: a real `ws_server_transport_t` (flat, one peer) dialled by a raw socket
 *   that speaks masked client frames (#2140). The ingress cases only: the server's listener
 *   stays up when its peer goes, so "closed" is the peer's wire closing, and its egress cases
 *   are N/A here.
 * - **tcp server**: a real `tcp_server_transport_t` (flat, one peer) dialled by a raw socket
 *   that speaks length-prefixed records (#2141). The ingress cases only, for the same reason
 *   as the ws server; no fragments on its wire.
 *
 * Not here, and why: UDP has no partial frame, no several-per-read and no stall on a
 * datagram socket; the other server links are buses (`ctest -LE bus` deselects them) whose
 * peers reach the router through the peer facet; the device WebSocket link is not built on a
 * host. Each joins by adding a rig.
 *
 * The real-socket peer shrinks its receive buffer before accepting, so a peer that stops
 * reading fills the link's window in a few frames. Built under ASan+UBSan and TSan with the
 * rest of the tests.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fake_link.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"
#include "link_seam_suite.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using namespace tr::testing::link_seam;

/**
 * @brief The raw localhost peer a real link dials: a listening socket, then the one accepted
 *        connection, read and written byte for byte.
 */
class socket_peer_t {
   public:
    /** @brief Listen on an ephemeral port with a small receive buffer. */
    socket_peer_t() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        // Set before listen() so the accepted socket inherits it: a stalled peer then holds
        // only a few KiB, and the link's own window fills quickly.
        const int small = 4096;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        socklen_t len = sizeof(addr);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 1) != 0 ||
            ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        port_ = ntohs(addr.sin_port);
    }
    ~socket_peer_t() {
        if (fd_ >= 0) ::close(fd_);
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }
    socket_peer_t(const socket_peer_t&) = delete;
    socket_peer_t& operator=(const socket_peer_t&) = delete;

    /** @brief The port the link dials. */
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    /** @brief True once a connection was accepted. */
    [[nodiscard]] bool connected() const noexcept { return fd_ >= 0; }

    /** @brief Accept the link's connection, waiting up to @p budget. */
    bool accept(std::chrono::milliseconds budget) {
        if (listen_fd_ < 0) return false;
        pollfd p{listen_fd_, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(budget.count())) != 1) return false;
        fd_ = ::accept(listen_fd_, nullptr, nullptr);
        if (fd_ < 0) return false;
        const int one = 1;
        (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return true;
    }

    /** @brief Dial a link listening on localhost @p port, in place of accepting one. */
    bool dial(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            leave();
            return false;
        }
        const int one = 1;
        (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return true;
    }

    /** @brief Write every byte of @p b; false once the link has gone. */
    bool write(std::span<const std::byte> b) {
        std::size_t off = 0;
        while (off < b.size() && fd_ >= 0) {
            const ssize_t n = ::send(fd_, b.data() + off, b.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<std::size_t>(n);
        }
        return off == b.size();
    }

    /**
     * @brief Read for up to @p budget, or until @p done holds or the wire closes.
     * @param closed Set when the link closed its end.
     */
    template <typename Done>
    void read(bytes_t& into, std::chrono::milliseconds budget, Done done, bool& closed) {
        closed = false;
        const auto deadline = std::chrono::steady_clock::now() + budget;
        std::array<std::byte, 64u * 1024u> chunk;
        while (fd_ >= 0 && !done(into)) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return;
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) != 1) continue;
            const ssize_t n = ::recv(fd_, chunk.data(), chunk.size(), 0);
            if (n <= 0) {
                closed = true;
                return;
            }
            into.insert(into.end(), chunk.begin(), chunk.begin() + n);
        }
    }

    /** @brief Close our end, discarding anything unread (the link sees a reset). */
    void leave() {
        if (fd_ < 0) return;
        ::close(fd_);
        fd_ = -1;
    }

   private:
    int listen_fd_ = -1;
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

/** @brief The bytes of @p text. */
[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// --- tcp -----------------------------------------------------------------------------------

/** @brief One record on the tcp wire: a u32-LE length, then @p body. */
[[nodiscard]] bytes_t prefixed(std::span<const std::byte> body) {
    bytes_t out(4 + body.size());
    tr::wire::store_le<std::uint32_t>(out, static_cast<std::uint32_t>(body.size()));
    std::copy(body.begin(), body.end(), out.begin() + 4);
    return out;
}

/** @brief A tcp prefix announcing `kMaxFrame + 1` bytes, with no body after it. */
[[nodiscard]] bytes_t oversize_prefix() {
    bytes_t out(4);
    tr::wire::store_le<std::uint32_t>(out, static_cast<std::uint32_t>(kMaxFrame + 1));
    return out;
}

/** @brief A dialled `tcp_transport_t` and its raw peer. Wire: u32-LE length, then the body. */
class tcp_rig_t final : public link_rig_t {
   public:
    tcp_rig_t()
        : link_("127.0.0.1", peer_.port(),
                tr::net::tcp_config_t{.memory = {.rx = &rx_},
                                      .max_frame = kMaxFrame,
                                      .liveness_window_ms = kLivenessMs}) {
        (void)peer_.accept(kWait);
    }

    [[nodiscard]] std::string_view name() const override { return "tcp"; }
    [[nodiscard]] bool ready() const override { return link_.ok() && peer_.connected(); }
    [[nodiscard]] tr::net::transport_t& link() override { return link_; }
    [[nodiscard]] counting_backend_t& rx_backend() override { return rx_; }

    [[nodiscard]] bytes_t encode(std::span<const std::byte> body) const override {
        return prefixed(body);
    }
    [[nodiscard]] bool supports_fragments() const override { return false; }
    [[nodiscard]] bytes_t encode_fragments(std::span<const std::byte> body,
                                           std::size_t /*pieces*/) const override {
        return prefixed(body);
    }
    [[nodiscard]] bytes_t oversize_header() const override { return oversize_prefix(); }

    void peer_write(std::span<const std::byte> bytes) override { (void)peer_.write(bytes); }
    void peer_stall() override {}  // the raw peer reads only inside peer_drain
    [[nodiscard]] bool peer_drain(std::chrono::milliseconds budget,
                                  std::vector<bytes_t>& whole) override {
        bytes_t in;
        bool closed = false;
        peer_.read(in, budget, [](const bytes_t&) { return false; }, closed);
        std::size_t at = 0;
        while (in.size() - at >= 4) {
            const std::size_t len =
                tr::wire::load_le<std::uint32_t>(std::span<const std::byte>(in).subspan(at, 4));
            if (in.size() - at - 4 < len) break;
            const auto b = in.begin() + static_cast<std::ptrdiff_t>(at + 4);
            whole.emplace_back(b, b + static_cast<std::ptrdiff_t>(len));
            at += 4 + len;
        }
        return at == in.size() || closed;
    }
    void peer_leave() override { peer_.leave(); }

   private:
    counting_backend_t rx_;
    socket_peer_t peer_;
    tr::net::tcp_transport_t link_;
};

/**
 * @brief A flat, one-peer `tcp_server_transport_t` dialled by a raw socket that speaks
 *        length-prefixed records (#2141). The ingress cases only, as for the ws server.
 */
class tcp_server_rig_t final : public link_rig_t {
   public:
    tcp_server_rig_t()
        : link_(0, tr::net::tcp_server_config_t{.memory = {.rx = &rx_}, .max_frame = kMaxFrame}) {
        dialled_ = link_.ok() && peer_.dial(link_.local_port());
    }

    [[nodiscard]] std::string_view name() const override { return "tcp server"; }
    [[nodiscard]] bool ready() const override { return dialled_; }
    [[nodiscard]] tr::net::transport_t& link() override { return link_; }
    [[nodiscard]] counting_backend_t& rx_backend() override { return rx_; }
    [[nodiscard]] bool link_closed() override {
        bytes_t in;
        bool closed = false;
        peer_.read(in, 5ms, [](const bytes_t&) { return false; }, closed);
        return closed;
    }
    [[nodiscard]] bool egress_cases() const override { return false; }

    [[nodiscard]] bytes_t encode(std::span<const std::byte> body) const override {
        return prefixed(body);
    }
    [[nodiscard]] bool supports_fragments() const override { return false; }
    [[nodiscard]] bytes_t encode_fragments(std::span<const std::byte> body,
                                           std::size_t /*pieces*/) const override {
        return prefixed(body);
    }
    [[nodiscard]] bytes_t oversize_header() const override { return oversize_prefix(); }

    void peer_write(std::span<const std::byte> bytes) override { (void)peer_.write(bytes); }
    void peer_stall() override {}
    [[nodiscard]] bool peer_drain(std::chrono::milliseconds /*budget*/,
                                  std::vector<bytes_t>& /*whole*/) override {
        return true;
    }
    void peer_leave() override { peer_.leave(); }

   private:
    counting_backend_t rx_;
    socket_peer_t peer_;
    bool dialled_ = false;
    tr::net::tcp_server_transport_t link_;
};

// --- ws ------------------------------------------------------------------------------------

/**
 * @brief A dialled `ws_client_transport_t` and its raw peer, which answers the opening
 *        handshake and then speaks unmasked RFC 6455 server frames.
 */
class ws_rig_t final : public link_rig_t {
   public:
    ws_rig_t()
        : server_([this] { serve_handshake(); }),
          link_("127.0.0.1", peer_.port(),
                tr::net::ws_client_config_t{.memory = {.rx = &rx_},
                                            .max_frame = kMaxFrame,
                                            .liveness_window_ms = kLivenessMs}) {
        server_.join();
    }

    [[nodiscard]] std::string_view name() const override { return "ws"; }
    [[nodiscard]] bool ready() const override { return link_.ok() && handshaken_; }
    [[nodiscard]] tr::net::transport_t& link() override { return link_; }
    [[nodiscard]] counting_backend_t& rx_backend() override { return rx_; }

    [[nodiscard]] bytes_t encode(std::span<const std::byte> body) const override {
        return frame(tr::net::ws::opcode_t::BINARY, true, body.size(), body);
    }
    [[nodiscard]] bool supports_fragments() const override { return true; }
    [[nodiscard]] bytes_t encode_fragments(std::span<const std::byte> body,
                                           std::size_t pieces) const override {
        bytes_t out;
        const std::size_t step = (body.size() + pieces - 1) / pieces;
        for (std::size_t at = 0; at < body.size(); at += step) {
            const std::span<const std::byte> part =
                body.subspan(at, std::min(step, body.size() - at));
            const bool last = at + part.size() == body.size();
            const bytes_t one =
                frame(at == 0 ? tr::net::ws::opcode_t::BINARY : tr::net::ws::opcode_t::CONT, last,
                      part.size(), part);
            out.insert(out.end(), one.begin(), one.end());
        }
        return out;
    }
    [[nodiscard]] bytes_t oversize_header() const override {
        return frame(tr::net::ws::opcode_t::BINARY, true, kMaxFrame + 1, {});
    }

    void peer_write(std::span<const std::byte> bytes) override { (void)peer_.write(bytes); }
    void peer_stall() override {}  // the raw peer reads only inside peer_drain
    [[nodiscard]] bool peer_drain(std::chrono::milliseconds budget,
                                  std::vector<bytes_t>& whole) override {
        bytes_t in;
        bool closed = false;
        peer_.read(in, budget, [](const bytes_t&) { return false; }, closed);
        return decode_client_frames(in, whole) || closed;
    }
    void peer_leave() override { peer_.leave(); }

   private:
    /** @brief The peer thread: accept, read the upgrade request, answer 101. */
    void serve_handshake() {
        if (!peer_.accept(kWait)) return;
        bytes_t req;
        bool closed = false;
        const auto has_end = [](const bytes_t& b) {
            return std::string_view(reinterpret_cast<const char*>(b.data()), b.size())
                       .find("\r\n\r\n") != std::string_view::npos;
        };
        peer_.read(req, kWait, has_end, closed);
        const std::string_view text(reinterpret_cast<const char*>(req.data()), req.size());
        constexpr std::string_view kKey = "Sec-WebSocket-Key: ";
        const std::size_t k = text.find(kKey);
        if (k == std::string_view::npos) return;
        const std::size_t v = k + kKey.size();
        const std::string key(text.substr(v, text.find("\r\n", v) - v));
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
        resp += tr::net::ws::accept_key(key).view();
        resp += "\r\n\r\n";
        handshaken_ = peer_.write(as_bytes(resp));
    }

    /** @brief One unmasked server frame announcing @p len and carrying @p body. */
    [[nodiscard]] static bytes_t frame(tr::net::ws::opcode_t op, bool fin, std::size_t len,
                                       std::span<const std::byte> body) {
        bytes_t out;
        out.push_back(static_cast<std::byte>((fin ? 0x80u : 0u) | static_cast<std::uint8_t>(op)));
        if (len < 126) {
            out.push_back(static_cast<std::byte>(len));
        } else if (len <= 0xFFFFu) {
            out.push_back(std::byte{126});
            out.push_back(static_cast<std::byte>(len >> 8));
            out.push_back(static_cast<std::byte>(len & 0xFFu));
        } else {
            out.push_back(std::byte{127});
            for (int i = 7; i >= 0; --i)
                out.push_back(
                    static_cast<std::byte>((static_cast<std::uint64_t>(len) >> (i * 8)) & 0xFFu));
        }
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    /**
     * @brief Decode the masked client frames in @p in into whole messages; false when a
     *        trailing frame is incomplete.
     */
    [[nodiscard]] static bool decode_client_frames(const bytes_t& in, std::vector<bytes_t>& whole) {
        std::size_t at = 0;
        bytes_t message;
        while (in.size() - at >= 2) {
            const auto b0 = static_cast<std::uint8_t>(in[at]);
            const auto b1 = static_cast<std::uint8_t>(in[at + 1]);
            std::size_t hdr = 2;
            std::uint64_t len = b1 & 0x7Fu;
            const std::size_t ext = len == 126 ? 2 : len == 127 ? 8 : 0;
            if (in.size() - at < hdr + ext + 4) break;
            if (ext != 0) {
                len = 0;
                for (std::size_t i = 0; i < ext; ++i)
                    len = (len << 8) | static_cast<std::uint8_t>(in[at + hdr + i]);
            }
            hdr += ext;
            const std::size_t key_at = at + hdr;
            hdr += (b1 & 0x80u) != 0 ? 4 : 0;
            if (in.size() - at - hdr < len) break;
            const bool control = (b0 & 0x08u) != 0;
            for (std::size_t i = 0; i < len && !control; ++i) {
                std::byte c = in[at + hdr + i];
                if ((b1 & 0x80u) != 0) c ^= in[key_at + (i % 4)];
                message.push_back(c);
            }
            at += hdr + len;
            if (!control && (b0 & 0x80u) != 0) whole.push_back(std::exchange(message, {}));
        }
        return at == in.size();
    }

    counting_backend_t rx_;
    socket_peer_t peer_;
    bool handshaken_ = false;
    std::thread server_;
    tr::net::ws_client_transport_t link_;
};

// --- ws server ------------------------------------------------------------------------------

/**
 * @brief A `ws_server_transport_t` (flat, one peer) and a raw client that dials it, sends the
 *        opening handshake, and then speaks masked RFC 6455 client frames.
 *
 * Ingress cases only: the server is a bus whose listener stays up when one peer goes, so its
 * "link closed" is the peer's wire closing, and its egress cases belong to the shared egress.
 */
class ws_server_rig_t final : public link_rig_t {
   public:
    ws_server_rig_t()
        : link_(0, tr::net::ws_server_config_t{.memory = {.rx = &rx_}, .max_frame = kMaxFrame}) {
        handshaken_ = link_.ok() && peer_.dial(link_.local_port()) && handshake();
    }

    [[nodiscard]] std::string_view name() const override { return "ws server"; }
    [[nodiscard]] bool ready() const override { return handshaken_; }
    [[nodiscard]] tr::net::transport_t& link() override { return link_; }
    [[nodiscard]] counting_backend_t& rx_backend() override { return rx_; }
    [[nodiscard]] bool link_closed() override {
        bytes_t in;
        bool closed = false;
        peer_.read(in, 5ms, [](const bytes_t&) { return false; }, closed);
        return closed;
    }
    [[nodiscard]] bool egress_cases() const override { return false; }

    [[nodiscard]] bytes_t encode(std::span<const std::byte> body) const override {
        return frame(tr::net::ws::opcode_t::BINARY, true, body.size(), body);
    }
    [[nodiscard]] bool supports_fragments() const override { return true; }
    [[nodiscard]] bytes_t encode_fragments(std::span<const std::byte> body,
                                           std::size_t pieces) const override {
        bytes_t out;
        const std::size_t step = (body.size() + pieces - 1) / pieces;
        for (std::size_t at = 0; at < body.size(); at += step) {
            const std::span<const std::byte> part =
                body.subspan(at, std::min(step, body.size() - at));
            const bool last = at + part.size() == body.size();
            const bytes_t one =
                frame(at == 0 ? tr::net::ws::opcode_t::BINARY : tr::net::ws::opcode_t::CONT, last,
                      part.size(), part);
            out.insert(out.end(), one.begin(), one.end());
        }
        return out;
    }
    [[nodiscard]] bytes_t oversize_header() const override {
        return frame(tr::net::ws::opcode_t::BINARY, true, kMaxFrame + 1, {});
    }

    void peer_write(std::span<const std::byte> bytes) override { (void)peer_.write(bytes); }
    void peer_stall() override {}
    [[nodiscard]] bool peer_drain(std::chrono::milliseconds /*budget*/,
                                  std::vector<bytes_t>& /*whole*/) override {
        return true;
    }
    void peer_leave() override { peer_.leave(); }

   private:
    /** @brief Send the upgrade request and read the `101` back. */
    bool handshake() {
        const std::string req =
            "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!peer_.write(as_bytes(req))) return false;
        bytes_t resp;
        bool closed = false;
        const auto has_end = [](const bytes_t& b) {
            return std::string_view(reinterpret_cast<const char*>(b.data()), b.size())
                       .find("\r\n\r\n") != std::string_view::npos;
        };
        peer_.read(resp, kWait, has_end, closed);
        return std::string_view(reinterpret_cast<const char*>(resp.data()), resp.size())
            .starts_with("HTTP/1.1 101");
    }

    /** @brief One masked client frame announcing @p len and carrying @p body. */
    [[nodiscard]] static bytes_t frame(tr::net::ws::opcode_t op, bool fin, std::size_t len,
                                       std::span<const std::byte> body) {
        constexpr std::array<std::byte, 4> kKey{std::byte{0x37}, std::byte{0xFA}, std::byte{0x21},
                                                std::byte{0x3D}};
        bytes_t out;
        out.push_back(static_cast<std::byte>((fin ? 0x80u : 0u) | static_cast<std::uint8_t>(op)));
        if (len < 126) {
            out.push_back(static_cast<std::byte>(0x80u | len));
        } else if (len <= 0xFFFFu) {
            out.push_back(std::byte{0x80u | 126u});
            out.push_back(static_cast<std::byte>(len >> 8));
            out.push_back(static_cast<std::byte>(len & 0xFFu));
        } else {
            out.push_back(std::byte{0x80u | 127u});
            for (int i = 7; i >= 0; --i)
                out.push_back(
                    static_cast<std::byte>((static_cast<std::uint64_t>(len) >> (i * 8)) & 0xFFu));
        }
        out.insert(out.end(), kKey.begin(), kKey.end());
        for (std::size_t i = 0; i < body.size(); ++i) out.push_back(body[i] ^ kKey[i % 4]);
        return out;
    }

    counting_backend_t rx_;
    socket_peer_t peer_;
    bool handshaken_ = false;
    tr::net::ws_server_transport_t link_;
};

/** @brief The fake link, as a factory. */
std::unique_ptr<link_rig_t> make_fake() { return std::make_unique<fake_rig_t>(); }
/** @brief A dialled tcp link, as a factory. */
std::unique_ptr<link_rig_t> make_tcp() { return std::make_unique<tcp_rig_t>(); }

std::unique_ptr<link_rig_t> make_tcp_server() { return std::make_unique<tcp_server_rig_t>(); }
/** @brief A dialled ws link, as a factory. */
std::unique_ptr<link_rig_t> make_ws() { return std::make_unique<ws_rig_t>(); }
/** @brief A ws server with one dialled peer, as a factory. */
std::unique_ptr<link_rig_t> make_ws_server() { return std::make_unique<ws_server_rig_t>(); }

}  // namespace

int main() {
    std::printf("link seam suite: one case set over the fake link and today's stream links\n");
    run_suite(make_fake);
    run_suite(make_tcp);
    run_suite(make_tcp_server);
    run_suite(make_ws);
    run_suite(make_ws_server);
    return tr::testing::summary("link_seam");
}
