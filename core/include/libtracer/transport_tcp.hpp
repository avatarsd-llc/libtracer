/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_tcp — a reliable stream transport_t (M6). TCP is a byte stream with
 * no message boundaries, so each libtracer frame rides behind a 4-byte u32
 * little-endian LENGTH PREFIX. The prefix is TRANSPORT framing, not part of the
 * TLV: a TLV's own length field cannot delimit a stream read without peeking at
 * (and buffering across) a variable-width header, while a fixed prefix keeps the
 * reader trivial — read 4 bytes, then read exactly `len` bytes — and robust
 * against partial reads at every boundary. Frames above kMaxFrame are malformed
 * (a corrupt or hostile prefix): counted and the connection is torn down, since a
 * stream that has lost framing sync cannot be resynchronized.
 *
 * Two modes, one class: DIAL (connect out to host:port, synchronously at
 * construction) and LISTEN (accept ONE inbound peer at a time — the same
 * one-peer model as transport_ws_server, re-accepting after a peer departs).
 * POSIX sockets; a receive thread reassembles each frame and delivers it. Per
 * ADR-0042 §4 a stream frame is reassembled into ONE contiguous segment (the
 * rope overload arrives with rope-aware decode, not before). Reconnect is out of
 * scope — link-down is a torn connection reported via ok()/link state (#66 owns
 * lifecycle).
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
 * @brief A TCP stream transport_t (M6) — length-prefix framing over one peer.
 *
 * Every frame is sent as `u32-LE length ++ frame bytes`; the receive thread
 * reads the prefix (reassembling it across TCP segment boundaries), then reads
 * exactly `len` bytes straight into a refcounted segment drawn from the injected
 * `mem_backend_t` (ADR-0042 §2 — no library buffer beyond the per-frame
 * segment). With a view receiver installed the frame is handed up OWNING; the
 * span receiver otherwise gets a borrowed span over the same segment bytes.
 */
class tcp_transport_t : public transport_t {
   public:
    /** @brief The largest frame the length prefix may announce (16 MiB). A larger
     *         prefix is malformed — counted via @ref malformed_rx and the
     *         connection is closed (a desynced stream cannot be trusted again). */
    static constexpr std::size_t kMaxFrame = 16u * 1024u * 1024u;

    /**
     * @brief DIAL mode: connect to @p peer_host:@p peer_port (synchronous).
     *
     * The TCP connect happens in the constructor (the transport_ws_client
     * shape) — confirm with ok(); on failure no thread is spawned. On success
     * the receive thread starts immediately, so receivers must be installed
     * before frames flow (the set_receiver contract).
     *
     * @param peer_host Dotted-quad IPv4 address of the peer (e.g. "127.0.0.1").
     * @param peer_port TCP port of the peer (host byte order).
     * @param backend   The host-injected RX memory seam (ADR-0042 §2): each
     *                  inbound frame is read into a fresh exactly-`len`-byte
     *                  segment from it (default: the process heap; a bounded
     *                  host passes its pool). Exhaustion is backpressure — the
     *                  frame is drained off the stream, dropped, and
     *                  dropped_rx() ticks; never an OOM. Must outlive the
     *                  transport.
     */
    tcp_transport_t(const std::string& peer_host, std::uint16_t peer_port,
                    mem::mem_backend_t* backend = &mem::heap_backend(), std::size_t max_frame = 0);

    /**
     * @brief LISTEN mode: bind+listen on @p bind_port, accept ONE inbound peer.
     *
     * The same one-peer model as transport_ws_server: one connected client at a
     * time; after a peer departs the accept loop resumes for the next. Use ok()
     * to confirm the listen socket bound; the bound port (an ephemeral 0
     * request resolved) is observable via local_port().
     *
     * @param bind_port TCP port to listen on (host byte order; 0 → ephemeral).
     * @param backend   The RX memory seam — see the DIAL constructor.
     */
    explicit tcp_transport_t(std::uint16_t bind_port,
                             mem::mem_backend_t* backend = &mem::heap_backend(),
                             std::size_t max_frame = 0);

    /** @brief Stop the receive thread and close all sockets. */
    ~tcp_transport_t() override;

    tcp_transport_t(const tcp_transport_t&) = delete;
    tcp_transport_t& operator=(const tcp_transport_t&) = delete;

    /**
     * @brief Send @p frame as one length-prefixed record on the stream.
     *
     * Writes `u32-LE frame.size()` then the frame bytes — one writev, partial
     * writes resumed until complete. No-op until a peer is connected (and after
     * the connection is torn down). Thread-safe (writes are serialized, so two
     * senders can never interleave records on the stream).
     *
     * @param frame A complete TLV's bytes.
     */
    void send(std::span<const std::byte> frame) override;

    /**
     * @brief Scatter-gather send: the prefix + every span as ONE record, no
     *        gather copy — the length prefix rides as the first iovec entry and
     *        the rope's spans follow, lowered to writev (partials resumed).
     *
     * @param iov The frame's spans (a rope's `to_iovec()`), concatenated on the
     *            wire as one length-prefixed frame.
     */
    void send(std::span<const std::span<const std::byte>> iov) override;

    void set_receiver(receiver_t receiver) override;

    /**
     * @brief Install the owning inbound sink (ADR-0042): one frame = one
     *        refcounted segment from the injected backend, handed up as a view.
     *
     * Set before frames flow (the @ref set_receiver contract); fires on the
     * receive thread. When installed it takes precedence over the span receiver;
     * when absent, the span receiver gets a borrowed span over the same segment
     * bytes (the segment is released when the callback returns).
     */
    void set_rope_receiver(rope_receiver_t receiver) override;

    /** @brief True — this transport honors @ref set_rope_receiver (ADR-0042). */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief DIAL: the connect succeeded; LISTEN: the listen socket is bound. */
    [[nodiscard]] bool ok() const noexcept {
        return listen_ ? listen_fd_ >= 0 : conn_fd_.load(std::memory_order_relaxed) >= 0;
    }

    /** @brief LISTEN mode: the actual bound TCP port (resolves an ephemeral 0). */
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }

    /** @brief Frames dropped because the RX backend was exhausted (backpressure,
     *         ADR-0039 §4 / ADR-0042 §2) — drained off the stream, never an OOM. */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept {
        return dropped_rx_.load(std::memory_order_relaxed);
    }

    /** @brief Malformed length prefixes seen (announced length > @ref kMaxFrame).
     *         Each one tears the connection down — the stream has lost framing sync. */
    [[nodiscard]] std::uint64_t malformed_rx() const noexcept {
        return malformed_rx_.load(std::memory_order_relaxed);
    }

   private:
    void run_listen();   // accept loop (LISTEN mode)
    void serve(int fd);  // per-connection frame reassembly loop
    bool read_exact(int fd, std::byte* dst, std::size_t len);  // partial-read reassembly
    bool drain(int fd, std::size_t len);                       // discard len bytes (backpressure)

    bool listen_ = false;
    int listen_fd_ = -1;  // LISTEN mode only
    std::uint16_t bound_port_ = 0;

    // RX segment source for frame reassembly (ADR-0042 §2) + drop counters.
    mem::mem_backend_t* backend_;
    std::size_t max_frame_ = kMaxFrame;  // per-connection receive cap (:settings; 0 => kMaxFrame)
    std::atomic<std::uint64_t> dropped_rx_{0};
    std::atomic<std::uint64_t> malformed_rx_{0};

    receiver_t receiver_;            // guarded by m_
    rope_receiver_t rope_receiver_;  // guarded by m_; installed => owning delivery
    std::mutex m_;                   // guards the receivers
    std::mutex write_m_;             // serializes writes to conn_fd_
    std::atomic<int> conn_fd_{-1};   // the live peer connection (-1 = none)
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace tr::net
