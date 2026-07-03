/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_udp — a real UDP socket transport_t (M5). The first transport that
 * actually crosses the kernel network stack; the bridge / router / graph above
 * it are unchanged. One datagram carries one whole frame (no stream reassembly),
 * so it pairs with the flat decoder. POSIX sockets; a receive thread drains the
 * socket into the registered receiver. Datagram payloads are bounded by the UDP
 * limit (~65507 bytes) — large TLVs need a streaming transport (future).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include "libtracer/mem_heap.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

class udp_transport_t : public transport_t {
   public:
    /** @brief The largest datagram one frame can occupy — the RX segment size a view
     *         receiver's frames are allocated at (the UDP payload bound, one datagram
     *         = one frame = one segment, ADR-0042 §2). */
    static constexpr std::size_t kMaxDatagram = 65536;

    // Bind a local UDP socket on `bind_port` (0 = ephemeral; see local_port());
    // send() targets `peer_host:peer_port` (IPv4 dotted-quad, e.g. "127.0.0.1").
    // Listener mode: constructed with an unresolved peer (`peer_host` empty or
    // `peer_port` 0), the transport LEARNS its peer from each inbound datagram's
    // source address — the standard single-peer UDP-server shape, which lets a
    // config-created `listener` connection (#83) reply to a dialing client whose
    // ephemeral port is unknowable in advance. Until the first datagram arrives,
    // send() is a no-op (there is nobody to send to).
    //
    // `backend` is the host-injected RX memory seam (ADR-0042 §2): when a view
    // receiver is installed, each datagram is recvfrom'd straight into a fresh
    // segment drawn from it, sized min(kMaxDatagram, backend->max_segment_size())
    // — the injected backend BOUNDS the datagram a node accepts, so a pool over a
    // static MCU slab (slot payload ≪ 64 KiB) works as-is (default: the process
    // heap, which reports unbounded and keeps the full cap). Exhaustion (`alloc`
    // == nullptr) is backpressure — the datagram is dropped and dropped_rx()
    // ticks, never an OOM. Without a view receiver the backend is untouched
    // (span path unchanged). Must outlive the transport.
    udp_transport_t(std::uint16_t bind_port, const std::string& peer_host, std::uint16_t peer_port,
                    mem::mem_backend_t* backend = &mem::heap_backend());
    ~udp_transport_t() override;

    udp_transport_t(const udp_transport_t&) = delete;
    udp_transport_t& operator=(const udp_transport_t&) = delete;

    void send(std::span<const std::byte> frame) override;                 // sendto(peer)
    void send(std::span<const std::span<const std::byte>> iov) override;  // sendmsg(iovec)
    void set_receiver(receiver_t receiver) override;

    /**
     * @brief Install the owning inbound sink (ADR-0042): one datagram = one frame =
     *        one refcounted segment from the injected backend, handed up as a view.
     *
     * Set before frames flow (the @ref set_receiver contract); fires on the recv
     * thread. When installed it takes precedence over the span receiver for every
     * subsequent datagram; when absent, delivery is the borrowed-span path,
     * byte-identical to a backend-less transport.
     */
    void set_view_receiver(view_receiver_t receiver) override;

    /** @brief True — this transport honors @ref set_view_receiver (ADR-0042 §2). */
    [[nodiscard]] bool delivers_views() const override { return true; }

    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }
    /** @brief Datagrams dropped because the RX backend was exhausted (backpressure,
     *         ADR-0039 §4 / ADR-0042 §2) — never an OOM. */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept {
        return dropped_rx_.load(std::memory_order_relaxed);
    }

   private:
    void run();  // receive thread

    // The peer endpoint packed as (ip << 16) | port (ip network order, port host
    // order), atomic because listener mode updates it from the recv thread while
    // send() reads it from callers' threads. 0 = no peer yet (send is a no-op).
    [[nodiscard]] std::uint64_t peer() const noexcept {
        return peer_.load(std::memory_order_relaxed);
    }

    int fd_ = -1;
    std::uint16_t bound_port_ = 0;
    std::atomic<std::uint64_t> peer_{0};
    bool learn_peer_ = false;  // constructed peer-less => adopt each datagram's source

    // RX segment source for view delivery (ADR-0042 §2) + backend-exhaustion
    // drop counter (backpressure, never OOM).
    mem::mem_backend_t* backend_;
    std::atomic<std::uint64_t> dropped_rx_{0};

    receiver_t receiver_;            // guarded by m_
    view_receiver_t view_receiver_;  // guarded by m_; installed => owning delivery path
    std::mutex m_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace tr::net
