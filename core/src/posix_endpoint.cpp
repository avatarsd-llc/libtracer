/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/posix_endpoint.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <utility>

namespace tr::net {

namespace {
/** @brief SIGPIPE would otherwise kill the process if the peer vanishes
 *         mid-write (platforms without the flag fall back to 0). */
#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif
}  // namespace

posix_endpoint_t::~posix_endpoint_t() { stop_and_join(); }

void posix_endpoint_t::start(std::function<void()> body) { thread_ = std::thread(std::move(body)); }

void posix_endpoint_t::stop_and_join() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

void posix_endpoint_t::set_rcv_timeout(int fd) {
    timeval tv{.tv_sec = 0, .tv_usec = 100000};  // 100 ms
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int posix_endpoint_t::poll_readable(int fd) {
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    return ::poll(&pfd, 1, 100);
}

int posix_endpoint_t::poll_accept(int listen_fd) {
    if (poll_readable(listen_fd) <= 0) return -1;  // timeout / error → caller re-checks stop_
    return ::accept(listen_fd, nullptr, nullptr);
}

stream_endpoint_t::~stream_endpoint_t() {
    // The derived destructor already ran stop_and_join (its first act, per the
    // posix_endpoint_t teardown invariant), so nothing races this exchange.
    const int leftover = conn_fd_.exchange(-1, std::memory_order_relaxed);
    if (leftover >= 0) ::close(leftover);
}

void stream_endpoint_t::write_all(int fd, std::span<const std::byte> bytes) {
    if (fd < 0) return;
    std::size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;  // peer gone / error → drop the rest
        off += static_cast<std::size_t>(n);
    }
}

void stream_endpoint_t::send_all_locked(std::span<const std::byte> bytes) {
    // Hold write_m_ across the whole write so the recv thread cannot close and
    // reset conn_fd_ underneath us; read the fd inside the lock to pair with
    // teardown_peer.
    const std::lock_guard lock(write_m_);
    write_all(conn_fd_.load(std::memory_order_relaxed), bytes);
}

void stream_endpoint_t::teardown_peer(int fd) {
    // Reset under write_m_ BEFORE ::close so a concurrent send() never writes
    // to (or reads) a closed/reused fd.
    {
        const std::lock_guard lock(write_m_);
        conn_fd_.store(-1, std::memory_order_relaxed);
    }
    ::close(fd);
}

void stream_endpoint_t::run_accept_loop(int listen_fd, const std::function<bool(int)>& on_accept,
                                        const std::function<void(int)>& serve_peer) {
    while (!stop_.load(std::memory_order_relaxed)) {
        // One poll-100ms-recheck accept pass: timeout / error / no connection
        // → re-check stop_ and try again.
        const int fd = poll_accept(listen_fd);
        if (fd < 0) continue;
        if (!on_accept(fd)) {  // per-peer setup / handshake failed → reject
            ::close(fd);
            continue;
        }
        conn_fd_.store(fd, std::memory_order_relaxed);

        serve_peer(fd);

        teardown_peer(fd);  // then re-accept the next peer
    }
}

}  // namespace tr::net
