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

#include "libtracer/iov_table.hpp"

namespace tr::net {

namespace {
/** @brief Pack a peer endpoint as (ip << 16) | port; 0 means "no peer known yet". */
[[nodiscard]] std::uint64_t pack_peer(std::uint32_t ip_net, std::uint16_t port_host) noexcept {
    return (static_cast<std::uint64_t>(ip_net) << 16) | port_host;
}
}  // namespace

udp_transport_t::udp_transport_t(std::uint16_t bind_port, const std::string& peer_host,
                                 std::uint16_t peer_port, mem::mem_backend_t* backend,
                                 std::size_t max_frame, std::size_t recv_stack)
    : backend_(backend) {
    // `:settings max_frame` (0 = unset) tightens the accepted-datagram cap. kMaxDatagram is
    // not a policy default here but the datagram ceiling itself — a UDP payload cannot be
    // larger — so a configured value above it is inert, never a widened ingress bound.
    if (max_frame != 0 && max_frame < max_frame_) max_frame_ = max_frame;
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
    if (fd_ < 0 || p == 0) {  // no peer (learned or configured) => nobody to send to
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);  // shed, and counted (#932)
        return;
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = static_cast<std::uint32_t>(p >> 16);
    peer.sin_port = htons(static_cast<std::uint16_t>(p & 0xFFFF));
    ::sendto(fd_, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
}

void udp_transport_t::send(std::span<const std::span<const std::byte>> iov) {
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
    for (std::size_t i = 0; i < n; ++i)
        vec[i] = ::iovec{const_cast<std::byte*>(iov[i].data()), iov[i].size()};
    const std::uint64_t p = peer();
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
    ::sendmsg(fd_, &msg, 0);
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
    // Unset (kMaxDatagram) keeps the pre-#926 buffers exactly: cap+1 exceeds both bounds,
    // and no datagram can reach 65,536 bytes, so the refusal is unreachable.
    const std::size_t frame_cap = max_frame_;
    const std::size_t alloc_cap = std::min(rx_cap, frame_cap + 1);
    const std::size_t scratch_cap = std::min(kMaxDatagram, frame_cap + 1);
    // The borrowed-span path (and the exhaustion drain) needs that scratch buffer; it is
    // allocated LAZILY on first use, so a steady-state owning-delivery node (view receiver
    // installed, backend healthy) never pays for it — and the recv thread never carries a
    // 64 KiB frame on its stack (FreeRTOS/pthread stacks are a few KiB; a stack array here
    // would overflow them on-target).
    std::unique_ptr<std::byte[]> scratch;
    const auto scratch_buf = [&scratch, scratch_cap]() -> std::byte* {
        if (!scratch) scratch = std::make_unique<std::byte[]>(scratch_cap);
        return scratch.get();
    };
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
            if (!rx_seg) rx_seg = view::segment_ptr_t::adopt(backend_->alloc(alloc_cap));
            sockaddr_in from{};
            socklen_t flen = sizeof(from);
            std::byte* const dst = rx_seg ? rx_seg->bytes.data() : scratch_buf();
            const std::size_t cap = rx_seg ? rx_seg->bytes.size() : scratch_cap;
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
            if (static_cast<std::size_t>(n) > frame_cap) {
                // Over the configured cap: refuse it. The segment is KEPT (it is still a
                // clean buffer for the next datagram), so a peer spraying over-cap
                // datagrams costs no allocator traffic at all.
                malformed_rx_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Narrow the whole-segment view to the received length and hand it up
            // owning — the receiver may pin/subview it beyond this call. Built by
            // aggregate init, NOT over().subview(): `subview` is const and copies the
            // handle, so the discarded whole-segment temporary would hold a second
            // reference for the whole delivery call — a refcount round-trip per
            // datagram, and a library reference live across the receiver callback (#845).
            rx_.deliver(view::view_t{std::move(rx_seg), 0, static_cast<std::size_t>(n)});
            continue;
        }

        // Span path: recvfrom into the borrowed scratch (no owning segment committed).
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        std::byte* const buf = scratch_buf();
        const ssize_t n =
            ::recvfrom(fd_, buf, scratch_cap, 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0) continue;  // timeout / EAGAIN / error → re-check stop_
        // Listener mode: the latest datagram's source IS the peer (single-peer
        // UDP-server shape) — replies/sends target it from now on.
        if (learn_peer_)
            peer_.store(pack_peer(from.sin_addr.s_addr, ntohs(from.sin_port)),
                        std::memory_order_relaxed);
        // The configured cap applies to the borrowed path too — the sink shape is the
        // node's business, the admitted datagram size is the peer's.
        if (static_cast<std::size_t>(n) > frame_cap) {
            malformed_rx_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // A rope sink may have been installed while we were blocked in recvfrom
        // above with the span decision already made — re-check and, if so, deliver this
        // datagram owning via a one-time copy into a backend segment (race-window
        // datagrams only; every subsequent datagram takes the zero-copy path above).
        if (rx_.has_rope()) {
            view::segment_ptr_t seg = view::segment_ptr_t::adopt(backend_->alloc(alloc_cap));
            if (!seg || static_cast<std::size_t>(n) > seg->bytes.size()) {
                dropped_rx_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::memcpy(seg->bytes.data(), buf, static_cast<std::size_t>(n));
            rx_.deliver(view::view_t{std::move(seg), 0, static_cast<std::size_t>(n)});
            continue;
        }
        rx_.deliver_borrowed(std::span<const std::byte>(buf, static_cast<std::size_t>(n)));
    }
}

}  // namespace tr::net
