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
 * listener — the shared slot_server_t slot/poll machinery (`%posix_endpoint.hpp`,
 * one home with transport_ws_server since #871) over raw length-prefix framing,
 * with the ADR-0044 bus_link_t facet; the `kind=tcp` listener factory builds
 * this one). POSIX sockets; a receive thread reassembles each frame and
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

namespace detail {

/**
 * @brief TEST SEAM: run by the shared accept path (`slot_server_t::accept_peer`, through
 *        this server's `on_slot_publishing` override) at the exact instant a new
 *        peer's fd has been published and its slot is ONE store away from open — inside the
 *        `write_m_` hold that makes the two stores atomic to senders.
 *
 * A slot's two sender-visible fields are published under `write_m_`, fd first, so
 * "open ⇒ fd valid" holds for every sender: a broadcast holding that lock runs either
 * entirely before the slot goes live or entirely after it. Publish them unlocked with `open`
 * first and a broadcast can read `open == true` alongside `fd == -1` and write the frame to a
 * closed descriptor. That is not observable from the outside by racing: the window is two
 * instructions wide. This hook is where a test HOLDS the mid-publish instant open, broadcasts
 * into it, and checks the frame arrives at the peer being accepted; with the stores unlocked
 * and reordered the identical test reads an empty socket. Null in production — the cost is
 * one predictable null-check, once per accepted connection, on the (cold) accept path.
 *
 * Same shape and same rules as `ws_peer_published_hook`: install it before the peer that
 * should trip it connects, and clear it before the test returns.
 */
inline void (*tcp_peer_publishing_hook)() = nullptr;

}  // namespace detail

/**
 * @brief Suggested module name for a DIAL `tcp` connection (ADR-0073 §4).
 *
 * A *suggestion*, never a registration: the library auto-registers no module names — the
 * application passes this (or any name it prefers) to
 * `transport_vertex_t::register_module("tcp-client", "tcp", conn_role_t::DIAL)`.
 */
inline constexpr std::string_view kTcpClientSuggestedModule = "tcp-client";

/** @brief Suggested module name for a LISTEN `tcp` connection (see
 *         `kTcpClientSuggestedModule`; the application registers it, or any name it
 *         prefers). */
inline constexpr std::string_view kTcpServerSuggestedModule = "tcp-server";

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
     * shape) — confirm with ok(); on failure no thread is spawned. By default
     * the receive thread starts immediately, so receivers must be installed
     * before frames flow (the set_receiver contract); @p defer_recv is what
     * makes that contract satisfiable on this socket at all.
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
     * @param recv_stack Recv-thread stack size in bytes, 0 = platform default
     *                  (`posix_endpoint_t::start`). Non-zero right-sizes this
     *                  transport's recv thread on an MCU.
     * @param defer_recv Two-phase bring-up (#1045, the transport_ws_client
     *                  contract verbatim): with `true` the connect still runs
     *                  HERE (so ok() answers for it on return) but the recv
     *                  thread is NOT spawned — not one byte is read off the
     *                  socket until @ref start_receiving. That is the ordering in
     *                  which the set_receiver contract above is satisfiable on a
     *                  DIAL socket: a peer that pushes the instant our connect
     *                  completes has its first frame in flight before this
     *                  constructor returns, and the default (`false`, the
     *                  historical shape) decodes it on the recv thread into
     *                  whatever sink is installed by then — possibly none, in
     *                  which case it is dropped with no counter moving.
     */
    tcp_transport_t(const std::string& peer_host, std::uint16_t peer_port,
                    mem::mem_backend_t* backend = &mem::heap_backend(), std::size_t max_frame = 0,
                    std::size_t recv_stack = 0, bool defer_recv = false);

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
     * @param recv_stack Recv-thread stack size in bytes, 0 = platform default
     *                  (`posix_endpoint_t::start`).
     */
    explicit tcp_transport_t(std::uint16_t bind_port,
                             mem::mem_backend_t* backend = &mem::heap_backend(),
                             std::size_t max_frame = 0, std::size_t recv_stack = 0);

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

    /**
     * @brief Spawn the recv thread a `defer_recv` DIAL construction held back (#1045).
     *
     * The second phase of the two-phase bring-up: the socket is connected and NOTHING has
     * been read off it, so a sink installed before this call cannot have missed a frame.
     * From here the link behaves exactly as a one-phase one.
     *
     * IDEMPOTENT, and a no-op wherever there is nothing to arm — a one-phase DIAL link
     * (its thread is already running), a LISTEN link (its accept loop started in the
     * constructor), and a link whose dial failed (`ok()` false, no socket to serve) — so an
     * owner may call it unconditionally on every link it wires, which is what
     * `%transport_vertex_t::make_connection` does. A `defer_recv` link that is never armed
     * never receives and never reports the link down; it is simply an open socket until it
     * is destroyed.
     */
    void start_receiving() override;

    /** @brief True — this transport honors @ref set_rope_receiver (ADR-0042):
     *         one frame = one refcounted segment from the injected backend,
     *         handed up owning; a span-only sink gets the same bytes borrowed. */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief The came-up predicate (#1059) — DIAL: the connect succeeded; LISTEN: the
     *         listen socket is bound. Answered at construction and never reverting (this
     *         accessor used to read the live fd on a DIAL link, i.e. it doubled as
     *         liveness; that is @ref link_up now). */
    [[nodiscard]] bool ok() const noexcept { return listen_ ? listen_fd_ >= 0 : came_up_; }

    /** @brief Liveness (the @ref transport_t::link_up contract): true while the ONE
     *         connection is live — DIAL: from the connect until the recv loop's teardown;
     *         LISTEN: while an accepted peer is connected (false between peers). Derived
     *         from the connection fd, which the teardown path already resets under the
     *         write lock — state that is already atomic (relaxed; a hint, not a
     *         synchronisation point). */
    [[nodiscard]] bool link_up() const noexcept override {
        return conn_fd_.load(std::memory_order_relaxed) >= 0;
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

    /** @brief Frames shed on the way OUT (#932): a record over @ref kMaxFrame, a refused
     *         gather store, or no live peer to write to (dialing / torn down). */
    [[nodiscard]] std::uint64_t dropped_tx() const noexcept {
        return dropped_tx_.load(std::memory_order_relaxed);
    }

    /** @brief The interface-level snapshot (#932) — the concrete accessors above, as the
     *         one shape a generic `transport_t*` holder reads. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx(), malformed_rx(), dropped_tx()};
    }

   private:
    void run_listen();   // accept loop (LISTEN mode)
    void serve(int fd);  // per-connection frame reassembly loop
    bool read_exact(int fd, std::byte* dst, std::size_t len);  // partial-read reassembly
    bool drain(int fd, std::size_t len);                       // discard len bytes (backpressure)

    bool listen_ = false;
    int listen_fd_ = -1;  // LISTEN mode only
    std::uint16_t bound_port_ = 0;
    /** @brief The came-up fact `ok()` reports on a DIAL link (#1059): written once, in
     *         the constructor, before any thread this object owns exists — a plain bool
     *         is race-free here. */
    bool came_up_ = false;

    // RX segment source for frame reassembly (ADR-0042 §2) + drop counters.
    mem::mem_backend_t* backend_;
    std::size_t max_frame_ = kMaxFrame;  // per-connection receive cap (:settings; 0 => kMaxFrame)
    std::atomic<std::uint64_t> dropped_rx_{0};
    std::atomic<std::uint64_t> malformed_rx_{0};
    std::atomic<std::uint64_t> dropped_tx_{0};
    std::size_t recv_stack_ = 0; /**< @brief The stack hint, held for @ref start_receiving. */
    /** @brief One-shot latch making @ref start_receiving idempotent — `start()` may be
     *         called at most once per endpoint. DIAL only; a LISTEN link spends its one
     *         `start` on the accept loop and never enters the latched path at all. */
    std::atomic<bool> recv_started_{false};
    // conn_fd_ + write_m_ (and their teardown discipline) live in stream_endpoint_t.
};

/**
 * @brief A multi-peer TCP server transport_t — accepts many inbound peers on
 *        one listener and exposes them through the @ref bus_link_t facet
 *        (ADR-0044).
 *
 * The raw-stream sibling of transport_ws_server (#362): LITERALLY the same
 * slot/poll machinery, since #871 shared as @ref slot_server_t — ONE
 * poll-based thread accepts clients and serves every open connection
 * concurrently; peers occupy SLOTS recycled on departure, so steady-state
 * memory is bounded by the maximum concurrent peers ever reached (or @p
 * max_peers, the RFC-0006 injected bound).  What this class adds to that base
 * is its FRAMING: the shared u32-LE length prefix (one chunk-fed
 * length_prefix_framer per slot) where WS has RFC 6455 packaging.  There is NO
 * handshake phase: a peer is open (named `p<slot>`, ADR-0073 §2 / #426) from
 * the moment its connection is accepted.  The board↔board listener shape:
 * leaner than WS packaging (no HTTP upgrade, no frame masking) with the same
 * per-peer return-route identity when @p peer_named.
 */
class transport_tcp_server : public slot_server_t {
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
     *                   tcp_transport_t::kMaxFrame). TIGHTEN-ONLY: a value
     *                   above kMaxFrame is clamped to it
     *                   (`length_prefix_framer::configured_cap`, #1035) — a
     *                   config-writable key must not raise the ingress
     *                   buffering bound. A frame inside this cap that the
     *                   backend cannot hold (`length_prefix_framer::effective_cap`
     *                   — the no-synthetic-limits doctrine) is shed as
     *                   backpressure, NOT treated as malformed (#932); only a
     *                   length above this cap closes the connection.
     * @param max_peers  Concurrent-peer admission cap; 0 = unbounded (host
     *                   default).  A deployment-injected bound (RFC-0006) — a
     *                   connection beyond it is accepted and immediately
     *                   closed (a clean refusal, not a hung SYN).
     * @param peer_named Expose the @ref bus_link_t facet (see @ref bus) — the
     *                   board↔board wiring choice, same contract as
     *                   transport_ws_server's.
     * @param recv_stack Poll-thread stack size in bytes, 0 = platform default
     *                   (`posix_endpoint_t::start`). One thread serves every
     *                   peer, so this is the whole server's recv-stack knob.
     */
    explicit transport_tcp_server(std::uint16_t bind_port,
                                  mem::mem_backend_t* backend = &mem::heap_backend(),
                                  std::size_t max_frame = 0, std::size_t max_peers = 0,
                                  bool peer_named = false, std::size_t recv_stack = 0);

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

    /** @brief Frames shed on the way OUT (#932), summed over the whole link: a record
     *         over the frame cap, a refused gather store, or no open peer to fan to. */
    [[nodiscard]] std::uint64_t dropped_tx() const noexcept {
        return dropped_tx_.load(std::memory_order_relaxed);
    }

    /** @brief The interface-level snapshot (#932) — what a generic `transport_t*` reads. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx(), malformed_rx(), dropped_tx()};
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
         *         record to this facade's peer only.  No-op once departed. */
        void send(std::span<const std::span<const std::byte>> iov) override;

        /** @brief The owning server's snapshot (#932): a directed facade counts into the
         *         link's own counters, so it reports them rather than a fabricated zero. */
        [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
            return owner_ == nullptr ? transport_drop_stats_t{} : owner_->drop_stats();
        }

       private:
        friend class transport_tcp_server;
        transport_tcp_server* owner_ = nullptr; /**< @brief The owning server. */
        session_t* slot_ = nullptr;             /**< @brief The peer slot this sends to. */
    };

    /** @brief One fresh slot with its length-prefix framer and directed facade. */
    std::unique_ptr<session_base_t> make_session() override;

    /** @brief Per-accept setup: TCP_NODELAY + a reset framer.  Returns true — a raw
     *         stream peer has no handshake, so it is open the moment it is accepted. */
    bool on_accept(session_base_t& s, int fd) override;

    /** @brief Framing: feed the chunk through the slot's u32-LE reassembler and deliver
     *         each completed frame (tearing the slot down on a desynced stream). */
    void on_readable(session_base_t& s, const std::byte* data, std::size_t len) override;

    /** @brief Teardown: reset the slot's framer as it is recycled. */
    void on_slot_reset(session_base_t& s) override;

    /** @brief Fire `detail::tcp_peer_publishing_hook` inside the accept-side `write_m_`
     *         hold — the mid-publish instant a test holds open (#891). */
    void on_slot_publishing() override;

    // The slot vector, the listen socket, the accept/poll/teardown machinery and the
    // bus_link_t query trio all live in slot_server_t (#871); what stays here is the
    // length-prefix framing and its ingress bound.
    mem::mem_backend_t* backend_;
    std::size_t max_frame_ = tcp_transport_t::kMaxFrame;
    std::atomic<std::uint64_t> dropped_rx_{0};
    std::atomic<std::uint64_t> malformed_rx_{0};
    std::atomic<std::uint64_t> dropped_tx_{0};
};

}  // namespace tr::net
