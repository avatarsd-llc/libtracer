/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_udp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <utility>
#include <vector>

namespace tr::net {

namespace {
// Pack a peer endpoint as (ip << 16) | port; 0 means "no peer known yet".
[[nodiscard]] std::uint64_t pack_peer(std::uint32_t ip_net, std::uint16_t port_host) noexcept {
    return (static_cast<std::uint64_t>(ip_net) << 16) | port_host;
}
}  // namespace

udp_transport_t::udp_transport_t(std::uint16_t bind_port, const std::string& peer_host,
                                 std::uint16_t peer_port) {
    std::uint32_t peer_ip = 0;
    in_addr addr{};
    if (::inet_pton(AF_INET, peer_host.c_str(), &addr) == 1) peer_ip = addr.s_addr;
    peer_.store(pack_peer(peer_ip, peer_port), std::memory_order_relaxed);
    // An unresolved peer at construction = listener mode: learn it from inbound
    // datagrams' source addresses (see the header note).
    learn_peer_ = peer_ip == 0 || peer_port == 0;

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
    const std::uint64_t p = peer();
    if (fd_ < 0 || p == 0) return;  // no peer (learned or configured) => nobody to send to
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = static_cast<std::uint32_t>(p >> 16);
    peer.sin_port = htons(static_cast<std::uint16_t>(p & 0xFFFF));
    ::sendto(fd_, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
}

void udp_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    if (fd_ < 0 || iov.empty()) return;
    // Gather the rope's segments into one datagram with a single syscall — no
    // userspace flatten copy (the "rope we put into tx", lowered to sendmsg). The iovec
    // count is small and bounded (a FWD forward/reply is ≤ ~6 spans), so the common case
    // uses a fixed stack array — no per-datagram heap allocation. Only an unusually large
    // gather (more than kMaxInlineIov spans) falls back to the heap vector.
    constexpr std::size_t kMaxInlineIov = 16;
    std::array<::iovec, kMaxInlineIov> inline_vec;
    std::vector<::iovec> heap_vec;
    ::iovec* vec = inline_vec.data();
    std::size_t n = iov.size();
    if (n > kMaxInlineIov) {
        heap_vec.reserve(n);
        for (const auto& s : iov)
            heap_vec.push_back(::iovec{const_cast<std::byte*>(s.data()), s.size()});
        vec = heap_vec.data();
    } else {
        for (std::size_t i = 0; i < n; ++i)
            inline_vec[i] = ::iovec{const_cast<std::byte*>(iov[i].data()), iov[i].size()};
    }
    const std::uint64_t p = peer();
    if (p == 0) return;  // no peer (learned or configured) => nobody to send to
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = static_cast<std::uint32_t>(p >> 16);
    peer.sin_port = htons(static_cast<std::uint16_t>(p & 0xFFFF));
    msghdr msg{};
    msg.msg_name = &peer;
    msg.msg_namelen = sizeof(peer);
    msg.msg_iov = vec;
    msg.msg_iovlen = n;
    ::sendmsg(fd_, &msg, 0);
}

void udp_transport_t::run() {
    std::array<std::byte, 65536> buf;
    while (!stop_.load(std::memory_order_relaxed)) {
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        const ssize_t n =
            ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0) continue;  // timeout / EAGAIN / error → re-check stop_
        // Listener mode: the latest datagram's source IS the peer (single-peer
        // UDP-server shape) — replies/sends target it from now on.
        if (learn_peer_)
            peer_.store(pack_peer(from.sin_addr.s_addr, ntohs(from.sin_port)),
                        std::memory_order_relaxed);
        receiver_t receiver;
        {
            const std::lock_guard lock(m_);
            receiver = receiver_;
        }
        if (receiver) receiver(std::span<const std::byte>(buf.data(), static_cast<std::size_t>(n)));
    }
}

}  // namespace tr::net
