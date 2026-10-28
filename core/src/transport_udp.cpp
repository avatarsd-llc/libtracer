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
#include <utility>
#include <vector>

#include "libtracer/config.hpp"
#include "libtracer/iov_table.hpp"

namespace tr::net {

namespace {
/** @brief A send to a shut socket must not raise SIGPIPE (platforms without the flag use 0). */
#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif
}  // namespace

namespace {
/** @brief Pack a peer endpoint as (ip << 16) | port; 0 means "no peer known yet". */
[[nodiscard]] std::uint64_t pack_peer(std::uint32_t ip_net, std::uint16_t port_host) noexcept {
    return (static_cast<std::uint64_t>(ip_net) << 16) | port_host;
}
}  // namespace

udp_transport_t::udp_transport_t(std::uint16_t bind_port, std::string_view peer_host,
                                 std::uint16_t peer_port, const udp_config_t& config)
    : backend_(config.memory.rx) {
    const std::size_t max_frame = config.max_frame;
    const std::size_t recv_stack = config.recv_stack;
    // `:settings max_frame` (0 = unset) sets the accepted-datagram cap; unset is one Ethernet
    // MTU (#2132). kMaxDatagram is the datagram ceiling itself — a UDP payload cannot be
    // larger — so a configured value above it is inert, never a widened ingress bound.
    if (max_frame != 0) max_frame_ = std::min(max_frame, kMaxDatagram);
    std::uint32_t peer_ip = 0;
    in_addr addr{};
    if (parse_ipv4(peer_host, addr)) peer_ip = addr.s_addr;
    peer_.store(pack_peer(peer_ip, peer_port), std::memory_order_relaxed);
    // An unresolved peer at construction = listener mode: learn it from inbound
    // datagrams' source addresses (see the header note).
    learn_peer_ = peer_ip == 0 || peer_port == 0;

    // The receive loop keeps its stack scratch on the thread this spawns (#2142): a stack
    // too small to hold it is refused here, before any socket, never clamped.
    recv_stack_refused_ = recv_stack != 0 && recv_stack < kMinRecvStack;
    if (recv_stack_refused_) return;

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return;

    // SO_REUSEADDR only on a NAMED port (#2027). On an ephemeral bind it lets the kernel hand
    // out a port another reuse-enabled UDP socket already holds, and unicast datagrams for that
    // port then reach whichever socket bound last, not this one.
    if (bind_port != 0) {
        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }

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

    // A refused receive-thread spawn (#2288) leaves the link inert: ok() false.
    if (!start([this] { run(); }, recv_stack, config.memory.stack_or_default())) {
        ::close(fd_);
        fd_ = -1;
    }
}

udp_transport_t::~udp_transport_t() {
    stop_and_join();  // FIRST: the run() thread reads fd_ released below
    if (fd_ >= 0) ::close(fd_);
}

void udp_transport_t::shut_down() {
    stop_and_join();
    // `send` reads `fd_` without a lock, so the descriptor NUMBER must stay occupied until the
    // object goes; only the socket behind it is released here.
    if (fd_ < 0) return;
#if defined(ESP_PLATFORM)
    // lwIP has no `dup2`, and `shutdown` of a UDP socket is EOPNOTSUPP there: the socket, and
    // its port, stay until the object is destroyed, and a send that reaches it still leaves.
    (void)::shutdown(fd_, SHUT_RDWR);
#else
    // `dup2` swaps a socket that can send nothing (an unbound AF_UNIX datagram socket, which
    // refuses an AF_INET destination) in over the UDP one, closing it in the same step: its
    // port is free at once, and a send still in flight gets an error, never another socket.
    const int dead = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (dead >= 0 && ::dup2(dead, fd_) >= 0) {
        ::close(dead);
        return;
    }
    if (dead >= 0) ::close(dead);
    (void)::shutdown(fd_, SHUT_RDWR);  // no spare descriptor: shut, and close at destruction
#endif
}

void udp_transport_t::send(std::span<const std::byte> frame) {
    const std::uint64_t p = peer();
    if (fd_ < 0 || p == 0) {  // no peer (learned or configured) => nobody to send to
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);  // shed, and counted (#932)
        return;
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = static_cast<std::uint32_t>(p >> 16);
    peer.sin_port = htons(static_cast<std::uint16_t>(p & 0xFFFF));
    // A refused or short datagram (a full socket buffer, EMSGSIZE, a shut socket) is lost and
    // counted, never retried: the link carries no acks, and the next value supersedes it (#2279).
    if (::sendto(fd_, frame.data(), frame.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&peer),
                 sizeof(peer)) != static_cast<ssize_t>(frame.size()))
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
}

void udp_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    send_to(peer(), iov);
}

void udp_transport_t::send_to(std::uint64_t address,
                              std::span<const std::span<const std::byte>> iov) {
    if (fd_ < 0) {
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);  // no socket => a counted drop
        return;
    }
    if (iov.empty()) return;  // nothing to send is not a drop
    // Gather the rope's segments into one datagram with a single syscall — no
    // userspace flatten copy (the "rope we put into tx", lowered to sendmsg). The iovec
    // count is small and bounded (a FWD forward/reply is ≤ ~6 spans), so the common case
    // uses a fixed stack array — no per-datagram heap allocation. Only an unusually large
    // gather (more than kMaxInlineIov spans) falls back to the heap vector.
    //
    // MEASURED (`bench_transport_iov`): the fallback fires at exactly **17 spans**, costing
    // one allocation of ~288 B. `bench_forward_heap`'s `allocs=0` gate CANNOT see it — that
    // bench drives a stub link which never assembles an iovec. Headroom against
    // `kFwdMaxIov` (9) is **8 regions**, and a rope source may split any region further —
    // which is exactly why the fallback is drawn from the shared NOTHROW overflow store
    // (`%iov_table.hpp`) and exhaustion DROPS the datagram (#848): the entry count is the
    // sending peer's choice, and the old throwing `reserve` made that an abort() under
    // `-fno-exceptions`.
    std::array<::iovec, kMaxInlineIov> inline_vec;
    iov_table_t<::iovec> table(inline_vec, egress_source());
    const std::size_t n = iov.size();
    ::iovec* vec = table.acquire(n);
    if (vec == nullptr) {  // gather store exhausted => drop the datagram, counted (#932)
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::size_t total = 0;
    for (std::size_t i = 0; i < n; ++i) {
        vec[i] = ::iovec{const_cast<std::byte*>(iov[i].data()), iov[i].size()};
        total += iov[i].size();
    }
    const std::uint64_t p = address;
    if (p == 0) {  // no peer (learned or configured) => nobody to send to
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = static_cast<std::uint32_t>(p >> 16);
    peer.sin_port = htons(static_cast<std::uint16_t>(p & 0xFFFF));
    msghdr msg{};
    msg.msg_name = &peer;
    msg.msg_namelen = sizeof(peer);
    msg.msg_iov = vec;
    msg.msg_iovlen = n;
    if (::sendmsg(fd_, &msg, MSG_NOSIGNAL) != static_cast<ssize_t>(total))  // as in `send` (#2279)
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
}

void udp_transport_t::run() {
    // ADR-0042 §2 backpressure by injection: the RX segment size is bounded by the
    // injected backend — a pool's slot payload caps the datagram a bounded node
    // accepts (an MCU's lwIP never yields a 64 KiB datagram anyway); the heap
    // backend reports "unbounded", keeping the full kMaxDatagram cap.
    const std::size_t rx_cap = std::min(kMaxDatagram, backend_->max_segment_size());
    // The CONFIGURED half of the ingress bound (#926): the largest datagram this
    // connection accepts. Buffers are sized ONE BYTE past it, because that is what makes
    // an over-cap datagram observable at all — recvfrom silently truncates a datagram to
    // the buffer it is given and reports the truncated length, so a buffer of exactly the
    // cap would deliver an over-cap datagram as a short, corrupt frame. With a buffer of
    // cap+1, any datagram above the cap reads back as `n > cap` and is refused below.
    // Unset, the cap is kDefaultMaxFrame, so a link holds a 1,473-byte segment at rest, not
    // 64 KiB (#2132). A cap at kMaxDatagram makes cap+1 exceed both bounds, and no datagram
    // can reach 65,536 bytes, so the refusal is unreachable there.
    const std::size_t frame_cap = max_frame_;
    const std::size_t alloc_cap = std::min(rx_cap, frame_cap + 1);
    // The accepted length, on either path: the cap, and the backend's bound too, so an
    // injected bounded backend refuses a datagram too long for its slot whole (#1783).
    const std::size_t span_cap = std::min(frame_cap, rx_cap);
    // The datagram adapter of frame ingress (ADR-0087 §1, #2142). A borrowed sink with a cap
    // inside kUdpStackScratchBytes receives on this thread's stack: nothing held at rest,
    // one recvfrom per datagram, no copy. Every other link receives into ONE held segment of
    // `alloc_cap` from its own `memory.rx`, reused across receive timeouts and replaced once
    // handed up, so an owning sink gets the datagram with no copy. The stack array also
    // drains a datagram that has no segment to land in.
    std::array<std::byte, graph::kUdpStackScratchBytes + 1> stack;
    const bool fits_stack = span_cap <= graph::kUdpStackScratchBytes;
    view::segment_ptr_t rx_seg;

    // The owning-vs-span RECEIVE STRATEGY is decided per iteration off the slot's
    // rx_.has_rope() — BEFORE the blocking recvfrom (the segment must exist to
    // receive into); a stack receive re-checks AFTER recvfrom so a rope sink
    // installed DURING the blocking call still delivers that datagram owning.
    // Sink snapshot/tier-select itself lives in the slot (receiver_slot.hpp).
    while (!stop_.load(std::memory_order_relaxed)) {
        const bool on_stack = fits_stack && !rx_.has_rope();
        // Exhaustion is backpressure: the datagram drains through the stack array and is
        // dropped and counted, never an OOM.
        if (!on_stack && !rx_seg) rx_seg = view::segment_ptr_t::adopt(backend_->alloc(alloc_cap));
        const bool drained = !on_stack && !rx_seg;
        // A held segment takes the datagram; otherwise the stack array does, for a stack
        // receive (span_cap + 1 fits, by fits_stack) or a drain.
        const std::span<std::byte> buf =
            rx_seg ? rx_seg->bytes
                   : std::span<std::byte>(stack.data(), std::min(span_cap + 1, stack.size()));
        // recvmsg, not recvfrom, for MSG_TRUNC: a held segment bounded by a tight backend can
        // be smaller than the cap plus one, and a datagram it truncated must be refused, not
        // delivered short (#1783). Linux returns the truncated length, lwIP the whole one;
        // the flag is set either way. Still one syscall per datagram.
        sockaddr_in from{};
        ::iovec into{buf.data(), buf.size()};
        ::msghdr mh{};
        mh.msg_name = &from;
        mh.msg_namelen = sizeof(from);
        mh.msg_iov = &into;
        mh.msg_iovlen = 1;
        const ssize_t n = ::recvmsg(fd_, &mh, 0);
        if (n <= 0) continue;  // timeout / EAGAIN / error → re-check stop_ (rx_seg kept)
        // Listener mode: the latest datagram's source IS the peer (single-peer
        // UDP-server shape) — replies/sends target it from now on.
        source_ = pack_peer(from.sin_addr.s_addr, ntohs(from.sin_port));
        if (learn_peer_) peer_.store(source_, std::memory_order_relaxed);
        const auto len = static_cast<std::size_t>(n);
        if (drained) {
            dropped_rx_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (len > span_cap || (mh.msg_flags & MSG_TRUNC) != 0) {
            // Over the cap, or cut short by a held segment: refuse it whole. It is KEPT (it is
            // still a clean buffer for the next datagram), so a peer spraying over-cap datagrams
            // costs no allocator traffic at all.
            malformed_rx_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (rx_seg) {
            // A held segment (an owning sink, a cap above the stack bound, or a sink that
            // stopped owning since it was drawn) is handed up whole and not redrawn for a
            // stack receive. Narrow the whole-segment view to the received length and hand it up
            // owning. Built by aggregate init, NOT over().subview(): `subview` copies the handle,
            // so a second reference would live across the receiver callback (#845).
            rx_.deliver(view::view_t{std::move(rx_seg), 0, len});
            continue;
        }
        // A rope sink installed while this thread was blocked in recvfrom takes this one
        // datagram owning, by a one-time copy into a segment; every later one is received
        // into a segment above.
        if (rx_.has_rope()) {
            view::segment_ptr_t seg = view::segment_ptr_t::adopt(backend_->alloc(alloc_cap));
            if (!seg) {
                dropped_rx_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::memcpy(seg->bytes.data(), stack.data(), len);
            rx_.deliver(view::view_t{std::move(seg), 0, len});
            continue;
        }
        rx_.deliver_borrowed(std::span<const std::byte>(stack.data(), len));
    }
}

}  // namespace tr::net
