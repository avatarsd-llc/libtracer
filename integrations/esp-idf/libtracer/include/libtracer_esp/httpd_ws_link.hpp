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
 * Since #947 that split is EXCLUSIVE, not a coexistence: on a chip target
 * `core/src/transport_ws.cpp` is not compiled at all, so this link (with
 * @ref esp_ws_client_link_t) is the whole ESP-IDF WebSocket plane. ESP-IDF
 * WebSocket must never use POSIX sockets — the portable server's scatter-gather
 * egress asks `sendmsg` for `MSG_NOSIGNAL`, which `lwip_sendmsg` rejects with
 * `EOPNOTSUPP`, so on lwIP it silently discards every data frame while its
 * handshake and PING/PONG still answer (#948). An application that wants THIS
 * server therefore needs `CONFIG_HTTPD_WS_SUPPORT=y`: there is no portable
 * fallback behind it.
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
 *     same queue. TX failure is never silent, and each kind is aimed at what it is
 *     actually evidence about (#835): a failed SEND strikes its DESTINATION — the
 *     peer that did not drain — and kMaxConsecutiveTxDrops of them in a row, with
 *     no success between, closes that session; a failed ENQUEUE is evidence about
 *     the shared control queue and nobody's peer, so it is a dropped frame on a
 *     link-level counter (@ref enqueue_drops) and strikes no session at all.
 *   - Every upgraded socket gets a SHORT, derived SO_SNDTIMEO of its own (@ref
 *     send_timeout_ms) plus a send override that rejects a SHORT write. Without
 *     the bound one full-window peer parks the httpd task — the task that owns
 *     accept/recv for every other socket — in one send for the server's whole
 *     send_wait_timeout, and under fan-out those stalls serialize until the task
 *     watchdog fires (#835). REST sockets are untouched: the server's own
 *     send_wait_timeout still governs HTTP responses.
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
 *   - FAN-OUT: a broadcast (`send()` — the path a subscription push takes) snapshots
 *     its destinations into a FIXED on-stack chunk and resumes the scan for the next
 *     one, so the peer set is walked with no container of its own. Until #961 that
 *     snapshot was a `std::vector`, whose THROWING allocator put an abort ahead of
 *     every nothrow fallback on this exact path.
 * Peer slots remain heap, grown on demand and RECYCLED in place (never shrunk), so
 * the endpoint `peer_link` hands out stays pointer-valid for the link's life. Their
 * allocation is per SESSION (a new peer past the high-water mark), never per frame.
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
     * @param send_timeout_ms Per-socket send bound for UPGRADED sockets, milliseconds;
     *                   0 (the default) derives it — see @ref send_timeout_ms. Pass a
     *                   value only on a host whose watchdog regime differs from the
     *                   derivation's inputs; it is clamped to the server's own
     *                   `send_wait_timeout`.
     */
    explicit httpd_ws_link_t(std::uint16_t bind_port, std::size_t max_peers = 0,
                             bool peer_named = false, std::uint32_t send_timeout_ms = 0);

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
     * @param send_timeout_ms Per-socket send bound for UPGRADED sockets, milliseconds;
     *                   0 (the default) derives it — see @ref send_timeout_ms.
     */
    httpd_ws_link_t(httpd_handle_t external, const char* uri, std::size_t max_peers = 0,
                    bool peer_named = false, std::uint32_t send_timeout_ms = 0);

    /**
     * @brief Stop the owned httpd instance (or unregister the adopted WS URI) and release
     *        all peer slots.
     *
     * Adopted mode does much more, because the server outlives this link and keeps a
     * pointer to the WebSocket route inside every session it upgraded. In order: the
     * handler gate is shut and the in-flight handler frame joined (@ref close_gate), so
     * nothing can dispatch into the link again; the URI is unregistered so no further
     * session can latch it; every session's close callback is retired (@ref
     * detach_sessions); the in-flight TX slots are drained. It blocks for all of that —
     * the drains are bounded and leak rather than free on expiry, so a wedged server task
     * costs memory, never a use-after-free; the handler join is unbounded because there
     * is no safe way to free the link out from under a handler that is reading it.
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

    /**
     * @brief The per-socket send bound applied to every UPGRADED socket, milliseconds.
     *
     * DERIVED, never configured: the task-watchdog period divided by the peer cap. Both
     * factors are facts already in hand — the watchdog period is the system's own
     * definition of "too long to starve a task" (it is the tripwire #835 observed
     * firing), and the peer cap is the serialization multiplier — so one full fan-out
     * round, every peer stalled, still fits inside one watchdog window. Clamped to the
     * server's `send_wait_timeout`, which remains what REST sockets use.
     */
    [[nodiscard]] std::uint32_t send_timeout_ms() const noexcept { return send_timeout_ms_; }

    /**
     * @brief Frames dropped because the shared control queue refused them (or the work
     *        item could not be allocated), for this link's life.
     *
     * A LINK-level count on purpose. A refused enqueue says the httpd control queue is
     * saturated — which under #835's failure shape is caused by whichever peer is
     * stalling the task, not by the peer whose frame is being enqueued at that instant.
     * Charging it to that peer closed HEALTHY sessions while the culprit never accrued a
     * strike; the per-session streak now counts failed SENDS, which do name their peer.
     */
    [[nodiscard]] std::uint32_t enqueue_drops() const noexcept {
        return enqueue_drops_.load(std::memory_order_relaxed);
    }

    /**
     * @brief TX work slots reclaimed because the control socket silently binned their
     *        work item, for this link's life (#944).
     *
     * The failure @ref enqueue_drops does NOT cover, and could not: a refused enqueue is
     * observable and already recycles its slot, whereas `httpd_queue_work` on the default
     * path is a bare `sendto` to a loopback UDP socket, so an enqueue past the receiver's
     * mbox is dropped by lwIP while the call still reports ESP_OK. That work item never
     * runs and used to pin its slot for the rest of the boot — four of them killed the
     * pool, after which every frame took two global-heap allocations. Non-zero here means
     * the control queue is being overrun; the link recovers, but the node is losing
     * frames somewhere.
     */
    [[nodiscard]] std::uint32_t tx_strands() const noexcept {
        return tx_strands_.load(std::memory_order_relaxed);
    }

    /** @brief TX work slots claimed RIGHT NOW (filling, queued, or sending) — the pool
     *         occupancy a strand used to raise permanently. */
    [[nodiscard]] std::size_t tx_slots_busy() const noexcept;

    /** @brief Total TX work slots in the per-link pool; a send past it heap-falls-back. */
    [[nodiscard]] static std::size_t tx_slot_capacity() noexcept;

    /**
     * @brief Admission predicate: given the opening-handshake request, return true to
     *        admit the peer or false to refuse it — a clean refusal that closes the
     *        socket, exactly as the @ref max_peers cap does. @p ctx is the opaque
     *        pointer registered alongside it in @ref set_admission_cb.
     */
    using admission_fn_t = bool (*)(void* ctx, httpd_req_t* req);

    /**
     * @brief Install (or clear, with `nullptr`) an admission predicate consulted at the
     *        top of every opening handshake, BEFORE the peer-cap check and any slot
     *        allocation. Unset (the default) admits every peer — the historical behavior.
     *
     * The seam a host uses to authenticate the graph WS the same way it gates the rest of
     * its HTTP surface: inspect the handshake request's headers (a session cookie, a
     * shared token) and refuse an unauthenticated peer before it can read or write a
     * single vertex. NOT synchronized — set it once at wiring time, before the link
     * serves; the hook is read on the httpd task with no lock.
     */
    void set_admission_cb(admission_fn_t fn, void* ctx) noexcept {
        admission_fn_ = fn;
        admission_ctx_ = ctx;
    }

   private:
    struct gate_t;         // the handler-admission gate + teardown barrier (in the .cpp)
    struct session_t;      // one peer slot's connection state (defined in the .cpp)
    struct session_ref_t;  // a session identity that survives fd reuse (defined in the .cpp)
    struct tx_work_t;      // one queued outbound frame (defined in the .cpp)
    struct tx_slot_t;      // one pre-allocated TX work slot (defined in the .cpp)
    struct detach_req_t;   // the teardown session-detach work item (defined in the .cpp)

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
    // drops the frame on OOM (never aborts). The destination is a SESSION, never a bare
    // fd — see @ref session_ref_t.
    void queue_send(const session_ref_t& to, std::span<const std::span<const std::byte>> iov);
    void queue_send(const session_ref_t& to,
                    std::span<const std::byte> frame);  // one-span sugar over the gather

    /** @brief Allocate the once-per-link RX scratch + TX slot pool (nothrow; on failure
     *         the link still works — every frame takes the per-frame heap fallback). */
    void alloc_buffers();
    /** @brief Claim a free TX work slot lock-free (a CAS scan); nullptr when the pool
     *         is exhausted or absent — the caller falls back to a heap work item. Sweeps
     *         once (@ref sweep_tx_slots) before giving up. */
    [[nodiscard]] tx_slot_t* claim_tx_slot();
    /**
     * @brief Reclaim every slot whose work item was silently binned by the control socket
     *        (#944). Called ONLY from @ref claim_tx_slot's exhausted path — a strand costs
     *        nothing until the pool runs out, so this needs no timer and no steady-state work.
     */
    void sweep_tx_slots();
    /** @brief Return a drained/failed work item: recycle its pool slot (dropping any
     *         overflow heap payload) or delete the heap-fallback shell. */
    static void release_tx_work(tx_work_t* work);

    /**
     * @brief Per-session SEND accounting (takes @ref peers_m_ — callers must not hold
     *        it): a send that completed resets the session's consecutive-failure
     *        counter; one that failed bumps it and, once kMaxConsecutiveTxDrops is
     *        reached, triggers the session's close so the peer reconnects instead of
     *        missing frames silently.
     *
     * Fed from @ref tx_work only — the one result that names a peer. It is the peer's
     * OWN socket that did not accept the bytes within its bound, so the destination is
     * the culprit by construction (#835). Keyed by @ref session_ref_t and not by fd: a
     * result carried back for a session that has since departed must strike NOBODY, and
     * charging it to whoever inherited the descriptor condemns a stranger (#954).
     */
    void note_tx_result(const session_ref_t& to, bool sent, std::size_t bytes);

    /**
     * @brief Resolve @p to to the socket it may be sent on RIGHT NOW, or refuse it
     *        (takes @ref peers_m_ — callers must not hold it).
     *
     * The one question every producer and every queued send asks before spending anything
     * on a socket, and the checkpoint the fd-reuse hazard is closed at for everything that
     * happens AFTER a reference is minted (#954). It answers three at once, all of which
     * must hold: the reference still names the session it was minted for (@ref
     * session_ref_t::gen), that session is still open, and the link has not condemned it — the
     * link's OWN verdict, readable the instant it was reached, rather than the server's, which only
     * becomes true once a queued close it may never run has run.
     *
     * It cannot answer for the window BEFORE the mint: a caller that resolved a peer's
     * endpoint and is preempted before sending mints from the slot's generation as it is
     * THEN, so this check passes for whoever holds the slot at that moment. See
     * @ref session_ref_t and #1013.
     *
     * @retval -1  Do not send. The session departed, a DIFFERENT session now holds the
     *             slot (and possibly the same descriptor), or this one is condemned.
     */
    [[nodiscard]] int live_fd(const session_ref_t& to) const;

    /**
     * @brief Force @p fd's session closed without asking the control queue for anything
     *        (takes nothing; must run on the httpd task).
     *
     * `httpd_sess_trigger_close` is `httpd_queue_work(httpd_sess_close, …)` — the same
     * loopback control socket, drained by the same single task that is serialized behind
     * this fd's queued sends, and on the default non-blocking path an enqueue past that
     * socket's mbox is dropped inside lwIP while still reporting success. So a close
     * requested through it can be delayed by the backlog it exists to clear, or lost with
     * no error at all. `shutdown` is not a request of the server: it takes effect
     * immediately, makes every later write on the socket fail at once, and raises the
     * readable-at-EOF event that gets the session reaped through httpd's own select arm.
     * It never frees the descriptor, so httpd keeps sole ownership of the fd's lifetime.
     */
    void condemn(int fd);

    /**
     * @brief Count a frame the shared control queue would not take (takes nothing).
     *
     * The demoted half of the old accounting: a refused enqueue is charged to the link,
     * never to a session — see @ref enqueue_drops.
     */
    void note_enqueue_drop(int fd, std::size_t bytes);

    /**
     * @brief Apply this link's whole per-socket policy to a freshly-upgraded socket.
     *
     * All of it on the peer's own fd, at admission and nowhere else: `SO_SNDTIMEO` of
     * @ref send_timeout_ms, `TCP_NODELAY`, `SO_KEEPALIVE` with the idle/interval/count
     * tunables that make a peer vanishing without a FIN detectable at all (#957 — with
     * `lru_purge_enable = false` there is otherwise no timer on an inbound session, so
     * such a peer holds its slot and one unit of `max_peers` forever), and a send
     * override so a SHORT write is seen. Nothing touches the server's configuration, so
     * REST sockets on the same instance keep their long bound and the owner's keepalive
     * setting — and, by the same token, this link does not depend on an adopted server
     * having enabled keepalive.
     */
    void bound_socket(int fd) const;

    /**
     * @brief The per-session send override (`httpd_send_func_t`): the default write,
     *        plus the check `esp_http_server` does not do.
     *
     * IDF treats any non-negative return from the send function as a delivered frame,
     * but lwIP returns the PARTIAL count when a bounded write expires mid-buffer. Half a
     * WebSocket frame on the wire destroys the framing for everything after it, so this
     * turns a short write into an error AND closes the session at once — the one case
     * where "drop the frame, keep the socket" is unsound, and one a short bound makes
     * more likely rather than less.
     */
    static int send_guarded(httpd_handle_t handle, int fd, const char* buf, std::size_t len,
                            int flags);

    /**
     * @brief Handle a detected short write on @p slot's socket: log it and close that
     *        session immediately, bypassing the streak (takes @ref peers_m_).
     *
     * Takes the SLOT, not the fd. @ref send_guarded is inside the write when it calls
     * this, on the httpd task, so the server's own session table is authoritative about
     * who owns that descriptor at that instant and hands the slot over directly — no
     * generation check is needed here, and no fd-keyed rescan either (#954).
     */
    void note_send_desync(session_t* slot, std::size_t written, std::size_t len);

    /**
     * @brief Allocate the handler-admission gate and point it at this link; false when
     *        the allocation failed, in which case NO handler is registered (ok() stays
     *        false) — the gate is what makes a registered handler safe to dispatch.
     */
    [[nodiscard]] bool open_gate();

    /**
     * @brief Teardown step ZERO: stop every dispatch into this link, and join the one
     *        that is already inside it.
     *
     * The barrier the URI unregister is not. `esp_http_server` copies the WebSocket
     * route (`handler` + `user_ctx`) into each session as it answers the handshake and
     * only clears it when that session is deleted, so unregistering the URI stops new
     * handshakes and nothing else: already-upgraded peers keep dispatching frames into
     * the handler with the registered `user_ctx`. Registering the GATE as that
     * `user_ctx` is what makes the pointer survivable — after this call the gate holds
     * no link, so a later dispatch is refused (httpd closes that socket) and a later
     * `free_ctx` is inert. The wait for an in-flight handler frame is deliberately
     * unbounded: unlike the two drains there is no leak-instead-of-free fallback for
     * the link itself. A destructor running ON the server's task IS that frame and
     * skips the wait — the #814 case, and equally the proof that no other frame can be
     * in flight, since `esp_http_server` dispatches from one task.
     */
    void close_gate();

    /**
     * @brief Adopted-mode teardown step 1: retire the session contexts the adopted
     *        server would otherwise run a `free_ctx` on.
     *
     * Every admitted session carries `httpd_sess_set_ctx(handle, fd, slot,
     * on_session_closed)`, so an external server that outlives us would run that
     * `free_ctx` into freed memory on the peer's next disconnect (or at its own
     * `httpd_stop`). This clears each session's ctx AND free_ctx *from the server's own
     * task* — the only context in which the session table may be touched — via
     * @ref detach_work, then closes the sessions so their latched WS route goes with
     * them. Returns only once that work has run; if it cannot run (the destructor IS
     * the server task) it runs inline, and if it never runs within the teardown bound
     * the sessions are neutralised instead (@ref abandon_sessions). The one session it
     * can never detach is the request an in-flight handler is servicing — for that fd
     * `httpd_sess_set_ctx` edits the request, not the socket table, and the callback
     * runs after the destructor has returned — so that slot is neutralised too
     * (@ref abandon_session). Owning mode needs none of this: `httpd_stop` closes every
     * session synchronously, before any member dies.
     */
    void detach_sessions();

    /**
     * @brief Adopted-mode teardown fallback: neutralise, never free, EVERY session slot.
     *
     * Reached only when @ref detach_sessions could not get its work onto the server
     * task (queue refused, allocation refused, or the bound expired with the task
     * wedged), i.e. exactly when the server may still hold slots as session contexts
     * AND a detach item may still drain later. Every slot is @ref neutralise d and then
     * LEAKED — including already-closed ones, because the detach item identifies our
     * sessions by comparing ctx POINTERS and a freed shell's address could be reissued
     * to something else. The late callback therefore lands on valid, inert memory
     * instead of a freed slot. Bounded (one shell per slot ever opened), teardown-only,
     * and loudly logged: the #815 precedent that a drain expiry must leak rather than
     * free under a live callback.
     */
    void abandon_sessions();

    /** @brief Neutralise and leak the single slot bound to @p fd — the in-flight
     *         request's session, which no detach can reach (see @ref detach_sessions). */
    void abandon_session(int fd);

    /** @brief Take a slot out of service without freeing it: clear its fd/open and cut
     *         the endpoint facade loose, so a callback or a directed send that outlives
     *         the link lands on valid, inert memory. Caller holds @ref peers_m_. */
    static void neutralise(session_t* slot);

    httpd_handle_t handle_ = nullptr;  // nullptr => the instance never started
    std::uint16_t port_;
    std::size_t max_peers_;
    /** @brief Opening-handshake admission predicate + its opaque ctx; null admits every
     *         peer (the default). Read on the httpd task — see @ref set_admission_cb. */
    admission_fn_t admission_fn_ = nullptr;
    void* admission_ctx_ = nullptr;
    /** @brief The per-socket send bound applied at admission — see @ref send_timeout_ms. */
    std::uint32_t send_timeout_ms_ = 0;
    /** @brief Frames the control queue refused, for this link's life — see @ref
     *         enqueue_drops. Relaxed: a diagnostic count, ordered against nothing. */
    std::atomic<std::uint32_t> enqueue_drops_{0};
    /** @brief TX slots reclaimed from a silently-binned work item — see @ref tx_strands.
     *         Relaxed for the same reason: the slot's own state word carries the ordering. */
    std::atomic<std::uint32_t> tx_strands_{0};
    bool peer_named_;
    bool owns_httpd_ = true;  // false when adopting an external server (dtor must not httpd_stop)
    /** @brief Set at destructor entry: refuses new TX slot claims so the pool drain
     *         converges. The RX side needs no such flag — @ref close_gate stops
     *         dispatch outright, and stopping it any earlier would only lose frames
     *         the link is still able to serve. */
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
    /**
     * @brief The handler-admission gate registered as the URI's `user_ctx` — every
     *        dispatch resolves this link through it (see @ref close_gate).
     *
     * A raw pointer, and deliberately so: in adopted mode it must OUTLIVE this link,
     * because the adopted server keeps routing to it until each latched session is
     * deleted and no API of ours can force that. The destructor frees it only when it
     * can prove nothing still holds it (owning mode, after `httpd_stop`).
     */
    gate_t* gate_ = nullptr;
};

}  // namespace tr::net
