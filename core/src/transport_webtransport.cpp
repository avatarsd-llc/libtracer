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
 *     200, and kept open as the session's lifetime handle; the FIRST valid
 *     bidirectional WEBTRANSPORT_STREAM (0x41) is adopted as THE frame
 *     channel, everything after its session-id varint feeding the 4-byte
 *     length-prefix reassembler.
 *
 *     Classification pulls in two directions on purpose (#919 / #920):
 *     IDENTITY is strict — a 0x41 stream is adopted only after the extended
 *     CONNECT succeeded, only when its session-id varint names THAT CONNECT
 *     stream, and only once (first valid one wins); anything else is refused
 *     at STREAM scope, never by killing the session. UNKNOWN EXTENSIONS are
 *     lenient — an unrecognized H3 frame type is skipped by its declared
 *     length and classification continues, because RFC 9114 §7.2.8 requires
 *     unknown frame types to be ignored and §9 has conformant peers (Chrome)
 *     emit reserved GREASE types precisely to catch endpoints that don't.
 *     Pinning who may speak and ignoring what you don't understand are not
 *     in tension: the first is authentication, the second is extensibility.
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

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/config_reader.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/mem_heap.hpp"
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
    /**
     * @brief The session's CONNECT :path — DIAL: the one this endpoint
     *        requests (written in the constructor, before any callback exists);
     *        LISTEN: the one the accepted CONNECT named (written on the stream
     *        callback under conn_m, a derived slot of the live session).
     */
    std::string path;

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
     * @brief Open ONE locally-initiated stream on the live connection, write
     *        @p preamble on it, and publish its ctx — the whole sequence under
     *        conn_m, for every local open this endpoint performs (the H3 face,
     *        the dial's extended CONNECT, and the dial's frame channel).
     *
     * The liveness RECHECK is why the three sites share one body: `conn` is
     * read under the same lock the harvesters (the destructor's
     * harvest_and_close_streams, the listener's replace_peer) take to null it,
     * so an open can never be issued on a connection they have already claimed,
     * and the ctx joins `ctxs` before the lock is dropped — never orphaned
     * between StreamOpen and publication. The dial constructor open-coded this
     * sequence twice without the recheck; the invariant is now local to one
     * function instead of restated per site.
     *
     * @param flags    QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, or NONE for a
     *                 bidirectional stream.
     * @param kind     The ctx's classification (LOCAL / CONNECT_CLIENT /
     *                 FRAME). A FRAME open also adopts the stream as THE frame
     *                 channel, under the same lock.
     * @param preamble Bytes written on the stream immediately after it starts
     *                 (the stream-type varint, the CONNECT request, or the
     *                 0x41 + session-id header).
     * @param expect   The connection the caller means — a callback's event
     *                 handle, refusing the open if the peer has since been
     *                 replaced. Null means "whatever connection is live now"
     *                 (the dial constructor, which holds no event handle).
     * @return The started stream handle, or null when nothing was opened.
     */
    HQUIC open_stream(QUIC_STREAM_OPEN_FLAGS flags, stream_ctx_t::kind_t kind,
                      std::vector<std::uint8_t> preamble, HQUIC expect = nullptr) {
        const std::lock_guard lock(conn_m);
        HQUIC on_conn = conn;
        if (on_conn == nullptr) return nullptr;                      // tearing down
        if (expect != nullptr && expect != on_conn) return nullptr;  // replaced
        // Not peer-reachable (local opens only), but it shares `ctxs` with the peer-driven
        // path, so the capacity is taken here too and BEFORE `StreamOpen` — a throw at the
        // `push_back` below would strand a started stream whose ctx msquic already holds.
        if (!detail::try_reserve(ctxs, ctxs.size() + 1)) return nullptr;
        auto ctx = std::unique_ptr<stream_ctx_t>(new (std::nothrow) stream_ctx_t());
        if (!ctx) return nullptr;
        ctx->owner = this;
        ctx->kind = kind;
        HQUIC s = nullptr;
        if (QUIC_FAILED(api->StreamOpen(on_conn, flags, &stream_cb, ctx.get(), &s))) return nullptr;
        ctx->h = s;
        tsan_release(ctx.get());  // publish the ctx to its callbacks (see the base header)
        if (QUIC_FAILED(api->StreamStart(s, QUIC_STREAM_START_FLAG_NONE))) {
            api->StreamClose(s);
            return nullptr;
        }
        send_raw(s, std::move(preamble));
        // Only after the preamble is queued: a send() racing this open must not
        // slip a length-prefixed record in front of the 0x41 header.
        if (kind == stream_ctx_t::kind_t::FRAME) frame_stream = s;
        ctxs.push_back(ctx.release());
        return s;
    }

    /** @brief The endpoint's H3 face on @p on_conn: the control stream
     *         (SETTINGS) plus the two mandatory QPACK streams (RFC 9204 §4.2 —
     *         empty beyond their type byte, since the dynamic table stays at
     *         capacity 0). Null @p on_conn = the live connection (the dial
     *         constructor); the LISTEN side passes its CONNECTED event handle. */
    void open_h3_face(HQUIC on_conn = nullptr) {
        using kind_t = stream_ctx_t::kind_t;
        (void)open_stream(QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, kind_t::LOCAL,
                          wt_h3::control_stream_bytes(), on_conn);
        std::vector<std::uint8_t> enc;
        wt_h3::append_varint(enc, wt_h3::kStreamTypeQpackEncoder);
        (void)open_stream(QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, kind_t::LOCAL, std::move(enc),
                          on_conn);
        std::vector<std::uint8_t> dec;
        wt_h3::append_varint(dec, wt_h3::kStreamTypeQpackDecoder);
        (void)open_stream(QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, kind_t::LOCAL, std::move(dec),
                          on_conn);
    }

    // ---- inbound stream classification + the H3 handshake ----

    /** @brief Accumulate handshake bytes with the DoS cap. False => connection
     *         down. */
    bool accumulate(stream_ctx_t& c, const std::uint8_t* p, std::size_t n) {
        if (c.acc.size() + n > kMaxHandshakeBytes) {
            shutdown_conn(kAppErrBadRequest);
            return false;
        }
        // PEER-SIZED, so the growth goes through the failable seam (#1108). The cap above
        // bounds how MANY bytes a peer may make us hold; it does not make the allocation
        // that holds them succeed.
        //
        // The two failures get DIFFERENT scopes on purpose. Exceeding the cap is a
        // statement about the PEER — it sent more handshake than the protocol allows — so
        // it stays connection-fatal, as it always was. Running out of memory is a statement
        // about US, and taking down a session the peer already established because our heap
        // is tight is exactly the over-broad refusal #919 removed. So an OOM aborts just
        // this stream and returns true: the connection, and any live session on it, stay up.
        if (!detail::try_reserve(c.acc, c.acc.size() + n)) {
            refuse_stream(c);
            return true;
        }
        c.acc.insert(c.acc.end(), p, p + n);  // within capacity — cannot reallocate
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

    /**
     * @brief Refuse ONE peer stream: abort it in both directions and park its
     *        ctx as DRAIN, leaving the connection and the live session alone.
     *
     * The refusal SCOPE is the point (#919). A nonconforming or hostile stream
     * must not be able to take down a session the peer already established, so
     * a refused 0x41 candidate never reaches `shutdown_conn` — the same
     * peer-controlled-topology rejection the PONG ruling (#848) applies to
     * frames. The handle stays in `ctxs`: the destructor / replacement path
     * still owns every close, and callbacks never close handles.
     */
    void refuse_stream(stream_ctx_t& c) {
        c.kind = stream_ctx_t::kind_t::DRAIN;
        c.acc.clear();
        c.acc.shrink_to_fit();
        if (c.h != nullptr)
            api->StreamShutdown(c.h, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, kAppErrBadRequest);
    }

    /**
     * @brief A peer bidirectional stream on the LISTEN side: either the
     *        extended CONNECT request (HEADERS) or the WebTransport frame
     *        channel (0x41); any other frame type is an unknown extension and
     *        is skipped by its declared length. Returns false when the
     *        connection was shut down.
     *
     * A skip LOOP, not a single look (#920): RFC 9114 §7.2.8 makes ignoring
     * unrecognized frame types mandatory, and §9 has conformant peers emit
     * reserved (GREASE) types `0x1f * N + 0x21` on any stream to prove their
     * peers do it — Chrome sends them ahead of the extended CONNECT, so
     * treating the first non-0x41/non-HEADERS type as fatal let a CONFORMANT
     * browser take the node down. Every skipped frame is dropped from `acc`
     * before the "need more bytes" return, so an unbounded GREASE run is
     * bounded memory rather than a slow march into the accumulation cap.
     */
    bool classify_bidi(stream_ctx_t& c) {
        std::size_t skipped = 0;  // acc bytes consumed by unknown frames this pass
        while (true) {
            const std::span<const std::uint8_t> in =
                std::span<const std::uint8_t>(c.acc).subspan(skipped);
            const auto t = wt_h3::read_varint(in);
            if (!t) break;  // need more bytes

            if (t->value == wt_h3::kFrameWtStream) {
                // The frame channel. Adoption is STRICT — the deliberate
                // opposite pull from the lenient unknown-frame skip below, and
                // not a contradiction: identity must be pinned, unknown
                // extensions must be ignored. Three guards, in order:
                //   1. the extended CONNECT must already have been accepted
                //      (no frames before the handshake — the bypass arm);
                //   2. the session-id varint must name THAT CONNECT stream
                //      (draft-ietf-webtrans-http3 §4.2 — the id IS the CONNECT
                //      stream's id, so a mismatch is not our session);
                //   3. no frame channel may be adopted yet — FIRST valid one
                //      wins. A second 0x41 stream would silently overwrite
                //      frame_stream while the first ctx kept feeding the ONE
                //      shared length-prefix reassembler, interleaving two
                //      independent streams into it.
                // A refusal aborts only that stream (refuse_stream).
                auto rest = in.subspan(t->consumed);
                const auto sid = wt_h3::read_varint(rest);
                if (!sid) break;  // need the session-id varint
                bool refuse = false;
                {
                    // Adopt only while the session is live: a harvested ctx's
                    // handle is being closed by the destructor / the
                    // replacement path — resurrecting it into frame_stream
                    // would leave send() a dangling handle.
                    const std::lock_guard lock(conn_m);
                    if (c.harvested) return true;
                    refuse = !session.load(std::memory_order_relaxed) ||
                             sid->value != connect_stream_id || frame_stream != nullptr;
                    if (!refuse) frame_stream = c.h;
                }
                if (refuse) {
                    refuse_stream(c);
                    return true;  // the session stays up
                }
                c.kind = stream_ctx_t::kind_t::FRAME;
                // The bytes after the 0x41 preamble are the first frame-channel data, and
                // they live in `c.acc` — which must be released before the reassembler runs.
                // MOVE the buffer out rather than copying the tail into a fresh one (#1108):
                // a vector move transfers the heap block without touching it, so `rest` stays
                // valid over the same addresses, and the peer-SIZED allocation that stood here
                // is DELETED rather than made nothrow. The moved-from vector is then cleared
                // and shrunk explicitly, since a moved-from `std::vector` is only guaranteed
                // valid, not empty.
                const std::vector<std::uint8_t> held = std::move(c.acc);
                c.acc.clear();
                c.acc.shrink_to_fit();
                const auto tail = rest.subspan(sid->consumed);
                if (!tail.empty()) return on_rx_chunk(tail.data(), tail.size());
                return true;
            }

            if (t->value == wt_h3::kFrameHeaders) {
                auto rest = in.subspan(t->consumed);
                const auto len = wt_h3::read_varint(rest);
                if (!len) break;
                if (len->value > kMaxHandshakeBytes) {
                    shutdown_conn(kAppErrBadRequest);
                    return false;
                }
                rest = rest.subspan(len->consumed);
                if (rest.size() < len->value) break;  // need the full field section

                const auto headers =
                    wt_h3::decode_field_section(rest.first(static_cast<std::size_t>(len->value)));
                std::string_view method;
                std::string_view protocol;
                std::string_view req_path;
                if (headers) {
                    for (const auto& h : *headers) {
                        if (h.name == ":method") method = h.value;
                        if (h.name == ":protocol") protocol = h.value;
                        if (h.name == ":path") req_path = h.value;
                    }
                }
                if (method != "CONNECT" || protocol != "webtransport") {
                    shutdown_conn(kAppErrBadRequest);  // not a WebTransport session request
                    return false;
                }
                // Accept: 200 on this stream, which stays open as the session
                // handle (its closure ends the session). Any path is served —
                // the resource is RECORDED for session_path(), never used to
                // admit or refuse.
                {
                    const std::lock_guard lock(conn_m);
                    path = std::string(req_path);
                }
                std::vector<std::uint8_t> resp;
                wt_h3::append_h3_frame(resp, wt_h3::kFrameHeaders,
                                       wt_h3::encode_status_200_field_section());
                send_raw(c.h, std::move(resp));
                std::uint64_t sid = 0;
                std::uint32_t sz = sizeof(sid);
                if (QUIC_SUCCEEDED(api->GetParam(c.h, QUIC_PARAM_STREAM_ID, &sz, &sid)))
                    connect_stream_id = sid;
                session.store(true, std::memory_order_relaxed);
                // Everything after the CONNECT on this stream — capsules AND
                // any unknown/GREASE frame — is ignored by the SESSION arm of
                // on_stream_rx, which is exactly RFC 9114 §7.2.8's "ignore".
                c.kind = stream_ctx_t::kind_t::SESSION;
                c.acc.clear();
                c.acc.shrink_to_fit();
                return true;
            }

            // An unknown/reserved (GREASE) frame type: skip its declared body
            // and keep classifying. The length is capped like the HEADERS arm
            // — §7.2.8 obliges us to IGNORE the type, not to buffer an
            // arbitrary pre-handshake payload for it.
            auto rest = in.subspan(t->consumed);
            const auto len = wt_h3::read_varint(rest);
            if (!len) break;  // need the length varint
            if (len->value > kMaxHandshakeBytes) {
                shutdown_conn(kAppErrBadRequest);
                return false;
            }
            rest = rest.subspan(len->consumed);
            if (rest.size() < len->value) break;  // need the whole body before skipping it
            skipped += t->consumed + len->consumed + static_cast<std::size_t>(len->value);
        }

        if (skipped != 0)
            c.acc.erase(c.acc.begin(), c.acc.begin() + static_cast<std::ptrdiff_t>(skipped));
        return true;
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
        // `reap` is the ctx this callback took ownership of, and it is freed BELOW the guard
        // scope on purpose (#1163): `~tsan_cb_guard_t` writes through the ctx pointer, so a
        // `delete` inside the scope is a use-after-free that only a TSan build reports.
        stream_ctx_t* reap = nullptr;
        QUIC_STATUS st = QUIC_STATUS_SUCCESS;
        {
            // Two TSan edges (see the base header): the ctx guard pairs with the
            // publication release where the ctx was handed to msquic (and with the
            // acquire before the harvester deletes it); the impl guard restates
            // msquic's per-connection callback serialization.
            const tsan_cb_guard_t ctx_guard(c);
            impl_t* self = c->owner;
            const tsan_cb_guard_t guard(self);
            st = dispatch_stream_event(*self, *c, ev, reap);
        }
        if (reap != nullptr) {
            tsan_acquire(reap);  // pairs with the ctx guard's release just above
            delete reap;
        }
        return st;
    }

    /**
     * @brief The stream-event switch, split out of @ref stream_cb so the ctx can be freed
     *        after the TSan guards are gone.
     *
     * @param reap Out: set to the ctx when this event ended the stream and this call took
     *             ownership of it. Left null otherwise.
     */
    static QUIC_STATUS dispatch_stream_event(impl_t& self_r, stream_ctx_t& c_r,
                                             QUIC_STREAM_EVENT* ev, stream_ctx_t*& reap) {
        impl_t* const self = &self_r;
        stream_ctx_t* const c = &c_r;
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
            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
                // The stream is over and msquic guarantees no further callback for it, so
                // this is the one point at which a callback may take a ctx (#1163). Before
                // this case existed, nothing did: the only frees were the two WHOLESALE
                // harvests (peer replacement, endpoint teardown), so every stream a peer
                // opened and closed leaked its ctx, its `acc` buffer and its msquic handle
                // for the life of the session. `PeerBidiStreamCount`/`PeerUnidiStreamCount`
                // do not bound that — they cap how many streams may be open AT ONCE, not how
                // many may be opened over a session's life, so a peer that opens and closes
                // in a loop grows the list as fast as RTT allows.
                //
                // `AppCloseInProgress` means WE are already inside `StreamClose` on this
                // handle (a harvester), so the handle must not be closed a second time.
                reap =
                    self->detach_finished_stream(*c, ev->SHUTDOWN_COMPLETE.AppCloseInProgress != 0);
                return QUIC_STATUS_SUCCESS;
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    /**
     * @brief Take a finished stream out of the live session, closing its handle (#1163).
     *
     * @return The ctx, now owned by the caller and to be deleted once the TSan guards are
     *         gone — or null when a harvester already claimed it.
     *
     * The race this resolves is the harvest. `replace_peer` / `harvest_and_close_streams`
     * empty `ctxs` and set `harvested` under `conn_m`, then close and delete OUTSIDE the
     * lock. So whichever of the two reaches `conn_m` first owns the ctx, and the loser sees
     * its decision: a harvester that went first left `harvested` set and the entry gone, and
     * this returns null; a shutdown that goes first removes the entry, so the harvest's
     * `exchange` never sees it. Exactly one side frees.
     *
     * Both branches of the harvest release `conn_m` before calling `StreamClose`, which is
     * what keeps this deadlock-free: `StreamClose` blocks until in-flight callbacks return,
     * and this callback wants the same lock.
     */
    stream_ctx_t* detach_finished_stream(stream_ctx_t& c, bool app_close_in_progress) {
        {
            const std::lock_guard lock(conn_m);
            if (c.harvested) return nullptr;  // a harvester owns it — it will close and free
            const auto it = std::find(ctxs.begin(), ctxs.end(), &c);
            if (it == ctxs.end()) return nullptr;  // never adopted, or already taken
            ctxs.erase(it);
            if (frame_stream != nullptr && frame_stream == c.h) frame_stream = nullptr;
        }
        // Past the erase this ctx is unreachable from any harvest, so the handle is ours.
        HQUIC h = std::exchange(c.h, nullptr);
        if (h != nullptr && !app_close_in_progress) api->StreamClose(h);
        return &c;
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
        // PEER-DRIVEN — one ctx per stream the peer opens, so both allocations here are
        // made failable (#1108), in the order that keeps the handle accounted for:
        //   1. the list's CAPACITY first, so the `push_back` below is in-capacity and cannot
        //      throw. Growing it after the ctx has been handed to `SetCallbackHandler` would
        //      strand a ctx msquic already points at — which is what a bare `push_back` does
        //      today on a failed reallocation.
        //   2. the ctx itself, through nothrow `new`.
        // Either failure aborts JUST this stream. That is #919's refusal scope: a peer
        // stream we cannot afford must not take down a session the peer already
        // established. `ctxs` is bounded by the concurrency caps in `session_settings` plus
        // the per-stream reclamation added in #1163, so this is a growth of a SMALL list —
        // the point is the disposition on failure, not the size.
        if (!detail::try_reserve(ctxs, ctxs.size() + 1)) return QUIC_STATUS_ABORTED;
        auto* c = new (std::nothrow) stream_ctx_t{};
        if (c == nullptr) return QUIC_STATUS_ABORTED;
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

    // Our H3 face (control + QPACK streams), then the extended CONNECT — every
    // open through impl_t::open_stream, which rechecks the connection under
    // conn_m and publishes the ctx before dropping it.
    i.open_h3_face();
    std::vector<std::uint8_t> req;
    wt_h3::append_h3_frame(req, wt_h3::kFrameHeaders,
                           wt_h3::encode_connect_field_section(i.authority, i.path));
    HQUIC connect_stream = i.open_stream(
        QUIC_STREAM_OPEN_FLAG_NONE, impl_t::stream_ctx_t::kind_t::CONNECT_CLIENT, std::move(req));
    if (connect_stream == nullptr) return;
    // The id is read AFTER the request is queued (the request never carries it);
    // the 0x41 frame-channel header below is what needs it.
    std::uint64_t sid = 0;
    std::uint32_t sz = sizeof(sid);
    if (QUIC_SUCCEEDED(i.api->GetParam(connect_stream, QUIC_PARAM_STREAM_ID, &sz, &sid)))
        i.connect_stream_id = sid;

    // Stage 2: the 200 (the session).
    if (!i.wait_stage(i.session_done, i.session_ok)) return;

    // Open THE frame channel: a bidirectional WebTransport stream announcing
    // itself with 0x41 + the CONNECT stream's id (the browser
    // createBidirectionalStream() wire shape), then length-prefixed records.
    std::vector<std::uint8_t> preamble;
    wt_h3::append_varint(preamble, wt_h3::kFrameWtStream);
    wt_h3::append_varint(preamble, i.connect_stream_id);
    if (i.open_stream(QUIC_STREAM_OPEN_FLAG_NONE, impl_t::stream_ctx_t::kind_t::FRAME,
                      std::move(preamble)) == nullptr)
        return;
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

std::string webtransport_transport_t::session_path() const {
    const std::lock_guard lock(impl_->conn_m);
    return impl_->path;
}

std::uint64_t webtransport_transport_t::dropped_rx() const noexcept {
    return impl_->dropped_rx.load(std::memory_order_relaxed);
}

std::uint64_t webtransport_transport_t::malformed_rx() const noexcept {
    return impl_->malformed_rx.load(std::memory_order_relaxed);
}

std::size_t webtransport_transport_t::live_streams() const noexcept {
    const std::lock_guard lock(impl_->conn_m);
    return impl_->ctxs.size();
}

namespace {

/**
 * @brief The webtransport kind's PRIVATE config keys, parsed module-side from
 *        the raw SPEC config SETTINGS TLV (ADR-0043 §5 leanness — identical to
 *        the quic kind, plus one of its own): NAME "cert" NAME <file>, NAME
 *        "key" NAME <file>, NAME "ca" NAME <file>, NAME "insecure" VALUE <u8>,
 *        NAME "path" NAME <resource>; unknown pairs ignored.
 *
 * Two of the five are LISTEN-side (the served credential), two are DIAL-side
 * (how the server certificate is trusted, #918), and one is the DIAL-side
 * extended CONNECT `:path` (#1023) — the only key not shared with `quic`, which
 * has no HTTP layer to carry a resource.
 */
struct wt_private_cfg_t {
    std::string cert;      /**< @brief PEM server-certificate path (LISTEN). */
    std::string key;       /**< @brief PEM private-key path matching cert (LISTEN). */
    std::string ca;        /**< @brief PEM CA-bundle path the DIAL side verifies the
                                       server certificate against; empty = the system
                                       trust store (DIAL). */
    bool insecure = false; /**< @brief DEV ONLY: skip server-certificate validation on
                                       the DIAL side entirely. Must be asked for
                                       explicitly — the default is to verify (DIAL). */
    std::string path;      /**< @brief The extended CONNECT `:path` the dial requests;
                                       empty = the "/" default (DIAL, #1023). */
};

/** @brief The shared config_reader_t walk over the webtransport-private keys: NAME
 *         "cert" NAME <file>, NAME "key" NAME <file>, NAME "ca" NAME <file>, NAME
 *         "insecure" VALUE <u8>, NAME "path" NAME <resource>; unknown pairs ignored
 *         (forward-compat). Pair-consuming (#927), like every other config parse: a
 *         forward-compat pair whose string value reads `"key"` must not bind the
 *         FOLLOWING child as the private-key path. */
[[nodiscard]] wt_private_cfg_t parse_wt_config(const wire::tlv_t* raw_config) {
    wt_private_cfg_t out;
    const config_reader_t cfg(raw_config);
    if (const auto v = cfg.name("cert")) out.cert = std::string(*v);
    if (const auto v = cfg.name("key")) out.key = std::string(*v);
    if (const auto v = cfg.name("ca")) out.ca = std::string(*v);
    if (const auto v = cfg.flag("insecure")) out.insecure = *v;
    if (const auto v = cfg.name("path")) out.path = std::string(*v);
    return out;
}

}  // namespace

transport_vertex_t::transport_factory_t webtransport_transport_factory(
    mem::mem_backend_t* rx_backend) {
    return [rx_backend](
               const conn_settings_t& s,
               const wire::tlv_t* raw_config) -> graph::result_t<std::unique_ptr<transport_t>> {
        // BOTH roles carry kind-private keys, so the parse precedes the role split:
        // the DIAL branch used to return before parse_wt_config ever ran, which is
        // why no SPEC could reach the dial-side trust knobs at all (#918).
        const wt_private_cfg_t priv = parse_wt_config(raw_config);
        std::unique_ptr<webtransport_transport_t> t;
        if (s.role == conn_role_t::DIAL) {
            // #1039: an `https` request's `:path` is non-empty and, in origin-form,
            // begins with "/" (RFC 9114 §4.3.1 / RFC 9113 §8.3.1), so a value like
            // "tracer" cannot be right. It is refused HERE, with the other
            // creation-time preconditions, because the alternative is a 400 from a
            // conformant server that this side reports as a failed session —
            // indistinguishable from a rejected certificate or an unreachable peer.
            // Empty still means "unset" and normalises to "/" in the constructor;
            // nothing beyond the leading "/" is judged.
            const bool bad_path = !priv.path.empty() && !priv.path.starts_with('/');
            if (s.addr.empty() || s.port == 0 || bad_path)
                return std::unexpected(graph::status_t::TYPE_MISMATCH);
            // Secure by default (#918): absent both keys this verifies the server
            // certificate against the system trust store. `insecure = 1` is the
            // explicit dev opt-out; `ca` names a bundle to verify against instead.
            // The `:path` came from a hard-coded "/" until #1023, so a SPEC could
            // only ever reach a server that serves its session at the root; an
            // absent (or empty) `path` key still normalises to "/" in the ctor.
            t = std::make_unique<webtransport_transport_t>(
                s.addr, s.port, priv.path,
                webtransport_dial_tls_t{.ca_file = priv.ca, .insecure_no_verify = priv.insecure},
                rx_backend, s.max_frame);
            // A refused session is TRANSIENT, not a bad address (#929).
            if (!t->ok()) return std::unexpected(graph::status_t::TRANSPORT_DOWN);
            return t;
        }
        if (s.port == 0 || priv.cert.empty() || priv.key.empty())
            return std::unexpected(graph::status_t::TYPE_MISMATCH);
        t = std::make_unique<webtransport_transport_t>(s.port, priv.cert, priv.key, rx_backend,
                                                       s.max_frame);
        // bind/cred failed — the listener did not come up (#929).
        if (!t->ok()) return std::unexpected(graph::status_t::TRANSPORT_DOWN);
        return t;
    };
}

}  // namespace tr::net
