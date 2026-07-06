/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_ws.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/ws.hpp"

namespace tr::net {

namespace {

// RFC 6455 fragmented-message reassembly as ROPE CHAINING (ADR-0053 §5):
// each data fragment becomes one owning link (the copy out of the reused
// connection buffer is the legitimate substrate-boundary copy; chaining the
// fragments is pointer-linking, never a memcpy). One assembler per connection,
// used only on its recv thread.
struct ws_assembler_t {
    tr::view::rope_t partial;
    bool assembling = false;

    void reset() {
        partial = tr::view::rope_t{};
        assembling = false;
    }

    // Feed one data frame (BINARY or CONT). Returns the completed message as a
    // rope when this frame finishes one, std::nullopt otherwise. A BINARY that
    // arrives mid-assembly is an RFC 6455 protocol error: the stale assembly is
    // dropped and the new message starts. A stray CONT (no assembly open) is
    // dropped. An allocation failure drops the whole message (backpressure).
    std::optional<tr::view::rope_t> on_data(ws::opcode_t op, bool fin,
                                            std::span<const std::byte> payload) {
        if (op == ws::opcode_t::BINARY && assembling) reset();             // protocol error
        if (op == ws::opcode_t::CONT && !assembling) return std::nullopt;  // stray

        const std::optional<tr::view::view_t> link = tr::view::over_bytes(payload);
        if (!link) {  // allocation failure => drop the message (backpressure)
            reset();
            return std::nullopt;
        }
        partial.append(*link);
        assembling = true;
        if (!fin) return std::nullopt;
        tr::view::rope_t done = std::move(partial);
        reset();
        return done;
    }
};

}  // namespace

namespace {

// SIGPIPE would otherwise kill the process if the client vanishes mid-write.
#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif

// Case-insensitive search for an HTTP header and return its trimmed value.
// `request` is the raw header block; `name` is lowercase (e.g. "sec-websocket-key").
std::string header_value(std::string_view request, std::string_view name) {
    std::string lower(request);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t pos = 0;
    while ((pos = lower.find(name, pos)) != std::string::npos) {
        // Must sit at the start of a header line (start of buffer or after a newline).
        if (pos != 0 && request[pos - 1] != '\n') {
            pos += name.size();
            continue;
        }
        std::size_t colon = request.find(':', pos + name.size());
        if (colon == std::string_view::npos) return {};
        std::size_t eol = request.find("\r\n", colon);
        if (eol == std::string_view::npos) eol = request.size();
        std::string_view val = request.substr(colon + 1, eol - colon - 1);
        std::size_t b = val.find_first_not_of(" \t");
        if (b == std::string_view::npos) return {};
        std::size_t e = val.find_last_not_of(" \t");
        return std::string(val.substr(b, e - b + 1));
    }
    return {};
}

}  // namespace

transport_ws_server::transport_ws_server(std::uint16_t bind_port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bind_port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
        ::listen(listen_fd_, 1) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        bound_port_ = ntohs(bound.sin_port);

    thread_ = std::thread([this] { run(); });
}

transport_ws_server::~transport_ws_server() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void transport_ws_server::set_rope_receiver(rope_receiver_t receiver) {
    const std::lock_guard lock(m_);
    rope_receiver_ = std::move(receiver);
}

void transport_ws_server::set_receiver(receiver_t receiver) {
    const std::lock_guard lock(m_);
    receiver_ = std::move(receiver);
}

void transport_ws_server::write_all(int fd, std::span<const std::byte> bytes) {
    if (fd < 0) return;
    std::size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;  // peer gone / error → drop the rest
        off += static_cast<std::size_t>(n);
    }
}

void transport_ws_server::send(std::span<const std::byte> frame) {
    const std::vector<std::byte> encoded = ws::encode_frame(ws::opcode_t::BINARY, frame);
    // Hold write_m_ across the whole write so the recv thread cannot close and
    // reset client_fd_ underneath us (it tears the connection down under the same
    // lock); read the fd inside the lock to pair with that teardown.
    const std::lock_guard lock(write_m_);
    write_all(client_fd_.load(std::memory_order_relaxed), encoded);
}

bool transport_ws_server::handshake(int fd) {
    std::string req;
    std::array<char, 1024> chunk;
    // Read until the end of the HTTP header block (CRLFCRLF), bounded.
    while (req.find("\r\n\r\n") == std::string::npos) {
        if (stop_.load(std::memory_order_relaxed)) return false;
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr < 0) return false;
        if (pr == 0) continue;  // timeout → re-check stop_
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) return false;  // peer closed / error
        req.append(chunk.data(), static_cast<std::size_t>(n));
        if (req.size() > 16384) return false;  // runaway request guard
    }

    const std::string key = header_value(req, "sec-websocket-key");
    if (key.empty()) return false;

    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    resp += ws::accept_key(key);
    resp += "\r\n\r\n";

    std::vector<std::byte> bytes(resp.size());
    for (std::size_t i = 0; i < resp.size(); ++i) bytes[i] = static_cast<std::byte>(resp[i]);
    {
        const std::lock_guard lock(write_m_);
        write_all(fd, bytes);
    }
    return true;
}

void transport_ws_server::serve(int fd) {
    ws_assembler_t asm_state;  // per-connection fragment assembly (recv thread only)
    std::vector<std::byte> buf;
    std::array<std::byte, 4096> chunk;

    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr < 0) return;
        if (pr == 0) continue;  // timeout → re-check stop_

        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0) return;  // peer closed the TCP connection
        if (n < 0) return;   // error
        buf.insert(buf.end(), chunk.data(), chunk.data() + n);

        // Drain every complete frame currently in the buffer; leftover partial
        // bytes stay for the next read.
        while (true) {
            auto decoded = ws::decode_frame(buf);
            if (!decoded) break;
            ws::frame_t frame = std::move(decoded->first);
            const std::size_t consumed = decoded->second;
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));

            switch (frame.op) {
                case ws::opcode_t::BINARY:
                case ws::opcode_t::CONT: {
                    receiver_t receiver;
                    rope_receiver_t rope_receiver;
                    {
                        const std::lock_guard lock(m_);
                        receiver = receiver_;
                        rope_receiver = rope_receiver_;
                    }
                    // Unfragmented fast path on the span tier: the borrowed
                    // payload is delivered directly, no owning copy (as before).
                    if (frame.op == ws::opcode_t::BINARY && frame.fin && !asm_state.assembling &&
                        !rope_receiver) {
                        if (receiver) receiver(std::span<const std::byte>(frame.payload));
                        break;
                    }
                    auto msg = asm_state.on_data(frame.op, frame.fin, frame.payload);
                    if (!msg) break;  // mid-message (or dropped)
                    // The reassembled message IS a rope — one owning link per
                    // fragment (ADR-0053 §5). Only the span tier pays a flatten.
                    if (rope_receiver) {
                        rope_receiver(std::move(*msg));
                    } else if (receiver) {
                        const tr::view::view_t flat = msg->flatten();
                        if (!flat.empty() || msg->total_length() == 0) receiver(flat.bytes());
                    }
                    break;
                }
                case ws::opcode_t::PING: {
                    const std::vector<std::byte> pong =
                        ws::encode_frame(ws::opcode_t::PONG, frame.payload);
                    const std::lock_guard lock(write_m_);
                    write_all(fd, pong);
                    break;
                }
                case ws::opcode_t::CLOSE:
                    return;  // tear the connection down
                default:
                    break;  // TEXT / PONG: ignored
            }
        }
    }
}

void transport_ws_server::run() {
    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd pfd{.fd = listen_fd_, .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr <= 0) continue;  // timeout / error → re-check stop_

        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;

        if (!handshake(fd)) {
            ::close(fd);
            continue;
        }
        client_fd_.store(fd, std::memory_order_relaxed);

        serve(fd);

        // Tear down under write_m_ so a concurrent send() never writes to (or
        // reads) a closed/reused fd.
        {
            const std::lock_guard lock(write_m_);
            client_fd_.store(-1, std::memory_order_relaxed);
        }
        ::close(fd);
    }
}

// ---------------------------------------------------------------------------
// transport_ws_client — the dial-out half.
// ---------------------------------------------------------------------------

transport_ws_client::transport_ws_client(const std::string& host, std::uint16_t port) {
    // Seed the per-frame masking-key stream with something that varies between
    // connections (steady_clock + this address). Not crypto-strong — RFC 6455
    // masking exists for proxy/cache safety, not to defend against a peer.
    std::uint64_t seed =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    seed ^= reinterpret_cast<std::uintptr_t>(this);
    mask_state_.store(seed, std::memory_order_relaxed);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &peer.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
        ::close(fd);
        return;
    }

    if (!handshake(fd, host, port)) {
        ::close(fd);
        return;
    }

    conn_fd_.store(fd, std::memory_order_relaxed);
    connected_ = true;
    thread_ = std::thread([this, fd] { serve(fd); });
}

transport_ws_client::~transport_ws_client() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    // serve() tears the socket down on exit; this only catches a never-spawned
    // thread (handshake failed) where conn_fd_ is already -1, so it never
    // double-closes.
    const int leftover = conn_fd_.exchange(-1, std::memory_order_relaxed);
    if (leftover >= 0) ::close(leftover);
}

void transport_ws_client::set_rope_receiver(rope_receiver_t receiver) {
    const std::lock_guard lock(m_);
    rope_receiver_ = std::move(receiver);
}

void transport_ws_client::set_receiver(receiver_t receiver) {
    const std::lock_guard lock(m_);
    receiver_ = std::move(receiver);
}

std::uint32_t transport_ws_client::next_mask_key() {
    // SplitMix64 step → a varied (non-crypto) 32-bit masking key per frame.
    std::uint64_t z = mask_state_.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed) +
                      0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<std::uint32_t>(z);
}

void transport_ws_client::write_all(int fd, std::span<const std::byte> bytes) {
    if (fd < 0) return;
    std::size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;  // peer gone / error → drop the rest
        off += static_cast<std::size_t>(n);
    }
}

void transport_ws_client::send(std::span<const std::byte> frame) {
    const std::vector<std::byte> encoded =
        ws::encode_client_frame(ws::opcode_t::BINARY, frame, next_mask_key());
    // Same discipline as the server: hold write_m_ across the whole write so the
    // recv thread cannot tear down and reset conn_fd_ underneath us; read the fd
    // inside the lock to pair with that teardown.
    const std::lock_guard lock(write_m_);
    write_all(conn_fd_.load(std::memory_order_relaxed), encoded);
}

bool transport_ws_client::handshake(int fd, const std::string& host, std::uint16_t port) {
    // A fresh 16-byte nonce (RFC 6455 §4.1) base64'd into Sec-WebSocket-Key.
    std::array<std::byte, 16> nonce{};
    std::uint64_t s = mask_state_.load(std::memory_order_relaxed) ^ 0xD1B54A32D192ED03ull;
    for (auto& b : nonce) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        b = static_cast<std::byte>((s >> 56) & 0xFFu);
    }
    const std::string key = ws::base64(nonce);

    std::string req = "GET / HTTP/1.1\r\nHost: ";
    req += host;
    req += ':';
    req += std::to_string(port);
    req +=
        "\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: ";
    req += key;
    req += "\r\nSec-WebSocket-Version: 13\r\n\r\n";

    std::vector<std::byte> bytes(req.size());
    for (std::size_t i = 0; i < req.size(); ++i) bytes[i] = static_cast<std::byte>(req[i]);
    {
        const std::lock_guard lock(write_m_);
        write_all(fd, bytes);
    }

    // Read the response header block (CRLFCRLF), bounded so a stuck peer cannot
    // hang construction forever.
    std::string resp;
    std::array<char, 1024> chunk;
    while (resp.find("\r\n\r\n") == std::string::npos) {
        if (stop_.load(std::memory_order_relaxed)) return false;
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr < 0) return false;
        if (pr == 0) continue;  // timeout → re-check stop_
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) return false;  // peer closed / error
        resp.append(chunk.data(), static_cast<std::size_t>(n));
        if (resp.size() > 16384) return false;  // runaway response guard
    }

    if (resp.find("101") == std::string::npos) return false;
    const std::string accept = header_value(resp, "sec-websocket-accept");
    return !accept.empty() && accept == ws::accept_key(key);
}

void transport_ws_client::serve(int fd) {
    ws_assembler_t asm_state;  // per-connection fragment assembly (recv thread only)
    std::vector<std::byte> buf;
    std::array<std::byte, 4096> chunk;

    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr < 0) break;
        if (pr == 0) continue;  // timeout → re-check stop_

        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) break;  // peer closed / error
        buf.insert(buf.end(), chunk.data(), chunk.data() + n);

        // Drain every complete frame; leftover partial bytes stay for next read.
        while (true) {
            auto decoded = ws::decode_frame(buf);
            if (!decoded) break;
            ws::frame_t frame = std::move(decoded->first);
            const std::size_t consumed = decoded->second;
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));

            switch (frame.op) {
                case ws::opcode_t::BINARY:
                case ws::opcode_t::CONT: {
                    receiver_t receiver;
                    rope_receiver_t rope_receiver;
                    {
                        const std::lock_guard lock(m_);
                        receiver = receiver_;
                        rope_receiver = rope_receiver_;
                    }
                    // Unfragmented fast path on the span tier: the borrowed
                    // payload is delivered directly, no owning copy (as before).
                    if (frame.op == ws::opcode_t::BINARY && frame.fin && !asm_state.assembling &&
                        !rope_receiver) {
                        if (receiver) receiver(std::span<const std::byte>(frame.payload));
                        break;
                    }
                    auto msg = asm_state.on_data(frame.op, frame.fin, frame.payload);
                    if (!msg) break;  // mid-message (or dropped)
                    // The reassembled message IS a rope — one owning link per
                    // fragment (ADR-0053 §5). Only the span tier pays a flatten.
                    if (rope_receiver) {
                        rope_receiver(std::move(*msg));
                    } else if (receiver) {
                        const tr::view::view_t flat = msg->flatten();
                        if (!flat.empty() || msg->total_length() == 0) receiver(flat.bytes());
                    }
                    break;
                }
                case ws::opcode_t::PING: {
                    // A client PONG must be masked, just like client data frames.
                    const std::vector<std::byte> pong =
                        ws::encode_client_frame(ws::opcode_t::PONG, frame.payload, next_mask_key());
                    const std::lock_guard lock(write_m_);
                    write_all(fd, pong);
                    break;
                }
                case ws::opcode_t::CLOSE:
                    goto teardown;  // peer asked to close
                default:
                    break;  // TEXT / PONG: ignored
            }
        }
    }

teardown:
    // Tear down under write_m_ so a concurrent send() never writes to (or reads)
    // a closed/reused fd.
    {
        const std::lock_guard lock(write_m_);
        conn_fd_.store(-1, std::memory_order_relaxed);
    }
    ::close(fd);
}

}  // namespace tr::net
