/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * quic_transport_t (ADR-0043 Phase A) — msquic behind the transport_t seam.
 * The SEPARATE libtracer_quic module's single translation unit — the only code
 * that sees msquic; a host that doesn't talk QUIC never compiles it (the core
 * library carries no reference). Threading model: msquic invokes all callbacks on its worker
 * threads and serializes them per connection, so the RX reassembly state needs
 * no lock (one connection, one stream, one callback at a time); the handle
 * slots are mutex-guarded exactly like tcp_transport_t, and the receivers live
 * in the outer transport_t's receiver_slot_t (which does its own locking).
 *
 * Teardown ordering (the msquic contract, ASan-audited): close the listener
 * first (blocks until listener callbacks drain — no new peer can slip in),
 * then StreamClose + ConnectionClose the live peer (each blocks until that
 * handle's callbacks drain; pending sends complete Canceled, freeing their
 * buffers), then ConfigurationClose, then RegistrationClose (asserts every
 * child handle is gone), then MsQuicClose. Callbacks themselves NEVER close
 * handles — they only flip flags — so ownership stays in exactly two places:
 * the destructor, and the listener's one-peer replacement path.
 */

#include "libtracer/transport_quic.hpp"

#include <msquic.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/length_prefix_framer.hpp"

// msquic is not TSan-instrumented, so the happens-before edges it PROVIDES —
// StreamSend → SEND_COMPLETE (buffer ownership returns), and the per-connection
// serialization of callbacks across its worker threads — are invisible to TSan.
// These annotations restate exactly those two contracts (and nothing more) so
// TSan checks our code against msquic's real memory model instead of reporting
// the library boundary as a race. No-ops outside TSan builds.
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

namespace {

inline void tsan_release(void* p) {
#ifdef LIBTRACER_TSAN
    __tsan_release(p);
#else
    (void)p;
#endif
}

inline void tsan_acquire(void* p) {
#ifdef LIBTRACER_TSAN
    __tsan_acquire(p);
#else
    (void)p;
#endif
}

// RAII edge for one msquic callback invocation: acquire on entry (see what the
// previous callback / the constructor published), release on exit (publish for
// the next callback and the destructor) — the per-connection serialization
// msquic guarantees, restated for TSan.
struct tsan_cb_guard_t {
    void* p;
    explicit tsan_cb_guard_t(void* ptr) : p(ptr) { tsan_acquire(p); }
    ~tsan_cb_guard_t() { tsan_release(p); }
};

constexpr std::size_t kPrefixBytes = 4;             // the u32-LE length prefix (transport framing)
constexpr std::uint64_t kAppErrMalformed = 0x1;     // app-layer shutdown code: framing lost
constexpr std::uint32_t kHandshakeWaitMs = 10'000;  // dial ctor handshake budget

// The ALPN every libtracer QUIC endpoint negotiates (frame stream, Phase A).
const QUIC_BUFFER kAlpn{sizeof("libtracer") - 1,
                        reinterpret_cast<uint8_t*>(const_cast<char*>("libtracer"))};

// One in-flight send: the QUIC_BUFFER msquic reads from plus the owned copy of
// `prefix ++ frame`. msquic owns the bytes until SEND_COMPLETE (its documented
// buffer-lifetime contract); the seam's spans are only borrowed for the send()
// call, so ONE copy into this heap buffer is unavoidable — and the only
// library-held buffer, freed by SEND_COMPLETE (Canceled included, so shutdown
// leaks nothing).
struct send_ctx_t {
    QUIC_BUFFER buf{};
    std::vector<std::byte> bytes;

    explicit send_ctx_t(std::size_t frame_len) : bytes(kPrefixBytes + frame_len) {
        detail::store_le(std::span(bytes).first(kPrefixBytes),
                         static_cast<std::uint32_t>(frame_len));
        buf.Length = static_cast<std::uint32_t>(bytes.size());
        buf.Buffer = reinterpret_cast<uint8_t*>(bytes.data());
    }
};

}  // namespace

struct quic_transport_t::impl_t {
    // msquic object tree (parent → child): api → registration → configuration,
    // listener, connection → stream. All handles owned here (see file header).
    const QUIC_API_TABLE* api = nullptr;
    HQUIC reg = nullptr;
    HQUIC config = nullptr;
    HQUIC listener = nullptr;  // LISTEN mode only
    bool listen = false;
    bool open_ok = false;  // DIAL: handshake+stream up; LISTEN: listener started
    std::uint16_t bound_port = 0;

    // RX segment source for frame reassembly (ADR-0042 §2) + counters.
    mem::mem_backend_t* backend = nullptr;
    std::size_t max_frame = quic_transport_t::kMaxFrame;  // per-connection RX cap (:settings)
    std::atomic<std::uint64_t> dropped_rx{0};
    std::atomic<std::uint64_t> malformed_rx{0};

    // The outer transport's delivery-tier slot (receiver_slot.hpp): tier select
    // and its own locking live there. Wired to &rx_ at construction.
    receiver_slot_t<>* rx = nullptr;

    // The single live peer: its connection + frame stream. conn_m guards the
    // slots (send/replace/teardown); the handles are only CLOSED by the
    // destructor or the listener replacement path, never by callbacks.
    std::mutex conn_m;
    HQUIC conn = nullptr;
    HQUIC stream = nullptr;
    std::atomic<bool> up{false};  // CONNECTED .. shutdown (the link state)

    // DIAL handshake rendezvous: the ctor blocks until CONNECTED or shutdown.
    std::mutex wait_m;
    std::condition_variable wait_cv;
    bool handshake_done = false;
    bool handshake_ok = false;

    // RX frame reassembly across msquic RECEIVE chunks. Touched only on the
    // stream's callback (msquic serializes per-connection callbacks) and reset
    // by the listener before a replacement peer's stream can start.
    length_prefix_framer framer_;

    void reset_rx() { framer_.reset(); }

    void signal_handshake(bool ok) {
        {
            const std::lock_guard lock(wait_m);
            if (handshake_done) return;
            handshake_done = true;
            handshake_ok = ok;
        }
        wait_cv.notify_all();
    }

    // Feed one msquic RECEIVE chunk through the shared length-prefix reassembler
    // (length_prefix_framer — the state machine transport_quic and
    // transport_webtransport once open-coded). Each reassembled frame goes up
    // through the slot: tier select (owning rope sink, else the same segment
    // bytes borrowed) lives in receiver_slot.hpp. Returns false when the
    // connection was shut down (malformed prefix) — the caller stops consuming
    // this event's remaining chunks.
    bool on_rx_chunk(const std::uint8_t* p, std::size_t n) {
        const auto res =
            framer_.feed(*backend, max_frame, reinterpret_cast<const std::byte*>(p), n,
                         [this](view::segment_ptr_t seg, std::size_t len) {
                             rx->deliver(view::view_t::over(std::move(seg)).subview(0, len));
                         });
        if (res.dropped != 0) dropped_rx.fetch_add(res.dropped, std::memory_order_relaxed);
        if (res.malformed) {
            // A desynced stream cannot be re-framed: count it and shut the peer down.
            malformed_rx.fetch_add(1, std::memory_order_relaxed);
            HQUIC c = nullptr;
            {
                const std::lock_guard lock(conn_m);
                c = conn;
            }
            if (c != nullptr)
                api->ConnectionShutdown(c, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, kAppErrMalformed);
            return false;
        }
        return true;
    }

    // ---- msquic callbacks (worker threads; serialized per connection) ----

    static QUIC_STATUS QUIC_API stream_cb(HQUIC /*stream*/, void* ctx, QUIC_STREAM_EVENT* ev) {
        auto* self = static_cast<impl_t*>(ctx);
        const tsan_cb_guard_t guard(self);  // msquic serializes callbacks (see file top)
        switch (ev->Type) {
            case QUIC_STREAM_EVENT_RECEIVE:
                for (std::uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
                    const QUIC_BUFFER& b = ev->RECEIVE.Buffers[i];
                    if (!self->on_rx_chunk(b.Buffer, b.Length)) break;  // shut down
                }
                return QUIC_STATUS_SUCCESS;  // every byte consumed
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                // msquic is done with the send buffer (Canceled included) —
                // the one library-held buffer's lifetime ends exactly here.
                tsan_acquire(ev->SEND_COMPLETE.ClientContext);  // pairs with send()'s release
                delete static_cast<send_ctx_t*>(ev->SEND_COMPLETE.ClientContext);
                return QUIC_STATUS_SUCCESS;
            case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
                self->up.store(false, std::memory_order_relaxed);
                return QUIC_STATUS_SUCCESS;
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    static QUIC_STATUS QUIC_API conn_cb(HQUIC /*conn*/, void* ctx, QUIC_CONNECTION_EVENT* ev) {
        auto* self = static_cast<impl_t*>(ctx);
        const tsan_cb_guard_t guard(self);  // msquic serializes callbacks (see file top)
        switch (ev->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED:
                self->up.store(true, std::memory_order_relaxed);
                self->signal_handshake(true);
                return QUIC_STATUS_SUCCESS;
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
                // The dialing peer opened the frame stream — adopt it (one
                // bidirectional stream per connection, ADR-0043 Phase A; the
                // PeerBidiStreamCount=1 setting caps the peer to exactly one).
                if ((ev->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) != 0)
                    return QUIC_STATUS_NOT_SUPPORTED;  // frame stream is bidirectional
                self->api->SetCallbackHandler(ev->PEER_STREAM_STARTED.Stream,
                                              reinterpret_cast<void*>(&impl_t::stream_cb), self);
                const std::lock_guard lock(self->conn_m);
                self->stream = ev->PEER_STREAM_STARTED.Stream;
                return QUIC_STATUS_SUCCESS;
            }
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
                self->up.store(false, std::memory_order_relaxed);
                self->signal_handshake(false);  // a failed dial lands here
                return QUIC_STATUS_SUCCESS;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                // Link down. The handle is NOT closed here (callbacks never
                // close) — the destructor or the listener replacement owns it.
                self->up.store(false, std::memory_order_relaxed);
                self->signal_handshake(false);
                return QUIC_STATUS_SUCCESS;
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    static QUIC_STATUS QUIC_API listener_cb(HQUIC /*listener*/, void* ctx,
                                            QUIC_LISTENER_EVENT* ev) {
        auto* self = static_cast<impl_t*>(ctx);
        const tsan_cb_guard_t guard(self);  // msquic serializes callbacks (see file top)
        if (ev->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) return QUIC_STATUS_SUCCESS;

        // ONE peer at a time (the tcp_transport_t / transport_ws_server model):
        // refuse a second while the first is up; a DEPARTED peer's handles are
        // closed here (this thread, a different connection — legal) and replaced.
        HQUIC old_stream = nullptr;
        HQUIC old_conn = nullptr;
        {
            const std::lock_guard lock(self->conn_m);
            if (self->conn != nullptr && self->up.load(std::memory_order_relaxed))
                return QUIC_STATUS_CONNECTION_REFUSED;
            old_stream = std::exchange(self->stream, nullptr);
            old_conn = std::exchange(self->conn, nullptr);
        }
        // Closing blocks until the old handles' callbacks drain, so after this
        // point nothing touches the RX state — safe to reset for the new peer.
        if (old_stream != nullptr) self->api->StreamClose(old_stream);
        if (old_conn != nullptr) self->api->ConnectionClose(old_conn);
        self->reset_rx();

        HQUIC c = ev->NEW_CONNECTION.Connection;
        self->api->SetCallbackHandler(c, reinterpret_cast<void*>(&impl_t::conn_cb), self);
        const QUIC_STATUS st = self->api->ConnectionSetConfiguration(c, self->config);
        if (QUIC_FAILED(st)) return st;  // msquic tears the rejected connection down
        const std::lock_guard lock(self->conn_m);
        self->conn = c;
        return QUIC_STATUS_SUCCESS;
    }

    // Shared bring-up: the api table, registration, and configuration (with
    // the mode's credential). Returns false with everything not-yet-opened
    // left null, so the destructor unwinds partial construction cleanly.
    bool open_common(const QUIC_SETTINGS& settings, const QUIC_CREDENTIAL_CONFIG& cred) {
        if (QUIC_FAILED(MsQuicOpen2(&api))) {
            api = nullptr;
            return false;
        }
        const QUIC_REGISTRATION_CONFIG reg_cfg{"libtracer", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        if (QUIC_FAILED(api->RegistrationOpen(&reg_cfg, &reg))) return false;
        if (QUIC_FAILED(api->ConfigurationOpen(reg, &kAlpn, 1, &settings, sizeof(settings), nullptr,
                                               &config)))
            return false;
        return !QUIC_FAILED(api->ConfigurationLoadCredential(config, &cred));
    }
};

quic_transport_t::quic_transport_t(const std::string& peer_host, std::uint16_t peer_port,
                                   quic_dial_tls_t tls, mem::mem_backend_t* backend,
                                   std::size_t max_frame)
    : impl_(std::make_unique<impl_t>()) {
    impl_t& i = *impl_;
    i.rx = &rx_;  // the delivery-tier slot lives in the transport_t base
    i.backend = backend;
    if (max_frame != 0) i.max_frame = max_frame;

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 0;  // no idle teardown; link liveness is #66 lifecycle
    settings.IsSet.IdleTimeoutMs = TRUE;

    QUIC_CREDENTIAL_CONFIG cred{};
    cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
    unsigned flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (tls.insecure_no_verify) {
        flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;  // DEV ONLY (self-signed)
    } else if (!tls.ca_file.empty()) {
        flags |= QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE;
        cred.CaCertificateFile = tls.ca_file.c_str();
    }
    cred.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(flags);
    if (!i.open_common(settings, cred)) return;

    if (QUIC_FAILED(i.api->ConnectionOpen(i.reg, &impl_t::conn_cb, &i, &i.conn))) {
        i.conn = nullptr;
        return;
    }
    tsan_release(&i);  // publish the constructed impl to the callbacks (see file top)
    if (QUIC_FAILED(i.api->ConnectionStart(i.conn, i.config, QUIC_ADDRESS_FAMILY_UNSPEC,
                                           peer_host.c_str(), peer_port)))
        return;

    // The tcp_transport_t dial shape: block until the handshake resolves so
    // ok() is meaningful at construction (CONNECTED, or any shutdown = failure).
    {
        std::unique_lock lock(i.wait_m);
        i.wait_cv.wait_for(lock, std::chrono::milliseconds(kHandshakeWaitMs),
                           [&i] { return i.handshake_done; });
        if (!i.handshake_ok) return;
    }

    // Open + start the ONE bidirectional frame stream (the dialer owns opening
    // it; the listener adopts it via PEER_STREAM_STARTED).
    HQUIC stream = nullptr;
    if (QUIC_FAILED(
            i.api->StreamOpen(i.conn, QUIC_STREAM_OPEN_FLAG_NONE, &impl_t::stream_cb, &i, &stream)))
        return;
    if (QUIC_FAILED(i.api->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) {
        i.api->StreamClose(stream);
        return;
    }
    {
        const std::lock_guard lock(i.conn_m);
        i.stream = stream;
    }
    i.open_ok = true;
}

quic_transport_t::quic_transport_t(std::uint16_t bind_port, const std::string& cert_file,
                                   const std::string& key_file, mem::mem_backend_t* backend,
                                   std::size_t max_frame)
    : impl_(std::make_unique<impl_t>()) {
    impl_t& i = *impl_;
    i.rx = &rx_;  // the delivery-tier slot lives in the transport_t base
    i.backend = backend;
    if (max_frame != 0) i.max_frame = max_frame;
    i.listen = true;

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 0;  // no idle teardown; link liveness is #66 lifecycle
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = 1;  // exactly the ONE frame stream (ADR-0043 Phase A)
    settings.IsSet.PeerBidiStreamCount = TRUE;

    QUIC_CERTIFICATE_FILE cert{};
    cert.PrivateKeyFile = key_file.c_str();
    cert.CertificateFile = cert_file.c_str();
    QUIC_CREDENTIAL_CONFIG cred{};
    cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    cred.CertificateFile = &cert;
    if (!i.open_common(settings, cred)) return;  // bad cert/key paths fail HERE

    if (QUIC_FAILED(i.api->ListenerOpen(i.reg, &impl_t::listener_cb, &i, &i.listener))) {
        i.listener = nullptr;
        return;
    }
    QUIC_ADDR addr{};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, bind_port);
    tsan_release(&i);  // publish the constructed impl to the callbacks (see file top)
    if (QUIC_FAILED(i.api->ListenerStart(i.listener, &kAlpn, 1, &addr))) return;

    // Resolve an ephemeral bind (port 0) to the actual bound port.
    std::uint32_t len = sizeof(addr);
    if (QUIC_SUCCEEDED(i.api->GetParam(i.listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &len, &addr)))
        i.bound_port = QuicAddrGetPort(&addr);
    i.open_ok = true;
}

quic_transport_t::~quic_transport_t() {
    impl_t& i = *impl_;
    if (i.api == nullptr) return;  // MsQuicOpen2 failed — nothing to unwind
    // 1. Stop accepting: ListenerClose blocks until listener callbacks drain,
    //    so no replacement peer can be installed after the swap below.
    if (i.listener != nullptr) i.api->ListenerClose(i.listener);
    // 2. Take the live peer's handles (send() sees null and no-ops from here).
    HQUIC stream = nullptr;
    HQUIC conn = nullptr;
    {
        const std::lock_guard lock(i.conn_m);
        stream = std::exchange(i.stream, nullptr);
        conn = std::exchange(i.conn, nullptr);
    }
    // 3. Close stream then connection: each Close blocks until that handle's
    //    callbacks drain (in-flight sends complete Canceled — buffers freed).
    if (stream != nullptr) {
        i.api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        i.api->StreamClose(stream);
    }
    if (conn != nullptr) {
        i.api->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        i.api->ConnectionClose(conn);
    }
    // 4. Parents last: configuration, registration (asserts all children are
    //    closed), then the api table itself.
    if (i.config != nullptr) i.api->ConfigurationClose(i.config);
    if (i.reg != nullptr) i.api->RegistrationClose(i.reg);
    MsQuicClose(i.api);
    // Every callback has drained (the Closes above block on that) — take their
    // published writes before the members (framer_ et al.) destruct.
    tsan_acquire(&i);
}

void quic_transport_t::send(std::span<const std::byte> frame) {
    if (frame.size() > kMaxFrame) return;  // the peer would reject it as malformed
    auto ctx = std::make_unique<send_ctx_t>(frame.size());
    if (!frame.empty()) std::memcpy(ctx->bytes.data() + kPrefixBytes, frame.data(), frame.size());
    impl_t& i = *impl_;
    const std::lock_guard lock(i.conn_m);
    if (i.stream == nullptr) return;  // no peer stream (yet / anymore) — drop, like tcp
    // On success msquic owns the buffer until SEND_COMPLETE (which deletes the
    // ctx); on failure no event will fire, so the unique_ptr frees it here.
    tsan_release(ctx.get());  // pairs with SEND_COMPLETE's acquire (see file top)
    if (QUIC_SUCCEEDED(i.api->StreamSend(i.stream, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx.get())))
        (void)ctx.release();
}

void quic_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    std::size_t total = 0;
    for (const auto& s : iov) total += s.size();
    if (total > kMaxFrame) return;  // the peer would reject it as malformed

    // ONE gather copy (documented in the header): msquic's multi-buffer
    // StreamSend cannot help here because every QUIC_BUFFER must outlive the
    // call (until SEND_COMPLETE) while the seam's spans are borrowed only for
    // this call — so the rope's spans are gathered once into the single owned
    // send buffer, behind the prefix.
    auto ctx = std::make_unique<send_ctx_t>(total);
    std::size_t off = kPrefixBytes;
    for (const auto& s : iov) {
        if (s.empty()) continue;
        std::memcpy(ctx->bytes.data() + off, s.data(), s.size());
        off += s.size();
    }
    impl_t& i = *impl_;
    const std::lock_guard lock(i.conn_m);
    if (i.stream == nullptr) return;
    tsan_release(ctx.get());  // pairs with SEND_COMPLETE's acquire (see file top)
    if (QUIC_SUCCEEDED(i.api->StreamSend(i.stream, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx.get())))
        (void)ctx.release();
}

bool quic_transport_t::ok() const noexcept { return impl_->open_ok; }

std::uint16_t quic_transport_t::local_port() const noexcept { return impl_->bound_port; }

bool quic_transport_t::link_up() const noexcept {
    return impl_->up.load(std::memory_order_relaxed);
}

std::uint64_t quic_transport_t::dropped_rx() const noexcept {
    return impl_->dropped_rx.load(std::memory_order_relaxed);
}

std::uint64_t quic_transport_t::malformed_rx() const noexcept {
    return impl_->malformed_rx.load(std::memory_order_relaxed);
}

namespace {

// The quic kind's PRIVATE config keys, parsed module-side from the raw SPEC config
// SETTINGS TLV (ADR-0043 §5 leanness: the shared conn_settings_t carries only the
// universal keys, so cert/key never touch it). Same positional NAME-key / value-pair
// walk transport_vertex.cpp uses for the universal keys: NAME "cert" NAME <path>,
// NAME "key" NAME <path>; unknown pairs ignored (forward-compat).
struct quic_private_cfg_t {
    std::string cert;  // PEM server-certificate path (LISTEN)
    std::string key;   // PEM private-key path matching cert (LISTEN)
};

[[nodiscard]] quic_private_cfg_t parse_quic_config(const wire::tlv_t* raw_config) {
    quic_private_cfg_t out;
    if (raw_config == nullptr) return out;
    const std::vector<wire::tlv_t>& ch = raw_config->children;
    for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
        if (ch[i].type != wire::type_t::NAME) continue;
        const std::string_view key = detail::as_string_view(ch[i].payload);
        const wire::tlv_t& val = ch[i + 1];
        if (key == "cert" && val.type == wire::type_t::NAME) {
            out.cert = std::string(detail::as_string_view(val.payload));
        } else if (key == "key" && val.type == wire::type_t::NAME) {
            out.key = std::string(detail::as_string_view(val.payload));
        }
    }
    return out;
}

}  // namespace

transport_vertex_t::transport_factory_t quic_transport_factory(mem::mem_backend_t* rx_backend) {
    return [rx_backend](
               const conn_settings_t& s,
               const wire::tlv_t* raw_config) -> graph::result_t<std::unique_ptr<transport_t>> {
        std::unique_ptr<quic_transport_t> t;
        if (s.role == conn_role_t::DIAL) {
            if (s.addr.empty() || s.port == 0)
                return std::unexpected(graph::status_t::TYPE_MISMATCH);
            t = std::make_unique<quic_transport_t>(
                s.addr, s.port, quic_dial_tls_t{.ca_file = {}, .insecure_no_verify = true},
                rx_backend, s.max_frame);
            if (!t->ok()) return std::unexpected(graph::status_t::NOT_FOUND);  // handshake failed
            return t;
        }
        const quic_private_cfg_t priv = parse_quic_config(raw_config);
        if (s.port == 0 || priv.cert.empty() || priv.key.empty())
            return std::unexpected(graph::status_t::TYPE_MISMATCH);
        t = std::make_unique<quic_transport_t>(s.port, priv.cert, priv.key, rx_backend,
                                               s.max_frame);
        if (!t->ok()) return std::unexpected(graph::status_t::NOT_FOUND);  // bind/cred failed
        return t;
    };
}

}  // namespace tr::net
