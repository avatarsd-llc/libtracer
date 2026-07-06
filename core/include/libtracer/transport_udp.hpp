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

/**
 * @brief A single-peer UDP datagram @ref transport_t (one datagram = one frame).
 *
 * Binds a local UDP socket and sends to one peer; a listener-mode instance learns its peer
 * from the first inbound datagram's source address. Supports the owning rope-receiver seam
 * (ADR-0042 §2): each datagram is received straight into a refcounted segment from a
 * host-injected `mem_backend_t`, which also bounds the datagram size a node accepts.
 */
class udp_transport_t : public transport_t {
   public:
    /** @brief The largest datagram one frame can occupy — the RX segment size a view
     *         receiver's frames are allocated at (the UDP payload bound, one datagram
     *         = one frame = one segment, ADR-0042 §2). */
    static constexpr std::size_t kMaxDatagram = 65536;

    /**
     * @brief Bind a local UDP socket on @p bind_port and target @p peer_host : @p peer_port.
     *
     * @p bind_port 0 = ephemeral (see @ref local_port). @p peer_host is an IPv4 dotted-quad
     * (e.g. "127.0.0.1"). Listener mode: with an unresolved peer (@p peer_host empty or
     * @p peer_port 0) the transport LEARNS its peer from each inbound datagram's source
     * address — the single-peer UDP-server shape that lets a config-created `listener`
     * connection (#83) reply to a dialing client whose ephemeral port is unknowable in
     * advance; until the first datagram arrives, @ref send is a no-op.
     *
     * @p backend is the host-injected RX memory seam (ADR-0042 §2): when a rope receiver is
     * installed, each datagram is recvfrom'd straight into a fresh segment drawn from it,
     * sized `min(kMaxDatagram, backend->max_segment_size())` — so the backend BOUNDS the
     * datagram a node accepts (a pool over a static MCU slab works as-is; default is the
     * process heap, unbounded, keeping the full cap). Exhaustion is backpressure — the
     * datagram is dropped and @ref dropped_rx ticks, never an OOM. Must outlive the transport.
     */
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
    void set_rope_receiver(rope_receiver_t receiver) override;

    /** @brief True — this transport honors @ref set_rope_receiver (ADR-0042 §2). */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief True iff the socket bound successfully. */
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }
    /** @brief The bound local port (resolves an ephemeral @c bind_port of 0). */
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
    rope_receiver_t rope_receiver_;  // guarded by m_; installed => owning delivery path
    std::mutex m_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace tr::net
