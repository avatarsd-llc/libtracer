/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_quic — the msquic-based QUIC transport_t (ADR-0043 Phase A), a
 * SEPARATE MODULE (the `libtracer_quic` library target): a host that talks QUIC
 * links it and registers quic_transport_factory on its transport_vertex_t; a
 * host that doesn't never compiles these sources — the core library itself has
 * no msquic reference, no feature macro, no `quic` builtin (open/closed: the
 * catalog is extended through register_transport_type, never modified). One
 * QUIC connection carries ONE bidirectional stream, and that stream carries the
 * SAME 4-byte u32-LE length-prefix framing as tcp_transport_t — the M6 framing
 * seam reused verbatim over a link that adds TLS 1.3, connection migration, and
 * no TCP head-of-line blocking. Per-flow streams and RFC 9221 datagram mode are
 * staged follow-ons (ADR-0043 §3). This header keeps msquic out of the public
 * include surface (pimpl) so including it never requires msquic headers.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "libtracer/mem_heap.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_vertex.hpp"

namespace tr::net {

/**
 * @brief DIAL-side TLS trust options for @ref quic_transport_t (ADR-0043 Phase A).
 *
 * QUIC is TLS 1.3 by construction, so the dialer must decide how to trust the
 * server certificate. Exactly one of the two knobs is used: a CA bundle to
 * verify against, or the DEV-ONLY escape hatch that skips verification (the
 * only way to reach a self-signed dev cert, which cannot chain to any CA).
 */
struct quic_dial_tls_t {
    std::string ca_file;             /**< @brief PEM CA bundle path to verify the server
                                                 certificate against (empty = the system
                                                 trust store). */
    bool insecure_no_verify = false; /**< @brief DEV ONLY: skip server certificate
                                                 validation entirely (self-signed
                                                 dev certs — tools/gen-dev-cert.sh).
                                                 Never enable in deployment. */
};

/**
 * @brief The msquic QUIC transport_t (ADR-0043 Phase A) — length-prefix framing
 *        over ONE bidirectional stream on one connection.
 *
 * Every frame is sent as `u32-LE length ++ frame bytes` (identical to
 * tcp_transport_t, so the two wire framings are interchangeable above the
 * seam). msquic delivers received stream data in callback chunks; the transport
 * reassembles the prefix and exactly-`len` body bytes into ONE refcounted
 * segment drawn from the injected `mem_backend_t` (ADR-0042 §2), handed up
 * OWNING when a view receiver is installed. TX copies each frame ONCE into a
 * heap buffer that msquic owns until its SEND_COMPLETE event (the msquic
 * buffer-lifetime contract) — the only library-held buffer, and only for the
 * duration of the in-flight send.
 */
class quic_transport_t : public transport_t {
   public:
    /** @brief The largest frame the length prefix may announce (16 MiB — the
     *         tcp_transport_t cap). A larger prefix is malformed: counted via
     *         @ref malformed_rx and the connection is shut down (a desynced
     *         stream cannot be trusted again). */
    static constexpr std::size_t kMaxFrame = 16u * 1024u * 1024u;

    /**
     * @brief DIAL mode: connect to @p peer_host:@p peer_port and open the
     *        frame stream (synchronous — the constructor waits for the QUIC
     *        handshake, the tcp_transport_t dial shape).
     *
     * Confirm with ok(); on failure no connection is live and the object is
     * inert. On success the bidirectional frame stream is started and frames
     * may flow immediately, so receivers must be installed before the peer
     * sends (the set_receiver contract).
     *
     * @param peer_host Peer hostname or dotted-quad IPv4 (e.g. "127.0.0.1").
     * @param peer_port Peer UDP port (host byte order).
     * @param tls       Server-certificate trust: a CA bundle, or the DEV-ONLY
     *                  no-verify flag (see @ref quic_dial_tls_t).
     * @param backend   The host-injected RX memory seam (ADR-0042 §2): each
     *                  inbound frame is reassembled into a fresh
     *                  exactly-`len`-byte segment from it (default: the process
     *                  heap). Exhaustion is backpressure — the frame is drained
     *                  off the stream, dropped, and dropped_rx() ticks; never
     *                  an OOM. Must outlive the transport.
     */
    quic_transport_t(const std::string& peer_host, std::uint16_t peer_port,
                     quic_dial_tls_t tls = {}, mem::mem_backend_t* backend = &mem::heap_backend(),
                     std::size_t max_frame = 0);

    /**
     * @brief LISTEN mode: serve QUIC on @p bind_port with the PEM certificate
     *        at @p cert_file / private key at @p key_file, accepting ONE
     *        inbound peer at a time (the tcp_transport_t / transport_ws_server
     *        one-peer model; re-accepts after a peer departs).
     *
     * Use ok() to confirm the listener started (bad cert paths fail here); the
     * bound port (an ephemeral 0 request resolved) is observable via
     * local_port(). The peer opens the frame stream.
     *
     * @param bind_port UDP port to listen on (host byte order; 0 → ephemeral).
     * @param cert_file PEM server-certificate path (tools/gen-dev-cert.sh
     *                  emits a self-signed dev pair).
     * @param key_file  PEM private-key path matching @p cert_file.
     * @param backend   The RX memory seam — see the DIAL constructor.
     */
    quic_transport_t(std::uint16_t bind_port, const std::string& cert_file,
                     const std::string& key_file,
                     mem::mem_backend_t* backend = &mem::heap_backend(), std::size_t max_frame = 0);

    /** @brief Shut the connection down, drain msquic callbacks, and release the
     *         msquic API (listener → stream → connection → registration order). */
    ~quic_transport_t() override;

    quic_transport_t(const quic_transport_t&) = delete;
    quic_transport_t& operator=(const quic_transport_t&) = delete;

    /**
     * @brief Send @p frame as one length-prefixed record on the frame stream.
     *
     * The prefix and frame bytes are copied ONCE into a single heap buffer
     * handed to msquic, which owns it until SEND_COMPLETE (the msquic
     * buffer-lifetime contract; the seam's spans are only borrowed for this
     * call, so the copy is unavoidable and minimal). No-op until a peer's
     * stream is up (and after teardown). Thread-safe.
     *
     * @param frame A complete TLV's bytes.
     */
    void send(std::span<const std::byte> frame) override;

    /**
     * @brief Scatter-gather send: the prefix + every span as ONE record.
     *
     * ONE gather copy: msquic's StreamSend does take multiple QUIC_BUFFERs,
     * but it requires every buffer to stay alive until SEND_COMPLETE while the
     * seam's spans are only borrowed for this call — so the spans are gathered
     * once into the single owned send buffer (prefix first), exactly the copy
     * the single-span overload makes.
     *
     * @param iov The frame's spans (a rope's `to_iovec()`), concatenated on
     *            the wire as one length-prefixed frame.
     */
    void send(std::span<const std::span<const std::byte>> iov) override;

    void set_receiver(receiver_t receiver) override;

    /**
     * @brief Install the owning inbound sink (ADR-0042): one frame = one
     *        refcounted segment from the injected backend, handed up as a view.
     *
     * Set before frames flow (the @ref set_receiver contract); fires on an
     * msquic worker thread. When installed it takes precedence over the span
     * receiver; when absent, the span receiver gets a borrowed span over the
     * same segment bytes (the segment is released when the callback returns).
     */
    void set_view_receiver(view_receiver_t receiver) override;

    /** @brief True — this transport honors @ref set_view_receiver (ADR-0042). */
    [[nodiscard]] bool delivers_views() const override { return true; }

    /** @brief DIAL: the handshake completed and the frame stream started;
     *         LISTEN: the listener is up on its port. */
    [[nodiscard]] bool ok() const noexcept;

    /** @brief LISTEN mode: the actual bound UDP port (resolves an ephemeral 0). */
    [[nodiscard]] std::uint16_t local_port() const noexcept;

    /** @brief Link state from the QUIC connection events: true from CONNECTED
     *         until the connection shuts down (peer/transport/idle). */
    [[nodiscard]] bool link_up() const noexcept;

    /** @brief Frames dropped because the RX backend was exhausted (backpressure,
     *         ADR-0039 §4 / ADR-0042 §2) — drained off the stream, never an OOM. */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept;

    /** @brief Malformed length prefixes seen (announced length > @ref kMaxFrame).
     *         Each one shuts the connection down — the stream has lost framing sync. */
    [[nodiscard]] std::uint64_t malformed_rx() const noexcept;

   private:
    struct impl_t;  // all msquic types live in the .cpp (no msquic in public headers)
    std::unique_ptr<impl_t> impl_;
};

/**
 * @brief The ready-to-register `quic` transport factory — how this module plugs
 *        into the transport catalog (the register_transport_type extension seam;
 *        the core has no `quic` builtin).
 *
 * Register at setup: `net.register_transport_type("quic", quic_transport_factory())`.
 * A subsequent `:children[]` SPEC whose config carries `kind = quic` then constructs
 * a @ref quic_transport_t from the parsed settings — DIAL: `addr` + `port`, using the
 * DEV-ONLY no-verify TLS mode (the config carries no trust material yet; a CA config
 * key is a follow-on, so config-dialed QUIC is dev-grade); LISTEN: `port` plus the
 * REQUIRED `cert`/`key` PEM-path config keys (QUIC is TLS 1.3 by construction;
 * tools/gen-dev-cert.sh emits a dev pair). `cert`/`key` are quic-PRIVATE config
 * keys: the factory parses them itself from the raw config SETTINGS TLV it receives —
 * they never appear in the shared `conn_settings_t`, which stays lean with only the
 * universal keys (the ADR-0043 §5 leanness ruling). Missing fields fail creation with
 * `TYPE_MISMATCH`; a socket that failed to come up fails with `NOT_FOUND`.
 * `keepalive` is ignored (#66 owns link lifecycle).
 *
 * @param rx_backend The ADR-0042 §2 receive-segment seam every constructed socket
 *                   draws its inbound frame segments from (default: the process
 *                   heap). Must outlive the constructed transports.
 * @return The factory functor for @ref transport_vertex_t::register_transport_type.
 */
[[nodiscard]] transport_vertex_t::transport_factory_t quic_transport_factory(
    mem::mem_backend_t* rx_backend = &mem::heap_backend());

}  // namespace tr::net
