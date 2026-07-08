/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_ws server (#54) — the connection layer on top of the RFC 6455
 * PROTOCOL layer (ws.hpp). A board accepts ONE inbound WebSocket connection
 * (the headline browser↔board link): bind+listen on a TCP port, accept, run the
 * opening handshake (parse the HTTP Upgrade request, reply 101 Switching
 * Protocols with ws::accept_key), then a recv loop that ws::decode_frame()s the
 * TCP byte stream into complete frames. Each BINARY message is one libtracer
 * frame (one TLV) handed to the registered receiver; PING is answered with PONG,
 * CLOSE tears the connection down. send(frame) ws::encode_frame(BINARY, …)s and
 * writes the whole frame to the client.
 *
 * Both roles live here: transport_ws_server (accept an inbound peer) and
 * transport_ws_client (dial out to a ws:// peer — device-to-device / NAT egress),
 * the latter sending MASKED client frames per RFC 6455 §5.1. POSIX sockets;
 * mirrors transport_udp's lifecycle (a recv thread polled for a clean shutdown).
 * The framing itself is never reimplemented here — it all goes through tr::net::ws.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "libtracer/posix_endpoint.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

/**
 * @brief A WebSocket (RFC 6455) server transport_t — accepts one inbound peer.
 *
 * Binds and listens on a TCP port (localhost is fine for tests), accepts a
 * single client, performs the opening handshake, then runs a receive loop that
 * delivers each inbound BINARY message's unmasked payload to the receiver. The
 * dial-out counterpart is transport_ws_client below.
 */
class transport_ws_server : public transport_t, private stream_endpoint_t {
   public:
    /**
     * @brief Bind+listen on @p bind_port (0 = ephemeral; see local_port()).
     *
     * Spawns the accept/recv thread immediately. Use ok() to confirm the listen
     * socket bound. The bound port is observable via local_port().
     *
     * @param bind_port TCP port to listen on (host byte order; 0 → ephemeral).
     */
    explicit transport_ws_server(std::uint16_t bind_port);

    /** @brief Stop the recv thread and close all sockets. */
    ~transport_ws_server() override;

    transport_ws_server(const transport_ws_server&) = delete;
    transport_ws_server& operator=(const transport_ws_server&) = delete;

    /**
     * @brief Send @p frame as one server→client BINARY WebSocket message.
     *
     * Encodes via ws::encode_frame(BINARY, frame) (FIN=1, unmasked) and writes
     * the whole frame to the connected client. No-op until a client is
     * connected. Thread-safe (the socket write is guarded).
     *
     * @param frame A complete TLV's bytes.
     */
    void send(std::span<const std::byte> frame) override;

    /** @brief True — WS reassembles fragmented messages into ropes (ADR-0053 §5):
     *         each message crosses the seam as a `rope_t`, one owning link per WS
     *         fragment (a single link for an unfragmented message), chained by
     *         reassembly, never memcpy'd flat. */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief True if the listen socket is bound and listening. */
    [[nodiscard]] bool ok() const noexcept { return listen_fd_ >= 0; }

    /** @brief The actual bound TCP port (resolves an ephemeral 0 request). */
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }

   private:
    void run();              // accept + recv thread
    bool handshake(int fd);  // read Upgrade, reply 101
    void serve(int fd);      // frame recv loop

    int listen_fd_ = -1;
    std::uint16_t bound_port_ = 0;
    // The connected client's fd + write mutex (and their teardown discipline)
    // live in stream_endpoint_t (conn_fd_ / write_m_).
};

/**
 * @brief A WebSocket (RFC 6455) client transport_t — dials out to one peer.
 *
 * The mirror of transport_ws_server: a board that DIALS OUT to a ws:// peer
 * (device-to-device, or egress through a NAT). The constructor TCP-connects to
 * @p host:@p port, runs the opening handshake from the client side (sends an
 * HTTP GET Upgrade with a fresh Sec-WebSocket-Key, then verifies the 101
 * response's Sec-WebSocket-Accept against ws::accept_key), and on success spawns
 * a receive loop. Per RFC 6455 §5.1 every client→server frame is MASKED
 * (ws::encode_client_frame); inbound server frames are unmasked and decode the
 * same way the server's do. ok() confirms the handshake completed.
 */
class transport_ws_client : public transport_t, private stream_endpoint_t {
   public:
    /**
     * @brief Connect to @p host:@p port and run the client opening handshake.
     *
     * TCP-connects, sends the HTTP Upgrade request, and verifies the server's
     * 101 Sec-WebSocket-Accept. On success the receive loop thread is spawned;
     * confirm with ok(). On any failure the connection is closed and ok() is
     * false.
     *
     * @param host Dotted-quad IPv4 address of the peer (e.g. "127.0.0.1").
     * @param port TCP port of the peer (host byte order).
     */
    transport_ws_client(const std::string& host, std::uint16_t port);

    /** @brief Stop the recv thread and close the socket. */
    ~transport_ws_client() override;

    transport_ws_client(const transport_ws_client&) = delete;
    transport_ws_client& operator=(const transport_ws_client&) = delete;

    /**
     * @brief Send @p frame as one client→server MASKED BINARY WebSocket message.
     *
     * Encodes via ws::encode_client_frame(BINARY, frame, key) (FIN=1, MASK=1,
     * fresh per-frame key) and writes the whole frame to the peer. No-op once the
     * connection has been torn down. Thread-safe (the socket write is guarded).
     *
     * @param frame A complete TLV's bytes.
     */
    void send(std::span<const std::byte> frame) override;

    /** @brief True — WS reassembles fragmented messages into ropes (ADR-0053 §5):
     *         each message crosses the seam as a `rope_t`, one owning link per WS
     *         fragment (a single link for an unfragmented message), chained by
     *         reassembly, never memcpy'd flat. */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief True if the connection handshake succeeded and the link is up. */
    [[nodiscard]] bool ok() const noexcept { return connected_; }

   private:
    bool handshake(int fd, const std::string& host, std::uint16_t port);  // GET Upgrade, verify 101
    void serve(int fd);                                                   // frame recv loop
    std::uint32_t next_mask_key();  // per-frame masking key (varied, not crypto)

    // conn_fd_ + write_m_ (and their teardown discipline) live in stream_endpoint_t.
    std::atomic<std::uint64_t> mask_state_{0};
    bool connected_ = false;
};

}  // namespace tr::net
