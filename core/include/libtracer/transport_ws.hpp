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
 * This is the SERVER half only — a WebSocket CLIENT (a board dialing out) is a
 * separate later increment. POSIX sockets; mirrors transport_udp's lifecycle (a
 * recv thread polled for a clean shutdown). The framing itself is never
 * reimplemented here — it all goes through tr::net::ws.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "libtracer/transport.hpp"

namespace tr::net {

/**
 * @brief A WebSocket (RFC 6455) server transport_t — accepts one inbound peer.
 *
 * Binds and listens on a TCP port (localhost is fine for tests), accepts a
 * single client, performs the opening handshake, then runs a receive loop that
 * delivers each inbound BINARY message's unmasked payload to the receiver. Only
 * the server role lives here; the client role is a later increment.
 */
class transport_ws_server : public transport_t {
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

    /**
     * @brief Register the sink for inbound frames (one per BINARY message).
     *
     * @param receiver Callback invoked on the recv thread with unmasked payloads.
     */
    void set_receiver(receiver_t receiver) override;

    /** @brief True if the listen socket is bound and listening. */
    [[nodiscard]] bool ok() const noexcept { return listen_fd_ >= 0; }

    /** @brief The actual bound TCP port (resolves an ephemeral 0 request). */
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }

   private:
    void run();              // accept + recv thread
    bool handshake(int fd);  // read Upgrade, reply 101
    void serve(int fd);      // frame recv loop
    void write_all(int fd, std::span<const std::byte> bytes);

    int listen_fd_ = -1;
    std::uint16_t bound_port_ = 0;

    receiver_t receiver_;             // guarded by m_
    std::mutex m_;                    // guards receiver_
    std::mutex write_m_;              // serializes writes to client_fd_
    std::atomic<int> client_fd_{-1};  // the connected client (-1 = none)
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace tr::net
