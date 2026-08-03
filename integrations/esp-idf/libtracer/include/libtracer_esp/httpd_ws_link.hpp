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
 * from it on :80). This link instead rides `esp_http_server`: it EITHER stands up
 * its own instance on the node's WS port (the port-binding ctor) OR adopts the
 * firmware's already-running :80 SPA server and registers its WS URI on it (the
 * external-handle ctor — no second server), letting the tested platform stack own
 * the listen socket, the handshake, the masking/framing and the recv task — the
 * same "platform link" split as `twai_link_t` is for CAN. The portable
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
 *     same queue. TX failure is never silent: repeated enqueue drops (queue full /
 *     work-slot OOM) close the session after kMaxConsecutiveTxDrops in a row, and
 *     a failed oversized async send drops that one frame while keeping the socket
 *     (see tx_work).
 *
 * Steady-state allocation — the RX scratch and the TX work-slot pool are allocated
 * ONCE at construction, so typical graph traffic (control TLVs, value pushes,
 * directed replies) touches the heap in NEITHER direction:
 *   - RX: a frame that fits the once-allocated scratch is read into it and
 *     delivered borrowed — no per-frame allocation. Larger frames (up to the
 *     kMaxFrameBytes abuse cap) fall back to an exact-size nothrow buffer.
 *   - TX: a send claims a pool slot lock-free (CAS) and gathers straight into its
 *     inline payload. A frame past the inline capacity keeps the pooled shell and
 *     takes a nothrow heap payload; a momentarily exhausted pool falls back to a
 *     fully heap work item. Every fallback is `new (std::nothrow)` with
 *     drop-on-OOM backpressure — never an abort.
 * Peer slots remain heap, grown on demand and RECYCLED in place (never shrunk), so
 * the endpoint `peer_link` hands out stays pointer-valid for the link's life.
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

    /**
     * @brief Adopt an already-running `esp_http_server` and register the WebSocket URI
     *        handler on it at @p uri (no second server is started); confirm with @ref ok.
     *
     * The non-owning counterpart to the port-binding ctor: instead of standing up a
     * second `esp_http_server`, this registers the WS handler as one more `httpd_uri_t`
     * on the firmware's existing `:80` SPA server, so a same-origin browser reaches the
     * graph over the one port. The dtor unregisters that URI and leaves the server
     * running — this link never stops a server it did not start.
     *
     * @param external   A started `esp_http_server` handle to host the WS URI on; the
     *                   caller retains ownership and must outlive this link.
     * @param uri        WS URI pattern to register the handler at (e.g. "/ws"). Register
     *                   it BEFORE any wildcard route so registration-order precedence
     *                   routes it to the WS handler; keep it an exact literal.
     * @param max_peers  Concurrent-peer admission cap; 0 = unbounded. Beyond it a
     *                   new handshake is refused (the handler fails, httpd closes
     *                   the socket) — a clean refusal, mirroring transport_ws_server.
     * @param peer_named Expose the @ref bus_link_t facet: each inbound peer gets its
     *                   own `<ip>:<port>` return-route identity (the browser-tabs
     *                   deployment). Off keeps point-to-point hop naming (send()
     *                   fans out; inbound arrives as the registered child NAME).
     */
    httpd_ws_link_t(httpd_handle_t external, const char* uri, std::size_t max_peers = 0,
                    bool peer_named = false);

    /**
     * @brief Stop the owned httpd instance (or unregister the adopted WS URI) and release
     *        all peer slots.
     *
     * Adopted mode does more, because the server outlives this link: the destructor also
     * retires every session's close callback before any member dies (@ref
     * detach_sessions) and drains the in-flight TX slots. It may block for that — bounded,
     * teardown-only — and both expiries leak rather than free, so a wedged server task
     * costs memory, never a use-after-free.
     */
    ~httpd_ws_link_t() override;

    httpd_ws_link_t(const httpd_ws_link_t&) = delete;
    httpd_ws_link_t& operator=(const httpd_ws_link_t&) = delete;

    /** @brief Broadcast @p frame as one BINARY WebSocket message to every open peer. */
    void send(std::span<const std::byte> frame) override;

    /**
     * @brief Broadcast a scattered frame: gather @p iov once, straight into a
     *        pre-allocated tx work slot (nothrow heap fallback), one BINARY message
     *        per open peer.
     *
     * Overrides the base gather-into-a-temporary default: the reply rope's iovec is
     * copied exactly ONCE (into the queued work slot/item), so a large reply is
     * never double-buffered (gather temp + tx copy) on the heap — the transient
     * that exhausted the chip heap under concurrent SPA asset GETs. On allocation
     * failure the frame is dropped (backpressure), never an abort.
     */
    void send(std::span<const std::span<const std::byte>> iov) override;

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

    /** @brief The bound WS port (the value passed to the port-binding ctor; 0 when this link
     *         adopts an external server). */
    [[nodiscard]] std::uint16_t local_port() const noexcept { return port_; }

   private:
    struct session_t;     // one peer slot's connection state (defined in the .cpp)
    struct tx_work_t;     // one queued outbound frame (defined in the .cpp)
    struct tx_slot_t;     // one pre-allocated TX work slot (defined in the .cpp)
    struct detach_req_t;  // the teardown session-detach work item (defined in the .cpp)

    /**
     * @brief The directed per-peer sending endpoint @ref peer_link hands out:
     *        `send()` writes a BINARY frame to that peer's socket only (via the
     *        owning link's httpd send queue). No-op once the peer has departed.
     */
    class peer_endpoint_t final : public transport_t {
       public:
        void send(std::span<const std::byte> frame) override;
        /** @brief Directed scatter-gather send: gathered once into the nothrow tx
         *         work buffer (no intermediate flatten temporary — see the owning
         *         link's iovec @ref httpd_ws_link_t::send). */
        void send(std::span<const std::span<const std::byte>> iov) override;

       private:
        friend class httpd_ws_link_t;
        httpd_ws_link_t* owner_ = nullptr;
        session_t* slot_ = nullptr;
    };

    // --- httpd trampolines (static; recover `this` from req->user_ctx / work arg) ---
    static esp_err_t ws_handler(httpd_req_t* req);  // the WS URI handler (handshake + frames)
    static void on_session_closed(void* slot_ctx);  // free_ctx_fn: a peer departed
    static void tx_work(void* work_arg);            // httpd_queue_work fn: one queued send
    static void detach_work(void* req_arg);         // httpd_queue_work fn: teardown detach

    // --- instance handlers (run on the httpd task) ---
    esp_err_t on_handshake(httpd_req_t* req);   // admit or refuse a new peer
    esp_err_t on_data_frame(httpd_req_t* req);  // recv one WS frame, (reassemble,) deliver
    void reclaim_slot(session_t* slot);
    void deliver(std::string_view peer, std::span<const std::byte> frame);
    // ONE gather-copy (into a pool slot, else a nothrow heap item) + httpd_queue_work;
    // drops the frame on OOM (never aborts)
    void queue_send(int fd, std::span<const std::span<const std::byte>> iov);
    void queue_send(int fd, std::span<const std::byte> frame);  // one-span sugar over the gather

    /** @brief Allocate the once-per-link RX scratch + TX slot pool (nothrow; on failure
     *         the link still works — every frame takes the per-frame heap fallback). */
    void alloc_buffers();
    /** @brief Claim a free TX work slot lock-free (a CAS scan); nullptr when the pool
     *         is exhausted or absent — the caller falls back to a heap work item. */
    [[nodiscard]] tx_slot_t* claim_tx_slot();
    /** @brief Return a drained/failed work item: recycle its pool slot (dropping any
     *         overflow heap payload) or delete the heap-fallback shell. */
    static void release_tx_work(tx_work_t* work);

    /**
     * @brief Per-session TX-enqueue accounting (takes @ref peers_m_ — callers must
     *        not hold it): a successful enqueue resets the session's consecutive-drop
     *        counter; a drop (work-item OOM or a refused httpd_queue_work) bumps it
     *        and, once kMaxConsecutiveTxDrops is reached, triggers the session's
     *        close so the peer reconnects instead of missing frames silently.
     */
    void note_tx_result(int fd, bool queued, std::size_t bytes);

    /**
     * @brief Adopted-mode teardown step 1: make it IMPOSSIBLE for the adopted server
     *        to call back into this link after the destructor returns.
     *
     * Every admitted session carries `httpd_sess_set_ctx(handle, fd, slot,
     * on_session_closed)`, so an external server that outlives us would run that
     * `free_ctx` into freed memory on the peer's next disconnect (or at its own
     * `httpd_stop`). This clears each session's ctx AND free_ctx *from the server's own
     * task* — the only context in which the session table may be touched — via
     * @ref detach_work, then closes the sessions (harmless once detached). Returns only
     * once that work has run; if it cannot run (the dtor IS the server task) it runs
     * inline, and if it never runs within the teardown bound the sessions are
     * neutralised instead (@ref abandon_sessions). Owning mode needs none of this:
     * `httpd_stop` closes every session synchronously, before any member dies.
     */
    void detach_sessions();

    /**
     * @brief Adopted-mode teardown fallback: neutralise, never free, the sessions the
     *        adopted server still holds a `free_ctx` pointer into.
     *
     * Reached only when @ref detach_sessions could not get its work onto the server
     * task (queue refused, allocation refused, or the bound expired with the task
     * wedged). Each still-open slot has its `owner` cleared — which is exactly the
     * condition @ref on_session_closed already tests — and is then LEAKED, so the late
     * callback lands on valid, inert memory instead of a freed slot. Bounded
     * (one shell per live peer), teardown-only, and loudly logged: the #815 precedent
     * that a drain expiry must leak rather than free under a live callback.
     */
    void abandon_sessions();

    httpd_handle_t handle_ = nullptr;  // nullptr => the instance never started
    std::uint16_t port_;
    std::size_t max_peers_;
    bool peer_named_;
    bool owns_httpd_ = true;  // false when adopting an external server (dtor must not httpd_stop)
    /** @brief Set at destructor entry: suppresses the departure notifier for session
     *         closes the teardown itself provokes (see reclaim_slot), refuses new TX
     *         slot claims so the pool drain converges, and refuses to ARM a new session
     *         (a frame racing the URI unregister) so the detach snapshot is complete. */
    std::atomic<bool> stopping_{false};
    /**
     * @brief The `esp_http_server` task, latched the first time this link runs on it.
     *
     * `TaskHandle_t` as an opaque `void*` so the public header names no FreeRTOS type.
     * The teardown detach compares it against the current task: a destructor running ON
     * the adopting server's task can never see queued work drain (the work runs on the
     * task that would be sleeping in the dtor — the #814 deadlock), so it must do the
     * detach inline instead. Latched in the URI handler, which is the only way a session
     * — and therefore anything to detach — can exist at all.
     */
    std::atomic<void*> server_task_{nullptr};
    std::string uri_;  // the WS URI registered (unregistered by the adopting dtor)
    /** @brief Guards the slot vector and each slot's name/fd/open — the cross-thread
     *         reads (enumerate_peers / peer_link / a send's fd snapshot) against the
     *         httpd task's accept/close. The reassembly buffer is httpd-task-only. */
    mutable std::mutex peers_m_;
    std::vector<std::unique_ptr<session_t>> slots_;  // grown on demand; recycled in place
    /** @brief Once-allocated RX scratch (httpd-task-only, so lock-free by construction):
     *         a frame that fits reads here instead of a per-frame allocation. */
    std::unique_ptr<std::byte[]> rx_scratch_;
    /** @brief Once-allocated TX work-slot pool: claimed lock-free by sending tasks,
     *         released by the httpd task as each send drains. */
    std::unique_ptr<tx_slot_t[]> tx_pool_;
};

}  // namespace tr::net
