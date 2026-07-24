/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_udp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace tr::net {

namespace {
/** @brief Pack a peer endpoint as (ip << 16) | port; 0 means "no peer known yet". */
[[nodiscard]] std::uint64_t pack_peer(std::uint32_t ip_net, std::uint16_t port_host) noexcept {
    return (static_cast<std::uint64_t>(ip_net) << 16) | port_host;
}
}  // namespace

udp_transport_t::udp_transport_t(std::uint16_t bind_port, const std::string& peer_host,
                                 std::uint16_t peer_port, mem::mem_backend_t* backend,
                                 std::size_t recv_stack)
    : backend_(backend) {
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

    // A receive timeout lets the recv loop poll stop_ for a clean shutdown
    // (the posix_endpoint_t SO_RCVTIMEO idiom).
    set_rcv_timeout(fd_);

    start([this] { run(); }, recv_stack);
}

udp_transport_t::~udp_transport_t() {
    stop_and_join();  // FIRST: the run() thread reads fd_ released below
    if (fd_ >= 0) ::close(fd_);
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
    // The borrowed-span path (and the exhaustion drain) needs a full-cap scratch
    // buffer; it is allocated LAZILY on first use, so a steady-state owning-delivery
    // node (view receiver installed, backend healthy) never pays for it — and the
    // recv thread never carries a 64 KiB frame on its stack (FreeRTOS/pthread
    // stacks are a few KiB; a stack array here would overflow them on-target).
    std::unique_ptr<std::byte[]> scratch;
    const auto scratch_buf = [&scratch]() -> std::byte* {
        if (!scratch) scratch = std::make_unique<std::byte[]>(kMaxDatagram);
        return scratch.get();
    };
    // ADR-0042 §2 backpressure by injection: the RX segment size is bounded by the
    // injected backend — a pool's slot payload caps the datagram a bounded node
    // accepts (an MCU's lwIP never yields a 64 KiB datagram anyway); the heap
    // backend reports "unbounded", keeping the full kMaxDatagram cap.
    const std::size_t rx_cap = std::min(kMaxDatagram, backend_->max_segment_size());
    view::segment_ptr_t rx_seg;  // pending RX segment, reused across recv timeouts

    // The owning-vs-span RECEIVE STRATEGY is decided per iteration off the slot's
    // rx_.has_rope() — BEFORE the blocking recvfrom (the segment must exist to
    // receive into); the span path re-checks AFTER recvfrom so a rope sink
    // installed DURING the blocking call still delivers that datagram owning.
    // Sink snapshot/tier-select itself lives in the slot (receiver_slot.hpp).
    while (!stop_.load(std::memory_order_relaxed)) {
        if (rx_.has_rope()) {
            // ADR-0042 §2: one datagram = one frame = one segment — recvfrom straight
            // into a fresh refcounted segment from the injected backend; no library
            // buffer, no copy. The pending segment is reused across recv timeouts
            // (no idle churn). Exhaustion is backpressure: drain the datagram into
            // the lazy scratch, drop it, tick the counter — never an OOM.
            if (!rx_seg) rx_seg = view::segment_ptr_t::adopt(backend_->alloc(rx_cap));
            sockaddr_in from{};
            socklen_t flen = sizeof(from);
            std::byte* const dst = rx_seg ? rx_seg->bytes.data() : scratch_buf();
            const std::size_t cap = rx_seg ? rx_seg->bytes.size() : kMaxDatagram;
            const ssize_t n =
                ::recvfrom(fd_, dst, cap, 0, reinterpret_cast<sockaddr*>(&from), &flen);
            if (n <= 0) continue;  // timeout / EAGAIN / error → re-check stop_ (rx_seg kept)
            if (learn_peer_)
                peer_.store(pack_peer(from.sin_addr.s_addr, ntohs(from.sin_port)),
                            std::memory_order_relaxed);
            if (!rx_seg) {
                dropped_rx_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Narrow the whole-segment view to the received length and hand it up
            // owning — the receiver may pin/subview it beyond this call.
            rx_.deliver(
                view::view_t::over(std::move(rx_seg)).subview(0, static_cast<std::size_t>(n)));
            continue;
        }

        // Span path: recvfrom into the borrowed scratch (no owning segment committed).
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        std::byte* const buf = scratch_buf();
        const ssize_t n =
            ::recvfrom(fd_, buf, kMaxDatagram, 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0) continue;  // timeout / EAGAIN / error → re-check stop_
        // Listener mode: the latest datagram's source IS the peer (single-peer
        // UDP-server shape) — replies/sends target it from now on.
        if (learn_peer_)
            peer_.store(pack_peer(from.sin_addr.s_addr, ntohs(from.sin_port)),
                        std::memory_order_relaxed);
        // A rope sink may have been installed while we were blocked in recvfrom
        // above with the span decision already made — re-check and, if so, deliver this
        // datagram owning via a one-time copy into a backend segment (race-window
        // datagrams only; every subsequent datagram takes the zero-copy path above).
        if (rx_.has_rope()) {
            view::segment_ptr_t seg = view::segment_ptr_t::adopt(backend_->alloc(rx_cap));
            if (!seg || static_cast<std::size_t>(n) > seg->bytes.size()) {
                dropped_rx_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::memcpy(seg->bytes.data(), buf, static_cast<std::size_t>(n));
            rx_.deliver(view::view_t::over(std::move(seg)).subview(0, static_cast<std::size_t>(n)));
            continue;
        }
        rx_.deliver_borrowed(std::span<const std::byte>(buf, static_cast<std::size_t>(n)));
    }
}

}  // namespace tr::net
