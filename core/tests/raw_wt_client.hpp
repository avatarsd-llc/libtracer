/**
 * @file
 * @brief The raw msquic HTTP/3 client shared by the WebTransport harnesses, plus the
 *        byte builders its vectors are written in (#1182).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Extracted from `webtransport_test.cpp`, behaviour unchanged — the one addition is
 * `open_stream`'s `immediate` flag, which no existing caller passes.
 *
 * It lives in a header because a SECOND consumer needs the very same peer:
 * `wt_peer_driver.cpp` runs this client as a separate PROCESS, which is the only way
 * `tr::detail::probe_fail_hook` — a process-wide seam — can be armed against the server's
 * msquic workers without also refusing the peer's own opens (#1182). Writing a second client
 * instead would mean the gated peer and the peer every other vector uses could drift apart,
 * which is exactly the divergence the classifier vectors exist to catch.
 */
#ifndef LIBTRACER_TESTS_RAW_WT_CLIENT_HPP
#define LIBTRACER_TESTS_RAW_WT_CLIENT_HPP

#include <msquic.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../src/wt_h3.hpp"
#include "libtracer/byteorder.hpp"
#include "test_support.hpp"

/** @brief The raw WebTransport peer: byte builders plus the msquic H3 client itself. */
namespace tr::testing::wt {

using namespace std::chrono_literals;

/** @brief A reserved (GREASE) H3 frame type — RFC 9114 §7.2.8's `0x1f * N + 0x21`. */
constexpr std::uint64_t grease_type(std::uint64_t n) { return 0x1f * n + 0x21; }

/** @brief The WebTransport bidirectional-stream preamble: 0x41 ++ the session id
 *         (draft-ietf-webtrans-http3 §4.2 — that id IS the CONNECT stream's id). */
inline std::vector<std::uint8_t> wt_preamble(std::uint64_t session_id) {
    std::vector<std::uint8_t> p;
    tr::net::wt_h3::append_varint(p, tr::net::wt_h3::kFrameWtStream);
    tr::net::wt_h3::append_varint(p, session_id);
    return p;
}

/** @brief One length-prefixed record on the frame channel: u32-LE len ++ payload. */
inline std::vector<std::uint8_t> record(std::span<const std::uint8_t> payload) {
    std::vector<std::byte> prefix;
    tr::detail::append_le(prefix, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::uint8_t> out;
    for (const std::byte b : prefix) out.push_back(static_cast<std::uint8_t>(b));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/** @brief A payload of @p len bytes, the `test_frame` pattern as raw octets. */
inline std::vector<std::uint8_t> payload(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> p(len);
    for (std::size_t i = 0; i < len; ++i) p[i] = static_cast<std::uint8_t>(seed + i);
    return p;
}

/** @brief The extended CONNECT request frame (HEADERS) a browser-shaped client sends.
 *  @param path The `:path` requested. Defaults to the root; a LONG one is what lets a
 *              vector aim at the server's `:path` copy, whose size the PEER chooses
 *              (#934). */
inline std::vector<std::uint8_t> connect_frame(std::string_view authority,
                                               std::string_view path = "/") {
    std::vector<std::uint8_t> out;
    tr::net::wt_h3::append_h3_frame(out, tr::net::wt_h3::kFrameHeaders,
                                    tr::net::wt_h3::encode_connect_field_section(authority, path));
    return out;
}

/** @brief One complete GREASE frame: reserved type @p n with a @p body-byte payload. */
inline std::vector<std::uint8_t> grease_frame(std::uint64_t n, std::size_t body) {
    const std::vector<std::uint8_t> b(body, 0x5A);
    std::vector<std::uint8_t> out;
    tr::net::wt_h3::append_h3_frame(out, grease_type(n), b);
    return out;
}

/**
 * @brief A raw msquic HTTP/3 client — the test's hand on the server's stream
 *        classifier (the `raw_quic_client_t` role, one layer up).
 *
 * The library's own DIAL mode is well-behaved by construction: it opens exactly
 * ONE frame stream, only after the 200, always naming the CONNECT stream's id,
 * and it never emits an unknown H3 frame type. Every #919 and #920 vector is
 * precisely a peer that does one of those things differently, so this client
 * drives QUIC + the H3 face itself and then writes arbitrary bytes on arbitrary
 * streams. Handles are closed only by the destructor.
 */
struct raw_wt_client_t {
    /** @brief One client-opened stream and what the server did with it. */
    struct wt_stream_t {
        HQUIC h = nullptr;                /**< @brief The msquic handle. */
        std::uint64_t id = 0;             /**< @brief Its QUIC stream id (the session id). */
        std::atomic<bool> aborted{false}; /**< @brief Refused (RESET / STOP_SENDING). */
        std::atomic<std::size_t> rx{0};   /**< @brief Bytes the server wrote back. */
    };

    /** @brief One in-flight StreamSend and the bytes msquic borrows for it. */
    struct send_buf_t {
        QUIC_BUFFER buf{};               /**< @brief The descriptor handed to msquic. */
        std::vector<std::uint8_t> bytes; /**< @brief The borrowed payload. */
    };

    const QUIC_API_TABLE* api = nullptr; /**< @brief The msquic API table. */
    HQUIC reg = nullptr;                 /**< @brief Registration handle. */
    HQUIC cfg = nullptr;                 /**< @brief Configuration handle (ALPN h3). */
    HQUIC conn = nullptr;                /**< @brief The connection handle. */
    bool ok = false;                     /**< @brief QUIC up and the H3 face sent. */

    std::mutex m;                  /**< @brief Guards the rendezvous + the handle lists. */
    std::condition_variable cv;    /**< @brief The handshake rendezvous. */
    bool handshake_done = false;   /**< @brief Handshake stage resolved. */
    bool handshake_ok = false;     /**< @brief Handshake stage outcome. */
    std::atomic<bool> shut{false}; /**< @brief The server tore the CONNECTION down. */
    std::vector<std::unique_ptr<wt_stream_t>> streams; /**< @brief Our streams. */
    std::vector<HQUIC> peer_streams; /**< @brief The server's H3 streams (accepted, drained). */

    /** @brief The server's own control/QPACK streams: accepted so msquic has a
     *         handler, contents irrelevant to every vector here. */
    static QUIC_STATUS QUIC_API peer_stream_cb(HQUIC, void*, QUIC_STREAM_EVENT*) {
        return QUIC_STATUS_SUCCESS;
    }

    /** @brief Connection events: the handshake rendezvous, peer-stream adoption,
     *         and the connection-down flag every "was it the STREAM or the
     *         CONNECTION?" assertion keys off. */
    static QUIC_STATUS QUIC_API conn_cb(HQUIC, void* ctx, QUIC_CONNECTION_EVENT* ev) {
        auto* self = static_cast<raw_wt_client_t*>(ctx);
        const tsan_cb_guard_t guard(self);
        switch (ev->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED: {
                const std::lock_guard lock(self->m);
                self->handshake_done = self->handshake_ok = true;
                self->cv.notify_all();
                return QUIC_STATUS_SUCCESS;
            }
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
                self->api->SetCallbackHandler(ev->PEER_STREAM_STARTED.Stream,
                                              reinterpret_cast<void*>(&peer_stream_cb), nullptr);
                const std::lock_guard lock(self->m);
                self->peer_streams.push_back(ev->PEER_STREAM_STARTED.Stream);
                return QUIC_STATUS_SUCCESS;
            }
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
                self->shut.store(true, std::memory_order_relaxed);
                const std::lock_guard lock(self->m);
                self->handshake_done = true;
                self->cv.notify_all();
                return QUIC_STATUS_SUCCESS;
            }
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    /** @brief Stream events: byte counting, send-buffer release, and the two
     *         refusal signals a `StreamShutdown(ABORT)` produces on this side. */
    static QUIC_STATUS QUIC_API stream_cb(HQUIC, void* ctx, QUIC_STREAM_EVENT* ev) {
        auto* s = static_cast<wt_stream_t*>(ctx);
        const tsan_cb_guard_t guard(s);
        switch (ev->Type) {
            case QUIC_STREAM_EVENT_RECEIVE:
                for (std::uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i)
                    s->rx.fetch_add(ev->RECEIVE.Buffers[i].Length, std::memory_order_relaxed);
                return QUIC_STATUS_SUCCESS;
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                tsan_acquire(ev->SEND_COMPLETE.ClientContext);  // pairs with write()'s release
                delete static_cast<send_buf_t*>(ev->SEND_COMPLETE.ClientContext);
                return QUIC_STATUS_SUCCESS;
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
                s->aborted.store(true, std::memory_order_relaxed);
                return QUIC_STATUS_SUCCESS;
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    explicit raw_wt_client_t(std::uint16_t port) {
        if (QUIC_FAILED(MsQuicOpen2(&api))) {
            api = nullptr;
            return;
        }
        const QUIC_REGISTRATION_CONFIG rc{"wt_test_raw", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        if (QUIC_FAILED(api->RegistrationOpen(&rc, &reg))) return;
        const QUIC_BUFFER alpn{sizeof("h3") - 1,
                               reinterpret_cast<uint8_t*>(const_cast<char*>("h3"))};
        QUIC_SETTINGS settings{};
        settings.IdleTimeoutMs = 0;
        settings.IsSet.IdleTimeoutMs = TRUE;
        // The server opens three unidirectional streams at us (control + the two
        // QPACK streams); leave headroom so none of them stalls.
        settings.PeerUnidiStreamCount = 8;
        settings.IsSet.PeerUnidiStreamCount = TRUE;
        settings.PeerBidiStreamCount = 4;
        settings.IsSet.PeerBidiStreamCount = TRUE;
        if (QUIC_FAILED(
                api->ConfigurationOpen(reg, &alpn, 1, &settings, sizeof(settings), nullptr, &cfg)))
            return;
        QUIC_CREDENTIAL_CONFIG cred{};
        cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
        cred.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
            QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
        if (QUIC_FAILED(api->ConfigurationLoadCredential(cfg, &cred))) return;
        if (QUIC_FAILED(api->ConnectionOpen(reg, &conn_cb, this, &conn))) {
            conn = nullptr;
            return;
        }
        tsan_release(this);  // publish the constructed client to its callbacks
        if (QUIC_FAILED(
                api->ConnectionStart(conn, cfg, QUIC_ADDRESS_FAMILY_UNSPEC, "127.0.0.1", port)))
            return;
        {
            std::unique_lock lock(m);
            cv.wait_for(lock, 5s, [this] { return handshake_done; });
            if (!handshake_ok) return;
        }
        // Our H3 face: the control stream (SETTINGS) + the two QPACK streams,
        // exactly what the library's DIAL side sends.
        wt_stream_t* const ctl = open_stream(true);
        if (ctl->h == nullptr) return;
        write(ctl, tr::net::wt_h3::control_stream_bytes());
        for (const std::uint64_t type :
             {tr::net::wt_h3::kStreamTypeQpackEncoder, tr::net::wt_h3::kStreamTypeQpackDecoder}) {
            wt_stream_t* const q = open_stream(true);
            if (q->h == nullptr) return;
            std::vector<std::uint8_t> b;
            tr::net::wt_h3::append_varint(b, type);
            write(q, b);
        }
        ok = true;
    }

    ~raw_wt_client_t() {
        if (api == nullptr) return;
        // Harvest the server-opened handles UNDER the lock and close them
        // outside it: conn_cb pushes them from an msquic worker, and StreamClose
        // blocks until that stream's callbacks drain — closing while holding the
        // lock a callback also takes would deadlock. (`streams` needs no lock:
        // only this thread ever touches it.)
        std::vector<HQUIC> peers;
        {
            const std::lock_guard lock(m);
            peers.swap(peer_streams);
        }
        for (const auto& s : streams)
            if (s->h != nullptr) api->StreamClose(s->h);
        for (HQUIC h : peers) api->StreamClose(h);
        if (conn != nullptr) {
            api->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api->ConnectionClose(conn);
        }
        if (cfg != nullptr) api->ConfigurationClose(cfg);
        if (reg != nullptr) api->RegistrationClose(reg);
        MsQuicClose(api);
        tsan_acquire(this);  // all callbacks drained — take their published writes
    }

    /**
     * @brief Open one stream and read back its QUIC stream id.
     *
     * Always returns a live record, `h == nullptr` when the open failed — a
     * connection the server has already torn down refuses StreamOpen, and every
     * vector here deliberately provokes teardowns. Returning null instead would
     * turn a REDDENING assertion into a segfault in the harness, which is
     * exactly the failure mode a non-vacuity revert must not hit.
     */
    wt_stream_t* open_stream(bool uni, bool immediate = false) {
        auto s = std::make_unique<wt_stream_t>();
        wt_stream_t* const raw = s.get();
        streams.push_back(std::move(s));
        const QUIC_STREAM_OPEN_FLAGS flags =
            uni ? QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL : QUIC_STREAM_OPEN_FLAG_NONE;
        HQUIC h = nullptr;
        if (QUIC_FAILED(api->StreamOpen(conn, flags, &stream_cb, raw, &h))) return raw;
        raw->h = h;
        tsan_release(raw);  // publish the ctx to its callbacks
        // IMMEDIATE announces the stream to the peer WITHOUT any payload (#1182): it is what
        // lets a vector provoke the server's stream-adoption path alone, with no RECEIVE
        // behind it to also exercise the handshake accumulator.
        const QUIC_STREAM_START_FLAGS start =
            immediate ? QUIC_STREAM_START_FLAG_IMMEDIATE : QUIC_STREAM_START_FLAG_NONE;
        if (QUIC_FAILED(api->StreamStart(h, start))) {
            api->StreamClose(h);
            raw->h = nullptr;
            return raw;
        }
        std::uint64_t id = 0;
        std::uint32_t sz = sizeof(id);
        if (QUIC_SUCCEEDED(api->GetParam(h, QUIC_PARAM_STREAM_ID, &sz, &id))) raw->id = id;
        return raw;
    }

    /** @brief Open a bidirectional stream (the CONNECT / frame-channel shape). */
    wt_stream_t* open_bidi() { return open_stream(false); }

    /** @brief Write raw bytes on @p s in ONE StreamSend — no framing added. */
    void write(wt_stream_t* s, std::span<const std::uint8_t> bytes) {
        if (s == nullptr || s->h == nullptr) return;  // the connection is already gone
        auto* sb = new send_buf_t{};
        sb->bytes.assign(bytes.begin(), bytes.end());
        sb->buf.Length = static_cast<std::uint32_t>(sb->bytes.size());
        sb->buf.Buffer = sb->bytes.data();
        tsan_release(sb);  // pairs with SEND_COMPLETE's acquire
        if (QUIC_FAILED(api->StreamSend(s->h, &sb->buf, 1, QUIC_SEND_FLAG_NONE, sb))) delete sb;
    }

    /** @brief Block until the server wrote at least @p n bytes back on @p s. */
    [[nodiscard]] static bool wait_rx(wt_stream_t* s, std::size_t n,
                                      std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (s->rx.load(std::memory_order_relaxed) < n) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    /** @brief Block until the server refused @p s (stream-scoped abort). */
    [[nodiscard]] static bool wait_aborted(wt_stream_t* s, std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (!s->aborted.load(std::memory_order_relaxed)) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    /**
     * @brief Abort and close ONE of our streams, so the SERVER sees it finish (#1163).
     *
     * The abort is what makes this a stream event rather than a connection one: the vector
     * needs the server to reach `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` for this stream while
     * the session stays up, which is exactly the state nothing used to reclaim.
     */
    void close_stream(wt_stream_t* s) {
        if (s == nullptr || s->h == nullptr) return;
        api->StreamShutdown(s->h, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        api->StreamClose(std::exchange(s->h, nullptr));
    }

    /** @brief Block until the server tore the whole CONNECTION down. */
    [[nodiscard]] bool wait_shutdown(std::chrono::milliseconds budget) const {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (!shut.load(std::memory_order_relaxed)) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }
};

}  // namespace tr::testing::wt

#endif  // LIBTRACER_TESTS_RAW_WT_CLIENT_HPP
