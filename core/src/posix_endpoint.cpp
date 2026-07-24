/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/posix_endpoint.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
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

void* posix_endpoint_t::thread_entry(void* self) {
    static_cast<posix_endpoint_t*>(self)->body_();
    return nullptr;
}

void posix_endpoint_t::start(std::function<void()> body, std::size_t stack_size) {
    body_ = std::move(body);

    pthread_attr_t attr;
    ::pthread_attr_init(&attr);
    if (stack_size != 0) {
        // A hint below the platform floor makes setstacksize return EINVAL and
        // leaves the attr's default stacksize in place — we fall back to the
        // default stack rather than fail the spawn.
        (void)::pthread_attr_setstacksize(&attr, stack_size);
    }

    // Error-code return, not a throw: see start()'s header contract (a std::thread
    // spawn failure would std::abort under -fno-exceptions).
    started_ = (::pthread_create(&thread_, &attr, &posix_endpoint_t::thread_entry, this) == 0);
    ::pthread_attr_destroy(&attr);
}

void posix_endpoint_t::stop_and_join() {
    stop_.store(true, std::memory_order_relaxed);
    if (started_) {
        ::pthread_join(thread_, nullptr);
        started_ = false;
    }
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

void stream_endpoint_t::write_all_iov(int fd, ::iovec* vec, std::size_t count) {
    if (fd < 0) return;
    while (count > 0) {
        msghdr msg{};
        msg.msg_iov = vec;
        msg.msg_iovlen = count;
        const ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;  // peer gone / error → drop the rest
        }
        std::size_t done = static_cast<std::size_t>(n);
        // Advance past every fully-written entry, then trim the one the write
        // stopped inside — the stream may stop at any byte boundary.
        while (count > 0 && done >= vec->iov_len) {
            done -= vec->iov_len;
            ++vec;
            --count;
        }
        if (count > 0 && done > 0) {
            vec->iov_base = static_cast<std::byte*>(vec->iov_base) + done;
            vec->iov_len -= done;
        }
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
