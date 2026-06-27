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
#include <string>
#include <string_view>
#include <utility>

#include "libtracer/ws.hpp"

namespace tr::net {

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
                case ws::opcode_t::BINARY: {
                    receiver_t receiver;
                    {
                        const std::lock_guard lock(m_);
                        receiver = receiver_;
                    }
                    if (receiver) receiver(std::span<const std::byte>(frame.payload));
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
                    break;  // TEXT / PONG / CONT: ignored
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

}  // namespace tr::net
