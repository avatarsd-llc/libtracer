/**
 * @file
 * @brief `esp_ws_client_link_t` — a libtracer WebSocket *client* `transport_t`
 *        backed by ESP-IDF's abstracted `esp_transport_ws` (over `tcp_transport`).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The embedded-native counterpart to core's portable `transport_ws_client`
 * (core/src/transport_ws.cpp). That portable client opens its OWN ::socket, hand-
 * rolls the RFC 6455 opening handshake (`Sec-WebSocket-Key`/`-Accept`) and the
 * frame codec/masking over lwIP BSD sockets — which does not reliably complete on
 * ESP32 silicon (an lwIP/handshake interaction observed dropping board-to-board
 * dials). This link instead rides `esp_transport_ws`: ESP-IDF's tested transport
 * stack owns the socket, the client handshake (`esp_transport_connect` performs
 * the full GET-upgrade + 101 validation for the configured `ws_path`), the
 * masking/framing, and PING/PONG/CLOSE control-frame handling. It is the client
 * mirror of @ref httpd_ws_link_t (the esp_http_server-backed *server* link) — the
 * same "platform link picked by which TU compiles, never an in-source #ifdef"
 * split as `twai_link_t` is for CAN. The portable `transport_ws_client` stays for
 * the linux virtual board (glibc sockets); the two are selected by the build, and
 * a chip node that binds this link leaves the portable one unreferenced for
 * `--gc-sections` to drop.
 *
 * It presents the same `transport_t` contract as `transport_ws_client`: one
 * inbound BINARY WebSocket message is one libtracer TLV, delivered borrowed
 * in-call to the router; `send()` emits one masked BINARY frame per libtracer
 * frame. Point-to-point (one dialed peer), so it is NOT a bus link
 * (`bus() == nullptr`) and delivers borrowed spans (`delivers_ropes() == false`).
 * It drops into the node's construction site behind the request-plane admission
 * gate with the same `provide_link("ws-client", name, link)` + DIAL `:children[]`
 * SPEC wiring `httpd_ws_link_t` uses for the listener.
 *
 * Threading (the review-critical part):
 *   - A single recv thread (spawned in the ctor) owns the read side: it dials
 *     (with reconnect/backoff), then loops `esp_transport_poll_read` +
 *     `esp_transport_read` — reading each frame's payload DIRECTLY into a reusable
 *     buffer (server→client frames are unmasked per RFC 6455 §5.1, so the read is
 *     a zero-copy fill) — and delivers it borrowed to the router SYNCHRONOUSLY on
 *     that thread, exactly as the raw client delivered on its recv thread. Control
 *     frames are handled inside `esp_transport_ws` (`propagate_control_frames =
 *     false`), so the loop only sees data frames.
 *   - `send()` may be called from ANY task (a subscription push on an io/timer
 *     thread, a reply on the recv thread itself). `esp_transport_ws`'s handle is
 *     NOT thread-safe, so all handle access is serialized by one small mutex held
 *     ONLY for the read/write syscall (the poll runs outside it). The read after a
 *     positive poll is prompt, so the mutex is never held blocking. The reads run
 *     otherwise lock-free on the one recv thread; the graph/router lock-stripes
 *     below are unchanged — this link adds no lock beyond that syscall serializer.
 *   - `send()` COPIES the frame into a reusable tx scratch before writing, because
 *     `esp_transport_write` masks IN-PLACE (RFC 6455 client rule) and a delivered
 *     frame may be shared with the concurrent server link — so the caller's (const,
 *     possibly shared) bytes must not be transiently mutated. RX is zero-copy; the
 *     one TX copy is the irreducible cost of client masking on shared bytes.
 *
 * NO per-frame heap: the rx/tx buffers are allocated ONCE at construction (bounded,
 * tunable); steady-state send/recv touch neither the global heap nor a per-frame
 * allocation. One recv thread per dialed peer (each blocks on its own connection),
 * mirroring `transport_ws_client`.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "esp_transport.h"
#include "libtracer/transport.hpp"

namespace tr::net {

/**
 * @brief A WebSocket (RFC 6455) *client* `transport_t` on ESP-IDF `esp_transport_ws`
 *        — dials one peer and exposes it through the point-to-point `transport_t` seam.
 *
 * Public surface mirrors `transport_ws_client`: a chip node substitutes this type at
 * its dial construction site with no other change. Span delivery (not ropes): each
 * frame is delivered borrowed and the router services it in-call, so nothing outlives
 * the callback (@ref delivers_ropes is false); point-to-point, so @ref bus is nullptr.
 */
class esp_ws_client_link_t : public transport_t {
   public:
    /**
     * @brief Dial `ws://<host>:<port><ws_path>` and start the recv thread; the ctor
     *        does NOT block on the connection (it dials on the recv thread, with
     *        reconnect), so a slow/unreachable peer never stalls the caller — confirm
     *        readiness with @ref ok.
     *
     * @param host     The peer's IPv4 dotted-quad or hostname (the graph plane's WS host).
     * @param port     The peer's TCP port (its :80 esp_http_server, for the /ws mount).
     * @param ws_path  The WS URI to request in the handshake (default "/ws", matching
     *                 the httpd_ws_link_t server mount).
     * @param rx_bytes Reusable receive-buffer size (one inbound message must fit); our
     *                 control TLVs are small, so the default is modest.
     * @param tx_bytes Reusable send-scratch size (one outbound frame must fit).
     * @param recv_stack Recv-thread stack (0 = the pthread default, which the node sizes
     *                   for in-call delivery through the graph's on_write seam).
     */
    explicit esp_ws_client_link_t(std::string host, std::uint16_t port, std::string ws_path = "/ws",
                                  std::size_t rx_bytes = 2048, std::size_t tx_bytes = 2048,
                                  std::size_t recv_stack = 0);

    /** @brief Stop the recv thread, then close + destroy the esp_transport handles. */
    ~esp_ws_client_link_t() override;

    esp_ws_client_link_t(const esp_ws_client_link_t&) = delete;
    esp_ws_client_link_t& operator=(const esp_ws_client_link_t&) = delete;

    /** @brief Send @p frame as one masked BINARY WebSocket message to the peer. Drops
     *         (best-effort, like the UDP path) when not currently connected. */
    void send(std::span<const std::byte> frame) override;

    /** @brief Span delivery: the router services each inbound frame in-call, so no
     *         frame outlives its callback. */
    [[nodiscard]] bool delivers_ropes() const override { return false; }

    /** @brief Point-to-point link — no bus/peer-enumeration facet. */
    [[nodiscard]] bus_link_t* bus() override { return nullptr; }

    /** @brief True once the opening handshake has completed (and while connected). */
    [[nodiscard]] bool ok() const noexcept { return connected_.load(std::memory_order_acquire); }

    /**
     * @brief Set (or clear, with an empty string) extra HTTP header lines appended to the
     *        opening-handshake request. Each line must be `Name: value\r\n`-terminated
     *        (`esp_transport_ws` emits them verbatim). Applied on the NEXT dial.
     *
     * The client counterpart to @ref httpd_ws_link_t::set_admission_cb: a board-to-board
     * dial carries no browser session cookie, so a token header here is how a dialing node
     * authenticates itself to a peer whose graph WS gates admission. NOT synchronized — set
     * it once at wiring time, before the link first dials; the recv thread reads it when it
     * (re)builds the transport.
     */
    void set_handshake_headers(std::string headers) { handshake_headers_ = std::move(headers); }

   private:
    /** @brief The recv thread body: (re)connect, then poll+read+deliver until stopped. */
    void recv_loop();
    /** @brief One dial attempt: (lazily) build the tcp+ws transports, connect, mark up.
     *         @return true on a completed handshake. */
    bool connect_once();
    /** @brief Close the connection and mark down; the next recv_loop turn re-dials. */
    void drop();

    const std::string host_;
    const std::uint16_t port_;
    const std::string ws_path_;
    /** @brief Extra CRLF-terminated handshake header lines, empty = none; read on the recv
     *         thread when it (re)dials — see @ref set_handshake_headers. */
    std::string handshake_headers_;

    esp_transport_handle_t tcp_ = nullptr;  // parent TCP transport (owned)
    esp_transport_handle_t ws_ = nullptr;   // WS transport over tcp_ (owned)

    std::vector<std::byte> rx_buf_;  // reusable RX fill (zero-copy read target)
    std::vector<std::byte> tx_buf_;  // reusable TX scratch (masked in-place under write_m_)

    std::mutex write_m_;  // serializes all esp_transport handle access (syscall-brief)
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::thread recv_thread_;
};

}  // namespace tr::net
