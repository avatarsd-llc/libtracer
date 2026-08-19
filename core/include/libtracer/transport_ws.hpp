/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_ws server (#54, multi-peer per #362) — the connection layer on top
 * of the RFC 6455 PROTOCOL layer (`%ws.hpp`). A board accepts MANY concurrent
 * inbound WebSocket connections (the headline browser↔board link — an SPA plus
 * a peer node, or several tabs): bind+listen on a TCP port, then ONE poll-based
 * thread multiplexes the listen socket and every peer socket (no per-peer
 * thread — FreeRTOS stacks are the scarce resource on the MCU target). Each
 * peer runs the opening handshake (parse the HTTP Upgrade request, reply 101
 * Switching Protocols with ws::accept_key), then its byte stream is
 * ws::decode_frame_checked()d into complete frames — the same call on both the
 * server and the client path. Each BINARY message is one libtracer frame (one
 * TLV) handed to the receiver — tagged with the SENDING peer's name through the
 * bus_link_t facet (ADR-0044), so return routes name the right browser tab; PING
 * is answered with a stack-built PONG. Every connection dies down ONE path
 * (slot_server_t::teardown_slot — one home, shared with transport_tcp_server since
 * #871; five call sites in this server's `on_readable` framing hook): the
 * peer closing the socket or a read error, at either phase; on an ESTABLISHED
 * stream a CLOSE or an RFC 6455 violation the checked decode reports (an
 * oversized or non-final control frame, §5.5/§7.1.7, or a DATA frame declaring
 * more than the injected receive cap, #872); during the opening
 * HANDSHAKE a request that overruns 16 KiB or carries no `Sec-WebSocket-Key`.
 *
 * INGRESS IS BOUNDED BY AN INJECTED SEAM, not by the peer. Both roles take the same
 * `(mem::mem_backend_t*, max_frame)` pair tcp/quic/webtransport take, and both expose the
 * same `dropped_rx()`/`malformed_rx()` counters, so one operator vocabulary reads across
 * every framed transport. The cap is checked against the DECLARED length in the WS frame
 * header — before a body byte is buffered — and against the reassembled total of a
 * fragmented message, so neither a 64-bit length nor a million CONTs can make the receiver
 * hold more than the deployment allowed.
 * send(frame) broadcasts to every open peer (the flat point-to-point surface);
 * a directed per-peer send is peer_link(name)->send().
 *
 * Both roles live here: transport_ws_server (accept inbound peers) and
 * transport_ws_client (dial out to a ws:// peer — device-to-device / NAT egress),
 * the latter sending MASKED client frames per RFC 6455 §5.1. POSIX sockets;
 * mirrors transport_udp's lifecycle (a recv thread polled for a clean shutdown).
 * The framing itself is never reimplemented here — it all goes through tr::net::ws.
 *
 * SCOPE — this is the HOST/POSIX WebSocket, and ESP-IDF is NOT in it (#947 ruling).
 * The single-thread-multiplexes-every-peer design above is stated in FreeRTOS terms
 * because stacks are the scarce resource wherever this runs, but do not read that as
 * an invitation to run it on an MCU: on ESP-IDF the supported plane is the IDF-native
 * pair (tr::net::httpd_ws_link_t on esp_http_server, tr::net::esp_ws_client_link_t on
 * esp_transport_ws), and these two types are NOT COMPILED into an ESP-IDF chip build
 * at all. That is not a footprint preference — it is a correctness one: the
 * scatter-gather egress underneath (posix_endpoint_t::write_all_iov) asks sendmsg for
 * MSG_NOSIGNAL, a flag lwIP defines but lwip_sendmsg rejects with EOPNOTSUPP, and the
 * failure is read as peer-gone, so on lwIP every data frame is silently dropped while
 * the opening handshake and PING/PONG keep working (#948). Nothing here needs a
 * platform #ifdef: the ESP-IDF component simply does not list this TU on a chip
 * target, exactly as socketcan_link.cpp is swapped for its stub.
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
#include "libtracer/mem_source.hpp"
#include "libtracer/posix_endpoint.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

namespace detail {

/**
 * @brief TEST SEAM: run by `transport_ws_server::on_readable` at the exact instant a peer's
 *        `101 Switching Protocols` response is on the wire AND its slot is published open.
 *
 * The handshake's two visible transitions — "the peer may believe the connection is up" and
 * "a sender may use this slot" — are ONE step only because the `open` store sits inside the
 * same `write_m_` critical section as the response write. That is not observable from the
 * outside by racing: the window is a few instructions wide. This hook is where a test HOLDS
 * that instant open, sends into it, and checks the frame arrives; with the store moved back
 * out of the lock the identical test reads an empty socket. Null in production — the cost is
 * one predictable null-check, once per accepted peer, on the (cold) handshake path.
 *
 * Same shape and same rules as `tr::detail::probe_fail_hook`: install it before the peer that
 * should trip it connects, and clear it before the test returns.
 */
inline void (*ws_peer_published_hook)() = nullptr;

}  // namespace detail

/**
 * @brief Suggested module name for a DIAL `ws` connection (ADR-0073 §4).
 *
 * A *suggestion*, never a registration: the library auto-registers no module names — the
 * application passes this (or any name it prefers) to
 * `transport_vertex_t::register_module("ws-client", "ws", conn_role_t::DIAL)`.
 */
inline constexpr std::string_view kWsClientSuggestedModule = "ws-client";

/** @brief Suggested module name for a LISTEN `ws` connection (see
 *         `kWsClientSuggestedModule`; the application registers it, or any name it
 *         prefers). */
inline constexpr std::string_view kWsServerSuggestedModule = "ws-server";

/**
 * @brief A WebSocket (RFC 6455) server transport_t — accepts many inbound peers
 *        and exposes them through the @ref bus_link_t facet (ADR-0044).
 *
 * Binds and listens on a TCP port (localhost is fine for tests); one poll-based
 * thread accepts clients and serves every open connection concurrently. Each
 * inbound BINARY message is delivered tagged with its peer's name
 * (the routable `p<slot>` fallback, ADR-0073 §2 / #426) when a peer-named sink is installed (the
 * router's bus wiring), or to the flat @ref transport_t receiver otherwise —
 * so a single-client deployment behaves exactly as the point-to-point server
 * always did. The dial-out counterpart is transport_ws_client below.
 *
 * Peer lifecycle: peers occupy SLOTS. A departed peer's slot is recycled for
 * the next accept, so steady-state memory is bounded by the maximum number of
 * CONCURRENT peers ever reached (or by @p max_peers when set — the RFC-0006
 * injected bound), never by the number of connections ever served. That whole
 * slot/poll layer is @ref slot_server_t, shared verbatim with
 * transport_tcp_server since #871; what this class adds is the RFC 6455
 * packaging — the opening handshake and the frame codec.
 */
class transport_ws_server : public slot_server_t {
   public:
    /** @brief The largest MESSAGE a peer may announce — the shared
     *         length_prefix_framer::kDefaultMaxFrame (16 MiB) unless `:settings max_frame`
     *         tightens it, and further bounded by the injected backend's real capacity.
     *
     * One WS message is one libtracer frame, so this is the same per-connection receive cap
     * tcp/quic/webtransport apply to their length prefix — it just reads off a WS frame
     * header instead. A frame (or a reassembled message) claiming more is malformed:
     * @ref malformed_rx ticks and the connection is failed, RFC 6455 §7.1.7. */
    static constexpr std::size_t kMaxFrame = length_prefix_framer::kDefaultMaxFrame;

    /**
     * @brief Bind+listen on @p bind_port (0 = ephemeral; see local_port()).
     *
     * Spawns the poll/serve thread immediately. Use ok() to confirm the listen
     * socket bound. The bound port is observable via local_port().
     *
     * @param bind_port TCP port to listen on (host byte order; 0 → ephemeral).
     * @param backend   The host-injected RX memory seam (ADR-0042 §2), the same
     *                  parameter tcp/quic/webtransport take in the same position:
     *                  every inbound message fragment is copied into a fresh
     *                  segment drawn from it (default: the process heap; a
     *                  bounded host passes its pool). Exhaustion is
     *                  backpressure — the message is shed and dropped_rx()
     *                  ticks; never an OOM. Must outlive the transport.
     * @param max_frame Per-connection receive cap (0 → @ref kMaxFrame).
     *                  TIGHTEN-ONLY: a value above kMaxFrame is clamped to it
     *                  (`length_prefix_framer::configured_cap`, #1035) — a
     *                  config-writable key must not raise the ingress
     *                  buffering bound; the effective cap also honors the
     *                  backend's real capacity
     *                  (`length_prefix_framer::effective_cap` — the
     *                  no-synthetic-limits doctrine). It is checked against the
     *                  DECLARED length in the WS frame header, so an oversize
     *                  announcement is refused before one body byte is buffered.
     * @param max_peers Concurrent-peer admission cap. A deployment-injected
     *                  bound (RFC-0006) — a connection beyond it is accepted
     *                  and immediately closed (a clean refusal, not a hung
     *                  SYN). `0` no longer means UNBOUNDED (#1295): it takes
     *                  the liveness window's own ceiling
     *                  (`window / kBoundedWaitMs`), and a larger request is
     *                  clamped to that ceiling, because the cap is the
     *                  denominator every send bound divides by. Read the
     *                  enforced value back from `slot_server_t::max_peers`.
     * @param peer_named Expose the @ref bus_link_t facet (see @ref bus). A
     *                   wiring-time deployment choice: the browser-SPA/tabs
     *                   server sets it so each tab gets its own return route;
     *                   a point-to-point link keeps the default (its registered
     *                   child NAME stays the hop name, as tcp/quic).
     * @param recv_stack Poll-thread stack size in bytes, 0 = platform default
     *                   (`posix_endpoint_t::start`). One thread multiplexes
     *                   the listener and every peer, so this is the whole
     *                   server's recv-stack knob.
     * @param liveness_window_ms The app-provided PEER LIVENESS WINDOW in ms, `0` =
     *                   `kDefaultLivenessWindowMs` (#838). One fan-out round is bounded
     *                   by it (each peer gets window ÷ peers-in-the-round) and a DIRECTED
     *                   send by window ÷ @p max_peers (#1295), so a browser tab that stops
     *                   reading — a throttled background tab is the shipped case — can no
     *                   longer freeze the sending thread or the other tabs' frames behind
     *                   it, on either path; a session that stalls `kMaxConsecutiveStalls`
     *                   records in a row, or once mid-record, is closed. It also SIZES the
     *                   peer cap: see @p max_peers.
     */
    explicit transport_ws_server(std::uint16_t bind_port,
                                 mem::mem_backend_t* backend = &mem::heap_backend(),
                                 std::size_t max_frame = 0, std::size_t max_peers = 0,
                                 bool peer_named = false, std::size_t recv_stack = 0,
                                 std::uint32_t liveness_window_ms = 0);

    /** @brief Stop the recv thread and close all sockets. */
    ~transport_ws_server() override;

    transport_ws_server(const transport_ws_server&) = delete;
    transport_ws_server& operator=(const transport_ws_server&) = delete;

    /**
     * @brief Send @p frame as one server→client BINARY WebSocket message to
     *        EVERY open peer (the flat point-to-point surface).
     *
     * Encodes once via ws::encode_frame(BINARY, frame) (FIN=1, unmasked) and
     * writes the whole frame to each connected client. No-op until a client is
     * connected. Thread-safe (socket writes are guarded). A directed
     * single-peer send is `peer_link(name)->send(frame)`.
     *
     * @param frame A complete TLV's bytes.
     */
    void send(std::span<const std::byte> frame) override;

    /**
     * @brief Zero-copy scatter-gather broadcast: emit the gathered @p iov spans as
     *        ONE server→client BINARY message to EVERY open peer, no flatten copy.
     *
     * Overrides the base flatten-then-encode default (`%transport.hpp`): server frames
     * are UNMASKED (RFC 6455 §5.1), so the frame header rides as the first iovec
     * entry and the payload spans follow it straight to the wire via one gathered
     * scatter-gather write per peer — no allocation, no copy. Each peer writes from
     * a fresh copy of the iovec array (the write consumes it). No-op until a client
     * is connected. Thread-safe (peers_m_ → write_m_, the header lock order).
     *
     * @param iov The spans to emit, in order, as a single frame.
     */
    void send(std::span<const std::span<const std::byte>> iov) override;

    /** @brief True — WS reassembles fragmented messages into ropes (ADR-0053 §5):
     *         each message crosses the seam as a `rope_t`, one owning link per WS
     *         fragment (a single link for an unfragmented message), chained by
     *         reassembly, never memcpy'd flat. Covers both the transport_t and
     *         bus_link_t facets (one override, same contract). */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief Messages dropped because the RX backend was exhausted (backpressure,
     *         ADR-0039 §4 / ADR-0042 §2) — shed mid-reassembly, never an OOM. Summed
     *         over every peer this server has served. */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept {
        return dropped_rx_.load(std::memory_order_relaxed);
    }

    /** @brief RFC 6455 violations seen: an over-cap declared length (see @ref kMaxFrame),
     *         a reassembled message past the cap, or a §5.5 control-frame breach. Each one
     *         fails its connection (§7.1.7). Summed over every peer. */
    [[nodiscard]] std::uint64_t malformed_rx() const noexcept {
        return malformed_rx_.load(std::memory_order_relaxed);
    }

    /** @brief Messages shed on the way OUT (#932): a refused gather store, or no open peer
     *         slot to write to — each one used to be a bare return no observer could see. */
    [[nodiscard]] std::uint64_t dropped_tx() const noexcept {
        return dropped_tx_.load(std::memory_order_relaxed);
    }

    /** @brief The interface-level snapshot (#932) — what a generic `transport_t*` reads. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx(), malformed_rx(), dropped_tx()};
    }

    /** @brief The cap actually honored: `min(max_frame, backend.max_segment_size())` — what
     *         a declared frame length is compared against, resolved from the two injected
     *         resources rather than restated as a number. */
    [[nodiscard]] std::size_t effective_max_frame() const noexcept {
        return length_prefix_framer::effective_cap(*backend_, max_frame_);
    }

   private:
    struct session_t;  // one peer slot's connection state (defined in the .cpp)

    /**
     * @brief The directed per-peer sending endpoint @ref peer_link hands out:
     *        `send()` writes a server BINARY frame to that peer's socket only.
     *        Ingress stays on the owning server's peer-named slot — this
     *        facade's own inherited receiver is never delivered to.
     */
    class peer_endpoint_t final : public transport_t {
       public:
        /** @brief Send @p frame to this facade's peer only (no-op once departed). */
        void send(std::span<const std::byte> frame) override;

        /**
         * @brief Zero-copy scatter-gather directed send: emit the gathered @p iov
         *        spans as ONE server BINARY message to this facade's peer only.
         *
         * The single-fd twin of the broadcast override: server frames are UNMASKED
         * (RFC 6455 §5.1), so the frame header rides as the first iovec entry and
         * the payload spans follow via one gathered scatter-gather write — no copy.
         * No-op once departed.
         *
         * @param iov The spans to emit, in order, as a single frame.
         */
        void send(std::span<const std::span<const std::byte>> iov) override;

        /** @brief The owning server's snapshot (#932): a directed facade counts into the
         *         link's own counters, so it reports them rather than a fabricated zero. */
        [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
            return owner_ == nullptr ? transport_drop_stats_t{} : owner_->drop_stats();
        }

       private:
        friend class transport_ws_server;
        transport_ws_server* owner_ = nullptr; /**< @brief The owning server. */
        session_t* slot_ = nullptr;            /**< @brief The peer slot this sends to. */
    };

    /** @brief One fresh slot with its handshake/frame buffers, reassembler and facade. */
    std::unique_ptr<session_base_t> make_session() override;

    /** @brief Per-accept setup: clear the slot's handshake and frame buffers. Returns
     *         false — a WS session only carries frames PAST its `101`, which
     *         `on_readable` publishes (that ordering is deliberate, not drift). */
    bool on_accept(session_base_t& s, int fd) override;

    /** @brief Framing: the opening handshake while the slot is not yet open, then
     *         RFC 6455 decode of every complete frame the chunk finished. */
    void on_readable(session_base_t& s, const std::byte* data, std::size_t len) override;

    /** @brief Teardown: release the slot's buffers and reset its reassembler. */
    void on_slot_reset(session_base_t& s) override;

    bool drain_frames(session_t& s);  // decode buffered frames; false ⇒ teardown

    // The slot vector, the listen socket, the accept/poll/teardown machinery and the
    // bus_link_t query trio all live in slot_server_t (#871); what stays here is the
    // RFC 6455 packaging and its ingress bound.
    //
    // RX segment source for message reassembly (ADR-0042 §2) + the ingress bound and
    // its counters — the same four members tcp/quic/webtransport carry.
    mem::mem_backend_t* backend_;
    std::size_t max_frame_ = kMaxFrame;  // per-connection receive cap (:settings; 0 => kMaxFrame)
    std::atomic<std::uint64_t> dropped_rx_{0};
    std::atomic<std::uint64_t> malformed_rx_{0};
    std::atomic<std::uint64_t> dropped_tx_{0};
};

/**
 * @brief A WebSocket (RFC 6455) client transport_t — dials out to one peer.
 *
 * The mirror of transport_ws_server: a board that DIALS OUT to a ws:// peer
 * (device-to-device, or egress through a NAT). The constructor TCP-connects to
 * @p host:@p port, runs the opening handshake from the client side (sends an
 * HTTP GET Upgrade with a fresh Sec-WebSocket-Key, then verifies the 101
 * response's Sec-WebSocket-Accept against ws::accept_key), and on success spawns
 * a receive loop. Per RFC 6455 §5.1 every client→server frame is MASKED
 * (ws::encode_client_frame); inbound server frames are unmasked and decode the
 * same way the server's do. ok() confirms the handshake completed.
 */
class transport_ws_client : public transport_t, private stream_endpoint_t {
   public:
    /**
     * @brief Connect to @p host:@p port and run the client opening handshake.
     *
     * TCP-connects, sends the HTTP Upgrade request, and verifies the server's
     * 101 Sec-WebSocket-Accept. On success the receive loop thread is spawned;
     * confirm with ok(). On any failure the connection is closed and ok() is
     * false.
     *
     * @param host Dotted-quad IPv4 address of the peer (e.g. "127.0.0.1").
     * @param port TCP port of the peer (host byte order).
     * @param backend The host-injected RX memory seam — see
     *             transport_ws_server's constructor; a DIALLED peer is no more
     *             trusted than an accepted one, so the client takes the same
     *             seam in the same position as `tcp_transport_t`'s DIAL form.
     * @param max_frame Per-connection receive cap (0 → @ref
     *             transport_ws_server::kMaxFrame; a value above it is clamped —
     *             tighten-only, `length_prefix_framer::configured_cap`, #1035),
     *             bounded by the backend's real capacity — see
     *             transport_ws_server's constructor.
     * @param recv_stack Recv-thread stack size in bytes, 0 = platform default
     *             (`posix_endpoint_t::start`).
     * @param defer_recv Two-phase bring-up (#1025): with `true` the handshake still runs
     *             HERE (so ok() answers on return) but the recv thread is NOT spawned —
     *             nothing can be decoded, let alone delivered, until @ref start_receiving
     *             is called. That is the only ordering in which
     *             `%transport_t::set_receiver`'s "must be set before frames flow" is
     *             satisfiable on a DIAL socket: a server that pushes its state the instant
     *             the handshake completes has its first message in flight before this
     *             constructor returns, and the default (`false`, the historical shape)
     *             decodes it on the recv thread into whatever sink is installed by then —
     *             possibly none, in which case it is dropped with no counter moving.
     * @param liveness_window_ms The app-provided PEER LIVENESS WINDOW in ms, `0` =
     *             `kDefaultLivenessWindowMs` (#838): it bounds every send (and the
     *             write-mutex hold it takes), so a server that stops reading cannot freeze
     *             the sending thread; `kMaxConsecutiveStalls` stalled records in a row,
     *             or one that half-reached the wire, close the connection.
     * @param egress_src The ADR-0079 EGRESS store (#873) this link's masked-frame buffer
     *             (`%tx_buf_`) is drawn from. It is passed to the CONSTRUCTOR rather than
     *             wired afterwards because `mem::block_array_t` binds its source once, at
     *             construction: a later @ref transport_t::set_egress_source moves the
     *             base's gather temporary but can no longer reach this member. This
     *             constructor applies @p egress_src to both, so a link built here has ONE
     *             egress store. `nullptr` (and the default) means the process heap —
     *             today's behaviour unchanged. Must outlive this transport.
     */
    transport_ws_client(const std::string& host, std::uint16_t port,
                        mem::mem_backend_t* backend = &mem::heap_backend(),
                        std::size_t max_frame = 0, std::size_t recv_stack = 0,
                        bool defer_recv = false, std::uint32_t liveness_window_ms = 0,
                        mem::block_source_t* egress_src = &mem::heap_source());

    /** @brief Stop the recv thread and close the socket. */
    ~transport_ws_client() override;

    transport_ws_client(const transport_ws_client&) = delete;
    transport_ws_client& operator=(const transport_ws_client&) = delete;

    /**
     * @brief Send @p frame as one client→server MASKED BINARY WebSocket message.
     *
     * Encodes via ws::encode_client_frame(BINARY, frame, key) (FIN=1, MASK=1,
     * fresh per-frame key) and writes the whole frame to the peer. No-op once the
     * connection has been torn down. Thread-safe (the socket write is guarded).
     *
     * @param frame A complete TLV's bytes.
     */
    void send(std::span<const std::byte> frame) override;

    /**
     * @brief Spawn the recv thread a `defer_recv` construction held back (#1025).
     *
     * The second phase of the two-phase bring-up: the socket is connected and handshaken,
     * the bytes the server pipelined behind its `101` are held, and NOTHING has been
     * decoded yet — so a sink installed before this call cannot have missed a frame. From
     * here the client behaves exactly as a one-phase one: the thread's first act is to
     * drain what the handshake carried over.
     *
     * Idempotent and safe on a one-phase client (the thread is already running → no-op) and
     * on a failed handshake (`ok()` false → no-op, nothing to serve). A `defer_recv` client
     * that is never started never receives, never answers a PING and never reports the link
     * down; it is simply an open socket until it is destroyed.
     */
    void start_receiving() override;

    /** @brief True — WS reassembles fragmented messages into ropes (ADR-0053 §5):
     *         each message crosses the seam as a `rope_t`, one owning link per WS
     *         fragment (a single link for an unfragmented message), chained by
     *         reassembly, never memcpy'd flat. */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief The came-up predicate (#1059): the dial and the client opening handshake
     *         succeeded. Answered at construction and never reverting — a link that came
     *         up and later died still answers true here; liveness is @ref link_up. */
    [[nodiscard]] bool ok() const noexcept { return came_up_; }

    /** @brief Liveness (the @ref transport_t::link_up contract): true from the completed
     *         handshake until the recv loop's teardown — a peer CLOSE, a remote hangup, a
     *         fatal receive error or an RFC 6455 breach all clear it (relaxed atomic; the
     *         push twin is @ref set_down_notifier). A `defer_recv` client that is never
     *         started never observes the wire and so never reports down (see
     *         @ref start_receiving). */
    [[nodiscard]] bool link_up() const noexcept override {
        return connected_.load(std::memory_order_relaxed);
    }

    /** @brief Messages dropped to RX-backend exhaustion (backpressure) — the server-side
     *         counter's twin, same name, same meaning. */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept {
        return dropped_rx_.load(std::memory_order_relaxed);
    }

    /** @brief RFC 6455 violations seen — an over-cap declared length, an over-cap
     *         reassembled message, or a §5.5 control breach. Each fails the connection. */
    [[nodiscard]] std::uint64_t malformed_rx() const noexcept {
        return malformed_rx_.load(std::memory_order_relaxed);
    }

    /** @brief Messages shed on the way OUT (#932) — the client frame could not be encoded
     *         (gather store refused) or there is no live connection to write to. */
    [[nodiscard]] std::uint64_t dropped_tx() const noexcept {
        return dropped_tx_.load(std::memory_order_relaxed);
    }

    /** @brief The subset of @ref dropped_tx a STALLED server caused (#838): records
     *         abandoned because their send bound expired. `kMaxConsecutiveStalls` in a
     *         row — or one that half-reached the wire — closes the connection. */
    [[nodiscard]] std::uint64_t stalled_tx() const noexcept {
        return stalled_tx_.load(std::memory_order_relaxed);
    }

    /** @brief The peer liveness window this link bounds its sends by, ms, as constructed
     *         (`0` ⇒ `kDefaultLivenessWindowMs`) (#838). */
    [[nodiscard]] std::uint32_t liveness_window_ms() const noexcept { return liveness_window_ms_; }

    /** @brief The interface-level snapshot (#932) — what a generic `transport_t*` reads. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx(), malformed_rx(), dropped_tx()};
    }

    /** @brief The cap actually honored: `min(max_frame, backend.max_segment_size())`. */
    [[nodiscard]] std::size_t effective_max_frame() const noexcept {
        return length_prefix_framer::effective_cap(*backend_, max_frame_);
    }

   private:
    /**
     * @brief Send the GET Upgrade, verify the 101, and hand back what came after it.
     *
     * The response `recv` routinely returns the 101 AND the bytes the server pipelined
     * behind it — a server that pushes state the instant the handshake completes puts its
     * first frame in that same segment. Those bytes are the start of the frame stream, so
     * they are moved into @p pipelined (cleared first, empty in the common case) for
     * `serve` to decode; dropping them loses that frame silently. The server half has
     * carried them over since it grew a second peer (`on_readable`'s `s.buf.assign`) —
     * this is the DIAL half of the same rule (#1020).
     */
    bool handshake(int fd, const std::string& host, std::uint16_t port,
                   std::vector<std::byte>& pipelined);
    /** @brief Frame recv loop, seeded with the bytes `handshake` found behind the 101. */
    void serve(int fd, std::vector<std::byte> pipelined);
    std::uint32_t next_mask_key();  // per-frame masking key (varied, not crypto)

    // conn_fd_ + write_m_ (and their teardown discipline) live in stream_endpoint_t.
    std::atomic<std::uint64_t> mask_state_{0};
    // The RX seam + ingress bound + counters, identical to the server's (and to tcp's).
    mem::mem_backend_t* backend_;
    std::size_t max_frame_ = transport_ws_server::kMaxFrame;
    std::atomic<std::uint64_t> dropped_rx_{0};
    std::atomic<std::uint64_t> malformed_rx_{0};
    std::atomic<std::uint64_t> dropped_tx_{0};
    /**
     * @brief The REUSED masked-frame buffer `send` encodes into, guarded by `write_m_`.
     *
     * A client frame must be masked (RFC 6455 §5.1), so its wire bytes are not the caller's
     * bytes and cannot be gathered by reference — this is the one WS egress path that still
     * needs a buffer. Reused across sends so the steady state allocates nothing, and drawn
     * from the failable seam (ADR-0065) so exhaustion is a `nullptr` the send turns into a
     * dropped frame, never the `abort()` a throwing grow becomes under `-fno-exceptions`
     * (#848).
     *
     * The source is the constructor's `egress_src` (#873), NOT the process heap: a
     * `block_array_t` binds its source at construction, so this member is the reason that
     * argument exists rather than relying on @ref transport_t::set_egress_source.
     */
    mem::block_array_t<std::byte> tx_buf_;
    /** @brief The came-up fact `ok()` reports: written once, in the constructor, on
     *         handshake success — before any thread this object owns exists — and never
     *         again, so a plain bool is race-free here. */
    bool came_up_ = false;
    /** @brief The liveness flag `link_up()` reports (#1059): set with `came_up_`, cleared
     *         by the recv thread's teardown path. Written and read across threads —
     *         relaxed atomic (a hint, not a synchronisation point; deliberately no
     *         lock-free assertion — the rv32/no-A target). */
    std::atomic<bool> connected_{false};
    /**
     * @brief The bytes the server pipelined behind its `101`, parked between the handshake
     *        and @ref start_receiving (moved into the recv thread there, empty after).
     *
     * Written by the constructor, read once by the thread-spawning `start_receiving`; the
     * recv thread never touches this member (it owns its own copy).
     */
    std::vector<std::byte> pipelined_;
    std::size_t recv_stack_ = 0; /**< @brief The stack hint, held for `start_receiving`. */
    /** @brief One-shot latch making @ref start_receiving idempotent — `start()` may be
     *         called at most once per endpoint. */
    std::atomic<bool> recv_started_{false};
};

}  // namespace tr::net
