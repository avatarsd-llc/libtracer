/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * webtransport_transport_t (ADR-0043 Phase B) — the WebTransport-over-HTTP/3
 * endpoint inside the SEPARATE libtracer_quic module (the same msquic
 * investment as Phase A; the core library never references any of this). The
 * H3/QPACK surface is the deliberately minimal subset in src/wt_h3.hpp (see
 * its header for exactly what is implemented and why it suffices); everything
 * QUIC-mechanical — handle ownership, teardown ordering, the per-connection
 * callback serialization and its TSan annotations, the RX length-prefix
 * reassembly and the one-copy TX contract — is the transport_quic.cpp model,
 * restated here over a session with MULTIPLE streams:
 *
 *   - LISTEN: on CONNECTED the server opens its control stream (SETTINGS:
 *     extended CONNECT + H3 datagrams + ENABLE_WEBTRANSPORT/WT_MAX_SESSIONS)
 *     plus the two mandatory QPACK streams. Peer streams are classified by
 *     their first varint(s): control/QPACK/push/WT-uni streams are drained;
 *     the bidirectional HEADERS (0x01) stream is the extended CONNECT — it is
 *     validated (`:method=CONNECT`, `:protocol=webtransport`), answered with
 *     200, and kept open as the session's lifetime handle; the bidirectional
 *     WEBTRANSPORT_STREAM (0x41) is adopted as THE frame channel, everything
 *     after its session-id varint feeding the 4-byte length-prefix
 *     reassembler.
 *   - DIAL (the self-contained e2e counterpart, and a native client): after
 *     the QUIC handshake it sends its control/QPACK streams, performs the
 *     extended CONNECT, waits for the 200, then opens the frame stream
 *     (0x41 + the CONNECT stream's id) — the browser
 *     `createBidirectionalStream()` shape.
 *
 * Callbacks never close handles (they only flip flags and adopt streams); the
 * destructor and the listener's one-peer replacement path own every close,
 * exactly the Phase A discipline.
 */

#include "libtracer/transport_webtransport.hpp"

#include <msquic.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/length_prefix_framer.hpp"
#include "wt_h3.hpp"

// msquic is not TSan-instrumented — restate its two happens-before contracts
// (StreamSend -> SEND_COMPLETE buffer ownership, per-connection callback
// serialization) for TSan. See transport_quic.cpp for the full rationale.
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

// RAII edge for one msquic callback invocation (the transport_quic.cpp
// tsan_cb_guard_t): acquire on entry, release on exit.
struct tsan_cb_guard_t {
    void* p;
    explicit tsan_cb_guard_t(void* ptr) : p(ptr) { tsan_acquire(p); }
    ~tsan_cb_guard_t() { tsan_release(p); }
};

constexpr std::size_t kPrefixBytes = 4;             // the u32-LE length prefix (transport framing)
constexpr std::uint64_t kAppErrMalformed = 0x1;     // framing lost on the frame stream
constexpr std::uint64_t kAppErrBadRequest = 0x2;    // not a WebTransport extended CONNECT
constexpr std::uint32_t kHandshakeWaitMs = 10'000;  // dial ctor budget per stage
constexpr std::size_t kMaxHandshakeBytes = 16'384;  // per-stream classification/HEADERS cap

// The HTTP/3 ALPN every WebTransport endpoint negotiates.
const QUIC_BUFFER kAlpnH3{sizeof("h3") - 1, reinterpret_cast<uint8_t*>(const_cast<char*>("h3"))};

// One in-flight send: the QUIC_BUFFER msquic reads from plus the owned bytes.
// msquic owns the bytes until SEND_COMPLETE (Canceled included) — the only
// library-held buffer (the transport_quic.cpp TX contract). Used both for
// length-prefixed frame records and for raw handshake bytes.
struct send_ctx_t {
    QUIC_BUFFER buf{};
    std::vector<std::byte> bytes;

    // A length-prefixed frame record: prefix ++ frame (frame filled by caller).
    explicit send_ctx_t(std::size_t frame_len) : bytes(kPrefixBytes + frame_len) {
        detail::store_le(std::span(bytes).first(kPrefixBytes),
                         static_cast<std::uint32_t>(frame_len));
        arm();
    }
    // Raw bytes (H3 handshake material) — no prefix.
    explicit send_ctx_t(std::vector<std::uint8_t> raw)
        : bytes(reinterpret_cast<std::byte*>(raw.data()),
                reinterpret_cast<std::byte*>(raw.data()) + raw.size()) {
        arm();
    }

    void arm() {
        buf.Length = static_cast<std::uint32_t>(bytes.size());
        buf.Buffer = reinterpret_cast<uint8_t*>(bytes.data());
    }
};

}  // namespace

struct webtransport_transport_t::impl_t {
    // ---- per-stream state (classification + handshake accumulation) ----
    // Touched only on that stream's callback (msquic serializes per-connection
    // callbacks); the ctx LIST is guarded by conn_m. Contexts are deleted only
    // by the destructor / the listener replacement path — never by callbacks.
    struct stream_ctx_t {
        enum class kind_t {
            CLASSIFY_UNI,    // inbound unidirectional: awaiting its stream-type varint
            CLASSIFY_BIDI,   // inbound bidirectional: HEADERS(CONNECT) or 0x41 frame channel
            CONNECT_CLIENT,  // DIAL: our CONNECT stream, awaiting the 200 response
            SESSION,         // the accepted CONNECT stream — the session's lifetime handle
            FRAME,           // the adopted WebTransport frame channel
            DRAIN,           // classified, contents irrelevant — discard
            LOCAL,           // locally-opened control/QPACK stream (sends only)
        };
        impl_t* owner = nullptr;
        HQUIC h = nullptr;
        kind_t kind = kind_t::DRAIN;
        std::vector<std::uint8_t> acc;  // bounded by kMaxHandshakeBytes
        bool harvested = false;         // guarded by conn_m: the dtor/replacement path took
                                        // this handle for closing — never re-adopt it
    };

    // msquic object tree — owned here (see file header for teardown ordering).
    const QUIC_API_TABLE* api = nullptr;
    HQUIC reg = nullptr;
    HQUIC config = nullptr;
    HQUIC listener = nullptr;  // LISTEN mode only
    bool listen = false;
    bool open_ok = false;
    std::uint16_t bound_port = 0;
    std::string authority;  // DIAL: the CONNECT :authority
    std::string path;       // DIAL: the CONNECT :path

    // RX segment source for frame reassembly (ADR-0042 §2) + counters.
    mem::mem_backend_t* backend = nullptr;
    std::atomic<std::uint64_t> dropped_rx{0};
    std::atomic<std::uint64_t> malformed_rx{0};

    receiver_t receiver;            // guarded by m
    view_receiver_t view_receiver;  // guarded by m; installed => owning delivery
    std::mutex m;                   // guards the receivers

    // The single live session: its connection, every stream context, and the
    // adopted frame channel. conn_m guards the slots; handles are only CLOSED
    // by the destructor or the listener replacement path.
    std::mutex conn_m;
    HQUIC conn = nullptr;
    HQUIC frame_stream = nullptr;
    std::vector<stream_ctx_t*> ctxs;
    std::atomic<bool> up{false};       // QUIC CONNECTED .. shutdown
    std::atomic<bool> session{false};  // extended CONNECT accepted (200)
    std::uint64_t connect_stream_id = 0;

    // DIAL rendezvous, two stages: QUIC CONNECTED, then session established.
    std::mutex wait_m;
    std::condition_variable wait_cv;
    bool handshake_done = false;
    bool handshake_ok = false;
    bool session_done = false;
    bool session_ok = false;

    // RX frame reassembly across msquic RECEIVE chunks — the shared
    // length_prefix_framer, touched only on the frame stream's callback.
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

    void signal_session(bool ok) {
        {
            const std::lock_guard lock(wait_m);
            if (session_done) return;
            session_done = true;
            session_ok = ok;
        }
        wait_cv.notify_all();
    }

    void shutdown_conn(std::uint64_t code) {
        HQUIC c = nullptr;
        {
            const std::lock_guard lock(conn_m);
            c = conn;
        }
        if (c != nullptr) api->ConnectionShutdown(c, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, code);
    }

    // Hand one reassembled frame up (owning when a view receiver is installed).
    void deliver(view::segment_ptr_t seg, std::size_t len) {
        view_receiver_t vr;
        receiver_t r;
        {
            const std::lock_guard lock(m);
            vr = view_receiver;
            r = receiver;
        }
        if (vr) {
            vr(view::view_t::over(std::move(seg)).subview(0, len));
        } else if (r) {
            r(std::span<const std::byte>(seg->bytes.data(), len));
        }
    }

    // The length-prefix reassembly, delegated to the shared length_prefix_framer
    // (one exactly-sized segment per frame, backpressure drain, malformed oversize
    // prefix => connection shutdown). Returns false once shut down.
    bool on_rx_chunk(const std::uint8_t* p, std::size_t n) {
        const auto res = framer_.feed(
            *backend, kMaxFrame, reinterpret_cast<const std::byte*>(p), n,
            [this](view::segment_ptr_t seg, std::size_t len) { deliver(std::move(seg), len); });
        if (res.dropped != 0) dropped_rx.fetch_add(res.dropped, std::memory_order_relaxed);
        if (res.malformed) {
            malformed_rx.fetch_add(1, std::memory_order_relaxed);
            shutdown_conn(kAppErrMalformed);
            return false;
        }
        return true;
    }

    // ---- raw byte sends (H3 handshake material) ----

    void send_raw(HQUIC stream, std::vector<std::uint8_t> bytes) {
        auto ctx = std::make_unique<send_ctx_t>(std::move(bytes));
        tsan_release(ctx.get());  // pairs with SEND_COMPLETE's acquire
        if (QUIC_SUCCEEDED(api->StreamSend(stream, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx.get())))
            (void)ctx.release();
    }

    // Open a local unidirectional stream on @p on_conn and write @p bytes on
    // it (the control and QPACK encoder/decoder streams). Runs entirely under
    // conn_m and only while @p on_conn is STILL the live connection, so a
    // teardown/replacement racing this open can never orphan the handle: the
    // ctx joins ctxs under the same lock the harvester takes.
    void open_local_uni(HQUIC on_conn, std::vector<std::uint8_t> bytes) {
        const std::lock_guard lock(conn_m);
        if (conn != on_conn || on_conn == nullptr) return;  // tearing down / replaced
        auto ctx = std::make_unique<stream_ctx_t>();
        ctx->owner = this;
        ctx->kind = stream_ctx_t::kind_t::LOCAL;
        HQUIC s = nullptr;
        if (QUIC_FAILED(api->StreamOpen(on_conn, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, &stream_cb,
                                        ctx.get(), &s)))
            return;
        ctx->h = s;
        tsan_release(ctx.get());  // publish the ctx to its callbacks (see file top)
        if (QUIC_FAILED(api->StreamStart(s, QUIC_STREAM_START_FLAG_NONE))) {
            api->StreamClose(s);
            return;
        }
        send_raw(s, std::move(bytes));
        ctxs.push_back(ctx.release());
    }

    // The endpoint's H3 face on @p on_conn: the control stream (SETTINGS) plus
    // the two mandatory QPACK streams (RFC 9204 §4.2 — empty beyond their type
    // byte, since the dynamic table stays at capacity 0).
    void open_h3_face(HQUIC on_conn) {
        open_local_uni(on_conn, wt_h3::control_stream_bytes());
        std::vector<std::uint8_t> enc;
        wt_h3::append_varint(enc, wt_h3::kStreamTypeQpackEncoder);
        open_local_uni(on_conn, std::move(enc));
        std::vector<std::uint8_t> dec;
        wt_h3::append_varint(dec, wt_h3::kStreamTypeQpackDecoder);
        open_local_uni(on_conn, std::move(dec));
    }

    // ---- inbound stream classification + the H3 handshake ----

    // Accumulate handshake bytes with the DoS cap. False => connection down.
    bool accumulate(stream_ctx_t& c, const std::uint8_t* p, std::size_t n) {
        if (c.acc.size() + n > kMaxHandshakeBytes) {
            shutdown_conn(kAppErrBadRequest);
            return false;
        }
        c.acc.insert(c.acc.end(), p, p + n);
        return true;
    }

    // A peer unidirectional stream: its first varint is the stream type; every
    // type (control / QPACK / push / WT-uni) is drained — the SETTINGS the peer
    // sends are not needed (we are lenient; ours are always advertised).
    void classify_uni(stream_ctx_t& c) {
        if (wt_h3::read_varint(c.acc)) {
            c.kind = stream_ctx_t::kind_t::DRAIN;
            c.acc.clear();
            c.acc.shrink_to_fit();
        }
    }

    // A peer bidirectional stream on the LISTEN side: either the extended
    // CONNECT request (HEADERS) or the WebTransport frame channel (0x41).
    // Returns false when the connection was shut down.
    bool classify_bidi(stream_ctx_t& c) {
        const std::span<const std::uint8_t> in(c.acc);
        const auto t = wt_h3::read_varint(in);
        if (!t) return true;  // need more bytes

        if (t->value == wt_h3::kFrameWtStream) {
            // The frame channel: consume the session-id varint (one session is
            // live at a time, so any id is accepted), adopt the stream, and
            // feed the leftover bytes straight into the frame reassembler.
            auto rest = in.subspan(t->consumed);
            const auto sid = wt_h3::read_varint(rest);
            if (!sid) return true;
            {
                // Adopt only while the session is live: a harvested ctx's
                // handle is being closed by the destructor / the replacement
                // path — resurrecting it into frame_stream would leave send()
                // a dangling handle.
                const std::lock_guard lock(conn_m);
                if (c.harvested) return true;
                frame_stream = c.h;
            }
            c.kind = stream_ctx_t::kind_t::FRAME;
            const std::vector<std::uint8_t> leftover(rest.begin() + sid->consumed, rest.end());
            c.acc.clear();
            c.acc.shrink_to_fit();
            if (!leftover.empty()) return on_rx_chunk(leftover.data(), leftover.size());
            return true;
        }

        if (t->value == wt_h3::kFrameHeaders) {
            auto rest = in.subspan(t->consumed);
            const auto len = wt_h3::read_varint(rest);
            if (!len) return true;
            if (len->value > kMaxHandshakeBytes) {
                shutdown_conn(kAppErrBadRequest);
                return false;
            }
            rest = rest.subspan(len->consumed);
            if (rest.size() < len->value) return true;  // need the full field section

            const auto headers =
                wt_h3::decode_field_section(rest.first(static_cast<std::size_t>(len->value)));
            std::string_view method;
            std::string_view protocol;
            if (headers) {
                for (const auto& h : *headers) {
                    if (h.name == ":method") method = h.value;
                    if (h.name == ":protocol") protocol = h.value;
                }
            }
            if (method != "CONNECT" || protocol != "webtransport") {
                shutdown_conn(kAppErrBadRequest);  // not a WebTransport session request
                return false;
            }
            // Accept: 200 on this stream, which stays open as the session
            // handle (its closure ends the session). Any path is served.
            std::vector<std::uint8_t> resp;
            wt_h3::append_h3_frame(resp, wt_h3::kFrameHeaders,
                                   wt_h3::encode_status_200_field_section());
            send_raw(c.h, std::move(resp));
            std::uint64_t sid = 0;
            std::uint32_t sz = sizeof(sid);
            if (QUIC_SUCCEEDED(api->GetParam(c.h, QUIC_PARAM_STREAM_ID, &sz, &sid)))
                connect_stream_id = sid;
            session.store(true, std::memory_order_relaxed);
            c.kind = stream_ctx_t::kind_t::SESSION;  // capsules after CONNECT are ignored
            c.acc.clear();
            c.acc.shrink_to_fit();
            return true;
        }

        shutdown_conn(kAppErrBadRequest);  // an ordinary H3 request — not served here
        return false;
    }

    // The DIAL side's CONNECT stream: parse the response HEADERS, demand 200.
    void parse_connect_response(stream_ctx_t& c) {
        const std::span<const std::uint8_t> in(c.acc);
        const auto t = wt_h3::read_varint(in);
        if (!t) return;
        if (t->value != wt_h3::kFrameHeaders) {
            signal_session(false);
            shutdown_conn(kAppErrBadRequest);
            return;
        }
        auto rest = in.subspan(t->consumed);
        const auto len = wt_h3::read_varint(rest);
        if (!len) return;
        if (len->value > kMaxHandshakeBytes) {
            signal_session(false);
            shutdown_conn(kAppErrBadRequest);
            return;
        }
        rest = rest.subspan(len->consumed);
        if (rest.size() < len->value) return;  // need the full field section

        const auto headers =
            wt_h3::decode_field_section(rest.first(static_cast<std::size_t>(len->value)));
        std::string_view status;
        if (headers) {
            for (const auto& h : *headers) {
                if (h.name == ":status") status = h.value;
            }
        }
        c.kind = stream_ctx_t::kind_t::SESSION;
        c.acc.clear();
        c.acc.shrink_to_fit();
        if (status == "200") {
            session.store(true, std::memory_order_relaxed);
            signal_session(true);
        } else {
            signal_session(false);
            shutdown_conn(kAppErrBadRequest);
        }
    }

    // Route one RECEIVE chunk to the stream's state. False => stop consuming
    // this event's remaining chunks (the connection was shut down).
    bool on_stream_rx(stream_ctx_t& c, const std::uint8_t* p, std::size_t n) {
        using kind_t = stream_ctx_t::kind_t;
        switch (c.kind) {
            case kind_t::FRAME:
                return on_rx_chunk(p, n);
            case kind_t::SESSION:  // post-CONNECT capsules are ignored
            case kind_t::DRAIN:
            case kind_t::LOCAL:
                return true;  // contents irrelevant
            case kind_t::CLASSIFY_UNI:
                if (!accumulate(c, p, n)) return false;
                classify_uni(c);
                return true;
            case kind_t::CLASSIFY_BIDI:
                if (!accumulate(c, p, n)) return false;
                return classify_bidi(c);
            case kind_t::CONNECT_CLIENT:
                if (!accumulate(c, p, n)) return false;
                parse_connect_response(c);
                return true;
        }
        return true;
    }

    // ---- msquic callbacks (worker threads; serialized per connection) ----

    static QUIC_STATUS QUIC_API stream_cb(HQUIC /*stream*/, void* ctx, QUIC_STREAM_EVENT* ev) {
        auto* c = static_cast<stream_ctx_t*>(ctx);
        // Two TSan edges (see file top): the ctx guard pairs with the
        // publication release where the ctx was handed to msquic (and with the
        // acquire before the harvester deletes it); the impl guard restates
        // msquic's per-connection callback serialization.
        const tsan_cb_guard_t ctx_guard(c);
        impl_t* self = c->owner;
        const tsan_cb_guard_t guard(self);
        switch (ev->Type) {
            case QUIC_STREAM_EVENT_RECEIVE:
                for (std::uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
                    const QUIC_BUFFER& b = ev->RECEIVE.Buffers[i];
                    if (!self->on_stream_rx(*c, b.Buffer, b.Length)) break;  // shut down
                }
                return QUIC_STATUS_SUCCESS;  // every byte consumed
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                tsan_acquire(ev->SEND_COMPLETE.ClientContext);  // pairs with send's release
                delete static_cast<send_ctx_t*>(ev->SEND_COMPLETE.ClientContext);
                return QUIC_STATUS_SUCCESS;
            case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
                // Losing the CONNECT stream ends the session (its lifetime IS
                // the session's, per the WebTransport draft); losing the frame
                // stream ends the link's usefulness — flip both flags either
                // way (the one-peer replacement path keys off `up`).
                if (c->kind == stream_ctx_t::kind_t::FRAME ||
                    c->kind == stream_ctx_t::kind_t::SESSION) {
                    self->up.store(false, std::memory_order_relaxed);
                    self->session.store(false, std::memory_order_relaxed);
                }
                return QUIC_STATUS_SUCCESS;
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    static QUIC_STATUS QUIC_API conn_cb(HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev) {
        auto* self = static_cast<impl_t*>(ctx);
        const tsan_cb_guard_t guard(self);  // msquic serializes callbacks (see file top)
        switch (ev->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED:
                self->up.store(true, std::memory_order_relaxed);
                // The server presents its H3 face as soon as QUIC is up — the
                // browser waits for SETTINGS before sending extended CONNECT.
                // (The dial side sends its face from the constructor thread.)
                if (self->listen) self->open_h3_face(conn);
                self->signal_handshake(true);
                return QUIC_STATUS_SUCCESS;
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
                // Classify lazily from the stream's first bytes; the context
                // joins the session's ctx list for teardown. Adoption is
                // conditional on this STILL being the live connection, under
                // conn_m: the destructor (and the listener replacement path)
                // harvests the ctx list under the same lock, so a stream that
                // arrives after the harvest must be REFUSED — msquic then
                // closes it itself — or its handle would never be closed and
                // RegistrationClose would wait on the connection forever.
                const std::lock_guard lock(self->conn_m);
                if (self->conn != conn) return QUIC_STATUS_ABORTED;  // tearing down / replaced
                auto* c = new stream_ctx_t{};
                c->owner = self;
                c->h = ev->PEER_STREAM_STARTED.Stream;
                c->kind =
                    (ev->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) != 0
                        ? stream_ctx_t::kind_t::CLASSIFY_UNI
                        : stream_ctx_t::kind_t::CLASSIFY_BIDI;
                tsan_release(c);  // publish the ctx to its callbacks (see file top)
                self->api->SetCallbackHandler(ev->PEER_STREAM_STARTED.Stream,
                                              reinterpret_cast<void*>(&impl_t::stream_cb), c);
                self->ctxs.push_back(c);
                return QUIC_STATUS_SUCCESS;
            }
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                // Link down. Handles are NOT closed here (callbacks never
                // close) — the destructor or the listener replacement owns it.
                self->up.store(false, std::memory_order_relaxed);
                self->session.store(false, std::memory_order_relaxed);
                self->signal_handshake(false);
                self->signal_session(false);
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

        // ONE session at a time (the quic_transport_t one-peer model): refuse a
        // second while the first is up; a departed peer's handles are closed
        // here (this thread, a different connection — legal) and replaced.
        std::vector<stream_ctx_t*> old_ctxs;
        HQUIC old_conn = nullptr;
        {
            const std::lock_guard lock(self->conn_m);
            if (self->conn != nullptr && self->up.load(std::memory_order_relaxed))
                return QUIC_STATUS_CONNECTION_REFUSED;
            old_ctxs = std::exchange(self->ctxs, {});
            for (stream_ctx_t* c : old_ctxs) c->harvested = true;  // never re-adopted
            old_conn = std::exchange(self->conn, nullptr);
            self->frame_stream = nullptr;
        }
        // Closing blocks until each handle's callbacks drain, so after this
        // point nothing touches the RX state — safe to reset for the new peer.
        for (stream_ctx_t* c : old_ctxs) {
            if (c->h != nullptr) self->api->StreamClose(c->h);
            tsan_acquire(c);  // its callbacks have drained — take their writes
            delete c;
        }
        if (old_conn != nullptr) self->api->ConnectionClose(old_conn);
        self->reset_rx();
        self->session.store(false, std::memory_order_relaxed);

        HQUIC c = ev->NEW_CONNECTION.Connection;
        self->api->SetCallbackHandler(c, reinterpret_cast<void*>(&impl_t::conn_cb), self);
        const QUIC_STATUS st = self->api->ConnectionSetConfiguration(c, self->config);
        if (QUIC_FAILED(st)) return st;  // msquic tears the rejected connection down
        const std::lock_guard lock(self->conn_m);
        self->conn = c;
        return QUIC_STATUS_SUCCESS;
    }

    // Shared bring-up: api table, registration, configuration + credential.
    bool open_common(const QUIC_SETTINGS& settings, const QUIC_CREDENTIAL_CONFIG& cred) {
        if (QUIC_FAILED(MsQuicOpen2(&api))) {
            api = nullptr;
            return false;
        }
        const QUIC_REGISTRATION_CONFIG reg_cfg{"libtracer_wt", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        if (QUIC_FAILED(api->RegistrationOpen(&reg_cfg, &reg))) return false;
        if (QUIC_FAILED(api->ConfigurationOpen(reg, &kAlpnH3, 1, &settings, sizeof(settings),
                                               nullptr, &config)))
            return false;
        return !QUIC_FAILED(api->ConfigurationLoadCredential(config, &cred));
    }

    // Fill the QUIC_SETTINGS both roles share: no idle teardown (#66 owns link
    // lifecycle), room for the session's streams (CONNECT + frame channel +
    // slack bidi; control + 2 QPACK + WT-uni slack), and datagram receive
    // support (H3 datagrams are advertised in SETTINGS; browsers expect the
    // transport parameter even for a streams-only session).
    static QUIC_SETTINGS session_settings() {
        QUIC_SETTINGS s{};
        s.IdleTimeoutMs = 0;
        s.IsSet.IdleTimeoutMs = TRUE;
        s.PeerBidiStreamCount = 4;
        s.IsSet.PeerBidiStreamCount = TRUE;
        s.PeerUnidiStreamCount = 8;
        s.IsSet.PeerUnidiStreamCount = TRUE;
        s.DatagramReceiveEnabled = TRUE;
        s.IsSet.DatagramReceiveEnabled = TRUE;
        return s;
    }

    // Wait for a two-stage rendezvous flag (dial ctor). Returns the ok flag.
    bool wait_stage(bool& done, bool& ok) {
        std::unique_lock lock(wait_m);
        wait_cv.wait_for(lock, std::chrono::milliseconds(kHandshakeWaitMs),
                         [&done] { return done; });
        return done && ok;
    }
};

webtransport_transport_t::webtransport_transport_t(const std::string& peer_host,
                                                   std::uint16_t peer_port, const std::string& path,
                                                   webtransport_dial_tls_t tls,
                                                   mem::mem_backend_t* backend)
    : impl_(std::make_unique<impl_t>()) {
    impl_t& i = *impl_;
    i.backend = backend;
    i.authority = peer_host + ":" + std::to_string(peer_port);
    i.path = path.empty() ? "/" : path;

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
    if (!i.open_common(impl_t::session_settings(), cred)) return;

    if (QUIC_FAILED(i.api->ConnectionOpen(i.reg, &impl_t::conn_cb, &i, &i.conn))) {
        i.conn = nullptr;
        return;
    }
    tsan_release(&i);  // publish the constructed impl to the callbacks (see file top)
    if (QUIC_FAILED(i.api->ConnectionStart(i.conn, i.config, QUIC_ADDRESS_FAMILY_UNSPEC,
                                           peer_host.c_str(), peer_port)))
        return;

    // Stage 1: the QUIC handshake (the transport_quic.cpp dial shape).
    if (!i.wait_stage(i.handshake_done, i.handshake_ok)) return;

    // Our H3 face (control + QPACK streams), then the extended CONNECT.
    i.open_h3_face(i.conn);
    auto connect_ctx = std::make_unique<impl_t::stream_ctx_t>();
    connect_ctx->owner = &i;
    connect_ctx->kind = impl_t::stream_ctx_t::kind_t::CONNECT_CLIENT;
    HQUIC connect_stream = nullptr;
    if (QUIC_FAILED(i.api->StreamOpen(i.conn, QUIC_STREAM_OPEN_FLAG_NONE, &impl_t::stream_cb,
                                      connect_ctx.get(), &connect_stream)))
        return;
    connect_ctx->h = connect_stream;
    tsan_release(connect_ctx.get());  // publish the ctx to its callbacks (see file top)
    if (QUIC_FAILED(i.api->StreamStart(connect_stream, QUIC_STREAM_START_FLAG_NONE))) {
        i.api->StreamClose(connect_stream);
        return;
    }
    {
        const std::lock_guard lock(i.conn_m);
        i.ctxs.push_back(connect_ctx.release());
    }
    std::uint64_t sid = 0;
    std::uint32_t sz = sizeof(sid);
    if (QUIC_SUCCEEDED(i.api->GetParam(connect_stream, QUIC_PARAM_STREAM_ID, &sz, &sid)))
        i.connect_stream_id = sid;
    std::vector<std::uint8_t> req;
    wt_h3::append_h3_frame(req, wt_h3::kFrameHeaders,
                           wt_h3::encode_connect_field_section(i.authority, i.path));
    i.send_raw(connect_stream, std::move(req));

    // Stage 2: the 200 (the session).
    if (!i.wait_stage(i.session_done, i.session_ok)) return;

    // Open THE frame channel: a bidirectional WebTransport stream announcing
    // itself with 0x41 + the CONNECT stream's id (the browser
    // createBidirectionalStream() wire shape), then length-prefixed records.
    auto frame_ctx = std::make_unique<impl_t::stream_ctx_t>();
    frame_ctx->owner = &i;
    frame_ctx->kind = impl_t::stream_ctx_t::kind_t::FRAME;
    HQUIC fs = nullptr;
    if (QUIC_FAILED(i.api->StreamOpen(i.conn, QUIC_STREAM_OPEN_FLAG_NONE, &impl_t::stream_cb,
                                      frame_ctx.get(), &fs)))
        return;
    frame_ctx->h = fs;
    tsan_release(frame_ctx.get());  // publish the ctx to its callbacks (see file top)
    if (QUIC_FAILED(i.api->StreamStart(fs, QUIC_STREAM_START_FLAG_NONE))) {
        i.api->StreamClose(fs);
        return;
    }
    std::vector<std::uint8_t> preamble;
    wt_h3::append_varint(preamble, wt_h3::kFrameWtStream);
    wt_h3::append_varint(preamble, i.connect_stream_id);
    i.send_raw(fs, std::move(preamble));
    {
        const std::lock_guard lock(i.conn_m);
        i.ctxs.push_back(frame_ctx.release());
        i.frame_stream = fs;
    }
    i.open_ok = true;
}

webtransport_transport_t::webtransport_transport_t(std::uint16_t bind_port,
                                                   const std::string& cert_file,
                                                   const std::string& key_file,
                                                   mem::mem_backend_t* backend)
    : impl_(std::make_unique<impl_t>()) {
    impl_t& i = *impl_;
    i.backend = backend;
    i.listen = true;

    QUIC_CERTIFICATE_FILE cert{};
    cert.PrivateKeyFile = key_file.c_str();
    cert.CertificateFile = cert_file.c_str();
    QUIC_CREDENTIAL_CONFIG cred{};
    cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    cred.CertificateFile = &cert;
    if (!i.open_common(impl_t::session_settings(), cred)) return;  // bad cert/key fails HERE

    if (QUIC_FAILED(i.api->ListenerOpen(i.reg, &impl_t::listener_cb, &i, &i.listener))) {
        i.listener = nullptr;
        return;
    }
    QUIC_ADDR addr{};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, bind_port);
    tsan_release(&i);  // publish the constructed impl to the callbacks (see file top)
    if (QUIC_FAILED(i.api->ListenerStart(i.listener, &kAlpnH3, 1, &addr))) return;

    std::uint32_t len = sizeof(addr);
    if (QUIC_SUCCEEDED(i.api->GetParam(i.listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &len, &addr)))
        i.bound_port = QuicAddrGetPort(&addr);
    i.open_ok = true;
}

webtransport_transport_t::~webtransport_transport_t() {
    impl_t& i = *impl_;
    if (i.api == nullptr) return;  // MsQuicOpen2 failed — nothing to unwind
    // 1. Stop accepting: ListenerClose blocks until listener callbacks drain.
    if (i.listener != nullptr) i.api->ListenerClose(i.listener);
    // 2. Take the live session's handles (send() sees null and no-ops).
    std::vector<impl_t::stream_ctx_t*> ctxs;
    HQUIC conn = nullptr;
    {
        const std::lock_guard lock(i.conn_m);
        ctxs = std::exchange(i.ctxs, {});
        for (impl_t::stream_ctx_t* c : ctxs) c->harvested = true;  // never re-adopted
        i.frame_stream = nullptr;
        conn = std::exchange(i.conn, nullptr);
    }
    // 3. Close every stream, then the connection: each Close blocks until that
    //    handle's callbacks drain (in-flight sends complete Canceled).
    for (impl_t::stream_ctx_t* c : ctxs) {
        if (c->h != nullptr) {
            i.api->StreamShutdown(c->h, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            i.api->StreamClose(c->h);
        }
        tsan_acquire(c);  // its callbacks have drained — take their writes
        delete c;
    }
    if (conn != nullptr) {
        i.api->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        i.api->ConnectionClose(conn);
    }
    // 4. Parents last: configuration, registration, then the api table.
    if (i.config != nullptr) i.api->ConfigurationClose(i.config);
    if (i.reg != nullptr) i.api->RegistrationClose(i.reg);
    MsQuicClose(i.api);
    // Every callback has drained — take their published writes before the
    // members (framer_ et al.) destruct.
    tsan_acquire(&i);
}

void webtransport_transport_t::set_receiver(receiver_t receiver) {
    const std::lock_guard lock(impl_->m);
    impl_->receiver = std::move(receiver);
}

void webtransport_transport_t::set_view_receiver(view_receiver_t receiver) {
    const std::lock_guard lock(impl_->m);
    impl_->view_receiver = std::move(receiver);
}

void webtransport_transport_t::send(std::span<const std::byte> frame) {
    if (frame.size() > kMaxFrame) return;  // the peer would reject it as malformed
    auto ctx = std::make_unique<send_ctx_t>(frame.size());
    if (!frame.empty()) std::memcpy(ctx->bytes.data() + kPrefixBytes, frame.data(), frame.size());
    impl_t& i = *impl_;
    const std::lock_guard lock(i.conn_m);
    if (i.frame_stream == nullptr) return;  // no session frame stream (yet / anymore) — drop
    tsan_release(ctx.get());                // pairs with SEND_COMPLETE's acquire (see file top)
    if (QUIC_SUCCEEDED(
            i.api->StreamSend(i.frame_stream, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx.get())))
        (void)ctx.release();
}

void webtransport_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    std::size_t total = 0;
    for (const auto& s : iov) total += s.size();
    if (total > kMaxFrame) return;  // the peer would reject it as malformed

    // ONE gather copy (the transport_quic.cpp rationale: every QUIC_BUFFER
    // must outlive the call until SEND_COMPLETE; the seam's spans are borrowed
    // only for this call).
    auto ctx = std::make_unique<send_ctx_t>(total);
    std::size_t off = kPrefixBytes;
    for (const auto& s : iov) {
        if (s.empty()) continue;
        std::memcpy(ctx->bytes.data() + off, s.data(), s.size());
        off += s.size();
    }
    impl_t& i = *impl_;
    const std::lock_guard lock(i.conn_m);
    if (i.frame_stream == nullptr) return;
    tsan_release(ctx.get());  // pairs with SEND_COMPLETE's acquire (see file top)
    if (QUIC_SUCCEEDED(
            i.api->StreamSend(i.frame_stream, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx.get())))
        (void)ctx.release();
}

bool webtransport_transport_t::ok() const noexcept { return impl_->open_ok; }

std::uint16_t webtransport_transport_t::local_port() const noexcept { return impl_->bound_port; }

bool webtransport_transport_t::link_up() const noexcept {
    return impl_->up.load(std::memory_order_relaxed);
}

bool webtransport_transport_t::session_up() const noexcept {
    return impl_->session.load(std::memory_order_relaxed);
}

std::uint64_t webtransport_transport_t::dropped_rx() const noexcept {
    return impl_->dropped_rx.load(std::memory_order_relaxed);
}

std::uint64_t webtransport_transport_t::malformed_rx() const noexcept {
    return impl_->malformed_rx.load(std::memory_order_relaxed);
}

namespace {

// The webtransport kind's PRIVATE config keys, parsed module-side from the raw
// SPEC config SETTINGS TLV (ADR-0043 §5 leanness — identical to the quic kind):
// NAME "cert" NAME <path>, NAME "key" NAME <path>; unknown pairs ignored.
struct wt_private_cfg_t {
    std::string cert;  // PEM server-certificate path (LISTEN)
    std::string key;   // PEM private-key path matching cert (LISTEN)
};

[[nodiscard]] wt_private_cfg_t parse_wt_config(const wire::tlv_t* raw_config) {
    wt_private_cfg_t out;
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

transport_vertex_t::transport_factory_t webtransport_transport_factory(
    mem::mem_backend_t* rx_backend) {
    return [rx_backend](
               const conn_settings_t& s,
               const wire::tlv_t* raw_config) -> graph::result_t<std::unique_ptr<transport_t>> {
        std::unique_ptr<webtransport_transport_t> t;
        if (s.role == conn_role_t::DIAL) {
            if (s.addr.empty() || s.port == 0)
                return std::unexpected(graph::status_t::TYPE_MISMATCH);
            t = std::make_unique<webtransport_transport_t>(
                s.addr, s.port, "/",
                webtransport_dial_tls_t{.ca_file = {}, .insecure_no_verify = true}, rx_backend);
            if (!t->ok()) return std::unexpected(graph::status_t::NOT_FOUND);  // handshake failed
            return t;
        }
        const wt_private_cfg_t priv = parse_wt_config(raw_config);
        if (s.port == 0 || priv.cert.empty() || priv.key.empty())
            return std::unexpected(graph::status_t::TYPE_MISMATCH);
        t = std::make_unique<webtransport_transport_t>(s.port, priv.cert, priv.key, rx_backend);
        if (!t->ok()) return std::unexpected(graph::status_t::NOT_FOUND);  // bind/cred failed
        return t;
    };
}

}  // namespace tr::net
