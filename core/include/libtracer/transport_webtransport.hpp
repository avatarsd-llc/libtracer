/**
 * @file
 * @brief The WebTransport-over-HTTP/3 endpoint (ADR-0043 Phase B), in the separate
 *        `libtracer_quic` module.
 *
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
#include <span>
#include <string_view>

#include "libtracer/length_prefix_framer.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/tls_profile.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_factory.hpp"

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
    std::string_view ca_file;        /**< @brief PEM CA bundle path to verify the server
                                                 certificate against (empty = the system
                                                 trust store). Borrowed for the
                                                 constructor call only (#1780). */
    bool insecure_no_verify = false; /**< @brief DEV ONLY: skip server certificate
                                                 validation entirely (self-signed dev
                                                 certs — tools/gen-dev-cert.sh). Never
                                                 enable in deployment. */
};

/**
 * @brief `webtransport_transport_t`'s knobs as one aggregate (#1593), after the address (and,
 *        on a dial, the CONNECT path and TLS trust).
 */
struct webtransport_config_t {
    /** @brief The link's memory (@ref link_memory_t). `rx`: each inbound frame lands in a
     *         fresh exactly-sized segment from it; exhaustion is backpressure (`dropped_rx()`),
     *         never an OOM. */
    link_memory_t memory{};
    /** @brief Per-link RX cap (`:settings max_frame`); 0 = the default. */
    std::size_t max_frame = 0;
    /**
     * @brief DIAL only — hold inbound FRAMES until `start_receiving` (#1101, ADR-0081 §2).
     *        The session is established as always, but the frame channel's bytes are left in
     *        msquic's per-stream flow-control window, so a server that pushes the instant the
     *        session comes up cannot be decoded into a sink the owner has not installed yet.
     *        The LISTEN constructor ignores it.
     */
    bool defer_rx = false;
    /**
     * @brief Pre-auth H3 handshake budget; 0 = `kMaxHandshakeBytes`. TIGHTEN-ONLY
     *        (`handshake_cap`). A dial bounds the CONNECT RESPONSE's field section; a listener
     *        bounds per-stream classification/HEADERS accumulation and a HEADERS frame's
     *        DECLARED length.
     */
    std::size_t max_handshake = 0;
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
     *         length_prefix_framer_t::kDefaultMaxFrame (16 MiB) unless `:settings
     *         max_frame` tightens it. A larger prefix is malformed: counted via
     *         @ref malformed_rx and the session's connection is shut down. */
    static constexpr std::size_t kMaxFrame = length_prefix_framer_t::kDefaultMaxFrame;

    /**
     * @brief The largest H3 HANDSHAKE a PRE-AUTH peer may make this node buffer
     *        (16 KiB) — the ceiling `max_handshake` tightens against (#1408).
     *
     * A different budget from @ref kMaxFrame and deliberately so: a frame arrives on an
     * established session under whatever the deployment allowed, while H3 classification
     * bytes arrive from a host that has done nothing but complete a QUIC handshake and
     * open one stream — no session, no ACL, no subscription, no router, nothing
     * authenticated. Pre-auth work is REFUSED EARLY, not carefully allocated: an
     * accumulation that would pass this budget is refused BEFORE the byte that would
     * exceed it is copied, and a HEADERS or unknown/GREASE frame DECLARING more than it is
     * refused before one body byte is buffered.
     *
     * The two dispositions this module already distinguishes are unchanged and must not be
     * merged: over-budget is a statement about the PEER, so it shuts the connection down
     * with the bad-request code; running out of memory is a statement about US, so it is
     * stream-scoped (#919) or count-then-close (@ref refused_sessions, #934).
     */
    static constexpr std::size_t kMaxHandshakeBytes = 16u * 1024u;

    /**
     * @brief Resolve a `max_handshake` request into the honored budget — TIGHTEN-ONLY
     *        against @ref kMaxHandshakeBytes, exactly as
     *        `length_prefix_framer_t::configured_cap` is against `kDefaultMaxFrame`.
     *
     * `0` (unset) keeps the default; a nonzero value yields
     * `min(max_handshake, kMaxHandshakeBytes)`. The value arrives through a config-writable
     * key (`webtransport`-private `max_handshake`), and a config-writable key must never
     * RAISE a pre-auth bound — only narrow it.
     *
     * @note The same shape `ws_server_transport_t::handshake_cap` carries for the WS plane
     *       (#1407), deliberately spelled rather than shared: `transport_ws.hpp` sits
     *       behind `LIBTRACER_TRANSPORT_WS` and can be configured OFF, so consuming its
     *       symbol here would make an optional core module a hard dependency of this
     *       optional transport module.
     */
    [[nodiscard]] static constexpr std::size_t handshake_cap(std::size_t max_handshake) noexcept {
        if (max_handshake == 0) return kMaxHandshakeBytes;
        return max_handshake < kMaxHandshakeBytes ? max_handshake : kMaxHandshakeBytes;
    }

    /**
     * @brief DIAL mode: establish a WebTransport session to
     *        `https://peer_host:peer_port/path` and open the frame stream
     *        (synchronous — the constructor waits for the QUIC handshake, the
     *        H3 SETTINGS/CONNECT exchange, and the 200).
     *
     * Confirm with ok(); on failure the object is inert. On success frames may
     * flow immediately, so receivers must be installed before the peer sends
     * (the set_receiver contract) — or the link is constructed with `defer_rx`
     * and armed with @ref start_receiving once they are.
     *
     * @param peer_host Server hostname or dotted-quad IPv4 (the CONNECT
     *                  `:authority` host part).
     * @param peer_port Server UDP port (host byte order).
     * @param path      The CONNECT `:path` (a server-side namespace knob;
     *                  this server accepts any path — default "/"). Empty is
     *                  normalised to "/". A SPEC-created dialer reaches this
     *                  through the kind-private `path` config key (#1023).
     * @param tls       Server-certificate trust (see @ref webtransport_dial_tls_t).
     * @param config    The link's knobs (@ref webtransport_config_t): memory, receive cap,
     *                  deferred receive, handshake budget.
     */
    webtransport_transport_t(std::string_view peer_host, std::uint16_t peer_port,
                             std::string_view path = "/", webtransport_dial_tls_t tls = {},
                             const webtransport_config_t& config = {});

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
     * @param config    The link's knobs (@ref webtransport_config_t); `defer_rx` is
     *                  DIAL-only and ignored here.
     */
    webtransport_transport_t(std::uint16_t bind_port, std::string_view cert_file,
                             std::string_view key_file, const webtransport_config_t& config = {});

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

    /**
     * @brief Open this link's delivery gate — the second phase of a `defer_rx`
     *        DIAL bring-up (#1101, ADR-0081 §2).
     *
     * Re-enables msquic's receive on the WebTransport frame stream, so everything
     * the peer pushed while the owner was installing its sinks is re-indicated and
     * delivered rather than dropped. IDEMPOTENT and inert on every other link — a
     * one-phase dialer, a listener, and a dial that never came up all have nothing
     * to arm — because `%transport_vertex_t::make_connection` calls it on every
     * link it wires.
     */
    void start_receiving() override;

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
     * is how a host sees which resource a session asked for. Copies into the caller's
     * buffer (#1780: no owning string crosses the API) — thread-safe, and not on any
     * frame path.
     *
     * STABLE for the life of a session (#1410): a second extended CONNECT on a live
     * session is refused at stream scope, so a peer that has already been answered cannot
     * rewrite what a host observes here. It changes only when the session itself does —
     * connection teardown, or the one-peer replacement path accepting a new peer.
     *
     * @param out Where the path goes: its first `min(out.size(), length)` characters,
     *            no terminator.
     * @return The path's full length — larger than `out.size()` means it was truncated.
     */
    [[nodiscard]] std::size_t session_path(std::span<char> out) const;

    /** @brief Frames dropped because the RX backend was exhausted (backpressure,
     *         ADR-0042 §2) — drained off the stream, never an OOM. */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept;

    /** @brief Malformed length prefixes seen (announced length > @ref kMaxFrame).
     *         Each one shuts the connection down (framing sync is lost). */
    [[nodiscard]] std::uint64_t malformed_rx() const noexcept;

    /** @brief Frames shed on the way OUT (#932): a record over THIS CONNECTION's cap
     *         (`:settings max_frame`, resolved tighten-only against @ref kMaxFrame — it is
     *         the same number the peer measures the arriving prefix by, #1409), no live
     *         peer stream to write to (dialing / torn down), or a `StreamSend` msquic
     *         refused. H3 handshake material is not counted — it is not a frame. */
    [[nodiscard]] std::uint64_t dropped_tx() const noexcept;

    /**
     * @brief Extended CONNECT handshakes REFUSED because the node could not afford to
     *        answer them ([#934](https://github.com/avatarsd-llc/libtracer/issues/934)).
     *
     * The LISTEN side reaches two allocations on the strength of one unauthenticated
     * peer's HEADERS frame: recording the requested `:path`, and the one owned copy of the
     * 200 response msquic borrows until SEND_COMPLETE. Both are nothrow, and a refusal is
     * COUNT-THEN-CLOSE — this counter, then the connection is shut down with the
     * bad-request code, so the peer's memory is freed at once and nothing is left
     * half-established. Never moves on a healthy node; a rising value means the node is
     * shedding pre-auth work under memory pressure, which is the event the standing
     * "no peer-provoked path may abort the node" commitment
     * (docs/reference/07-host-embedding.md) makes observable rather than fatal.
     *
     * Distinct from @ref dropped_rx (a FRAME shed for backpressure on an established
     * session) and from the stream-scoped handshake-buffer refusal, which aborts one
     * stream and leaves an already-established session alone (#919).
     */
    [[nodiscard]] std::uint64_t refused_sessions() const noexcept;

    /** @brief The interface-level snapshot (#932) — the concrete accessors above, as the
     *         one shape a generic `transport_t*` holder reads. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx(), malformed_rx(), dropped_tx()};
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

    /** @brief The pre-auth handshake budget actually honored: `handshake_cap(max_handshake)`
     *         as constructed (#1408). It names no backend — H3 handshake bytes accumulate in
     *         the stream context's own buffer, not in an RX segment, so unlike the frame cap
     *         there is no second injected resource to take the min against. Answered on
     *         every link, including one whose dial never came up. */
    [[nodiscard]] std::size_t effective_max_handshake() const noexcept;

   private:
    struct impl_t;  // all msquic + H3 state lives in the .cpp
    /** @brief The endpoint, drawn from `config.memory.io` (default the net sub-pool); empty
     *         when that store refused it, which leaves the link inert (#1780). */
    mem::poly_ptr_t<impl_t> impl_;
};

/**
 * @brief The ready-to-register `webtransport` transport factory — how the
 *        module plugs this kind into the transport catalog (the
 *        register_transport_type extension seam; no core builtin).
 *
 * Register at setup:
 * `net.register_transport_type("webtransport", webtransport_transport_factory(profiles))`.
 * A `:children[]` SPEC whose config carries `kind = webtransport` then
 * constructs a @ref webtransport_transport_t — DIAL: `addr` + `port` plus the
 * OPTIONAL `path` and trust keys below; LISTEN: `port` plus a profile that
 * carries the served credential. Both roles additionally read the OPTIONAL
 * `tls` profile name and `max_handshake` budget (#1408). All four are kind-PRIVATE config keys
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
 * **The `max_handshake` key (#1408)** carries the pre-auth H3 handshake budget in
 * bytes — VALUE u32, default `0` = @ref webtransport_transport_t::kMaxHandshakeBytes
 * (16 KiB), read on BOTH roles and TIGHTEN-ONLY
 * (@ref webtransport_transport_t::handshake_cap clamps a larger request). It is the
 * injected spelling of a bound that used to be a file-local literal, so the
 * deployment sets the ceiling rather than the compiler.
 *
 * **TLS material is app-owned**, exactly as for the `quic` kind: a SPEC never
 * carries a file path. The certificate, key and CA bundle come from @p profiles,
 * and the SPEC's `tls` key (NAME) can at most select one by name — absent selects
 * the profile named `""`, and a name the table does not hold is refused with
 * `TYPE_MISMATCH` before any file is opened. A LISTEN serves its profile's
 * `cert_file`/`key_file` and is refused without them.
 *
 * **A SPEC-created dialer verifies the server certificate (#918)** — against its
 * profile's `ca_file`, or, with no profile or no anchor in it, against the system
 * trust store; a certificate that does not chain is REFUSED (creation answers
 * `TRANSPORT_DOWN`). `insecure` (VALUE u8, default 0) set to `1` skips validation
 * entirely — DEV ONLY, and it requires the build capability
 * @ref tr::graph::default_config_t::kAllowInsecureTls (default `false`): without it
 * a SPEC carrying `insecure` = nonzero is REFUSED at creation with
 * `PERMISSION_DENIED` and counted in `%webtransport_insecure_refusals()` (below), on either
 * role. `insecure = 0` is accepted on every build.
 *
 * @param profiles   The app's TLS profiles (default: none — dials verify against
 *                   the system trust store, listens are refused). The factory keeps
 *                   the span, not a copy: the table and the strings it views must
 *                   outlive the factory and every transport it constructs.
 * @param rx_backend The ADR-0042 §2 receive-segment seam every constructed
 *                   endpoint draws inbound frame segments from (default: the
 *                   process net sub-pool). Must outlive the constructed transports.
 * @return The factory functor for @ref transport_vertex_t::register_transport_type.
 */
[[nodiscard]] transport_factory_t webtransport_transport_factory(
    std::span<const tls_profile_t> profiles = {},
    mem::mem_backend_t* rx_backend = &mem::net_backend());

/**
 * @brief How many `webtransport` SPECs this process refused for carrying `insecure` =
 *        nonzero on a build without @ref tr::graph::default_config_t::kAllowInsecureTls.
 *
 * Process-wide and monotonic (a relaxed atomic, touched only on the refusal). Always `0` on
 * a build that binds the capability, where the key is honoured instead.
 */
[[nodiscard]] std::uint64_t webtransport_insecure_refusals() noexcept;

}  // namespace tr::net
