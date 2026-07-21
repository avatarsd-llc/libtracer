/**
 * @file
 * @brief webtransport_transport_t (ADR-0043 Phase B) — the
 *        WebTransport-over-HTTP/3 endpoint inside the SEPARATE libtracer_quic
 *        module.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The same msquic investment as Phase A; the core library never references any
 * of this. The H3/QPACK surface is the deliberately minimal subset in
 * src/wt_h3.hpp (see its header for exactly what is implemented and why it
 * suffices); everything QUIC-mechanical — handle ownership, teardown ordering,
 * the per-connection callback serialization and its TSan annotations, the RX
 * length-prefix reassembly and the one-copy TX contract — lives in the shared
 * msquic_endpoint_t base (src/msquic_endpoint.hpp). This transport keeps ONLY
 * its variance points, a session with MULTIPLE streams:
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
 * exactly the Phase A discipline (both now enforced by the base).
 */

#include "libtracer/transport_webtransport.hpp"

#include <msquic.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame.hpp"
#include "msquic_endpoint.hpp"
#include "wt_h3.hpp"

namespace tr::net {

namespace {

/** @brief App-layer connection-shutdown code: not a WebTransport extended
 *         CONNECT. */
constexpr std::uint64_t kAppErrBadRequest = 0x2;
/** @brief Per-stream classification/HEADERS accumulation cap (DoS bound). */
constexpr std::size_t kMaxHandshakeBytes = 16'384;

/** @brief The HTTP/3 ALPN every WebTransport endpoint negotiates. */
const QUIC_BUFFER kAlpnH3{sizeof("h3") - 1, reinterpret_cast<uint8_t*>(const_cast<char*>("h3"))};

}  // namespace

/**
 * @brief The pimpl: the msquic-mechanical base plus this transport's variance
 *        points — per-stream classification contexts and the H3 handshake.
 */
struct webtransport_transport_t::impl_t : msquic_endpoint_t {
    /**
     * @brief Per-stream state (classification + handshake accumulation).
     *
     * Touched only on that stream's callback (msquic serializes per-connection
     * callbacks); the ctx LIST is guarded by conn_m. Contexts are deleted only
     * by the destructor / the listener replacement path — never by callbacks.
     */
    struct stream_ctx_t {
        /** @brief What the stream is (or is still being classified as). */
        enum class kind_t {
            CLASSIFY_UNI,   /**< @brief Inbound unidirectional: awaiting its stream-type
                                        varint. */
            CLASSIFY_BIDI,  /**< @brief Inbound bidirectional: HEADERS(CONNECT) or 0x41 frame
                                        channel. */
            CONNECT_CLIENT, /**< @brief DIAL: our CONNECT stream, awaiting the 200 response. */
            SESSION,        /**< @brief The accepted CONNECT stream — the session's lifetime
                                        handle. */
            FRAME,          /**< @brief The adopted WebTransport frame channel. */
            DRAIN,          /**< @brief Classified, contents irrelevant — discard. */
            LOCAL,          /**< @brief Locally-opened control/QPACK stream (sends only). */
        };
        impl_t* owner = nullptr;       /**< @brief The owning endpoint. */
        HQUIC h = nullptr;             /**< @brief The stream handle. */
        kind_t kind = kind_t::DRAIN;   /**< @brief The classification state. */
        std::vector<std::uint8_t> acc; /**< @brief Handshake bytes, bounded by
                                                   kMaxHandshakeBytes. */
        bool harvested = false;        /**< @brief Guarded by conn_m: the dtor/replacement path
                                                   took this handle for closing — never
                                                   re-adopt it. */
    };

    std::string authority; /**< @brief DIAL: the CONNECT :authority. */
    std::string path;      /**< @brief DIAL: the CONNECT :path. */

    /** @brief Every stream context of the live session (guarded by conn_m). */
    std::vector<stream_ctx_t*> ctxs;
    /** @brief Extended CONNECT accepted (200) — the session state. */
    std::atomic<bool> session{false};
    /** @brief The CONNECT stream's id (the 0x41 preamble references it). */
    std::uint64_t connect_stream_id = 0;

    /**
     * @name DIAL rendezvous stage 2: session established (stage 1, the QUIC
     *       handshake, lives in the base).
     * @{
     */
    bool session_done = false; /**< @brief Session stage resolved. */
    bool session_ok = false;   /**< @brief Session stage outcome. */
    /** @} */

    /** @brief Teardown-first destructor (the msquic_endpoint_t contract). */
    ~impl_t() { teardown(); }

    /** @brief Resolve the session rendezvous stage (idempotent). */
    void signal_session(bool ok) {
        {
            const std::lock_guard lock(wait_m);
            if (session_done) return;
            session_done = true;
            session_ok = ok;
        }
        wait_cv.notify_all();
    }

    /**
     * @brief Open a local unidirectional stream on @p on_conn and write
     *        @p bytes on it (the control and QPACK encoder/decoder streams).
     *
     * Runs entirely under conn_m and only while @p on_conn is STILL the live
     * connection, so a teardown/replacement racing this open can never orphan
     * the handle: the ctx joins ctxs under the same lock the harvester takes.
     */
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
        tsan_release(ctx.get());  // publish the ctx to its callbacks (see the base header)
        if (QUIC_FAILED(api->StreamStart(s, QUIC_STREAM_START_FLAG_NONE))) {
            api->StreamClose(s);
            return;
        }
        send_raw(s, std::move(bytes));
        ctxs.push_back(ctx.release());
    }

    /** @brief The endpoint's H3 face on @p on_conn: the control stream
     *         (SETTINGS) plus the two mandatory QPACK streams (RFC 9204 §4.2 —
     *         empty beyond their type byte, since the dynamic table stays at
     *         capacity 0). */
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

    /** @brief Accumulate handshake bytes with the DoS cap. False => connection
     *         down. */
    bool accumulate(stream_ctx_t& c, const std::uint8_t* p, std::size_t n) {
        if (c.acc.size() + n > kMaxHandshakeBytes) {
            shutdown_conn(kAppErrBadRequest);
            return false;
        }
        c.acc.insert(c.acc.end(), p, p + n);
        return true;
    }

    /** @brief A peer unidirectional stream: its first varint is the stream
     *         type; every type (control / QPACK / push / WT-uni) is drained —
     *         the SETTINGS the peer sends are not needed (we are lenient; ours
     *         are always advertised). */
    void classify_uni(stream_ctx_t& c) {
        if (wt_h3::read_varint(c.acc)) {
            c.kind = stream_ctx_t::kind_t::DRAIN;
            c.acc.clear();
            c.acc.shrink_to_fit();
        }
    }

    /** @brief A peer bidirectional stream on the LISTEN side: either the
     *         extended CONNECT request (HEADERS) or the WebTransport frame
     *         channel (0x41). Returns false when the connection was shut
     *         down. */
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

    /** @brief The DIAL side's CONNECT stream: parse the response HEADERS,
     *         demand 200. */
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

    /** @brief Route one RECEIVE chunk to the stream's state. False => stop
     *         consuming this event's remaining chunks (the connection was shut
     *         down). */
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

    /** @brief The msquic stream callback (worker threads; serialized per
     *         connection): routes RX by the ctx's classification, frees send
     *         buffers on SEND_COMPLETE, and flips the session flags when the
     *         CONNECT or frame stream goes down. */
    static QUIC_STATUS QUIC_API stream_cb(HQUIC /*stream*/, void* ctx, QUIC_STREAM_EVENT* ev) {
        auto* c = static_cast<stream_ctx_t*>(ctx);
        // Two TSan edges (see the base header): the ctx guard pairs with the
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
                complete_send(ev->SEND_COMPLETE.ClientContext);
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

    /** @brief CONNECTED hook: the server presents its H3 face as soon as QUIC
     *         is up — the browser waits for SETTINGS before sending extended
     *         CONNECT. (The dial side sends its face from the constructor
     *         thread.) */
    void on_connected(HQUIC c) override {
        if (listen) open_h3_face(c);
    }

    /** @brief Connection-down hook: the session falls with the connection. */
    void on_conn_down() override {
        session.store(false, std::memory_order_relaxed);
        signal_session(false);
    }

    /**
     * @brief A peer stream arrived: classify lazily from its first bytes; the
     *        context joins the session's ctx list for teardown.
     *
     * Adoption is conditional on this STILL being the live connection, under
     * conn_m: the destructor (and the listener replacement path) harvests the
     * ctx list under the same lock, so a stream that arrives after the harvest
     * must be REFUSED — msquic then closes it itself — or its handle would
     * never be closed and RegistrationClose would wait on the connection
     * forever.
     */
    QUIC_STATUS on_peer_stream_started(HQUIC c_h, QUIC_CONNECTION_EVENT* ev) override {
        const std::lock_guard lock(conn_m);
        if (conn != c_h) return QUIC_STATUS_ABORTED;  // tearing down / replaced
        auto* c = new stream_ctx_t{};
        c->owner = this;
        c->h = ev->PEER_STREAM_STARTED.Stream;
        c->kind = (ev->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) != 0
                      ? stream_ctx_t::kind_t::CLASSIFY_UNI
                      : stream_ctx_t::kind_t::CLASSIFY_BIDI;
        tsan_release(c);  // publish the ctx to its callbacks (see the base header)
        api->SetCallbackHandler(ev->PEER_STREAM_STARTED.Stream,
                                reinterpret_cast<void*>(&impl_t::stream_cb), c);
        ctxs.push_back(c);
        return QUIC_STATUS_SUCCESS;
    }

    /** @brief One-peer replacement harvest: detach and close every departed
     *         stream ctx + the connection (refuse while the peer is up). */
    bool replace_peer() override {
        std::vector<stream_ctx_t*> old_ctxs;
        HQUIC old_conn = nullptr;
        {
            const std::lock_guard lock(conn_m);
            if (conn != nullptr && up.load(std::memory_order_relaxed)) return false;
            old_ctxs = std::exchange(ctxs, {});
            for (stream_ctx_t* c : old_ctxs) c->harvested = true;  // never re-adopted
            old_conn = std::exchange(conn, nullptr);
            frame_stream = nullptr;
        }
        // Closing blocks until each handle's callbacks drain — after this,
        // nothing touches the RX state (the base resets it for the new peer).
        for (stream_ctx_t* c : old_ctxs) {
            if (c->h != nullptr) api->StreamClose(c->h);
            tsan_acquire(c);  // its callbacks have drained — take their writes
            delete c;
        }
        if (old_conn != nullptr) api->ConnectionClose(old_conn);
        session.store(false, std::memory_order_relaxed);
        return true;
    }

    /** @brief Teardown harvest: detach every stream ctx + the connection under
     *         conn_m, abort+close each stream, hand the connection back. */
    HQUIC harvest_and_close_streams() override {
        std::vector<stream_ctx_t*> old_ctxs;
        HQUIC c = nullptr;
        {
            const std::lock_guard lock(conn_m);
            old_ctxs = std::exchange(ctxs, {});
            for (stream_ctx_t* sc : old_ctxs) sc->harvested = true;  // never re-adopted
            frame_stream = nullptr;
            c = std::exchange(conn, nullptr);
        }
        for (stream_ctx_t* sc : old_ctxs) {
            if (sc->h != nullptr) {
                api->StreamShutdown(sc->h, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                api->StreamClose(sc->h);
            }
            tsan_acquire(sc);  // its callbacks have drained — take their writes
            delete sc;
        }
        return c;
    }

    /** @brief Fill the QUIC_SETTINGS both roles share: no idle teardown (#66
     *         owns link lifecycle), room for the session's streams (CONNECT +
     *         frame channel + slack bidi; control + 2 QPACK + WT-uni slack),
     *         and datagram receive support (H3 datagrams are advertised in
     *         SETTINGS; browsers expect the transport parameter even for a
     *         streams-only session). */
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
};

webtransport_transport_t::webtransport_transport_t(const std::string& peer_host,
                                                   std::uint16_t peer_port, const std::string& path,
                                                   webtransport_dial_tls_t tls,
                                                   mem::mem_backend_t* backend,
                                                   std::size_t max_frame)
    : impl_(std::make_unique<impl_t>()) {
    impl_t& i = *impl_;
    i.rx = &rx_;  // the delivery-tier slot lives in the transport_t base
    i.backend = backend;
    if (max_frame != 0) i.max_frame = max_frame;
    // Departure seam (RFC-0009 §D extended to peer departure): wire the base's
    // connection-down / one-peer replacement harvest to this transport_t's flat
    // link-down notifier. A WebTransport endpoint carries ONE session (one peer
    // at a time), so a departure IS the whole link down (notify_down, like
    // quic / ws-client; never notify_peer_down, the multi-peer bus facet's).
    i.link_down_ctx = this;
    i.link_down_fn = [](void* ctx) { static_cast<webtransport_transport_t*>(ctx)->notify_down(); };
    i.authority = peer_host + ":" + std::to_string(peer_port);
    i.path = path.empty() ? "/" : path;

    // Stage 1: the QUIC handshake (the transport_quic.cpp dial shape — base).
    if (!i.dial("libtracer_wt", kAlpnH3, impl_t::session_settings(), tls.ca_file,
                tls.insecure_no_verify, peer_host, peer_port))
        return;

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
    tsan_release(connect_ctx.get());  // publish the ctx to its callbacks (see the base header)
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
    tsan_release(frame_ctx.get());  // publish the ctx to its callbacks (see the base header)
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
                                                   mem::mem_backend_t* backend,
                                                   std::size_t max_frame)
    : impl_(std::make_unique<impl_t>()) {
    impl_t& i = *impl_;
    i.rx = &rx_;  // the delivery-tier slot lives in the transport_t base
    i.backend = backend;
    if (max_frame != 0) i.max_frame = max_frame;
    // Departure seam (RFC-0009 §D extended to peer departure): wire the base's
    // connection-down / one-peer replacement harvest to this transport_t's flat
    // link-down notifier. A WebTransport endpoint carries ONE session (one peer
    // at a time), so a departure IS the whole link down (notify_down, like
    // quic / ws-client; never notify_peer_down, the multi-peer bus facet's).
    i.link_down_ctx = this;
    i.link_down_fn = [](void* ctx) { static_cast<webtransport_transport_t*>(ctx)->notify_down(); };

    // Listener bring-up (base) — bad cert/key paths fail in there; the session
    // peer opens the frame stream.
    (void)i.listen_start("libtracer_wt", kAlpnH3, impl_t::session_settings(), cert_file, key_file,
                         bind_port);
}

webtransport_transport_t::~webtransport_transport_t() = default;  // ~impl_t runs teardown()

void webtransport_transport_t::send(std::span<const std::byte> frame) { impl_->send_frame(frame); }

void webtransport_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    impl_->send_frame(iov);
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

/**
 * @brief The webtransport kind's PRIVATE config keys, parsed module-side from
 *        the raw SPEC config SETTINGS TLV (ADR-0043 §5 leanness — identical to
 *        the quic kind): NAME "cert" NAME <path>, NAME "key" NAME <path>;
 *        unknown pairs ignored.
 */
struct wt_private_cfg_t {
    std::string cert; /**< @brief PEM server-certificate path (LISTEN). */
    std::string key;  /**< @brief PEM private-key path matching cert (LISTEN). */
};

/** @brief The positional NAME-key / value-pair walk over the raw config TLV. */
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
                webtransport_dial_tls_t{.ca_file = {}, .insecure_no_verify = true}, rx_backend,
                s.max_frame);
            if (!t->ok()) return std::unexpected(graph::status_t::NOT_FOUND);  // handshake failed
            return t;
        }
        const wt_private_cfg_t priv = parse_wt_config(raw_config);
        if (s.port == 0 || priv.cert.empty() || priv.key.empty())
            return std::unexpected(graph::status_t::TYPE_MISMATCH);
        t = std::make_unique<webtransport_transport_t>(s.port, priv.cert, priv.key, rx_backend,
                                                       s.max_frame);
        if (!t->ok()) return std::unexpected(graph::status_t::NOT_FOUND);  // bind/cred failed
        return t;
    };
}

}  // namespace tr::net
