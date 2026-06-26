/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_udp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <utility>

namespace tr::net {

udp_transport_t::udp_transport_t(std::uint16_t bind_port, const std::string& peer_host,
                                 std::uint16_t peer_port)
    : peer_port_(peer_port) {
    in_addr addr{};
    if (::inet_pton(AF_INET, peer_host.c_str(), &addr) == 1) peer_ip_ = addr.s_addr;

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return;

    const int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bind_port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        bound_port_ = ntohs(bound.sin_port);

    // A receive timeout lets the recv loop poll stop_ for a clean shutdown.
    timeval tv{.tv_sec = 0, .tv_usec = 100000};  // 100 ms
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    thread_ = std::thread([this] { run(); });
}

udp_transport_t::~udp_transport_t() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) ::close(fd_);
}

void udp_transport_t::set_receiver(receiver_t receiver) {
    const std::lock_guard lock(m_);
    receiver_ = std::move(receiver);
}

void udp_transport_t::send(std::span<const std::byte> frame) {
    if (fd_ < 0) return;
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = peer_ip_;
    peer.sin_port = htons(peer_port_);
    ::sendto(fd_, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
}

void udp_transport_t::run() {
    std::array<std::byte, 65536> buf;
    while (!stop_.load(std::memory_order_relaxed)) {
        const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (n <= 0) continue;  // timeout / EAGAIN / error → re-check stop_
        receiver_t receiver;
        {
            const std::lock_guard lock(m_);
            receiver = receiver_;
        }
        if (receiver) receiver(std::span<const std::byte>(buf.data(), static_cast<std::size_t>(n)));
    }
}

}  // namespace tr::net
