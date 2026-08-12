/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_webtransport — the WebTransport-over-HTTP/3 endpoint (ADR-0043
 * Phase B), part of the SEPARATE `libtracer_quic` module target (it is the
 * same msquic investment: QUIC is the substrate WebTransport requires). A host
 * that serves browsers links the module and registers
 * webtransport_transport_factory; the core library itself has no msquic or H3
 * reference, no feature macro, no `webtransport` builtin — the catalog is
 * extended through register_transport_type, never modified.
 *
 * The endpoint speaks the minimal HTTP/3 layer a WebTransport session needs
 * (see src/wt_h3.hpp for the precise subset and its rationale): a SETTINGS
 * exchange advertising extended CONNECT + H3 datagrams + WebTransport, the
 * extended CONNECT handshake (`:method=CONNECT, :protocol=webtransport`) with
 * a 200 response, then ONE WebTransport bidirectional stream (opened by the
 * dialer / the browser's createBidirectionalStream()) carrying the SAME
 * 4-byte u32-LE length-prefix framing as tcp_transport_t / quic_transport_t —
 * the M6 framing seam, reachable from a browser. Datagram mode and per-flow
 * streams remain the ADR-0043 §3 staged follow-ons. This header keeps msquic
 * out of the public include surface (pimpl).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "libtracer/length_prefix_framer.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_vertex.hpp"

namespace tr::net {

/**
 * @brief DIAL-side TLS trust options for @ref webtransport_transport_t (the
 *        quic_dial_tls_t shape for the HTTP/3 dial).
 *
 * A browser trusts the server via WebCrypto `serverCertificateHashes` (dev) or
 * a real certificate; this C++ dial side — used by the self-contained e2e
 * tests and native clients — trusts a CA bundle, or skips validation in the
 * DEV-ONLY mode a self-signed dev cert requires.
 */
struct webtransport_dial_tls_t {
    std::string ca_file;             /**< @brief PEM CA bundle path to verify the server
                                                 certificate against (empty = the system
                                                 trust store). */
    bool insecure_no_verify = false; /**< @brief DEV ONLY: skip server certificate
                                                 validation entirely (self-signed dev
                                                 certs — tools/gen-dev-cert.sh). Never
                                                 enable in deployment. */
};

/**
 * @brief The WebTransport transport_t (ADR-0043 Phase B): an HTTP/3 extended
 *        CONNECT session whose ONE bidirectional WebTransport stream carries
 *        the 4-byte u32-LE length-prefix framing.
 *
 * LISTEN mode is the #92 deliverable: a browser (the TS
 * `@avatarsd-llc/libtracer-webtransport` package) or the DIAL mode of this
 * class connects with `new WebTransport(url)` semantics — H3 SETTINGS both
 * ways, extended CONNECT, 200 — and then opens one bidirectional stream that
 * becomes the frame channel. RX frames are reassembled into ONE refcounted
 * segment each from the injected `mem_backend_t` (ADR-0042 §2, owning
 * delivery); TX copies each frame once into the buffer msquic owns until
 * SEND_COMPLETE — exactly the quic_transport_t contracts.
 */
class webtransport_transport_t : public transport_t {
   public:
    /** @brief The largest frame the length prefix may announce — the shared
     *         length_prefix_framer::kDefaultMaxFrame (16 MiB) unless `:settings
     *         max_frame` tightens it. A larger prefix is malformed: counted via
     *         @ref malformed_rx and the session's connection is shut down. */
    static constexpr std::size_t kMaxFrame = length_prefix_framer::kDefaultMaxFrame;

    /**
     * @brief DIAL mode: establish a WebTransport session to
     *        `https://peer_host:peer_port/path` and open the frame stream
     *        (synchronous — the constructor waits for the QUIC handshake, the
     *        H3 SETTINGS/CONNECT exchange, and the 200).
     *
     * Confirm with ok(); on failure the object is inert. On success frames may
     * flow immediately, so receivers must be installed before the peer sends
     * (the set_receiver contract).
     *
     * @param peer_host Server hostname or dotted-quad IPv4 (the CONNECT
     *                  `:authority` host part).
     * @param peer_port Server UDP port (host byte order).
     * @param path      The CONNECT `:path` (a server-side namespace knob;
     *                  this server accepts any path — default "/"). Empty is
     *                  normalised to "/". A SPEC-created dialer reaches this
     *                  through the kind-private `path` config key (#1023).
     * @param tls       Server-certificate trust (see @ref webtransport_dial_tls_t).
     * @param backend   The host-injected RX memory seam (ADR-0042 §2); each
     *                  inbound frame lands in a fresh exactly-sized segment
     *                  from it. Exhaustion is backpressure (dropped_rx()),
     *                  never an OOM. Must outlive the transport.
     */
    webtransport_transport_t(const std::string& peer_host, std::uint16_t peer_port,
                             const std::string& path = "/", webtransport_dial_tls_t tls = {},
                             mem::mem_backend_t* backend = &mem::heap_backend(),
                             std::size_t max_frame = 0);

    /**
     * @brief LISTEN mode: serve WebTransport (ALPN `h3`) on @p bind_port with
     *        the PEM certificate at @p cert_file / key at @p key_file,
     *        accepting ONE session at a time (the quic_transport_t one-peer
     *        model; re-accepts after a peer departs).
     *
     * Use ok() to confirm the listener started; the bound port is observable
     * via local_port(). The session peer opens the frame stream. Browser dev
     * trust: `serverCertificateHashes` needs an ECDSA cert valid <= 14 days —
     * see the TS package README; the C++ DIAL side accepts any cert under its
     * DEV-ONLY no-verify mode.
     *
     * @param bind_port UDP port to listen on (host byte order; 0 → ephemeral).
     * @param cert_file PEM server-certificate path.
     * @param key_file  PEM private-key path matching @p cert_file.
     * @param backend   The RX memory seam — see the DIAL constructor.
     */
    webtransport_transport_t(std::uint16_t bind_port, const std::string& cert_file,
                             const std::string& key_file,
                             mem::mem_backend_t* backend = &mem::heap_backend(),
                             std::size_t max_frame = 0);

    /** @brief Shut the session down, drain msquic callbacks, and release the
     *         msquic API (listener → streams → connection → registration order). */
    ~webtransport_transport_t() override;

    webtransport_transport_t(const webtransport_transport_t&) = delete;
    webtransport_transport_t& operator=(const webtransport_transport_t&) = delete;

    /**
     * @brief Send @p frame as one length-prefixed record on the WebTransport
     *        frame stream.
     *
     * One copy into the buffer msquic owns until SEND_COMPLETE (the
     * quic_transport_t TX contract). No-op until the session's frame stream is
     * up (and after teardown). Thread-safe.
     *
     * @param frame A complete TLV's bytes.
     */
    void send(std::span<const std::byte> frame) override;

    /**
     * @brief Scatter-gather send: the prefix + every span as ONE record (one
     *        gather copy — the quic_transport_t rationale).
     *
     * @param iov The frame's spans (a rope's `to_iovec()`), concatenated on
     *            the wire as one length-prefixed frame.
     */
    void send(std::span<const std::span<const std::byte>> iov) override;

    /** @brief True — this transport honors @ref set_rope_receiver (ADR-0042). */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief The came-up predicate (#1059) — DIAL: the WebTransport session is
     *         established (200 received) and the frame stream started; LISTEN: the
     *         listener is up on its port. Answered at construction and never
     *         reverting; liveness is @ref link_up. */
    [[nodiscard]] bool ok() const noexcept;

    /** @brief LISTEN mode: the actual bound UDP port (resolves an ephemeral 0). */
    [[nodiscard]] std::uint16_t local_port() const noexcept;

    /** @brief Liveness (the @ref transport_t::link_up contract): true from the
     *         QUIC CONNECTED event until the connection (and with it the
     *         session) shuts down. Relaxed atomic. */
    [[nodiscard]] bool link_up() const noexcept override;

    /** @brief True once the WebTransport session is established — the extended
     *         CONNECT was accepted (LISTEN: request validated + 200 sent;
     *         DIAL: 200 received). */
    [[nodiscard]] bool session_up() const noexcept;

    /**
     * @brief The extended CONNECT `:path` of this endpoint's session — DIAL:
     *        the path this endpoint requests (known from construction); LISTEN:
     *        the path the peer's ACCEPTED CONNECT named, empty until one is
     *        accepted.
     *
     * The listener serves every path (it validates `:method`/`:protocol`, never
     * the resource), so this is an observation, not an admission decision: it
     * is how a host sees which resource a session asked for. Returns a copy —
     * thread-safe, and not on any frame path.
     */
    [[nodiscard]] std::string session_path() const;

    /** @brief Frames dropped because the RX backend was exhausted (backpressure,
     *         ADR-0042 §2) — drained off the stream, never an OOM. */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept;

    /** @brief Malformed length prefixes seen (announced length > @ref kMaxFrame).
     *         Each one shuts the connection down (framing sync is lost). */
    [[nodiscard]] std::uint64_t malformed_rx() const noexcept;

    /** @brief The interface-level snapshot (#932) — what a generic `transport_t*` reads.
     *         `dropped_tx` stays zero: the msquic egress does not count its shed sends yet
     *         (a follow-up), and this reports nothing rather than a fabricated number. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx(), malformed_rx(), 0};
    }

    /**
     * @brief Stream contexts the live session currently holds — the leak observable (#1163).
     *
     * A peer opens streams and this endpoint keeps one context per stream until the stream
     * finishes. The count is therefore bounded by what the peer has open *at once*
     * (`PeerBidiStreamCount` + `PeerUnidiStreamCount` + this endpoint's own H3 streams), and
     * NOT by how many the peer has ever opened. Before #1163 the second bound was the real
     * one: nothing reclaimed a finished stream, so open/close cycling grew this without limit.
     *
     * Exposed because a count that only ever rises is the signature of that class of bug and a
     * deployment cannot see it otherwise — the same reason @ref dropped_rx and @ref
     * malformed_rx are public. It is a live gauge, not a monotonic counter: it falls.
     */
    [[nodiscard]] std::size_t live_streams() const noexcept;

   private:
    struct impl_t;  // all msquic + H3 state lives in the .cpp
    std::unique_ptr<impl_t> impl_;
};

/**
 * @brief The ready-to-register `webtransport` transport factory — how the
 *        module plugs this kind into the transport catalog (the
 *        register_transport_type extension seam; no core builtin).
 *
 * Register at setup:
 * `net.register_transport_type("webtransport", webtransport_transport_factory())`.
 * A `:children[]` SPEC whose config carries `kind = webtransport` then
 * constructs a @ref webtransport_transport_t — DIAL: `addr` + `port` plus the
 * OPTIONAL `path` and trust keys below; LISTEN: `port` plus the REQUIRED
 * `cert`/`key` PEM-path config keys. All five are kind-PRIVATE config keys
 * parsed by this factory from the raw SPEC config TLV — they never appear on
 * the shared `conn_settings_t` (the ADR-0043 §5 leanness ruling). Missing
 * fields fail with `TYPE_MISMATCH`; a session that failed to come up fails with
 * `TRANSPORT_DOWN` — the TRANSIENT status, because the address resolved and it
 * was the link that did not come up (#929).
 *
 * **The DIAL `path` key (#1023)** carries the extended CONNECT `:path` — the
 * resource the WebTransport session is opened on. NAME, default `/`, so a SPEC
 * that omits it dials the same `/` this factory used to hard-code. Reaching a
 * server that serves its session elsewhere needs it: this DIAL side treats any
 * non-`200` answer to the extended CONNECT as a failed session, so a wrong
 * resource surfaces as `TRANSPORT_DOWN` from creation — the same status a
 * rejected certificate gives. The key is kind-private, so it does not collide
 * with the `can` kind's unrelated `path` key (an advertised group path).
 *
 * **A SPEC-created dialer verifies the server certificate (#918)** — the trust
 * mode is whatever @ref webtransport_dial_tls_t defaults to, so with neither
 * DIAL key present a certificate that does not chain to the system trust store
 * is REFUSED (creation answers `TRANSPORT_DOWN`). The same two DIAL-side keys as
 * the `quic` kind move it: `ca` (NAME, a PEM CA-bundle path) verifies against that
 * bundle instead, and `insecure` (VALUE u8, default 0) set to `1` skips
 * validation entirely — DEV ONLY, and explicit on purpose.
 *
 * @param rx_backend The ADR-0042 §2 receive-segment seam every constructed
 *                   endpoint draws inbound frame segments from (default: the
 *                   process heap). Must outlive the constructed transports.
 * @return The factory functor for @ref transport_vertex_t::register_transport_type.
 */
[[nodiscard]] transport_vertex_t::transport_factory_t webtransport_transport_factory(
    mem::mem_backend_t* rx_backend = &mem::heap_backend());

}  // namespace tr::net
