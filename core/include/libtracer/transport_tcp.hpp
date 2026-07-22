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
 * Three shapes: tcp_transport_t DIAL (connect out to host:port, synchronously
 * at construction), tcp_transport_t LISTEN (accept ONE inbound peer at a time,
 * re-accepting after a peer departs), and transport_tcp_server (the MULTI-peer
 * listener — the transport_ws_server slot/poll machinery over raw length-prefix
 * framing, with the ADR-0044 bus_link_t facet; the `kind=tcp` listener factory
 * builds this one). POSIX sockets; a receive thread reassembles each frame and
 * delivers it. Per ADR-0042 §4 a stream frame is reassembled into ONE contiguous
 * segment (the rope overload arrives with rope-aware decode, not before).
 * Reconnect is out of scope — link-down is a torn connection reported via
 * ok()/link state (#66 owns lifecycle).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/length_prefix_framer.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/posix_endpoint.hpp"
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
class tcp_transport_t : public transport_t, private stream_endpoint_t {
   public:
    /** @brief The largest frame the length prefix may announce — the shared
     *         length_prefix_framer::kDefaultMaxFrame (16 MiB) unless `:settings
     *         max_frame` tightens it. A larger prefix is malformed — counted via
     *         @ref malformed_rx and the connection is closed (a desynced stream
     *         cannot be trusted again). */
    static constexpr std::size_t kMaxFrame = length_prefix_framer::kDefaultMaxFrame;

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

    /** @brief True — this transport honors @ref set_rope_receiver (ADR-0042):
     *         one frame = one refcounted segment from the injected backend,
     *         handed up owning; a span-only sink gets the same bytes borrowed. */
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
    // conn_fd_ + write_m_ (and their teardown discipline) live in stream_endpoint_t.
};

/**
 * @brief A multi-peer TCP server transport_t — accepts many inbound peers on
 *        one listener and exposes them through the @ref bus_link_t facet
 *        (ADR-0044).
 *
 * The raw-stream sibling of transport_ws_server (#362): the same slot/poll
 * machinery — ONE poll-based thread accepts clients and serves every open
 * connection concurrently; peers occupy SLOTS recycled on departure, so
 * steady-state memory is bounded by the maximum concurrent peers ever reached
 * (or @p max_peers, the RFC-0006 injected bound) — with the WS protocol layer
 * replaced by the shared u32-LE length-prefix stream framing (one chunk-fed
 * length_prefix_framer per slot).  There is NO handshake phase: a peer is open
 * (named `<ip>:<port>`) from the moment its connection is accepted.  The
 * board↔board listener shape: leaner than WS packaging (no HTTP upgrade, no
 * frame masking) with the same per-peer return-route identity when
 * @p peer_named.
 */
class transport_tcp_server : public transport_t, public bus_link_t, private stream_endpoint_t {
   public:
    /**
     * @brief Bind+listen on @p bind_port (0 = ephemeral; see local_port()).
     *
     * Spawns the poll/serve thread immediately.  Use ok() to confirm the
     * listen socket bound; the bound port is observable via local_port().
     *
     * @param bind_port  TCP port to listen on (host byte order; 0 → ephemeral).
     * @param backend    The host-injected RX memory seam (ADR-0042 §2): each
     *                   inbound frame reassembles into a fresh exactly-len-byte
     *                   segment from it.  Exhaustion is backpressure — the
     *                   frame is drained in-framer, dropped, and dropped_rx()
     *                   ticks; never an OOM.  Must outlive the transport.
     * @param max_frame  Per-connection receive cap (0 → @ref
     *                   tcp_transport_t::kMaxFrame); the effective cap also
     *                   honors the backend's real capacity
     *                   (length_prefix_framer::effective_cap — the
     *                   no-synthetic-limits doctrine).
     * @param max_peers  Concurrent-peer admission cap; 0 = unbounded (host
     *                   default).  A deployment-injected bound (RFC-0006) — a
     *                   connection beyond it is accepted and immediately
     *                   closed (a clean refusal, not a hung SYN).
     * @param peer_named Expose the @ref bus_link_t facet (see @ref bus) — the
     *                   board↔board wiring choice, same contract as
     *                   transport_ws_server's.
     */
    explicit transport_tcp_server(std::uint16_t bind_port,
                                  mem::mem_backend_t* backend = &mem::heap_backend(),
                                  std::size_t max_frame = 0, std::size_t max_peers = 0,
                                  bool peer_named = false);

    /** @brief Stop the poll thread and close all sockets. */
    ~transport_tcp_server() override;

    transport_tcp_server(const transport_tcp_server&) = delete;
    transport_tcp_server& operator=(const transport_tcp_server&) = delete;

    /**
     * @brief Send @p frame as one length-prefixed record to EVERY open peer
     *        (the flat point-to-point surface).
     *
     * The prefix is encoded once; each peer gets one serialized gathered
     * write.  No-op until a peer is connected.  Thread-safe (peers_m_ →
     * write_m_, the header lock order).  A directed single-peer send is
     * `peer_link(name)->send(frame)`.
     *
     * @param frame A complete TLV's bytes.
     */
    void send(std::span<const std::byte> frame) override;

    /**
     * @brief Zero-copy scatter-gather broadcast: the length prefix rides as
     *        the first iovec entry and the rope's spans follow — ONE gathered
     *        record per open peer, no flatten copy.
     *
     * Each peer writes from a fresh copy of the iovec array (the write
     * consumes it).  Thread-safe (peers_m_ → write_m_).
     *
     * @param iov The spans to emit, in order, as a single record.
     */
    void send(std::span<const std::span<const std::byte>> iov) override;

    /** @brief True — every frame reassembles into ONE refcounted segment from
     *         the injected backend, handed up owning (ADR-0042); a span-only
     *         sink gets the same bytes borrowed.  Covers both facets. */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief The @ref bus_link_t facet (ADR-0044) when constructed
     *         `peer_named`, else `nullptr` — the transport_ws_server contract
     *         verbatim, including the RFC-0009 §D.5 departure-eviction split
     *         (peer-named evicts the departed peer's edges; FLAT mode reports
     *         the whole link down on any session's close). */
    [[nodiscard]] bus_link_t* bus() override { return peer_named_ ? this : nullptr; }

    /** @brief Visit the currently-OPEN peers' names, `<ip>:<port>`. */
    void enumerate_peers(const peer_visitor_t& visit) const override;

    /**
     * @brief Resolve an open peer's name to its directed sending endpoint.
     *
     * Owned by the peer's slot; pointer-valid for this server's lifetime
     * (slots are never freed, only recycled).  After the peer departs its
     * sends no-op until the slot is reused.
     * @retval nullptr @p peer names no currently-open connection.
     */
    [[nodiscard]] transport_t* peer_link(std::string_view peer) override;

    /**
     * @brief Close the open peer named @p peer, freeing its slot for reuse.
     *
     * Shuts the socket down (`SHUT_RDWR`); the poll thread observes the close
     * and runs the SAME teardown as a remote hangup (recycle is asynchronous,
     * within one poll bound) — never touching poll-thread-only state here.
     * @retval true  @p peer named an open connection and was shut down.
     * @retval false @p peer names no currently-open connection.
     */
    [[nodiscard]] bool close_peer(std::string_view peer) override;

    /** @brief True if the listen socket is bound and listening. */
    [[nodiscard]] bool ok() const noexcept { return listen_fd_ >= 0; }

    /** @brief The actual bound TCP port (resolves an ephemeral 0 request). */
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }

    /** @brief Frames dropped to RX-backend exhaustion (backpressure), summed
     *         over all peers. */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept {
        return dropped_rx_.load(std::memory_order_relaxed);
    }

    /** @brief Malformed length prefixes seen (announced length above the
     *         effective cap).  Each one tears that peer's connection down. */
    [[nodiscard]] std::uint64_t malformed_rx() const noexcept {
        return malformed_rx_.load(std::memory_order_relaxed);
    }

   private:
    struct session_t;  // one peer slot's connection state (defined in the .cpp)

    /**
     * @brief The directed per-peer sending endpoint @ref peer_link hands out:
     *        `send()` writes one length-prefixed record to that peer's socket
     *        only.  Ingress stays on the owning server's peer-named slot.
     */
    class peer_endpoint_t final : public transport_t {
       public:
        /** @brief Send @p frame to this facade's peer only (no-op once departed). */
        void send(std::span<const std::byte> frame) override;

        /** @brief Directed scatter-gather twin: prefix + spans as ONE gathered
         *         record to this facade's peer only (no pristine copy — single
         *         consumer).  No-op once departed. */
        void send(std::span<const std::span<const std::byte>> iov) override;

       private:
        friend class transport_tcp_server;
        transport_tcp_server* owner_ = nullptr; /**< @brief The owning server. */
        session_t* slot_ = nullptr;             /**< @brief The peer slot this sends to. */
    };

    void run();                        // the ONE poll/accept/serve thread
    void accept_peer();                // admit into a free (or new) slot
    void service_peer(session_t& s);   // one readable pass: recv + framer feed
    void teardown_slot(session_t& s);  // close + free the slot for reuse

    int listen_fd_ = -1;
    std::uint16_t bound_port_ = 0;
    mem::mem_backend_t* backend_;
    std::size_t max_frame_ = tcp_transport_t::kMaxFrame;
    std::size_t max_peers_ = 0;  // 0 = unbounded (deployment-injected, RFC-0006)
    bool peer_named_ = false;    // expose bus() — wiring-time deployment choice
    std::atomic<std::uint64_t> dropped_rx_{0};
    std::atomic<std::uint64_t> malformed_rx_{0};
    /**
     * @brief Guards the slot vector and every slot's NAME (cross-thread reads:
     *        enumerate_peers / peer_link vs the poll thread's accept/teardown).
     *        Per-slot fds are atomics read under `write_m_` by senders; each
     *        slot's framer is poll-thread-only.  Lock order where nested:
     *        peers_m_ → write_m_.
     */
    mutable std::mutex peers_m_;
    std::vector<std::unique_ptr<session_t>> slots_;  // insert-only; recycled in place
    // write_m_ (stream_endpoint_t) serializes ALL socket writes (any peer);
    // conn_fd_ is unused by the multi-peer server (each slot owns its fd).
};

}  // namespace tr::net
