/**
 * @file
 * @brief `httpd_ws_link_t` — a libtracer WebSocket server `transport_t` backed by
 *        ESP-IDF's native `esp_http_server` WebSocket support.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The embedded-native counterpart to core's raw-socket `transport_ws_server`
 * (core/src/transport_ws.cpp). That portable server opens its OWN
 * ::socket/::listen/::accept, hand-rolls the RFC 6455 handshake + frame codec +
 * fragment reassembly, and runs a dedicated poll thread — ~16 KB of flash and an
 * extra task on a chip that ALREADY links `esp_http_server` (the SPA is served
 * from it on :80). This link instead stands up a SECOND `esp_http_server`
 * instance on the node's WS port and lets the tested platform stack own the
 * listen socket, the handshake, the masking/framing and the recv task — the same
 * "platform link" split as `twai_link_t` is for CAN. The portable
 * `transport_ws_server` stays for the linux virtual board, which has no
 * `esp_http_server` (it uses glibc sockets); the two are picked by which TU the
 * build compiles, never an in-source `#ifdef`.
 *
 * It presents the SAME `transport_t` + `bus_link_t` contract `transport_ws_server`
 * does — one inbound BINARY WebSocket frame is one libtracer TLV; a peer-named
 * server tags each frame with the sending peer's `<ip>:<port>` so a directed FWD
 * reply reaches only the tab that asked (ADR-0044); `send()` broadcasts. So it
 * drops into the node's construction site (provide_link + a `:children[]` SPEC)
 * behind the request-plane admission gate with no wiring change.
 *
 * Threading (the review-critical part):
 *   - RX runs on the `esp_http_server` task: the WS URI handler is invoked once at
 *     the opening handshake (HTTP GET) and again for each subsequent data frame.
 *     A data frame is read with httpd_ws_recv_frame() and delivered to the graph
 *     SYNCHRONOUSLY on that task — the router services the request (decode /
 *     resolve / reply) in-call, exactly as the raw server delivered on its recv
 *     thread. The httpd task stack is sized for that in-call servicing (the batch
 *     apply overflows the 4 KB httpd default — see kHttpdTaskStack).
 *   - TX marshals every outbound frame onto the httpd task via httpd_queue_work()
 *     -> httpd_ws_send_frame_async() (the documented async-send pattern). All
 *     socket writes therefore happen on the one httpd task, so there is NO
 *     cross-thread write to a socket the task may be closing, and no write
 *     interleave — the payload is heap-copied into the work item and freed after
 *     the send. send() may be called from any task (subscription pushes on the
 *     io/event threads, a reply on the httpd task itself); all funnel through the
 *     same queue.
 *
 * NO fixed-size static buffers: peer slots and the per-frame payload copies are
 * heap; the slot vector is grown on demand and RECYCLED in place (never shrunk),
 * so the endpoint `peer_link` hands out stays pointer-valid for the link's life.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "esp_http_server.h"
#include "libtracer/transport.hpp"

namespace tr::net {

/**
 * @brief A WebSocket (RFC 6455) server `transport_t` on `esp_http_server` — accepts
 *        many inbound peers and exposes them through the @ref bus_link_t facet.
 *
 * Public surface mirrors `transport_ws_server` (the node's introspection —
 * enumerate_peers / local_port / ok — and its S7 census depend on it), so a chip
 * node substitutes this type at its construction site with no other change. Span
 * delivery (not ropes): each frame is delivered borrowed and the router services
 * it in-call, so nothing outlives the callback (@ref delivers_ropes is false).
 */
class httpd_ws_link_t : public transport_t, public bus_link_t {
   public:
    /**
     * @brief Start an `esp_http_server` instance on @p bind_port with a WebSocket
     *        URI handler at "/"; confirm with @ref ok.
     *
     * @param bind_port  TCP port to serve the graph WS on (the node's WS port).
     * @param max_peers  Concurrent-peer admission cap; 0 = unbounded. Beyond it a
     *                   new handshake is refused (the handler fails, httpd closes
     *                   the socket) — a clean refusal, mirroring transport_ws_server.
     * @param peer_named Expose the @ref bus_link_t facet: each inbound peer gets its
     *                   own `<ip>:<port>` return-route identity (the browser-tabs
     *                   deployment). Off keeps point-to-point hop naming (send()
     *                   fans out; inbound arrives as the registered child NAME).
     */
    explicit httpd_ws_link_t(std::uint16_t bind_port, std::size_t max_peers = 0,
                             bool peer_named = false);

    /** @brief Stop the httpd instance and release all peer slots. */
    ~httpd_ws_link_t() override;

    httpd_ws_link_t(const httpd_ws_link_t&) = delete;
    httpd_ws_link_t& operator=(const httpd_ws_link_t&) = delete;

    /** @brief Broadcast @p frame as one BINARY WebSocket message to every open peer. */
    void send(std::span<const std::byte> frame) override;

    /** @brief Span delivery: the router services each inbound frame in-call, so no
     *         frame outlives its callback (one override covers both bases). */
    [[nodiscard]] bool delivers_ropes() const override { return false; }

    /** @brief The @ref bus_link_t facet when constructed `peer_named`, else nullptr. */
    [[nodiscard]] bus_link_t* bus() override { return peer_named_ ? this : nullptr; }

    /** @brief Visit the currently-open peers' names, `<ip>:<port>`. */
    void enumerate_peers(const peer_visitor_t& visit) const override;

    /** @brief Resolve an open peer's name to its directed sending endpoint (owned by
     *         the peer's slot, pointer-valid for this link's lifetime). */
    [[nodiscard]] transport_t* peer_link(std::string_view peer) override;

    /** @brief True if the httpd instance started and the WS handler registered. */
    [[nodiscard]] bool ok() const noexcept { return handle_ != nullptr; }

    /** @brief The bound WS port (the value passed to the ctor). */
    [[nodiscard]] std::uint16_t local_port() const noexcept { return port_; }

   private:
    struct session_t;  // one peer slot's connection state (defined in the .cpp)

    /**
     * @brief The directed per-peer sending endpoint @ref peer_link hands out:
     *        `send()` writes a BINARY frame to that peer's socket only (via the
     *        owning link's httpd send queue). No-op once the peer has departed.
     */
    class peer_endpoint_t final : public transport_t {
       public:
        void send(std::span<const std::byte> frame) override;

       private:
        friend class httpd_ws_link_t;
        httpd_ws_link_t* owner_ = nullptr;
        session_t* slot_ = nullptr;
    };

    // --- httpd trampolines (static; recover `this` from req->user_ctx / work arg) ---
    static esp_err_t ws_handler(httpd_req_t* req);  // the WS URI handler (handshake + frames)
    static void on_session_closed(void* slot_ctx);  // free_ctx_fn: a peer departed
    static void tx_work(void* work_arg);            // httpd_queue_work fn: one queued send

    // --- instance handlers (run on the httpd task) ---
    esp_err_t on_handshake(httpd_req_t* req);   // admit or refuse a new peer
    esp_err_t on_data_frame(httpd_req_t* req);  // recv one WS frame, (reassemble,) deliver
    void reclaim_slot(session_t* slot);
    void deliver(std::string_view peer, std::span<const std::byte> frame);
    void queue_send(int fd, std::span<const std::byte> frame);  // heap-copy + httpd_queue_work

    httpd_handle_t handle_ = nullptr;  // nullptr => the instance never started
    std::uint16_t port_;
    std::size_t max_peers_;
    bool peer_named_;
    /** @brief Guards the slot vector and each slot's name/fd/open — the cross-thread
     *         reads (enumerate_peers / peer_link / a send's fd snapshot) against the
     *         httpd task's accept/close. The reassembly buffer is httpd-task-only. */
    mutable std::mutex peers_m_;
    std::vector<std::unique_ptr<session_t>> slots_;  // grown on demand; recycled in place
};

}  // namespace tr::net
