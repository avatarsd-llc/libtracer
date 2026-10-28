/**
 * @file
 * @brief A reliable TCP stream transport with a 4-byte little-endian length prefix per frame.
 *
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
 * re-accepting after a peer departs), and tcp_server_transport_t (the MULTI-peer
 * listener — the shared slot_server_t slot/poll machinery (`%posix_endpoint.hpp`,
 * one home with ws_server_transport_t since #871) over raw length-prefix framing,
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
#include <mutex>
#include <span>
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
 *        peer's fd has been published and its slot is not yet open.
 *
 * The fd goes first, then the session's egress opens, then `open`, so "open ⇒ fd valid"
 * holds for every sender: a sender reads the fd only inside the egress role, which cannot be
 * claimed before the egress opens (#891, #2144). Open the session first and a broadcast can
 * write the frame to fd -1. That is not observable from the outside by racing: the window is
 * two instructions wide. This hook is where a test HOLDS the mid-publish instant open,
 * broadcasts into it, and checks the broadcast neither blocks nor writes to a closed
 * descriptor, and that the peer then takes the next frame.
 *
 * Same shape and same rules as `ws_peer_published_hook`: install it before the peer that
 * should trip it connects, and clear it before the test returns; consulted only when the build
 * binds `%tr::graph::default_config_t::kFaultInjection` (#1719).
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
 * @brief `tcp_transport_t`'s knobs as one aggregate (#1593): what both its constructors take
 *        after the peer address, in place of positional, defaulted parameters.
 *
 * Every member defaults to the historical default, so `tcp_config_t{}` is the unconfigured
 * link and a caller names only what it sets: `{.memory = {.rx = &pool}, .max_frame = 4096}`.
 */
struct tcp_config_t {
    /**
     * @brief The link's memory (@ref link_memory_t). `rx`: each inbound frame is read into a
     *        fresh exactly-`len`-byte segment from it (ADR-0042 §2); exhaustion is
     *        backpressure — the frame is drained off the stream, dropped, and `dropped_rx()`
     *        ticks; never an OOM.
     */
    link_memory_t memory{};
    /**
     * @brief Receive cap, bytes (0 → @ref tcp_transport_t::kMaxFrame). TIGHTEN-ONLY: a value
     *        above `kMaxFrame` is clamped to it (`length_prefix_framer_t::configured_cap`,
     *        #1035); a frame inside the cap the backend cannot hold is shed as backpressure,
     *        not treated as malformed (#932).
     */
    std::size_t max_frame = 0;
    /** @brief Recv-thread stack size in bytes, 0 = platform default
     *         (`posix_endpoint_t::start`). Non-zero right-sizes the recv thread on an MCU. */
    std::size_t recv_stack = 0;
    /**
     * @brief The app-provided PEER LIVENESS WINDOW in ms, `0` = `kDefaultLivenessWindowMs`
     *        (#838): how long the peer may fail to take bytes before it is treated as broken.
     *        It bounds each egress write turn; offers past a full lane whose turns moved
     *        nothing, `kMaxConsecutiveStalls` rings of them in a row, close the connection. A
     *        record the socket took only part of keeps
     *        its place and finishes on the next turn (#2144).
     */
    std::uint32_t liveness_window_ms = 0;
};

/**
 * @brief `tcp_server_transport_t`'s knobs as one aggregate (#1593), after the bind port.
 */
struct tcp_server_config_t {
    /** @brief The link's memory (@ref link_memory_t); `rx` is the per-connection receive
     *         seam — see @ref tcp_config_t::memory. */
    link_memory_t memory{};
    /** @brief Per-connection receive cap (0 → @ref tcp_transport_t::kMaxFrame);
     *         tighten-only — see @ref tcp_config_t::max_frame. */
    std::size_t max_frame = 0;
    /**
     * @brief Concurrent-peer admission cap (RFC-0006) — a connection beyond it is accepted
     *        and immediately closed. `0` takes `%kDefaultMaxPeers`; any other value is
     *        enforced as given. Read the enforced value back from `slot_server_t::max_peers`.
     */
    std::size_t max_peers = 0;
    /** @brief Expose the @ref bus_link_t facet (see @ref transport_t::bus) — the board↔board
     *         wiring choice, same contract as `ws_server_transport_t`'s. */
    bool peer_named = false;
    /** @brief Poll-thread stack size in bytes, 0 = platform default. One thread serves every
     *         peer, so this is the whole server's recv-stack knob. */
    std::size_t recv_stack = 0;
    /**
     * @brief The PEER LIVENESS WINDOW in ms, `0` = `kDefaultLivenessWindowMs` (#838). It
     *        SIZES the peer cap. A server's egress writes never wait (#2144): a session that
     *        is offered `kEgressStallStrikes` rings of frames in a row past a full STREAM lane,
     *        its socket moving nothing, is closed. A SAMPLE lane never strikes.
     */
    std::uint32_t liveness_window_ms = 0;
};

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
class tcp_transport_t final : public transport_t, private stream_endpoint_t {
   public:
    /** @brief The largest frame the length prefix may announce — the shared
     *         length_prefix_framer_t::kDefaultMaxFrame (16 MiB) unless `:settings
     *         max_frame` tightens it. A larger prefix is malformed — counted via
     *         @ref malformed_rx and the connection is closed (a desynced stream
     *         cannot be trusted again). */
    static constexpr std::size_t kMaxFrame = length_prefix_framer_t::kDefaultMaxFrame;

    /**
     * @brief DIAL mode: connect to @p peer_host:@p peer_port (synchronous).
     *
     * The TCP connect happens in the constructor (the ws_client_transport_t
     * shape) — confirm with ok(); on failure no thread is spawned. The receive
     * thread is NOT spawned here (#1045, #2247): the owner installs its receivers,
     * then calls @ref start_receiving, so no frame can land in an empty sink.
     *
     * @param peer_host Dotted-quad IPv4 address of the peer (e.g. "127.0.0.1").
     * @param peer_port TCP port of the peer (host byte order).
     * @param config    The link's knobs (@ref tcp_config_t): memory, receive cap, recv-thread
     *                  stack, liveness window.
     */
    tcp_transport_t(std::string_view peer_host, std::uint16_t peer_port,
                    const tcp_config_t& config = {});

    /**
     * @brief LISTEN mode: bind+listen on @p bind_port, accept ONE inbound peer.
     *
     * The same one-peer model as ws_server_transport_t: one connected client at a
     * time; after a peer departs the accept loop resumes for the next. Use ok()
     * to confirm the listen socket bound; the bound port (an ephemeral 0
     * request resolved) is observable via local_port().
     *
     * @param bind_port TCP port to listen on (host byte order; 0 → ephemeral).
     * @param config    The link's knobs (@ref tcp_config_t).
     */
    explicit tcp_transport_t(std::uint16_t bind_port, const tcp_config_t& config = {});

    /** @brief Stop the receive thread and close all sockets. */
    ~tcp_transport_t();

    /** @brief See @ref transport_t::shut_down. */
    void shut_down() override;

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
     * @brief Retained send (RFC-0028 §6.9): one length-prefixed record of @p head then
     *        @p value — written in-call when no write is in flight, and otherwise QUEUED as the
     *        prefix and head bytes plus one reference to @p value, never as a copy of it.
     *
     * The queued form is the one this override exists for: the enqueue-then-write queue used
     * to copy the whole record into its slot, so a large value published while another write
     * was in flight cost a payload copy. The writer that drains the record gathers the head
     * and the value's links into one `sendmsg` record under the same write lock and bound as
     * any other record, so the two-frame race on one socket (RFC-0028 §9 item 6) stays
     * whole: a record is written entirely, or a partial write condemns the peer.
     *
     * @param head  The record's leading spans (borrowed for the call).
     * @param value The payload (kept by reference only if the record is queued).
     * @param lane  The vertex's ring the record waits in, if it waits (ADR-0087 §2).
     */
    void send(std::span<const std::span<const std::byte>> head, const graph::value_t& value,
              const graph::delivery_lane_t& lane) override;

    /**
     * @brief Spawn the recv thread the DIAL constructor held back (#1045).
     *
     * The second phase of the two-phase bring-up: the socket is connected and NOTHING has
     * been read off it, so a sink installed before this call cannot have missed a frame.
     *
     * IDEMPOTENT, and a no-op wherever there is nothing to arm — an armed DIAL link
     * (its thread is already running), a LISTEN link (its accept loop started in the
     * constructor), and a link whose dial failed (`ok()` false, no socket to serve) — so an
     * owner may call it unconditionally on every link it wires, which is what
     * `%transport_vertex_t::make_connection` does. A DIAL link that is never armed
     * never receives and never reports the link down; it is simply an open socket until it
     * is destroyed.
     */
    bool start_receiving() override;

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

    /** @brief Strikes a STALLED peer cost (#838, #2144): offers past a full STREAM lane whose
     *         turn moved nothing, each one a frame shed. `kMaxConsecutiveStalls` rings of them in a
     *         row close the connection, so a non-zero count with @ref link_up still true is a peer
     * that is falling behind but recovering. */
    [[nodiscard]] std::uint64_t stalled_tx() const noexcept {
        return stalled_tx_.load(std::memory_order_relaxed);
    }

    /** @brief The peer liveness window this link bounds its sends by, ms, as constructed
     *         (`0` ⇒ `kDefaultLivenessWindowMs`) (#838). */
    [[nodiscard]] std::uint32_t liveness_window_ms() const noexcept { return liveness_window_ms_; }

    /** @brief The application's liveness check (#2198, @ref transport_t::close_frozen_peers):
     *         a frame held on the wire that moved no byte since @p deadline shuts the
     *         connection down, counted in @ref frozen_closed. A dialer owes TX only. */
    std::size_t close_frozen_peers(std::uint64_t now, std::uint64_t deadline) override {
        return close_frozen_one(now, deadline);
    }

    /** @brief Connections the application's liveness check closed as frozen (#2198). */
    [[nodiscard]] std::size_t frozen_closed() const noexcept {
        return frozen_closed_.load(std::memory_order_relaxed);
    }

    /** @brief The interface-level snapshot (#932) — the concrete accessors above, as the
     *         one shape a generic `transport_t*` holder reads. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx(), malformed_rx(), dropped_tx()};
    }

   private:
    void run_listen();  // accept loop (LISTEN mode)
    /** @brief The per-connection receive loop: frame ingress with no read-ahead (#2141), so
     *         each body lands in its exact segment with no copy. */
    void serve(int fd);
    /** @brief The one send body: prefix @p parts into one record and put it through the
     *         egress, in @p lane, keeping @p value (whose links end @p parts) if it waits. */
    void send_record(std::span<const std::span<const std::byte>> parts, const graph::value_t* value,
                     const graph::delivery_lane_t& lane);

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
    // Drop counters: word-wide, not 64-bit (core/STYLE.md §Introspection clause 5, #1697) —
    // a 64-bit atomic is a libatomic call on every rv32, the ESP32-C6 included. The 64-bit
    // accessors widen on read; a 32-bit target wraps after 2^32.
    std::atomic<std::size_t> dropped_rx_{0};
    std::atomic<std::size_t> malformed_rx_{0};
    std::size_t recv_stack_ = 0; /**< @brief The stack hint, held for @ref start_receiving. */
    /** @brief `memory.stack`, held for @ref start_receiving (#2288). */
    mem::block_source_t* stack_src_ = nullptr;
    /** @brief One-shot latch making @ref start_receiving idempotent — `start()` may be
     *         called at most once per endpoint. DIAL only; a LISTEN link spends its one
     *         `start` on the accept loop and never enters the latched path at all. */
    std::atomic<bool> recv_started_{false};
    // conn_fd_, dropped_tx_ and the egress (and their teardown discipline) live in
    // stream_endpoint_t.
};

/**
 * @brief A multi-peer TCP server transport_t — accepts many inbound peers on
 *        one listener and exposes them through the @ref bus_link_t facet
 *        (ADR-0044).
 *
 * The raw-stream sibling of ws_server_transport_t (#362): LITERALLY the same
 * slot/poll machinery, since #871 shared as @ref slot_server_t — ONE
 * poll-based thread accepts clients and serves every open connection
 * concurrently; peers occupy SLOTS recycled on departure, so steady-state
 * memory is bounded by the maximum concurrent peers ever reached (or @p
 * max_peers, the RFC-0006 injected bound).  What this class adds to that base
 * is its FRAMING: the shared u32-LE length prefix, read by frame ingress
 * (`%frame_ingress.hpp`, #2141) with no stream buffer, where WS has RFC 6455 packaging.  There is
 * NO handshake phase: a peer is open (named `p<slot>~<gen>`, ADR-0073 §2 / #426) from the moment
 * its connection is accepted.  The board↔board listener shape: leaner than WS packaging (no HTTP
 * upgrade, no frame masking) with the same per-peer return-route identity when @p peer_named.
 */
class tcp_server_transport_t final : public stream_server_base_t {
   public:
    /**
     * @brief Bind+listen on @p bind_port (0 = ephemeral; see local_port()).
     *
     * Spawns the poll/serve thread immediately.  Use ok() to confirm the
     * listen socket bound; the bound port is observable via local_port().
     *
     * @param bind_port  TCP port to listen on (host byte order; 0 → ephemeral).
     * @param config     The server's knobs (@ref tcp_server_config_t): memory, receive cap,
     *                   peer cap, bus facet, poll-thread stack, liveness window.
     */
    explicit tcp_server_transport_t(std::uint16_t bind_port,
                                    const tcp_server_config_t& config = {});

    /** @brief Stop the poll thread and close all sockets. */
    ~tcp_server_transport_t();

    /** @brief See @ref transport_t::shut_down. */
    void shut_down() override;

    tcp_server_transport_t(const tcp_server_transport_t&) = delete;
    tcp_server_transport_t& operator=(const tcp_server_transport_t&) = delete;

    /**
     * @brief Send @p frame as one length-prefixed record to EVERY open peer
     *        (the flat point-to-point surface).
     *
     * The prefix is encoded once; each peer gets one serialized gathered
     * write through its egress.  No-op until a peer is connected.  Thread-safe;
     * a stalled peer never delays the others (#2144).  A directed single-peer send is
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
     * Every idle peer writes from the same read-only gather; a peer that has to wait
     * gets one shared frame (ADR-0087 §2).  Thread-safe.
     *
     * @param iov The spans to emit, in order, as a single record.
     */
    void send(std::span<const std::span<const std::byte>> iov) override;

    /**
     * @brief Retained broadcast (RFC-0028 §6.9): @p head then @p value as one record to
     *        every open peer; a peer that has to wait holds one shared frame that keeps
     *        @p value by reference, in @p lane's ring (ADR-0087 §2).
     */
    void send(std::span<const std::span<const std::byte>> head, const graph::value_t& value,
              const graph::delivery_lane_t& lane) override;

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
     * @brief The directed per-peer sending endpoint @ref slot_server_t::peer_link hands out:
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

        /** @brief Directed retained send: @p head then @p value as one record to this
         *         facade's peer only, waiting in @p lane's ring if it waits. */
        void send(std::span<const std::span<const std::byte>> head, const graph::value_t& value,
                  const graph::delivery_lane_t& lane) override;

        /** @brief The owning server's snapshot (#932): a directed facade counts into the
         *         link's own counters, so it reports them rather than a fabricated zero. */
        [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
            return owner_ == nullptr ? transport_drop_stats_t{} : owner_->drop_stats();
        }

       private:
        friend class tcp_server_transport_t;
        tcp_server_transport_t* owner_ = nullptr; /**< @brief The owning server. */
        session_t* slot_ = nullptr;               /**< @brief The peer slot this sends to. */
    };

    /** @brief The one send body: prefix @p parts into one record, then to every open peer
     *         (@p to null) or to @p to alone, in @p lane, keeping @p value if it waits. */
    void send_record(session_base_t* to, std::span<const std::span<const std::byte>> parts,
                     const graph::value_t* value, const graph::delivery_lane_t& lane);

    /** @brief A retained send's record laid out on the stack for `send_record`; a value
     *         too wide for that takes the base's copy path. */
    void send_retained(session_base_t* to, std::span<const std::span<const std::byte>> head,
                       const graph::value_t& value, const graph::delivery_lane_t& lane);

    /** @brief One fresh slot with its frame-ingress state and directed facade. */
    mem::poly_ptr_t<session_base_t> make_session() override;

    /** @brief Per-accept setup: TCP_NODELAY + reset ingress state.  Returns true — a raw
     *         stream peer has no handshake, so it is open the moment it is accepted. */
    bool on_accept(session_base_t& s, int fd) override;

    /** @brief Framing: frame ingress (ADR-0087 §1, #2141) reads each u32-LE prefix first and
     *         each body straight into a segment drawn at its exact length from this server's
     *         receive source, delivering each frame (tearing the slot down on a close or a
     *         desynced stream). */
    void on_readable(session_base_t& s, int fd) override;

    /** @brief Teardown: reset the slot's ingress state as it is recycled. */
    void on_slot_reset(session_base_t& s) override;

    /** @brief A record's prefix or body is part-read (#2198). */
    [[nodiscard]] bool mid_frame(const session_base_t& s) const override;

    /** @brief Fire `detail::tcp_peer_publishing_hook` with the fd published and the slot not
     *         yet open — the mid-publish instant a test holds open (#891). Empty unless the
     *         build binds `kFaultInjection` (#1719). */
    void on_slot_publishing() override;

    // The slot vector, the listen socket, the accept/poll/teardown machinery and the
    // peer query trio all live in slot_server_t (#871), and the bus FACET one tier
    // below it in stream_server_base_t (#1438); what stays here is the length-prefix
    // framing and its ingress bound.
    mem::mem_backend_t* backend_;
    std::size_t max_frame_ = tcp_transport_t::kMaxFrame;
    // Drop counters: word-wide, not 64-bit (core/STYLE.md §Introspection clause 5, #1697) —
    // a 64-bit atomic is a libatomic call on every rv32, the ESP32-C6 included. The 64-bit
    // accessors widen on read; a 32-bit target wraps after 2^32.
    std::atomic<std::size_t> dropped_rx_{0};
    std::atomic<std::size_t> malformed_rx_{0};
};

/** @brief The pre-v0.18.0 spelling of @ref tcp_server_transport_t; removed in v0.19.0 (#1723). */
using transport_tcp_server = tcp_server_transport_t;

}  // namespace tr::net
