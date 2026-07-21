/**
 * @file
 * @brief quic_transport_t (ADR-0043 Phase A) — msquic behind the transport_t
 *        seam.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One of the two translation units of the SEPARATE libtracer_quic module — the
 * only code (with transport_webtransport.cpp) that sees msquic; a host that
 * doesn't talk QUIC never compiles it (the core library carries no reference).
 *
 * Everything msquic-MECHANICAL — the handle lifecycle and ownership
 * discipline, the teardown ordering, the TSan annotations, the one-copy TX
 * contract, the RX length-prefix reassembly, the dial rendezvous, and the
 * one-peer listener-replacement skeleton — lives in the shared
 * msquic_endpoint_t base (src/msquic_endpoint.hpp; see its header comment for
 * the full contracts). This transport keeps ONLY its variance points: ONE
 * bidirectional frame stream per connection (ADR-0043 Phase A), adopted
 * directly on PEER_STREAM_STARTED.
 */

#include "libtracer/transport_quic.hpp"

#include <msquic.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/config_reader.hpp"
#include "libtracer/frame.hpp"
#include "msquic_endpoint.hpp"

namespace tr::net {

namespace {

/** @brief The ALPN every libtracer QUIC endpoint negotiates (frame stream,
 *         Phase A). */
const QUIC_BUFFER kAlpn{sizeof("libtracer") - 1,
                        reinterpret_cast<uint8_t*>(const_cast<char*>("libtracer"))};

}  // namespace

/**
 * @brief The pimpl: the msquic-mechanical base plus this transport's variance
 *        points (single-stream adoption).
 */
struct quic_transport_t::impl_t : msquic_endpoint_t {
    /** @brief Teardown-first destructor (the msquic_endpoint_t contract: the
     *         harvest virtual must still dispatch to this class). */
    ~impl_t() { teardown(); }

    /** @brief The msquic stream callback for the ONE frame stream: RX chunks
     *         into the reassembler, SEND_COMPLETE frees the send buffer,
     *         peer-send shutdown flips the link flag. */
    static QUIC_STATUS QUIC_API stream_cb(HQUIC /*stream*/, void* ctx, QUIC_STREAM_EVENT* ev) {
        auto* self = static_cast<impl_t*>(ctx);
        const tsan_cb_guard_t guard(self);  // msquic serializes callbacks (see the base header)
        switch (ev->Type) {
            case QUIC_STREAM_EVENT_RECEIVE:
                for (std::uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
                    const QUIC_BUFFER& b = ev->RECEIVE.Buffers[i];
                    if (!self->on_rx_chunk(b.Buffer, b.Length)) break;  // shut down
                }
                return QUIC_STATUS_SUCCESS;  // every byte consumed
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                complete_send(ev->SEND_COMPLETE.ClientContext);
                return QUIC_STATUS_SUCCESS;
            case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
                self->up.store(false, std::memory_order_relaxed);
                return QUIC_STATUS_SUCCESS;
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    /** @brief The dialing peer opened the frame stream — adopt it (one
     *         bidirectional stream per connection, ADR-0043 Phase A; the
     *         PeerBidiStreamCount=1 setting caps the peer to exactly one). */
    QUIC_STATUS on_peer_stream_started(HQUIC /*c*/, QUIC_CONNECTION_EVENT* ev) override {
        if ((ev->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) != 0)
            return QUIC_STATUS_NOT_SUPPORTED;  // frame stream is bidirectional
        api->SetCallbackHandler(ev->PEER_STREAM_STARTED.Stream,
                                reinterpret_cast<void*>(&impl_t::stream_cb), this);
        const std::lock_guard lock(conn_m);
        frame_stream = ev->PEER_STREAM_STARTED.Stream;
        return QUIC_STATUS_SUCCESS;
    }

    /** @brief One-peer replacement harvest: detach and close the departed
     *         peer's stream + connection (refuse while the peer is up). */
    bool replace_peer() override {
        HQUIC old_stream = nullptr;
        HQUIC old_conn = nullptr;
        {
            const std::lock_guard lock(conn_m);
            if (conn != nullptr && up.load(std::memory_order_relaxed)) return false;
            old_stream = std::exchange(frame_stream, nullptr);
            old_conn = std::exchange(conn, nullptr);
        }
        // Closing blocks until the old handles' callbacks drain — after this,
        // nothing touches the RX state (the base resets it for the new peer).
        if (old_stream != nullptr) api->StreamClose(old_stream);
        if (old_conn != nullptr) api->ConnectionClose(old_conn);
        return true;
    }

    /** @brief Teardown harvest: detach the frame stream + connection under
     *         conn_m, abort+close the stream, hand the connection back. */
    HQUIC harvest_and_close_streams() override {
        HQUIC stream = nullptr;
        HQUIC c = nullptr;
        {
            const std::lock_guard lock(conn_m);
            stream = std::exchange(frame_stream, nullptr);
            c = std::exchange(conn, nullptr);
        }
        if (stream != nullptr) {
            api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            api->StreamClose(stream);
        }
        return c;
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
    // Departure seam (RFC-0009 §D extended to peer departure): wire the base's
    // connection-down / one-peer replacement harvest to this transport_t's flat
    // link-down notifier. quic is point-to-point — one peer at a time — so a
    // departure IS the whole link down (notify_down, like tcp / ws-client; never
    // notify_peer_down, which is the multi-peer bus facet's).
    i.link_down_ctx = this;
    i.link_down_fn = [](void* ctx) { static_cast<quic_transport_t*>(ctx)->notify_down(); };

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 0;  // no idle teardown; link liveness is #66 lifecycle
    settings.IsSet.IdleTimeoutMs = TRUE;

    // The tcp_transport_t dial shape (base): block until the handshake
    // resolves so ok() is meaningful at construction.
    if (!i.dial("libtracer", kAlpn, settings, tls.ca_file, tls.insecure_no_verify, peer_host,
                peer_port))
        return;

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
        i.frame_stream = stream;
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
    // Departure seam (RFC-0009 §D extended to peer departure): wire the base's
    // connection-down / one-peer replacement harvest to this transport_t's flat
    // link-down notifier. quic is point-to-point — one peer at a time — so a
    // departure IS the whole link down (notify_down, like tcp / ws-client; never
    // notify_peer_down, which is the multi-peer bus facet's).
    i.link_down_ctx = this;
    i.link_down_fn = [](void* ctx) { static_cast<quic_transport_t*>(ctx)->notify_down(); };

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 0;  // no idle teardown; link liveness is #66 lifecycle
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = 1;  // exactly the ONE frame stream (ADR-0043 Phase A)
    settings.IsSet.PeerBidiStreamCount = TRUE;

    // Listener bring-up (base) — bad cert/key paths fail in there; the peer
    // opens the frame stream.
    (void)i.listen_start("libtracer", kAlpn, settings, cert_file, key_file, bind_port);
}

quic_transport_t::~quic_transport_t() = default;  // ~impl_t runs the base teardown()

void quic_transport_t::send(std::span<const std::byte> frame) { impl_->send_frame(frame); }

void quic_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    impl_->send_frame(iov);
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

/**
 * @brief The quic kind's PRIVATE config keys, parsed module-side from the raw
 *        SPEC config SETTINGS TLV (ADR-0043 §5 leanness: the shared
 *        conn_settings_t carries only the universal keys, so cert/key never
 *        touch it).
 */
struct quic_private_cfg_t {
    std::string cert; /**< @brief PEM server-certificate path (LISTEN). */
    std::string key;  /**< @brief PEM private-key path matching cert (LISTEN). */
};

/** @brief The shared config_reader_t walk over the quic-private keys: NAME
 *         "cert" NAME <path>, NAME "key" NAME <path>; unknown pairs ignored
 *         (forward-compat). */
[[nodiscard]] quic_private_cfg_t parse_quic_config(const wire::tlv_t* raw_config) {
    quic_private_cfg_t out;
    const config_reader_t cfg(raw_config);
    if (const auto v = cfg.name("cert")) out.cert = std::string(*v);
    if (const auto v = cfg.name("key")) out.key = std::string(*v);
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
