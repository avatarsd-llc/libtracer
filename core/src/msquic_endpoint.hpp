/**
 * @file
 * @brief msquic_endpoint — the MODULE-PRIVATE msquic-mechanical base of the
 *        two QUIC transports (quic_transport_t and webtransport_transport_t).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This header lives under src/ (not include/), the wt_h3.hpp precedent: it
 * includes msquic.h, so it can never be public API — it is shared only by the
 * two translation units of the `libtracer_quic` module target.
 *
 * Everything msquic-MECHANICAL the two transports used to restate lives here,
 * one home for each contract:
 *
 *   - The msquic object tree (api → registration → configuration, listener,
 *     connection → stream) and its handle-ownership discipline: callbacks
 *     never close handles — they only flip flags and adopt streams; every
 *     close belongs to the destructor's teardown() or the listener's one-peer
 *     replacement path.
 *   - The 5-step teardown ordering (ASan-audited): ListenerClose first (blocks
 *     until listener callbacks drain — no new peer can slip in), then
 *     StreamClose + ConnectionClose the live peer (each blocks until that
 *     handle's callbacks drain; pending sends complete Canceled, freeing their
 *     buffers), then ConfigurationClose, then RegistrationClose (asserts every
 *     child handle is gone), then MsQuicClose.
 *   - The TSan annotations restating msquic's two happens-before contracts —
 *     StreamSend → SEND_COMPLETE buffer ownership, and the per-connection
 *     serialization of callbacks across msquic's worker threads — which TSan
 *     cannot see because libmsquic.so is not instrumented. No-ops outside
 *     TSan builds.
 *   - The one-copy TX contract (send_ctx_t): msquic owns the send buffer
 *     until SEND_COMPLETE (Canceled included), the seam's spans are only
 *     borrowed for the send() call, so ONE copy into the single owned heap
 *     buffer is unavoidable — and the only library-held buffer.
 *   - The RX length-prefix reassembly (the shared length_prefix_framer) and
 *     its delivery through the outer transport's receiver_slot_t.
 *   - The dial-side handshake rendezvous (the constructor blocks until
 *     CONNECTED or shutdown, the tcp_transport_t dial shape) and the shared
 *     LISTEN bring-up (listener start + ephemeral-port resolution).
 *
 * The derived transports keep ONLY their variance points — stream
 * classification/adoption, per-peer harvest shape, and (for webtransport) the
 * H3 handshake — expressed as the runtime virtuals below (appropriate per
 * ADR-0047 §4: peer arrival/departure is wiring-frequency, not hot-path).
 *
 * Threading model: msquic invokes all callbacks on its worker threads and
 * serializes them per connection, so the RX reassembly state needs no lock;
 * the handle slots are mutex-guarded exactly like tcp_transport_t, and the
 * receivers live in the outer transport_t's receiver_slot_t (which does its
 * own locking).
 */
#pragma once

#include <msquic.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/length_prefix_framer.hpp"
#include "libtracer/receiver_slot.hpp"

#if defined(__SANITIZE_THREAD__)
#define LIBTRACER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LIBTRACER_TSAN 1
#endif
#endif
#ifdef LIBTRACER_TSAN
#include <sanitizer/tsan_interface.h>
#endif

namespace tr::net {

/** @brief TSan release edge on @p p (no-op outside TSan builds). */
inline void tsan_release(void* p) {
#ifdef LIBTRACER_TSAN
    __tsan_release(p);
#else
    (void)p;
#endif
}

/** @brief TSan acquire edge on @p p (no-op outside TSan builds). */
inline void tsan_acquire(void* p) {
#ifdef LIBTRACER_TSAN
    __tsan_acquire(p);
#else
    (void)p;
#endif
}

/**
 * @brief RAII edge for one msquic callback invocation: acquire on entry (see
 *        what the previous callback / the constructor published), release on
 *        exit (publish for the next callback and the destructor) — the
 *        per-connection serialization msquic guarantees, restated for TSan.
 */
struct tsan_cb_guard_t {
    void* p; /**< @brief The annotated address (the endpoint or a stream ctx). */
    /** @brief Acquire on callback entry. */
    explicit tsan_cb_guard_t(void* ptr) : p(ptr) { tsan_acquire(p); }
    /** @brief Release on callback exit. */
    ~tsan_cb_guard_t() { tsan_release(p); }
};

/** @brief The u32-LE length prefix (transport framing) — 4 bytes. */
inline constexpr std::size_t kPrefixBytes = 4;
/** @brief App-layer connection-shutdown code: framing lost on the frame stream. */
inline constexpr std::uint64_t kAppErrMalformed = 0x1;
/** @brief Dial-constructor rendezvous budget per stage (milliseconds). */
inline constexpr std::uint32_t kHandshakeWaitMs = 10'000;

/**
 * @brief One in-flight send: the QUIC_BUFFER msquic reads from plus the owned
 *        bytes.
 *
 * msquic owns the bytes until SEND_COMPLETE (its documented buffer-lifetime
 * contract; Canceled included, so shutdown leaks nothing); the seam's spans
 * are only borrowed for the send() call, so ONE copy into this heap buffer is
 * unavoidable — and the only library-held buffer, freed by SEND_COMPLETE.
 * Used both for length-prefixed frame records and for raw handshake bytes.
 */
struct send_ctx_t {
    QUIC_BUFFER buf{};            /**< @brief The buffer descriptor handed to StreamSend. */
    std::vector<std::byte> bytes; /**< @brief The owned copy of `prefix ++ frame` (or raw). */

    /** @brief A length-prefixed frame record: prefix ++ frame (frame filled by caller). */
    explicit send_ctx_t(std::size_t frame_len) : bytes(kPrefixBytes + frame_len) {
        detail::store_le(std::span(bytes).first(kPrefixBytes),
                         static_cast<std::uint32_t>(frame_len));
        arm();
    }
    /** @brief Raw bytes (H3 handshake material) — no prefix. */
    explicit send_ctx_t(std::vector<std::uint8_t> raw)
        : bytes(reinterpret_cast<std::byte*>(raw.data()),
                reinterpret_cast<std::byte*>(raw.data()) + raw.size()) {
        arm();
    }

    /** @brief Point the QUIC_BUFFER at the owned bytes. */
    void arm() {
        buf.Length = static_cast<std::uint32_t>(bytes.size());
        buf.Buffer = reinterpret_cast<uint8_t*>(bytes.data());
    }
};

/**
 * @brief The msquic-mechanical endpoint base each QUIC transport's impl_t
 *        derives from (the posix_endpoint_t precedent, msquic-flavored).
 *
 * Owns the msquic object tree, the shared bring-up (open_common / dial /
 * listen_start), the handshake rendezvous, the one-peer listener-replacement
 * skeleton, the RX reassembly + delivery, the one-copy TX path, and the
 * 5-step teardown. Members are public on purpose: this is an
 * implementation-detail base of the module's pimpl structs (exactly the
 * access their all-public impl_t members had before the extraction), never
 * visible outside the two TUs and their target.
 *
 * @warning Derived impl_t destructors MUST call @ref teardown as their first
 *          act (it dispatches to the @ref harvest_and_close_streams virtual,
 *          which must still see the derived object alive).
 */
class msquic_endpoint_t {
   public:
    /** @brief The largest frame the length prefix may announce — the shared
     *         length_prefix_framer::kDefaultMaxFrame (16 MiB), restated by both
     *         public transport classes. */
    static constexpr std::size_t kMaxFrame = length_prefix_framer::kDefaultMaxFrame;

    /** @brief Constructs inert: no msquic state opened yet. */
    msquic_endpoint_t() = default;

    msquic_endpoint_t(const msquic_endpoint_t&) = delete;
    msquic_endpoint_t& operator=(const msquic_endpoint_t&) = delete;

    /**
     * @name msquic object tree (parent → child): api → registration →
     *       configuration, listener, connection → stream. All handles owned
     *       here (see the file header for the ownership discipline).
     * @{
     */
    const QUIC_API_TABLE* api = nullptr; /**< @brief The msquic API table (MsQuicOpen2). */
    HQUIC reg = nullptr;                 /**< @brief The registration handle. */
    HQUIC config = nullptr;              /**< @brief The configuration (ALPN + credential). */
    HQUIC listener = nullptr;            /**< @brief LISTEN mode only. */
    bool listen = false;                 /**< @brief LISTEN (vs DIAL) role flag. */
    bool open_ok = false; /**< @brief DIAL: handshake+stream up; LISTEN: listener started. */
    std::uint16_t bound_port = 0; /**< @brief LISTEN: the resolved bound UDP port. */
    /** @} */

    /**
     * @name RX segment source for frame reassembly (ADR-0042 §2) + counters.
     * @{
     */
    mem::mem_backend_t* backend = nullptr;      /**< @brief The injected RX memory seam. */
    std::size_t max_frame = kMaxFrame;          /**< @brief Per-connection RX cap (:settings). */
    std::atomic<std::uint64_t> dropped_rx{0};   /**< @brief Backpressure-dropped frames. */
    std::atomic<std::uint64_t> malformed_rx{0}; /**< @brief Malformed length prefixes seen. */
    /** @} */

    /**
     * @brief The outer transport's delivery-tier slot (receiver_slot.hpp):
     *        tier select and its own locking live there. Wired to the
     *        transport_t base's rx_ at construction.
     */
    receiver_slot_t<>* rx = nullptr;

    /**
     * @name The single live peer: its connection + frame stream. conn_m guards
     *       the slots (send/replace/teardown); the handles are only CLOSED by
     *       the destructor or the listener replacement path, never by
     *       callbacks.
     * @{
     */
    std::mutex conn_m;            /**< @brief Guards conn / frame_stream (and derived slots). */
    HQUIC conn = nullptr;         /**< @brief The live peer's connection handle. */
    HQUIC frame_stream = nullptr; /**< @brief The adopted frame channel send() writes to. */
    std::atomic<bool> up{false};  /**< @brief CONNECTED .. shutdown (the link state). */
    /** @} */

    /**
     * @name DIAL handshake rendezvous: the ctor blocks until CONNECTED or
     *       shutdown (derived transports may add further stages on the same
     *       mutex/cv — the webtransport session stage).
     * @{
     */
    std::mutex wait_m;               /**< @brief Guards the rendezvous flags. */
    std::condition_variable wait_cv; /**< @brief Signaled by the stage signals. */
    bool handshake_done = false;     /**< @brief QUIC handshake stage resolved. */
    bool handshake_ok = false;       /**< @brief QUIC handshake stage outcome. */
    /** @} */

    /**
     * @brief RX frame reassembly across msquic RECEIVE chunks. Touched only on
     *        the frame stream's callback (msquic serializes per-connection
     *        callbacks) and reset by the listener before a replacement peer's
     *        stream can start.
     */
    length_prefix_framer framer_;

    /** @brief Reset the RX reassembly state for a replacement peer. */
    void reset_rx() { framer_.reset(); }

    /** @brief Resolve the QUIC-handshake rendezvous stage (idempotent). */
    void signal_handshake(bool ok) {
        {
            const std::lock_guard lock(wait_m);
            if (handshake_done) return;
            handshake_done = true;
            handshake_ok = ok;
        }
        wait_cv.notify_all();
    }

    /**
     * @brief Wait for one rendezvous stage (dial ctor), bounded by
     *        @ref kHandshakeWaitMs.
     *
     * @param done The stage's done flag (guarded by @ref wait_m).
     * @param ok   The stage's outcome flag.
     * @return True when the stage resolved successfully within the budget.
     */
    bool wait_stage(bool& done, bool& ok) {
        std::unique_lock lock(wait_m);
        wait_cv.wait_for(lock, std::chrono::milliseconds(kHandshakeWaitMs),
                         [&done] { return done; });
        return done && ok;
    }

    /** @brief Shut the live connection down with app-layer @p code (no close —
     *         the handle stays owned by the destructor / replacement path). */
    void shutdown_conn(std::uint64_t code) {
        HQUIC c = nullptr;
        {
            const std::lock_guard lock(conn_m);
            c = conn;
        }
        if (c != nullptr) api->ConnectionShutdown(c, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, code);
    }

    /**
     * @brief Feed one msquic RECEIVE chunk through the shared length-prefix
     *        reassembler (length_prefix_framer).
     *
     * Each reassembled frame goes up through the slot: tier select (owning
     * rope sink, else the same segment bytes borrowed) lives in
     * receiver_slot.hpp. A malformed (oversize) prefix cannot be re-framed:
     * it is counted and the connection is shut down.
     *
     * @return False when the connection was shut down — the caller stops
     *         consuming this event's remaining chunks.
     */
    bool on_rx_chunk(const std::uint8_t* p, std::size_t n) {
        const auto res =
            framer_.feed(*backend, max_frame, reinterpret_cast<const std::byte*>(p), n,
                         [this](view::segment_ptr_t seg, std::size_t len) {
                             rx->deliver(view::view_t::over(std::move(seg)).subview(0, len));
                         });
        if (res.dropped != 0) dropped_rx.fetch_add(res.dropped, std::memory_order_relaxed);
        if (res.malformed) {
            malformed_rx.fetch_add(1, std::memory_order_relaxed);
            shutdown_conn(kAppErrMalformed);
            return false;
        }
        return true;
    }

    /**
     * @name TX — the one-copy contract (see @ref send_ctx_t).
     * @{
     */

    /** @brief The SEND_COMPLETE tail: msquic is done with the send buffer
     *         (Canceled included) — the one library-held buffer's lifetime
     *         ends exactly here. Pairs with the sender's tsan_release. */
    static void complete_send(void* client_ctx) {
        tsan_acquire(client_ctx);
        delete static_cast<send_ctx_t*>(client_ctx);
    }

    /** @brief Send raw @p bytes (H3 handshake material — no prefix) on
     *         @p stream; msquic owns the buffer until SEND_COMPLETE. */
    void send_raw(HQUIC stream, std::vector<std::uint8_t> bytes) {
        auto ctx = std::make_unique<send_ctx_t>(std::move(bytes));
        tsan_release(ctx.get());  // pairs with SEND_COMPLETE's acquire
        if (QUIC_SUCCEEDED(api->StreamSend(stream, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx.get())))
            (void)ctx.release();
    }

    /**
     * @brief Send @p frame as one length-prefixed record on the frame stream.
     *
     * ONE copy into the buffer msquic owns until SEND_COMPLETE. No-op until a
     * peer's frame stream is up (and after teardown) — drop, like tcp.
     * Thread-safe (the stream slot is read under @ref conn_m).
     */
    void send_frame(std::span<const std::byte> frame) {
        if (frame.size() > kMaxFrame) return;  // the peer would reject it as malformed
        auto ctx = std::make_unique<send_ctx_t>(frame.size());
        if (!frame.empty())
            std::memcpy(ctx->bytes.data() + kPrefixBytes, frame.data(), frame.size());
        const std::lock_guard lock(conn_m);
        if (frame_stream == nullptr) return;  // no peer stream (yet / anymore) — drop
        // On success msquic owns the buffer until SEND_COMPLETE (which deletes
        // the ctx); on failure no event will fire, so the unique_ptr frees it.
        tsan_release(ctx.get());  // pairs with SEND_COMPLETE's acquire
        if (QUIC_SUCCEEDED(
                api->StreamSend(frame_stream, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx.get())))
            (void)ctx.release();
    }

    /**
     * @brief Scatter-gather send: the prefix + every span as ONE record.
     *
     * ONE gather copy: msquic's multi-buffer StreamSend cannot help here
     * because every QUIC_BUFFER must outlive the call (until SEND_COMPLETE)
     * while the seam's spans are borrowed only for this call — so the rope's
     * spans are gathered once into the single owned send buffer, behind the
     * prefix.
     */
    void send_frame(std::span<const std::span<const std::byte>> iov) {
        std::size_t total = 0;
        for (const auto& s : iov) total += s.size();
        if (total > kMaxFrame) return;  // the peer would reject it as malformed
        auto ctx = std::make_unique<send_ctx_t>(total);
        std::size_t off = kPrefixBytes;
        for (const auto& s : iov) {
            if (s.empty()) continue;
            std::memcpy(ctx->bytes.data() + off, s.data(), s.size());
            off += s.size();
        }
        const std::lock_guard lock(conn_m);
        if (frame_stream == nullptr) return;
        tsan_release(ctx.get());  // pairs with SEND_COMPLETE's acquire
        if (QUIC_SUCCEEDED(
                api->StreamSend(frame_stream, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx.get())))
            (void)ctx.release();
    }
    /** @} */

    /**
     * @name Bring-up (shared by both roles and both transports).
     * @{
     */

    /**
     * @brief Shared bring-up: the api table, registration, and configuration
     *        (with the mode's credential).
     *
     * Returns false with everything not-yet-opened left null, so
     * @ref teardown unwinds partial construction cleanly.
     *
     * @param reg_name The msquic registration app name (per-transport).
     * @param alpn     The ALPN the endpoint negotiates (per-transport).
     */
    bool open_common(const char* reg_name, const QUIC_BUFFER& alpn, const QUIC_SETTINGS& settings,
                     const QUIC_CREDENTIAL_CONFIG& cred) {
        if (QUIC_FAILED(MsQuicOpen2(&api))) {
            api = nullptr;
            return false;
        }
        const QUIC_REGISTRATION_CONFIG reg_cfg{reg_name, QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        if (QUIC_FAILED(api->RegistrationOpen(&reg_cfg, &reg))) return false;
        if (QUIC_FAILED(api->ConfigurationOpen(reg, &alpn, 1, &settings, sizeof(settings), nullptr,
                                               &config)))
            return false;
        return !QUIC_FAILED(api->ConfigurationLoadCredential(config, &cred));
    }

    /**
     * @brief DIAL bring-up through the QUIC handshake: client credential
     *        (CA bundle or the DEV-ONLY no-verify escape), open_common,
     *        ConnectionOpen/Start, then the handshake rendezvous.
     *
     * The tcp_transport_t dial shape: blocks until the handshake resolves so
     * the caller's ok() is meaningful at construction (CONNECTED, or any
     * shutdown / timeout = failure).
     *
     * @return True when the handshake completed; on false the caller returns
     *         with open_ok still false and teardown() unwinds what opened.
     */
    bool dial(const char* reg_name, const QUIC_BUFFER& alpn, const QUIC_SETTINGS& settings,
              const std::string& ca_file, bool insecure_no_verify, const std::string& peer_host,
              std::uint16_t peer_port) {
        QUIC_CREDENTIAL_CONFIG cred{};
        cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
        unsigned flags = QUIC_CREDENTIAL_FLAG_CLIENT;
        if (insecure_no_verify) {
            flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;  // DEV ONLY (self-signed)
        } else if (!ca_file.empty()) {
            flags |= QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE;
            cred.CaCertificateFile = ca_file.c_str();
        }
        cred.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(flags);
        if (!open_common(reg_name, alpn, settings, cred)) return false;

        if (QUIC_FAILED(api->ConnectionOpen(reg, &msquic_endpoint_t::conn_cb, this, &conn))) {
            conn = nullptr;
            return false;
        }
        tsan_release(this);  // publish the constructed endpoint to the callbacks
        if (QUIC_FAILED(api->ConnectionStart(conn, config, QUIC_ADDRESS_FAMILY_UNSPEC,
                                             peer_host.c_str(), peer_port)))
            return false;
        return wait_stage(handshake_done, handshake_ok);
    }

    /**
     * @brief LISTEN bring-up: certificate credential, open_common,
     *        ListenerOpen/Start, and the ephemeral-port resolution. Sets
     *        @ref open_ok on success (bad cert/key paths fail HERE, in
     *        ConfigurationLoadCredential).
     */
    bool listen_start(const char* reg_name, const QUIC_BUFFER& alpn, const QUIC_SETTINGS& settings,
                      const std::string& cert_file, const std::string& key_file,
                      std::uint16_t bind_port) {
        listen = true;
        QUIC_CERTIFICATE_FILE cert{};
        cert.PrivateKeyFile = key_file.c_str();
        cert.CertificateFile = cert_file.c_str();
        QUIC_CREDENTIAL_CONFIG cred{};
        cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        cred.CertificateFile = &cert;
        if (!open_common(reg_name, alpn, settings, cred)) return false;

        if (QUIC_FAILED(api->ListenerOpen(reg, &msquic_endpoint_t::listener_cb, this, &listener))) {
            listener = nullptr;
            return false;
        }
        QUIC_ADDR addr{};
        QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
        QuicAddrSetPort(&addr, bind_port);
        tsan_release(this);  // publish the constructed endpoint to the callbacks
        if (QUIC_FAILED(api->ListenerStart(listener, &alpn, 1, &addr))) return false;

        // Resolve an ephemeral bind (port 0) to the actual bound port.
        std::uint32_t len = sizeof(addr);
        if (QUIC_SUCCEEDED(api->GetParam(listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &len, &addr)))
            bound_port = QuicAddrGetPort(&addr);
        open_ok = true;
        return true;
    }
    /** @} */

    /**
     * @brief The 5-step teardown (the msquic contract, ASan-audited — see the
     *        file header). MUST be called by the derived impl_t destructor as
     *        its FIRST act: step 2/3's stream harvest dispatches to the
     *        @ref harvest_and_close_streams virtual.
     */
    void teardown() noexcept {
        if (api == nullptr) return;  // MsQuicOpen2 failed — nothing to unwind
        // 1. Stop accepting: ListenerClose blocks until listener callbacks
        //    drain, so no replacement peer can be installed after the harvest.
        if (listener != nullptr) api->ListenerClose(listener);
        // 2+3. Take the live peer's handles (send() sees null and no-ops from
        //    here) and close every stream, then the connection: each Close
        //    blocks until that handle's callbacks drain (in-flight sends
        //    complete Canceled — buffers freed).
        HQUIC c = harvest_and_close_streams();
        if (c != nullptr) {
            api->ConnectionShutdown(c, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api->ConnectionClose(c);
        }
        // 4+5. Parents last: configuration, registration (asserts all children
        //    are closed), then the api table itself.
        if (config != nullptr) api->ConfigurationClose(config);
        if (reg != nullptr) api->RegistrationClose(reg);
        MsQuicClose(api);
        api = nullptr;
        // Every callback has drained (the Closes above block on that) — take
        // their published writes before the members (framer_ et al.) destruct.
        tsan_acquire(this);
    }

    /**
     * @name Variance points (runtime virtuals — ADR-0047 §4 wiring-frequency).
     * @{
     */

    /** @brief CONNECTED hook, after `up` is set and before the handshake
     *         signal (webtransport's server opens its H3 face here). */
    virtual void on_connected(HQUIC c) { (void)c; }

    /** @brief The peer opened a stream — classify/adopt it (the per-transport
     *         variance). Runs on a connection callback, under the cb guard. */
    virtual QUIC_STATUS on_peer_stream_started(HQUIC c, QUIC_CONNECTION_EVENT* ev) = 0;

    /** @brief Connection-down hook (any shutdown event), after `up` clears and
     *         before the handshake signal (webtransport clears its session). */
    virtual void on_conn_down() {}

    /**
     * @brief The one-peer replacement harvest: refuse a second peer while the
     *        first is up; otherwise detach the departed peer's stream slots +
     *        connection under @ref conn_m and CLOSE them (this thread, a
     *        different connection — legal).
     *
     * Called from the listener callback; the base resets the RX state and
     * installs the new connection after it returns true.
     *
     * @return False when a live peer exists — the new connection is refused.
     */
    virtual bool replace_peer() = 0;

    /**
     * @brief Teardown step 2+3a: detach every stream slot and the connection
     *        under @ref conn_m, close the streams (StreamShutdown ABORT +
     *        StreamClose — each blocks until its callbacks drain), and return
     *        the harvested connection handle for the base to close.
     */
    virtual HQUIC harvest_and_close_streams() = 0;
    /** @} */

    /**
     * @name msquic callbacks (worker threads; serialized per connection).
     * @{
     */

    /** @brief The connection callback: link-state flags, the handshake
     *         rendezvous, and dispatch to the stream-adoption virtual. */
    static QUIC_STATUS QUIC_API conn_cb(HQUIC conn_h, void* ctx, QUIC_CONNECTION_EVENT* ev) {
        auto* self = static_cast<msquic_endpoint_t*>(ctx);
        const tsan_cb_guard_t guard(self);  // msquic serializes callbacks (see file top)
        switch (ev->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED:
                self->up.store(true, std::memory_order_relaxed);
                self->on_connected(conn_h);
                self->signal_handshake(true);
                return QUIC_STATUS_SUCCESS;
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
                return self->on_peer_stream_started(conn_h, ev);
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                // Link down (a failed dial lands here too). The handle is NOT
                // closed here (callbacks never close) — the destructor or the
                // listener replacement path owns it.
                self->up.store(false, std::memory_order_relaxed);
                self->on_conn_down();
                self->signal_handshake(false);
                return QUIC_STATUS_SUCCESS;
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    /** @brief The listener callback: the one-peer replacement skeleton —
     *         refuse-or-harvest (virtual), RX reset, then adopt + configure
     *         the new connection. */
    static QUIC_STATUS QUIC_API listener_cb(HQUIC /*listener*/, void* ctx,
                                            QUIC_LISTENER_EVENT* ev) {
        auto* self = static_cast<msquic_endpoint_t*>(ctx);
        const tsan_cb_guard_t guard(self);  // msquic serializes callbacks (see file top)
        if (ev->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) return QUIC_STATUS_SUCCESS;

        // ONE peer at a time (the tcp_transport_t / transport_ws_server
        // model): refuse a second while the first is up; a DEPARTED peer's
        // handles are closed by the harvest and replaced. Closing blocks until
        // the old handles' callbacks drain, so after this point nothing
        // touches the RX state — safe to reset for the new peer.
        if (!self->replace_peer()) return QUIC_STATUS_CONNECTION_REFUSED;
        self->reset_rx();

        HQUIC c = ev->NEW_CONNECTION.Connection;
        self->api->SetCallbackHandler(c, reinterpret_cast<void*>(&msquic_endpoint_t::conn_cb),
                                      self);
        const QUIC_STATUS st = self->api->ConnectionSetConfiguration(c, self->config);
        if (QUIC_FAILED(st)) return st;  // msquic tears the rejected connection down
        const std::lock_guard lock(self->conn_m);
        self->conn = c;
        return QUIC_STATUS_SUCCESS;
    }
    /** @} */

   protected:
    /** @brief Protected, non-virtual: the derived impl_t is never deleted
     *         through a base pointer, and teardown() must run from the derived
     *         destructor (while the virtuals still dispatch). */
    ~msquic_endpoint_t() = default;
};

}  // namespace tr::net
