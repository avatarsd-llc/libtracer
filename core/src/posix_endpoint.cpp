/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/posix_endpoint.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <utility>

namespace tr::net {

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

}  // namespace tr::net
