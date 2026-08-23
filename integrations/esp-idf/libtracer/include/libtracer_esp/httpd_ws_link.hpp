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
 *   - RX runs on the `esp_http_server` task. The server answers the opening
 *     WebSocket GET ITSELF and, having answered it, does not call the URI handler
 *     for that request at all (`httpd_uri.c` returns from the handshake branch
 *     before `uri->handler`), so the handler is invoked ONLY for data frames; the
 *     opening GET is seen through the server's `ws_pre_handshake_cb` instead
 *     (@ref set_admission_cb), which runs before the 101 is written.
 *     A data frame is read with httpd_ws_recv_frame() and delivered to the graph
 *     SYNCHRONOUSLY on that task — the router services the request (decode /
 *     resolve / reply) in-call, exactly as the raw server delivered on its recv
 *     thread. That servicing needs @ref httpd_ws_link_t::kRequiredHttpdStack bytes of
 *     task stack (the batch apply overflows the 4 KB httpd default). The PORT-BINDING
 *     ctor configures that stack itself; the ADOPTING ctor cannot — the task belongs
 *     to the server it adopts — so the embedder must size it, which is why the figure
 *     is a public constant rather than a number in the .cpp.
 *   - TX marshals every outbound frame onto the httpd task via httpd_queue_work()
 *     -> httpd_ws_send_frame_async() (the documented async-send pattern). All
 *     socket writes therefore happen on the one httpd task, so there is NO
 *     cross-thread write to a socket the task may be closing, and no write
 *     interleave — the payload is copied into a pooled work item and the slot is
 *     released after the send. send() may be called from any task (subscription
 *     pushes on the io/event threads, a reply on the httpd task itself); all funnel
 *     through the same queue. Every enqueue failure is OBSERVABLE, which is a
 *     property of the ESP-IDF floor this component requires (>=5.5.5, see
 *     idf_component.yml) and not of this file: below it a full control mbox
 *     discarded the datagram inside lwIP with ESP_OK returned, and the link carried
 *     compensation for that until #949 deleted it with the floor raised. Each
 *     failure kind is aimed at what it is actually evidence about (#835): a failed
 *     SEND strikes its DESTINATION — the peer that did not drain — and
 *     kMaxConsecutiveTxDrops of them in a row, with no success between, closes that
 *     session; a refused ENQUEUE (or an exhausted TX pool) is evidence about the
 *     shared control queue and this link's own in-flight depth, not about any one
 *     peer, so it is a dropped frame on a link-level counter (@ref enqueue_drops)
 *     and strikes no session at all.
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
 *     takes a nothrow heap payload (`new (std::nothrow)`, drop-on-OOM backpressure —
 *     never an abort). An exhausted pool has NO buffer behind it: the pool is this link's
 *     outstanding-send bound, and a send that finds it full is dropped and counted
 *     (@ref enqueue_drops) rather than posted from a heap-allocated work item, which
 *     bounded the in-flight depth by the heap instead of by the queue behind it
 *     (#949). What it does have, since #1187, is a bounded WAIT: a send from any task
 *     other than the httpd task sleeps in short turns until the drain frees a slot,
 *     within one send occupancy, so a fan-out wider than the pool degrades to latency
 *     instead of losing the same publish-order tail every pass. Nothing is copied or
 *     parked to achieve that — the frame waits in the caller's memory, in the caller's
 *     call (ADR-0081 §1) — and an expired wait is the same counted drop it always was.
 *     One further slot, past the pool, is reserved for sends issued ON the httpd task
 *     (@ref tx_reply_reserve): the one claimer that cannot wait for a drain it is itself
 *     supposed to perform, so a delivery burst cannot starve a request's reply. Past the
 *     pool, and not out of it — carved out, it narrowed every producer's fan-out by a
 *     destination and cost a sweep that had always fit its tail (#1218).
 *   - FAN-OUT: a broadcast (`send()` — the path a subscription push takes) snapshots
 *     its destinations into a FIXED on-stack chunk and resumes the scan for the next
 *     one, so the peer set is walked with no container of its own. Until #961 that
 *     snapshot was a `std::vector`, whose THROWING allocator put an abort ahead of
 *     every nothrow fallback on this exact path.
 * Peer slots remain heap, grown on demand and RECYCLED in place (never shrunk), so
 * the handle `peer_link` hands out stays pointer-valid for the link's life. Their
 * allocation is per SESSION (a new peer past the high-water mark), never per frame — and
 * so is the resolution handle's (#1013): the pool grows to the peer population once and
 * is recycled from there, so a forward hop's resolve costs no allocation at all.
 *
 * WHAT IS CONFIGURABLE, and what is not (#1160). The three numbers that ARE this link's
 * per-link RAM — the RX scratch, the TX pool depth and a slot's inline capacity, together
 * ~8.4 KiB at their defaults — are constructor arguments (@ref kDefaultRxScratchBytes,
 * @ref kDefaultTxPoolSlots, @ref kDefaultTxInlineBytes), matching how
 * `esp_ws_client_link_t` has always taken `rx_bytes`/`tx_bytes`. Per-LINK and not Kconfig
 * because a Kconfig value is per-image and two links in one image legitimately serve
 * different peers. Their effective values are readable — @ref rx_scratch_bytes,
 * @ref tx_slot_capacity, @ref tx_inline_bytes, @ref buffer_bytes — so a node can REPORT
 * the ceiling that produced a drop instead of quoting a constant out of these sources.
 *
 * Everything else stays fixed ON PURPOSE, and each one says why where it is defined in
 * `httpd_ws_link.cpp`. In short: the send bound and the TX wait bound are DERIVED (from
 * `CONFIG_ESP_TASK_WDT_TIMEOUT_S`, the peer cap and the strike cap) and a knob over a
 * derivation is a way to configure a node into a watchdog panic; the consecutive-TX-drop
 * strike cap is a brokenness DETECTOR, not a tunable — it is what keeps one silently
 * stalled peer from parking the httpd task, and it is a divisor of the send bound; the
 * frame ceiling bounds an ABUSE case and costs no RAM; the socket slack and the assumed
 * peer cap are already reachable through `max_peers`; and the teardown drain is a
 * teardown-only bound whose both outcomes are safe and logged.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "esp_http_server.h"
#include "libtracer/transport.hpp"
#include "libtracer_esp/link_stats.hpp"

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
     * @brief Task stack the `esp_http_server` task must have for this link's in-call
     *        servicing, bytes — what the port-binding ctor configures, and what an
     *        ADOPTED server's owner has to configure itself.
     *
     * The inbound graph request is serviced IN-CALL on the httpd task — decode, resolve,
     * reply, and (the deep path) the whole /unit batch-apply transaction. The device node
     * measured that transaction overflowing an 8 KB stack and needing ~12 KB on the raw ws
     * recv thread (F2b, 2026-07-09); the 4 KB `esp_http_server` default is far too small.
     *
     * PUBLIC because the adopting ctor cannot apply it: it takes an already-started
     * server, so there is no `httpd_config_t` left to write, and `esp_http_server` exposes
     * no reader for a running server's config either — the link can neither set the stack
     * nor check it. An embedder writes `config.stack_size =
     * httpd_ws_link_t::kRequiredHttpdStack;` before `httpd_start` instead of guessing; a
     * downstream integration guessing 8192 is what put this figure here.
     *
     * The number is the MEASUREMENT, not a ceiling: httpd's own per-request framing adds a
     * little on top, so HIL should confirm the task's high-water mark under a batch apply
     * and bump this if a stack-protection reboot appears. The link samples that high-water
     * mark itself at each session claim and names the cause once if it is thin (see the
     * adopting ctor's preconditions) — a diagnostic, never a guarantee.
     */
    static constexpr std::size_t kRequiredHttpdStack = 12288;

    /**
     * @brief Default reusable RX scratch capacity, bytes — the ctor's `rx_scratch_bytes`
     *        when left 0.
     *
     * The httpd task is the only RX thread and delivery is synchronous (borrowed, serviced
     * in-call), so ONE scratch per link suffices and needs no lock. Graph control TLVs —
     * writes, subscribes, value pushes — sit well under this; a larger frame (up to the
     * abuse cap) falls back to the exact-size nothrow heap path, trading one allocation
     * for not pinning 32 KB of RAM permanently.
     *
     * It is a DEFAULT and not a constant since #1160: this and the two below are the
     * link's whole per-link static cost (`rx_scratch_bytes + (tx_pool_slots + 1) *
     * tx_inline_bytes`, ~8.4 KiB at these figures), and a node that knows its own frame
     * sizes must be able to trade them without editing this component. Per-LINK and not
     * Kconfig for the reason `esp_ws_client_link_t` already takes `rx_bytes`/`tx_bytes`
     * that way: two links in one image can serve different peers.
     */
    static constexpr std::size_t kDefaultRxScratchBytes = 2048;

    /**
     * @brief Default TX work slots any sender may claim — the ctor's `tx_pool_slots` when
     *        left 0, and this link's OUTSTANDING-SEND bound.
     *
     * Deliberately NOT a fan-out width (#1187): it is how many frames may be in flight
     * toward the httpd task at one instant, sized past the steady-state in-flight depth (a
     * reply plus a couple of subscription pushes). A sweep wider than this waits for the
     * drain rather than losing its tail, so raising it buys in-flight depth — and RAM at
     * @ref kDefaultTxInlineBytes a slot — never fan-out reach.
     *
     * @see tx_slot_capacity for the effective per-link value, and @ref tx_reply_reserve
     *      for the in-call slot that sits on TOP of it.
     */
    static constexpr std::size_t kDefaultTxPoolSlots = 4;

    /**
     * @brief Default inline payload capacity of one TX work slot, bytes — the ctor's
     *        `tx_inline_bytes` when left 0.
     *
     * Sized past the common outbound frames (value pushes, directed replies, census) so a
     * steady-state send gathers straight into the slot with NO allocation; a larger frame
     * (e.g. a composed-root snapshot reply) keeps the pooled shell but takes a nothrow heap
     * payload. Roughly one Ethernet MTU. It multiplies: the pool allocates
     * `tx_pool_slots + tx_reply_reserve()` of these, so it is the dominant half of the
     * link's per-link RAM.
     */
    static constexpr std::size_t kDefaultTxInlineBytes = 1600;

    /**
     * @brief Start an `esp_http_server` instance on @p bind_port with a WebSocket
     *        URI handler at "/"; confirm with @ref ok.
     *
     * @param bind_port  TCP port to serve the graph WS on (the node's WS port).
     * @param max_peers  Concurrent-peer admission cap; 0 = unbounded. Beyond it the
     *                   peer is refused at the edge that claims its slot — a clean refusal
     *                   either way, mirroring transport_ws_server. WHICH edge depends on
     *                   the admission verdict (#1334): a session the three-valued predicate
     *                   answered @ref admission_verdict_t::ADMIT_AUTHENTICATED for is charged
     *                   at the HANDSHAKE, where the upgrade is abandoned outright; every
     *                   other session is charged on its FIRST frame (the handler fails,
     *                   httpd closes the socket), one step later than the handshake because
     *                   that is the first moment such a peer is known to this link at all.
     * @param peer_named Expose the @ref bus_link_t facet: each inbound peer gets its
     *                   own `<ip>:<port>` return-route identity (the browser-tabs
     *                   deployment). Off keeps point-to-point hop naming (send()
     *                   fans out; inbound arrives as the registered child NAME).
     * @param send_timeout_ms Per-socket send bound for UPGRADED sockets, milliseconds;
     *                   0 (the default) derives it — see @ref send_timeout_ms. Pass a
     *                   value only on a host whose watchdog regime differs from the
     *                   derivation's inputs; it is clamped to the server's own
     *                   `send_wait_timeout`.
     * @param auth_deadline_ms How long an admitted session may stay UNAUTHENTICATED before
     *                   this link closes it, milliseconds; 0 (the default) uses
     *                   @ref kDefaultAuthDeadlineMs. Inert unless @ref set_auth_cb installed
     *                   a hook — with no hook every session is served immediately and there
     *                   is no unauthenticated state to bound. Inert also for a session the
     *                   admission predicate answered @ref
     *                   admission_verdict_t::ADMIT_AUTHENTICATED for (#1334): no deadline is
     *                   armed for it at all, because it has already answered the question the
     *                   deadline exists to time out, and a native dialer entitled to stay
     *                   silent has no way to answer it a second time in-band.
     * @param rx_scratch_bytes Reusable RX scratch capacity, bytes; 0 (the default) uses
     *                   @ref kDefaultRxScratchBytes. A frame past it still arrives — it
     *                   takes a per-frame nothrow heap buffer instead of the scratch.
     * @param tx_pool_slots TX work slots any sender may claim; 0 (the default) uses
     *                   @ref kDefaultTxPoolSlots. The in-call reserve
     *                   (@ref tx_reply_reserve) is allocated ON TOP of it.
     * @param tx_inline_bytes Inline payload capacity of one TX slot, bytes; 0 (the
     *                   default) uses @ref kDefaultTxInlineBytes. A frame past it keeps
     *                   its pooled shell and takes a nothrow heap payload.
     */
    explicit httpd_ws_link_t(std::uint16_t bind_port, std::size_t max_peers = 0,
                             bool peer_named = false, std::uint32_t send_timeout_ms = 0,
                             std::uint32_t auth_deadline_ms = 0, std::size_t rx_scratch_bytes = 0,
                             std::size_t tx_pool_slots = 0, std::size_t tx_inline_bytes = 0);

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
     * PRECONDITIONS ON @p external's `httpd_config_t`. The port-binding ctor sets these
     * three itself; here they belong to the server's owner, and `esp_http_server` exposes
     * no reader for a running server's config, so this link can neither apply them nor
     * verify them. They are stated here because this is where the person who can apply
     * them reads (#955):
     *   1. `stack_size >= kRequiredHttpdStack`. The WS handler services the graph request
     *      in-call on the server's task, so the deep batch-apply path runs on whatever
     *      stack that server was started with. Too small is a stack-protection panic on
     *      that path, not an error return. Unverifiable, so the link samples
     *      `uxTaskGetStackHighWaterMark` at each session claim and logs ONE error if the
     *      free headroom is thin — it names the cause of a reboot, it cannot prevent one.
     *   2. `lru_purge_enable = false`. With purge on, `httpd_accept_conn` closes the
     *      least-recently-used session before each accept once the socket table is full,
     *      and — apart from the explicit `httpd_sess_update_lru_counter` API — IDF advances
     *      a session's LRU counter only from `httpd_sess_process` (inbound request
     *      processing); a server-initiated push does not touch it. A
     *      graph peer that subscribes and thereafter only RECEIVES therefore ages toward
     *      the lowest counter, i.e. toward being the victim. The link mitigates this from
     *      its side in adopted mode — it calls `httpd_sess_update_lru_counter` after each
     *      successful send, so a peer it is actively pushing to is no longer preferentially
     *      chosen — but that is not immunity: at the host's socket ceiling SOME session is
     *      still evicted, and this link reports such an eviction as an ordinary departure.
     *   3. A socket budget consistent with @p max_peers. The port-binding ctor sizes
     *      `max_open_sockets` to the cap plus slack; a shared server's budget is its
     *      owner's, spent on SPA assets and browser keep-alives too. This link's OWN cap
     *      is still enforced in adopted mode (a peer past @p max_peers is refused at its
     *      claim, exactly as below), but which peers get accepted at all is the host's
     *      socket policy, decided before this handler runs.
     *
     * @param external   A started `esp_http_server` handle to host the WS URI on; the
     *                   caller retains ownership and must outlive this link.
     * @param uri        WS URI pattern to register the handler at (e.g. "/ws"). Register
     *                   it BEFORE any wildcard route so registration-order precedence
     *                   routes it to the WS handler; keep it an exact literal.
     * @param max_peers  Concurrent-peer admission cap; 0 = unbounded. Beyond it the peer is
     *                   refused at the edge that claims its slot — the HANDSHAKE for a
     *                   session the three-valued predicate answered @ref
     *                   admission_verdict_t::ADMIT_AUTHENTICATED for, its FIRST frame for
     *                   every other (#1334). Clean either way, mirroring transport_ws_server.
     * @param peer_named Expose the @ref bus_link_t facet: each inbound peer gets its
     *                   own `<ip>:<port>` return-route identity (the browser-tabs
     *                   deployment). Off keeps point-to-point hop naming (send()
     *                   fans out; inbound arrives as the registered child NAME).
     * @param send_timeout_ms Per-socket send bound for UPGRADED sockets, milliseconds;
     *                   0 (the default) derives it — see @ref send_timeout_ms.
     * @param auth_deadline_ms How long an admitted session may stay UNAUTHENTICATED before
     *                   this link closes it, milliseconds; 0 (the default) uses
     *                   @ref kDefaultAuthDeadlineMs. Inert unless @ref set_auth_cb installed
     *                   a hook, and inert for a session the admission predicate already
     *                   authenticated (#1334) — no deadline is armed for such a session.
     * @param rx_scratch_bytes Reusable RX scratch capacity, bytes; 0 (the default) uses
     *                   @ref kDefaultRxScratchBytes.
     * @param tx_pool_slots TX work slots any sender may claim; 0 (the default) uses
     *                   @ref kDefaultTxPoolSlots. On an ADOPTED server this is the knob
     *                   worth revisiting: the in-flight depth that fits depends on how
     *                   promptly the shared task drains, and that task is also serving
     *                   the host's own routes.
     * @param tx_inline_bytes Inline payload capacity of one TX slot, bytes; 0 (the
     *                   default) uses @ref kDefaultTxInlineBytes.
     */
    httpd_ws_link_t(httpd_handle_t external, const char* uri, std::size_t max_peers = 0,
                    bool peer_named = false, std::uint32_t send_timeout_ms = 0,
                    std::uint32_t auth_deadline_ms = 0, std::size_t rx_scratch_bytes = 0,
                    std::size_t tx_pool_slots = 0, std::size_t tx_inline_bytes = 0);

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
     *        pre-allocated tx work slot (nothrow heap payload only past its inline
     *        capacity), one BINARY message per open peer.
     *
     * Overrides the base gather-into-a-temporary default: the reply rope's iovec is
     * copied exactly ONCE (into the queued work slot), so a large reply is never
     * double-buffered (gather temp + tx copy) on the heap — the transient that
     * exhausted the chip heap under concurrent SPA asset GETs. With no slot free, or
     * on allocation failure, the frame is dropped and counted (backpressure), never
     * an abort.
     */
    void send(std::span<const std::span<const std::byte>> iov) override;

    /** @brief Span delivery: the router services each inbound frame in-call, so no
     *         frame outlives its callback (one override covers both bases). */
    [[nodiscard]] bool delivers_ropes() const override { return false; }

    /** @brief The @ref bus_link_t facet when constructed `peer_named`, else nullptr. */
    [[nodiscard]] bus_link_t* bus() override { return peer_named_ ? this : nullptr; }

    /**
     * @brief The mode authority (#889): the `peer_named` this link was constructed with.
     *
     * The same flag @ref bus keys off, published so `bus_link_t` can REFUSE its
     * peer-named wiring calls on a flat link — that base is public, so those setters are
     * reachable by an upcast past the null `bus()`. Delivery (@ref deliver) and the
     * departure fork already read the flag directly.
     */
    [[nodiscard]] bool peer_named() const noexcept override { return peer_named_; }

    /** @brief Visit the currently-open peers' names, `<ip>:<port>`. */
    void enumerate_peers(const peer_visitor_t& visit) const override;

    /**
     * @brief One open session's identity plus its passive counters, as handed to
     *        @ref enumerate_peer_stats.
     *
     * `slot` is this link's slot index: STABLE for as long as the session is open, and
     * reused by a later session once it departs — which is exactly why `gen` is here.
     * It is the SAME generation the link uses internally to tell one session's claim of
     * a slot from the next (it is half of the identity a queued frame carries), and it
     * increments on every CLAIM, so a consumer computing rates from successive snapshots
     * can tell "same connection, N more frames" from "a different peer landed on that
     * slot and its counters started over".
     *
     * `name` borrows the slot's string and is valid only for the duration of the visit
     * (it is handed out under `peers_m_`, like @ref enumerate_peers' name).
     */
    struct peer_stats_t {
        std::string_view name; /**< @brief The routable `p<slot>`, valid during the visit only. */
        std::size_t slot = 0;  /**< @brief Slot index, stable while the session is open. */
        std::uint32_t gen = 0; /**< @brief Bumped on every claim of this slot. */
        link_counters_t c;     /**< @brief Traffic counters (see link_stats.hpp). */
        /**
         * @brief The peer's `<ip>:<port>`, valid during the visit only — DIAGNOSTICS.
         *
         * The physical address that @ref name stopped carrying in #994, kept because
         * `p3` on its own tells an operator nothing about which client is misbehaving.
         * Never a path segment: it holds `.` and `:`, which `graph::valid_segment`
         * rejects, and feeding it back as an address is the defect #994 removed.
         */
        std::string_view endpoint_str;
        /**
         * @brief The identity the auth hook bound to this session, valid during the visit
         *        only; EMPTY when no hook is installed or it bound none.
         *
         * Never empty-because-unauthenticated: an unauthenticated session is not visited at
         * all (see @ref set_auth_cb), so everything enumerated here has already been
         * ACCEPTed. What the subject MEANS to a handler is #375's question, not this
         * accessor's — here it is the operator-facing answer to "who is on p3?".
         */
        std::string_view subject;
    };
    /** @brief Visitor for @ref enumerate_peer_stats. */
    using peer_stats_visitor_t = std::function<void(const peer_stats_t&)>;

    /**
     * @brief Visit every OPEN session's counters, copied out under @ref peers_m_.
     *
     * The metrics counterpart of @ref enumerate_peers, same house style and same
     * contract: brief, no allocation, callable from any task. The visitor runs WITH
     * `peers_m_` held, so it must not re-enter this link and must not block — the
     * expected visitor only stores bytes into its own buffer from here.
     *
     * NO CALLBACK INTO THE EMBEDDER RUNS UNDER `peers_m_` other than this visitor, and
     * this one is a pure copy-out by construction. That is what keeps a host's lock
     * order acyclic when it holds its own mutex across the call.
     *
     * "No allocation" includes the VISITOR ITSELF: `std::function`'s inline buffer is
     * one or two words on a 32-bit target, so a `[&]` closure over a handful of locals
     * spills to the heap on every call. Callers on a periodic path should capture a
     * single pointer to their own context struct.
     */
    void enumerate_peer_stats(const peer_stats_visitor_t& visit) const;

    /** @brief Resolve an open peer's name to its directed sending endpoint (owned by
     *         the peer's slot, pointer-valid for this link's lifetime). */
    [[nodiscard]] transport_t* peer_link(std::string_view peer) override;

    /**
     * @brief Resolve an inbound handle back to its `p<slot>` peer name (#1294).
     *
     * A pure function of the handle's index — the same formatting the claim edge stamps
     * @ref session_t::name with (ADR-0073 §2) — so it takes no `peers_m_` and can be called
     * from the delivery callback, on the httpd task, where the name used to arrive for free.
     */
    [[nodiscard]] std::string_view peer_name(peer_handle_t peer,
                                             std::span<char> scratch) const override;

    /**
     * @brief Close one peer's session by NAME — the "revoke this controller" action
     *        (#1146), callable from any task.
     *
     * The base contract holds honestly here, and only because of the component's ESP-IDF
     * floor: `true` means the teardown really was initiated. The close cannot run in-call
     * — `esp_http_server` owns the descriptor's lifetime on its own task, and an off-task
     * `shutdown` of a stale fd lands on whoever inherited the number (#954), the exact
     * hazard @ref condemn's precondition exists for — so this marshals a work item onto
     * the httpd task, exactly as every send does. Above the floor (>=5.5.5,
     * idf_component.yml) `httpd_queue_work`'s verdict is trustworthy: the mbox slot is
     * reserved before the control-socket `sendto`, so ESP_OK means the item WILL run and
     * ESP_FAIL is a refusal the caller sees — the pre-floor silent bin, which would have
     * made `true` a lie precisely when the queue is fullest (i.e. when a stalling peer
     * most deserves revoking), does not exist any more (#949).
     *
     * The work item carries the session's `(slot, generation)` identity minted at THIS
     * call, never a descriptor: if the named session departs — or its slot is reclaimed
     * and even re-earns the same positional name — before the item drains, the identity
     * test fails and the item does nothing, so a `true` can close only the session that
     * was named when it was returned (#954's rule applied to the close path). On the
     * httpd task the close is @ref close_session: a CLOSE frame with @ref kCloseRevoked
     * first, so the peer can tell revocation from a network fault, then the `shutdown`
     * that gets the session reaped and its departure notified through the ordinary
     * free_ctx seam.
     *
     * @retval true  @p peer named an open, served session and the close work item was
     *               accepted by the control queue — the teardown will run.
     * @retval false @p peer names no served session (unknown, departed, condemned, or
     *               still unauthenticated — an unauthenticated session is not a peer,
     *               same visibility rule as @ref enumerate_peers), this link is FLAT
     *               (per-peer naming does not exist), the control queue refused the
     *               enqueue, or the work item could not be allocated. Nothing happened;
     *               the caller may retry.
     */
    [[nodiscard]] bool close_peer(std::string_view peer) override;

    /**
     * @brief The CAME-UP predicate (#1059): true if the httpd instance started and the WS
     *        handler registered — a construction fact, asked once, never reverting.
     *
     * Already the ruled meaning: `handle_` is published by the constructor and cleared only
     * by the destructor, so this never answers live state (the defect #1203 fixed on the
     * client link). Which peers are currently attached is a different question entirely —
     * @ref enumerate_peers / @ref peer_stats_t — because this is a MULTI-PEER server.
     *
     * On a target that closed the ADR-0044 bus module out
     * (`tr::graph::default_config_t::kBusLinks = false`, #375 deliverable 3) it is
     * unconditionally false: this link is peer-named by construction — it has no flat mode to
     * fall back to — so a build that declared it carries no bus facet cannot serve it, and
     * saying so through the came-up predicate is what stops it being wired into a routing
     * plane that would resolve none of its peers. At the default binding the `if constexpr`
     * is discarded and this is `handle_ != nullptr`, the predicate it always was.
     */
    [[nodiscard]] bool ok() const noexcept {
        if constexpr (!kBusLinks) return false;
        return handle_ != nullptr;
    }

    /**
     * @brief Liveness (the @ref transport_t::link_up contract): true while this server is
     *        standing, i.e. exactly while it came up.
     *
     * A multi-peer server OUTLIVES any one peer — that is why the base default is `true`
     * and why no session's departure moves this — so the only thing that can make it false
     * is a server that never started at all. Answering the base default there would report
     * a link that cannot carry a frame as live, which is why this is overridden rather than
     * inherited (#1203). Per-peer liveness is not this accessor's question: a departed peer
     * is reported through the bus facet's per-session eviction seam.
     */
    [[nodiscard]] bool link_up() const noexcept override {
        return handle_.load(std::memory_order_relaxed) != nullptr;
    }

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
     * @brief Frames this link never handed to a socket, for its life: the shared control
     *        queue refused the enqueue, the TX slot pool was exhausted, or an oversize
     *        payload could not be allocated.
     *
     * A LINK-level count on purpose, and all three causes are link-level facts. A refused
     * enqueue says the httpd control queue is saturated — which under #835's failure shape
     * is caused by whichever peer is stalling the task, not by the peer whose frame is
     * being enqueued at that instant. Charging it to that peer closed HEALTHY sessions
     * while the culprit never accrued a strike; the per-session streak now counts failed
     * SENDS, which do name their peer. An exhausted pool says the same thing about this
     * link's own in-flight depth (@ref tx_slots_busy against @ref tx_slot_capacity).
     *
     * This is the whole TX-loss surface above the component's ESP-IDF floor. Below that
     * floor it was not: a full control mbox discarded the datagram inside lwIP while
     * reporting success, so the loss was unobservable by construction — see
     * `idf_component.yml` and #949.
     */
    [[nodiscard]] std::uint32_t enqueue_drops() const noexcept {
        return enqueue_drops_.load(std::memory_order_relaxed);
    }

    /**
     * @brief The LINK-level failure tally — the events that name this link or the shared
     *        server, not any one peer (#953).
     *
     * Read the per-PEER traffic counters through @ref enumerate_peer_stats instead: frames,
     * bytes and per-session drops are facts about a session and live on it. What is here is
     * everything with no session to charge — a peer refused before it ever had a slot, a
     * frame aimed at a session that had already gone, the TX pool's own depth — plus the
     * two session-lifecycle events an embedder cannot otherwise reconstruct.
     *
     * Every field is a monotone count since construction, sampled with relaxed loads: a
     * snapshot is cheap and lock-free, but the fields are NOT mutually consistent to a
     * single instant. Differences across two snapshots are the intended use.
     */
    struct stats_t {
        /** @brief Frames never handed to a socket; the sum of the three causes below.
         *         Same number @ref enqueue_drops returns. */
        std::uint32_t enqueue_drops = 0;
        /**
         * @brief Of those, the ones that found the TX pool with nothing free.
         *
         * Separated because it means something different from the other two (#953): a
         * pool miss is DEPTH pressure — this link already has @ref tx_slot_capacity sends
         * outstanding — while a refused enqueue names the shared control queue and an OOM
         * names the heap.
         *
         * Since #1187 an off-httpd-task send WAITS for a slot before it misses, so this
         * counts only the sends that were still unserved after a full send occupancy — a
         * DRAIN that is not keeping up, not merely a wide fan-out. Read it against
         * @ref tx_pool_waits: waits without misses is the pool doing its job as a bound
         * (wide passes costing latency), while misses mean the httpd task is stalled on
         * some peer for longer than one send bound. A send issued ON the httpd task never
         * waits — it is the drain — so an in-call fan-out still misses at the pool's depth.
         */
        std::uint32_t tx_pool_misses = 0;
        /**
         * @brief Sends that found the pool full and WAITED for the httpd task to free a
         *        slot (#1187), whether or not the wait then succeeded.
         *
         * The early-warning half of the pair above, and the one that is NOT a loss: every
         * frame counted here was either delivered late or is also counted in
         * @ref tx_pool_misses. A steadily rising count means fan-out passes are wider than
         * the link's in-flight depth and are being paced by the drain — normal, and the
         * whole point of the bound — while a count that starts climbing where it used to
         * be flat says a producer's publish sweep is now costing it latency it did not pay
         * before.
         */
        std::uint32_t tx_pool_waits = 0;
        /**
         * @brief Frames dropped at the head of the send path because the destination had
         *        already departed, been condemned, or had its slot reclaimed by a new
         *        session.
         *
         * Not a fault: it is the normal race between a fan-out that already snapshotted
         * its destinations and a peer leaving underneath it. It is counted because a
         * silent drop here is indistinguishable from a delivery, and because a LARGE
         * count means the producer is pushing to sessions long dead.
         */
        std::uint32_t tx_to_dead_peer = 0;
        /** @brief Opening handshakes turned away — by the admission predicate, by `max_peers`,
         *         or (#1247) by a FULL pending-handshake ledger, which is what a link with an
         *         auth hook answers instead of aborting; see @ref kMaxPendingHandshakes. These
         *         never reach a slot, so no session can carry them. */
        std::uint32_t peers_refused = 0;
        /** @brief Sessions this link KILLED — a three-strike streak, a rejected short
         *         write, an auth verdict (@ref auth_rejected / @ref auth_expired name the
         *         cause), or an administrative @ref close_peer — the count that separates
         *         a peer that left from one torn down. */
        std::uint32_t sessions_condemned = 0;
        /** @brief Inbound frames refused at the abuse cap, before any reassembly. The peer
         *         is dropped with them. No session is charged: the cap is applied before
         *         the slot is resolved. */
        std::uint32_t rx_dropped_oversize = 0;
        /** @brief Inbound frames dropped because the oversize payload buffer could not be
         *         allocated. The session is closed with them. */
        std::uint32_t rx_dropped_alloc = 0;
        /**
         * @brief Sessions closed because the auth hook answered REJECT — a credential this
         *        node was OFFERED and refused.
         *
         * Kept apart from @ref peers_refused, which counts handshakes turned away one stage
         * earlier (the admission predicate, or `max_peers`), and from @ref auth_expired,
         * which counts peers that offered nothing at all. The three are different events
         * about different actors and summing them hides which one is happening: a climbing
         * count here is credentials being TRIED, i.e. the shape of a brute-force attempt or
         * of a fleet holding a stale token.
         */
        std::uint32_t auth_rejected = 0;
        /**
         * @brief Sessions — and, since #1247, un-spoken upgraded SOCKETS — closed because they
         *        did not authenticate within `auth_deadline_ms`.
         *
         * The squatter count, and it now counts BOTH shapes of squatter: the peer that spoke
         * and then stalled (it holds a `session_t`) and the peer that completed the 101 and
         * said nothing at all (it holds only a socket, which is the scarcer of the two on an
         * embedded node). A steady trickle is normal — a browser tab closed mid-login, a peer
         * that lost its network between the 101 and its first frame — while a rate that tracks
         * connection attempts means something is opening sockets and never speaking, which is
         * the exposure the deadline exists for.
         */
        std::uint32_t auth_expired = 0;
    };

    /**
     * @brief Snapshot @ref stats_t. Lock-free, callable from any task, never re-enters the
     *        embedder — so a host may hold its own lock across it.
     */
    [[nodiscard]] stats_t stats() const noexcept;

    /**
     * @brief The interface-level shed-frame snapshot (#932) — the subset of @ref stats_t
     *        a generic `transport_t*` holder can read, in the shape every kind answers.
     *
     * Projected from this link's own richer counters: an ingress frame refused at the
     * abuse cap is the `malformed_rx` class (the peer broke the agreed bound and is
     * dropped with it), an ingress allocation failure is `dropped_rx` (backpressure),
     * and both egress classes — an enqueue that found no slot/queue/heap, and a send to
     * a peer that had already departed — sum into `dropped_tx`.
     */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        const stats_t s = stats();
        return {s.rx_dropped_alloc, s.rx_dropped_oversize,
                static_cast<std::uint64_t>(s.enqueue_drops) + s.tx_to_dead_peer};
    }

    /** @brief TX work slots claimed RIGHT NOW (filling, queued, or sending) — across the
     *         pool AND the in-call reserve, so the ceiling here is
     *         @ref tx_slot_capacity + @ref tx_reply_reserve. */
    [[nodiscard]] std::size_t tx_slots_busy() const noexcept;

    /**
     * @brief TX work slots ANY sender may claim — this link's OUTSTANDING-SEND bound.
     *
     * Not a fan-out ceiling, and the difference is what #1187 corrected. A send from any
     * task other than the httpd task WAITS (bounded, off the CPU) for the drain to free a
     * slot, so a broadcast to more peers than this costs latency rather than losing its
     * tail; only a send issued ON the httpd task — a reply serviced in-call, or a push
     * provoked by an inbound frame — hits this depth as a hard same-pass limit, since the
     * task it would wait for is the one asking.
     *
     * It is the depth available to EVERY sender, including a producer's fan-out: the
     * in-call reserve is @ref tx_reply_reserve slots on top of it, not a slice out of it.
     * That is #1218 — reserved out of this number instead, it made every off-task fan-out
     * one destination narrower than the bound stated here, so a sweep that had always fit
     * began waiting for the drain on every pass whether or not a reply was pending.
     *
     * A frame that is still unserved when the wait expires is dropped and counted
     * (@ref enqueue_drops, and @ref stats_t::tx_pool_misses for the cause); waits themselves
     * are counted in @ref stats_t::tx_pool_waits.
     *
     * PER-LINK since #1160 (the ctor's `tx_pool_slots`), hence no longer `static`: a
     * caller that needs the figure without a link in hand names @ref kDefaultTxPoolSlots,
     * which is what this reports for a link that took the default.
     */
    [[nodiscard]] std::size_t tx_slot_capacity() const noexcept;

    /** @brief Effective reusable RX scratch capacity, bytes — the ctor's
     *         `rx_scratch_bytes` as this link actually allocated it (0 if the allocation
     *         failed, in which case every frame takes the per-frame nothrow path). */
    [[nodiscard]] std::size_t rx_scratch_bytes() const noexcept;

    /** @brief Effective inline payload capacity of one TX work slot, bytes — the ctor's
     *         `tx_inline_bytes`. A frame past it keeps its pooled shell and takes a
     *         nothrow heap payload, so this is the size above which a send allocates. */
    [[nodiscard]] std::size_t tx_inline_bytes() const noexcept;

    /** @brief This link's whole per-link buffer cost, bytes:
     *         `rx_scratch_bytes() + (tx_slot_capacity() + tx_reply_reserve()) *
     *         tx_inline_bytes()`, plus the slot shells. What a RAM audit asks for, and
     *         what #1160 exists to make reportable rather than derivable-from-sources. */
    [[nodiscard]] std::size_t buffer_bytes() const noexcept;

    /**
     * @brief TX work slots held back for sends issued ON the httpd task, ADDITIONAL to
     *        @ref tx_slot_capacity (#1218).
     *
     * The in-call sender is the one claimer that cannot wait for a slot — the task that
     * frees them is the task asking — so the guarantee that a request's reply always finds
     * one has to be a slot no other sender can take. It costs the RAM of one more slot per
     * link, and buys back the fan-out width that carving the reserve out of the pool had
     * taken from every producer.
     */
    [[nodiscard]] static std::size_t tx_reply_reserve() noexcept;

    /**
     * @brief Admission predicate: given the parsed opening GET, return true to admit the
     *        peer or false to refuse it. A refusal is `ESP_FAIL` back to
     *        `esp_http_server`, which abandons the upgrade and closes the socket. @p ctx
     *        is the opaque pointer registered alongside it in @ref set_admission_cb.
     *
     * @p req is a REQUEST-scoped `httpd_req_t*`: `httpd_req_get_hdr_value_str`,
     * `httpd_req_get_url_query_str` and `req->uri` all answer for this handshake. It is
     * valid for the duration of the call and no longer.
     */
    using admission_fn_t = bool (*)(void* ctx, httpd_req_t* req);

    /**
     * @brief What an @ref admission_verdict_fn_t answer does with the handshake it was asked
     *        about — three outcomes, because a handshake can carry a credential of its own.
     *
     * The two-valued predicate cannot express the case that makes @ref set_admission_cb and
     * @ref set_auth_cb composable: a peer whose HANDSHAKE already authenticated it. Without
     * that, installing an auth hook puts every session into the unauthenticated state,
     * including one the predicate just validated — and a peer that presents a header (a
     * native dialer, @ref esp_ws_client_link_t) has no way to send an authentication frame,
     * so it is closed at the deadline however good its credential was (#1245).
     */
    enum class admission_verdict_t : std::uint8_t {
        /** @brief Refuse the handshake: no 101, no session. Identical to answering `false`. */
        REFUSE,
        /** @brief Admit it. With an auth hook installed the session starts UNAUTHENTICATED
         *         and must present a credential frame; without one it is served at once.
         *         Identical to answering `true`. */
        ADMIT,
        /** @brief Admit it AND treat the handshake as its authentication: no credential
         *         frame is asked for, no deadline is armed, and the session is served from
         *         its first data frame even though an auth hook is installed. Identical to
         *         @ref ADMIT on a link with no auth hook. */
        ADMIT_AUTHENTICATED,
    };

    /**
     * @brief Admission predicate that can also PRE-AUTHENTICATE the session — the three-valued
     *        form of @ref admission_fn_t, and the one a link serving both browsers (which must
     *        use the in-band frame) and native dialers (which cannot) needs.
     *
     * @p req is a REQUEST-scoped `httpd_req_t*` under the same rules @ref admission_fn_t
     * documents: valid for the duration of the call and no longer.
     */
    using admission_verdict_fn_t = admission_verdict_t (*)(void* ctx, httpd_req_t* req);

    /**
     * @brief Install (or clear, with `nullptr`) an admission predicate consulted on every
     *        opening handshake, in the server's WebSocket PRE-handshake callback — before
     *        the 101 is written, before the session is upgraded, and therefore before any
     *        slot of this link's exists. Unset (the default) admits every peer.
     *
     * The seam a host uses to authenticate the graph WS the same way it gates the rest of
     * its HTTP surface: inspect the handshake request's headers (a session cookie, a
     * shared token) and refuse an unauthenticated peer before it can read or write a
     * single vertex. NOT synchronized — set it once at wiring time, before the link
     * serves; the hook is read on the httpd task with no lock.
     *
     * WHERE it runs is not a detail. `esp_http_server` answers the WebSocket handshake
     * internally and then returns WITHOUT calling the URI handler for that request
     * (`httpd_uri.c`), so a predicate placed in the handler sees only data frames on an
     * ALREADY upgraded socket — never the opening GET, and never in time to stop the
     * upgrade. The server's `ws_pre_handshake_cb` is the one call that gets the parsed
     * GET, so that is where this link installs its thunk, at both registration sites
     * (own-server and adopted-server). The component's `Kconfig` `select`s
     * `HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT` for that reason; the member is present at the
     * component's ESP-IDF floor, so there is no fallback tier.
     *
     * In ADOPTED mode the predicate is scoped to THIS link's WS URI. Registration is
     * per-URI, so the rest of the caller's HTTP surface on that server is not affected.
     */
    void set_admission_cb(admission_fn_t fn, void* ctx) noexcept {
        admission_fn_ = fn;
        admission_verdict_fn_ = nullptr;
        admission_ctx_ = ctx;
    }

    /**
     * @brief Install (or clear, with `nullptr`) the three-valued admission predicate. Same
     *        call site, same timing and same rules as @ref set_admission_cb — the only
     *        difference is that this one may answer @ref
     *        admission_verdict_t::ADMIT_AUTHENTICATED.
     *
     * A SEPARATE name rather than an overload: `set_admission_cb(nullptr, nullptr)` is an
     * existing call, and a second overload would make it ambiguous — a source break on a
     * published surface, to save one identifier.
     *
     * The two forms are ALTERNATIVES, not a pair: installing either clears the other, so a
     * link always has exactly one admission predicate and there is no order in which two
     * could disagree.
     *
     * HOW THE VERDICT REACHES THE SESSION. It does not have to travel at all: since #1334 an
     * @ref admission_verdict_t::ADMIT_AUTHENTICATED verdict CLAIMS THE SESSION HERE, during the
     * pre-handshake, so the slot exists before the 101 is written and carries the verdict as
     * ordinary state (`auth_pending` false, no deadline). It used to be recorded against the
     * socket descriptor instead and consumed at the lazy first-frame claim — which meant the
     * deadline sweep, whose only handle on such a socket was that same row, closed a validly
     * authenticated peer that had simply not spoken yet (#1334).
     *
     * WHAT IT COSTS. A peer slot is charged at the 101 rather than at the first frame. That is
     * the correct charge — the peer is authenticated and entitled — and a link at `max_peers`
     * refuses the upgrade outright, which is a cleaner answer than admitting a peer and killing
     * it `auth_deadline_ms` later. The @ref pending_ ledger keeps only the sessions whose
     * entitlement is NOT yet known, which is what its fixed size was always sized for.
     */
    void set_admission_verdict_cb(admission_verdict_fn_t fn, void* ctx) noexcept {
        admission_verdict_fn_ = fn;
        admission_fn_ = nullptr;
        admission_ctx_ = ctx;
    }

    /**
     * @brief What an @ref auth_fn_t answer does with the session it was asked about.
     *
     * Three outcomes and not two, because the frame is specified as a CARRIER rather than as
     * a bearer-token check: a credential that needs one round trip answers ACCEPT on the
     * first frame, and a handshake that needs several (the ed25519/Noise direction this was
     * designed for) answers CONTINUE until it is done. The link's own behaviour is identical
     * either way — it is the hook that knows how many frames its scheme costs.
     */
    enum class auth_verdict_t : std::uint8_t {
        ACCEPT,   /**< @brief Credential good: serve this session from the next frame on. */
        CONTINUE, /**< @brief Not done yet: keep the session unauthenticated, same deadline. */
        REJECT    /**< @brief Credential bad: close with @ref kCloseAuthFailed. */
    };

    /**
     * @brief One @ref auth_fn_t answer: the verdict, an optional frame to send back, and —
     *        on ACCEPT — the identity to bind to the session.
     *
     * Both spans/views are BORROWED for the duration of the call: the link copies whatever
     * it keeps before returning, so a hook may answer out of a stack buffer or its own ctx.
     */
    struct auth_result_t {
        /** @brief What to do with the session. Value-initialises to REJECT so a hook that
         *         forgets to set it fails CLOSED. */
        auth_verdict_t verdict = auth_verdict_t::REJECT;
        /**
         * @brief Bytes to send back to the peer as one BINARY frame, or empty for none.
         *
         * The half of the carrier a bearer token never needs and a handshake cannot work
         * without — a Noise responder's message rides here. Sent BEFORE the session is
         * served (ACCEPT) or closed (REJECT), so a rejected peer can still be told why in
         * its own scheme's language on top of the close code.
         */
        std::span<const std::byte> reply;
        /**
         * @brief The session subject to bind on ACCEPT — ignored on the other two verdicts.
         *
         * TEXT, and printable: an identity that is natively bytes (an ed25519 public key)
         * is spelled by the hook in hex or base64, which is what keeps it reportable through
         * @ref peer_stats_t::subject beside `endpoint_str`. Truncated at
         * @ref kMaxSubjectChars.
         *
         * This link BINDS it and publishes it. The HANDLER-side half now exists (#375 Part 1):
         * `tr::graph::handlers_t::on_write` receives a `write_ctx_t` whose `subject` is the
         * writer's resolved subject token. What is still open is the JOIN — a subject bound
         * here does not yet become the graph's caller context for frames arriving on this
         * session, so a handler currently sees the inbound link's name. Carrying it is this
         * frame's job regardless, precisely because the frame is where a session's identity
         * is first known.
         */
        std::string_view subject;
    };

    /**
     * @brief The post-handshake authentication hook: given one whole inbound message from a
     *        not-yet-authenticated session, decide what to do with that session. @p ctx is
     *        the opaque pointer registered alongside it in @ref set_auth_cb.
     *
     * @p payload is the message's bytes, reassembled, and is OPAQUE to this link: it is not
     * decoded, not validated, and never mistaken for a TLV. That opacity is the design (the
     * frame is the permanent carrier, the credential kind is the hook's business), not an
     * omission.
     */
    using auth_fn_t = auth_result_t (*)(void* ctx, std::span<const std::byte> payload);

    /**
     * @brief Install (or clear, with `nullptr`) the post-handshake authentication hook.
     *        Unset (the default) serves every admitted session immediately — the historical
     *        behaviour, and still the right one for a link gated at the handshake.
     *
     * WHY THIS EXISTS ALONGSIDE @ref set_admission_cb. The browser `WebSocket` API cannot
     * set request headers, so a browser peer has no way to present the token an admission
     * predicate reads — leaving embedders a cookie (and the HTTP login endpoint that mints
     * it) or the credential in the URL query string, which leaks it into server logs,
     * browser history and referrers. This hook moves the credential IN-BAND, after the 101,
     * where a browser can send it. Header-based admission is unaffected and both may be
     * installed at once: the predicate still decides the handshake, and this hook then
     * decides the session.
     *
     * WHAT AN UNAUTHENTICATED SESSION GETS: nothing. Between its first frame and its ACCEPT
     * the session exists but is not served — its messages reach this hook and never the
     * graph, it is absent from @ref enumerate_peers, @ref enumerate_peer_stats and
     * @ref peer_link, and a broadcast @ref send skips it. It cannot read a vertex, write
     * one, subscribe, or be discovered by a peer that can.
     *
     * WHAT BOUNDS IT: the constructor's `auth_deadline_ms`. A session that has not been
     * ACCEPTed within it is closed with @ref kCloseAuthTimeout — and since #1247 so is a
     * SOCKET that completed the 101 and never sent a first frame at all, which has no session
     * to bound and used to live until the HTTP server's own keepalive policy noticed it. That
     * is the "open sockets, say nothing" case the deadline was documented to stop; it is now
     * carried by the pending-handshake ledger (@ref pending_). Without that bound an
     * unauthenticated peer would hold one unit of `max_peers` for as long as it cared to,
     * which on an embedded slot pool is a cheap denial of service — and a NEW exposure, since
     * before this hook existed no unadmitted peer ever reached a slot at all.
     *
     * NOT SYNCHRONIZED — set it once at wiring time, before the link serves; the hook is
     * read on the httpd task with no lock. It runs ON that task, in-call, with no lock of
     * this link held, and must be brief: the task it occupies owns accept, recv and every
     * other session's I/O.
     *
     * IT MUST NOT DESTROY THIS LINK. That is a narrower contract than the graph delivery
     * path carries — an inbound TLV may drive an app teardown in-call (#814) and this link
     * survives it by touching nothing afterwards — and the difference is that the verdict has
     * work to do ON RETURN: the reply to write, the subject to bind, the close code to send.
     * A credential check has no business tearing down the node it is authenticating, so the
     * contract is stated rather than engineered around.
     *
     * Installing a hook also ARMS the deadline sweep (one periodic `esp_timer` per link,
     * created here and never re-created). A sweep that cannot be armed is reported at ERROR
     * and the hook still takes effect: authentication without a bound is strictly better
     * than no authentication, and the alternative — refusing to authenticate because a timer
     * was unavailable — fails open, which is the one outcome this seam must never have.
     */
    void set_auth_cb(auth_fn_t fn, void* ctx) noexcept;

    /**
     * @brief WebSocket close code for a session the auth hook REJECTED — RFC 6455
     *        application range (4000–4999), mnemonic for HTTP 401.
     *
     * Distinct from @ref kCloseAuthTimeout on purpose: "your credential was refused" and
     * "you were too slow" call for different client behaviour (re-prompt versus retry), and
     * a client that has to guess which one it hit will guess wrong. Picked once, here.
     */
    static constexpr std::uint16_t kCloseAuthFailed = 4401;
    /** @brief WebSocket close code for a session that ran out its `auth_deadline_ms` —
     *         mnemonic for HTTP 408. See @ref kCloseAuthFailed for why the two differ. */
    static constexpr std::uint16_t kCloseAuthTimeout = 4408;
    /**
     * @brief WebSocket close code for a session torn down by @ref close_peer — mnemonic
     *        for HTTP 403.
     *
     * Distinct from the two auth codes for the same reason they are distinct from each
     * other: "your access was revoked" calls for different client behaviour (stop
     * reconnecting) than "your credential was refused" (re-prompt) or "you were too
     * slow" (retry), and a revoked controller that reads its close as a network fault
     * will reconnect forever.
     */
    static constexpr std::uint16_t kCloseRevoked = 4403;
    /** @brief Default `auth_deadline_ms`: long enough for a multi-round-trip handshake over
     *         a slow link, short enough that a squatting session is not a resource. */
    static constexpr std::uint32_t kDefaultAuthDeadlineMs = 10000;
    /** @brief Longest @ref auth_result_t::subject a session stores; a longer one is
     *         truncated. Sized to hold a hex-spelled 32-byte public key (64 chars) with
     *         room for a scheme prefix. */
    static constexpr std::size_t kMaxSubjectChars = 127;
    /**
     * @brief How many upgraded sockets may be awaiting their first data frame at once on a
     *        link with an authentication hook installed — the size of the pending-handshake
     *        ledger, and therefore a HARD bound on concurrent un-spoken upgrades.
     *
     * WHAT AN ENTRY IS. One row per socket that passed admission, whose entitlement is NOT YET
     * KNOWN, and which has not yet claimed a session — carrying the instant its
     * `auth_deadline_ms` runs out (#1247). It is a handoff between two points of the SAME
     * connection's setup, not a table: the row is consumed at the lazy first-frame claim, or
     * reaped by the deadline sweep, whichever comes first. A link with no auth hook records
     * nothing and is bounded by nothing here — and neither does a handshake that already
     * authenticated its peer (#1334), which claims its session outright instead of waiting in
     * a ledger for an answer it has already given.
     *
     * WHAT THE BOUND COSTS, and it is not "nothing". The `max_open_sockets`+1-th silent
     * upgrade cannot be admitted, so the handshake is REFUSED: `ws_pre_handshake` answers
     * `ESP_FAIL`, `esp_http_server` abandons the upgrade and closes the socket, and the
     * refusal is counted in @ref stats_t::peers_refused. That is deliberate and was ruled on
     * (#1247): the alternative — sizing this so overflow is "unreachable by construction" and
     * asserting the invariant — is an invariant checked by reasoning rather than by the build,
     * and on an MCU an `assert` is an `abort()` in the field. Exhaustion in this project is
     * answered BY VALUE (ADR-0065's nothrow `try_alloc`, the egress store that drops and
     * counts a frame rather than truncating it), and this ledger is not the one component that
     * dies instead. Refusing is also the only arm that can be exercised AND ablated by a test,
     * so it does not ship unverified.
     *
     * WHY IT IS NOT REACHED IN PRACTICE. A row lives only for the gap between the 101 and that
     * peer's first frame, bounded above by `auth_deadline_ms`, so the ledger holds at most the
     * upgrades in flight in one deadline window. 24 is above any `max_open_sockets` an ESP node
     * runs — but "above" is a sizing argument, not a proof, which is exactly why the overflow
     * arm exists and behaves.
     */
    static constexpr std::size_t kMaxPendingHandshakes = 24;
    /** @brief Former name of @ref kMaxPendingHandshakes, kept so an embedder that read the
     *         0.12.0 constant still compiles. The ledger it sizes holds a row per pending
     *         handshake rather than per pre-authenticated one (#1247) — and since #1334 a
     *         pre-authenticated handshake takes no row at all, so the old name now describes
     *         the one population the ledger does NOT hold. Kept anyway: it is a compatibility
     *         name, not a description. */
    static constexpr std::size_t kMaxPreauthenticated = kMaxPendingHandshakes;

   private:
    struct gate_t;            // the handler-admission gate + teardown barrier (in the .cpp)
    struct session_t;         // one peer slot's connection state (defined in the .cpp)
    struct session_ref_t;     // a session identity that survives fd reuse (defined in the .cpp)
    struct tx_work_t;         // one queued outbound frame (defined in the .cpp)
    struct tx_slot_t;         // one pre-allocated TX work slot (defined in the .cpp)
    struct detach_req_t;      // the teardown session-detach work item (defined in the .cpp)
    struct close_req_t;       // the close_peer work item (defined in the .cpp)
    class peer_resolution_t;  // one RESOLUTION's directed endpoint (defined in the .cpp)

    /**
     * @brief Take a resolution handle for @p slot at its CURRENT generation — the object
     *        @ref peer_link returns (#1013). Caller holds @ref peers_m_.
     *
     * Deduplicated per LIVE SESSION: two callers that resolve the same peer between two
     * claims share one handle, which is safe precisely because they resolved against the
     * same generation. Returns nullptr only when the pool could not be grown, and a
     * refused resolution is an honest "no such peer" rather than a handle that might
     * outlive its session.
     */
    [[nodiscard]] peer_resolution_t* acquire_resolution(session_t* slot);
    /**
     * @brief Retire @p slot's resolution handle, if it holds one — the session it named
     *        is over (@ref peers_m_ held).
     *
     * @param inert When true the handle is also unbound from the link, for the teardown
     *              path that LEAKS the slot shell rather than freeing it: the handle must
     *              land on valid, inert memory rather than dereference an abandoned slot.
     */
    void retire_resolution(session_t* slot, bool inert);

    // --- httpd trampolines (static; recover `this` from req->user_ctx / work arg) ---
    static esp_err_t ws_handler(httpd_req_t* req);  // the WS URI handler (data frames)
    /**
     * @brief `ws_pre_handshake_cb`: run @ref set_admission_cb's predicate against the
     *        parsed opening GET, before the server writes the 101.
     *
     * Registered at BOTH construction sites. Like @ref ws_handler it recovers the link
     * through the gate (`req->user_ctx`) and registers on the gate's barrier for the
     * duration of the predicate, so a concurrent destructor joins it instead of freeing
     * the members it reads.
     */
    static esp_err_t ws_pre_handshake(httpd_req_t* req);
    static void on_session_closed(void* slot_ctx);  // free_ctx_fn: a peer departed
    static void tx_work(void* work_arg);            // httpd_queue_work fn: one queued send
    static void detach_work(void* req_arg);         // httpd_queue_work fn: teardown detach
    static void close_work(void* req_arg);          // httpd_queue_work fn: one close_peer

    // --- instance handlers (run on the httpd task) ---
    esp_err_t on_data_frame(httpd_req_t* req);  // recv one WS frame, (reassemble,) deliver
    /**
     * @brief Turn @p fd into a peer slot: enforce `max_peers`, take a free or fresh slot,
     *        name it, stamp its identity and its authentication state (httpd task only;
     *        @ref peers_m_ must ALREADY be held).
     *
     * ONE claim, TWO edges (#1334). A plain @ref admission_verdict_t::ADMIT session is claimed
     * LAZILY, on its first data frame, because that is the first moment this link hears from it
     * at all — `esp_http_server` answers the 101 itself and never calls the URI handler for the
     * opening GET. A session the predicate answered @ref
     * admission_verdict_t::ADMIT_AUTHENTICATED for is claimed EAGERLY, in @ref
     * ws_pre_handshake, because its entitlement is already known there and the ledger that
     * used to carry the verdict across the gap could only bound it with a deadline it can never
     * answer (see @ref pending_).
     *
     * @param fd            The socket the session is claimed for.
     * @param authenticated The handshake already established this peer's entitlement, so the
     *                      session is claimed SERVED: no `auth_pending`, and — the point of
     *                      #1334 — no `auth_deadline_us` for the sweep to fire on.
     *
     * @return the claimed slot, or nullptr when `max_peers` is spent. The refusal is answered
     *         by value because its COUNTING differs per edge: the first-frame caller counts it
     *         itself, while the pre-handshake caller folds it into the one refusal count the
     *         abandoned upgrade already carries (#1334 — never both).
     */
    [[nodiscard]] session_t* claim_session(int fd, bool authenticated);
    /**
     * @brief Recycle a departed peer's slot and report what the routing plane is still
     *        owed — the departed peer's NAME, or an empty string for nothing.
     *
     * Deliberately does NOT fire the departure notifier itself (#960). The caller holds
     * the handler gate to reach this at all, and the notifier is an unbounded foreign
     * callback into router → graph; firing it here would run it under that mutex. The
     * name comes back instead and @ref on_session_closed fires it with the gate released.
     */
    [[nodiscard]] std::string reclaim_slot(session_t* slot, peer_handle_t& handle);
    /**
     * @brief Fire the routing plane's eviction hook for the departed @p peer.
     *
     * MUST be called with neither `peers_m_` nor the handler gate's mutex held — the
     * precondition `bus_link_t::notify_peer_down` documents. The caller keeps the link
     * alive across it by registering on the gate's barrier, not by holding its lock.
     * In FLAT mode the hook is the WHOLE link's, so it waits for the last open session
     * (@ref any_open_session, #889).
     */
    void notify_departed(peer_handle_t handle, std::string_view peer);
    /**
     * @brief Announce a newly claimed slot as a live, named session (#1223 step 2).
     *
     * The arrival twin of @ref notify_departed, and the ESP-side parity for
     * `slot_server_t::publish_peer_up`: this link has the same positional-slot plane
     * (@ref slot_name, first-free reuse), so it has the same slot-recycling identity problem
     * and needs the same anchor. Fired at the CLAIM edge — this link's "the session is now
     * usable" transition, which is the lazy first-frame claim for an ordinary session and the
     * pre-handshake for a session the admission predicate already authenticated (#1334) — and,
     * like its twin, with neither `peers_m_` nor the gate's mutex held, because the notifier
     * re-enters the graph.
     * FLAT mode announces nothing: it has one routing identity for every tab it carries.
     */
    void notify_arrived(peer_handle_t handle, std::string_view peer);
    /**
     * @brief True while ANY slot is still open — the flat mode's "is the link still up"
     *        question (#889).
     *
     * Asked by @ref notify_departed after @ref reclaim_slot has already cleared the
     * departing slot's `open` under `peers_m_`, so it never counts the departed session.
     * Takes `peers_m_`; must not be called holding it.
     */
    [[nodiscard]] bool any_open_session() const;
    void deliver(peer_handle_t peer, std::span<const std::byte> frame);
    // ONE gather-copy into a pool slot + httpd_queue_work; drops the frame (counted, never
    // aborting) when no slot is free, when the enqueue is refused, or on OOM. The
    // destination is a SESSION, never a bare fd — see @ref session_ref_t.
    void queue_send(const session_ref_t& to, std::span<const std::span<const std::byte>> iov);
    void queue_send(const session_ref_t& to,
                    std::span<const std::byte> frame);  // one-span sugar over the gather

    /**
     * @brief Send a directed reply to @p to RIGHT NOW — the fork @ref queue_send is not
     *        (httpd task, inside this link's handler frame; @ref peers_m_ must NOT be held).
     *
     * The #1494 fix. A reply serviced in-call is already on the task that owns the
     * descriptor, so marshalling it costs a gather-copy, a pool slot and a round through
     * the control mbox to arrive at the same socket write — and the mbox's one-message-per
     * -pass drain is what put that issue's throughput cliff at the pool depth (the in-call
     * reserve is claimable once per drain, not once per request, so a pipelining client's
     * reply falls into the general pool and an in-call send never waits). Off the queue,
     * a reply cannot be lost to a full pool, a full mbox or a fan-out's occupancy, and it
     * can strand no slot on any ESP-IDF. The definition carries the full argument.
     *
     * Accounted exactly as @ref tx_work accounts a queued send: the same two liveness
     * tests, @ref note_tx_skip for a peer that is gone and @ref note_tx_result for a
     * result that names one. A single-span reply is written from the CALLER's memory with
     * no copy at all; a rope is gathered through one pool slot used as scratch, claimed
     * and released inside this call and posted nowhere.
     *
     * @param to  The destination session, resolved and identity-checked by the caller.
     * @param iov The reply, as the rope's segments — sent as ONE WebSocket frame.
     */
    void send_in_call(const session_ref_t& to, std::span<const std::span<const std::byte>> iov);

    /** @brief Allocate the once-per-link RX scratch + TX slot pool and its inline payload
     *         block (nothrow), at the sizes the constructor resolved. RX failure is
     *         survivable (per-frame nothrow buffer); a link with no TX pool drops every
     *         send on the counted path — see @ref enqueue_drops. The pool's two
     *         allocations succeed or fail TOGETHER: a slot with no payload behind it is
     *         not a claimable slot. */
    void alloc_buffers();
    /**
     * @brief Claim a free TX work slot lock-free (a CAS scan); nullptr when the pool is
     *        exhausted, absent, or the link is stopping.
     *
     * @param in_call True when the caller runs ON the httpd task (a reply serviced in-call),
     *                which is the ONE claimer allowed the reserve BEYOND the pool
     *                (@ref tx_reply_reserve) — it cannot wait for a drain it is itself
     *                supposed to perform, and it reaches for that reserve first so the
     *                pool's full depth stays available to producers (#1218). See the
     *                definition for why the reserve exists.
     */
    [[nodiscard]] tx_slot_t* claim_tx_slot(bool in_call);

    /**
     * @brief Claim a TX work slot, WAITING for the httpd task to free one when the pool is
     *        full and this task is not the httpd task; nullptr once the wait bound expires.
     *
     * The #1187 fix. The pool is an in-flight bound, and a producer that walks a peer set
     * without ever leaving the CPU can only ever fill it once: on a unicore target the whole
     * fan-out is posted before the httpd task runs at all, so exactly the first
     * @ref tx_slot_capacity destinations landed every pass and the rest received nothing —
     * deterministic, publish-order-prefix loss with a silent tail. Sleeping in short turns
     * until a slot frees converts that into ADDED LATENCY for a wide pass, which is what a
     * bound is supposed to feel like.
     *
     * It buffers NOTHING: the frame waits in the caller's own memory, in the caller's own
     * call, and a wait that expires still drops and counts exactly as before (ADR-0081 §1 —
     * never park a frame in a library-owned queue). The wait is bounded by one slot
     * occupancy, and a whole fan-out pass by one such bound (the futility latch, @ref
     * tx_wait_futile_until_us_).
     */
    [[nodiscard]] tx_slot_t* claim_tx_slot_waiting();
    /** @brief Return a drained/failed work item: drop any oversize heap payload and
     *         recycle its pool slot. */
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
     * @brief Count a delivered inbound MESSAGE on @p slot (takes @ref peers_m_ — callers
     *        must not hold it), at reassembly-complete granularity.
     *
     * Message granularity, not WS-fragment granularity, is the whole point: it makes a
     * sending client's `tx_frames` directly comparable with this side's `rx_frames`.
     * Called BEFORE the delivery, never after — the delivery runs the app in-call and the
     * app may destroy this link (#814), after which the slot is gone.
     */
    void note_rx_message(session_t* slot, std::size_t bytes);

    /** @brief Count an inbound message discarded before delivery (reassembly cap or a
     *  failed nothrow growth) on @p slot — takes @ref peers_m_. */
    void note_rx_drop(session_t* slot);

    /**
     * @brief Count a queued frame that @ref tx_work skipped rather than attempted
     *        (takes @ref peers_m_ — callers must not hold it).
     *
     * A skip is deliberately NOT evidence for the consecutive-failure streak (see
     * @ref note_tx_result): the peer departed, or it was already condemned and the
     * verdict is in. It IS still a frame this connection never received, so it belongs
     * in the cumulative count — that count answers "did anything get lost toward this
     * peer?", which the streak (reset on every success) never could.
     *
     * @p to is the destination identity minted when the frame was ENQUEUED, and its
     * `(slot, gen)` pair must still match for the charge to land — the same test
     * @ref live_fd makes. A skip fires exactly when a peer just departed, which is
     * exactly when lwIP recycles its descriptor number and the slot is reclaimed, so
     * charging by fd alone — or by slot pointer alone — would hand the departed peer's
     * lost frame to whichever fresh session inherited it (#954).
     */
    void note_tx_skip(const session_ref_t& to);

    /**
     * @brief Apply this link's whole per-socket policy to a freshly-upgraded socket.
     *
     * All of it on the peer's own fd, at admission and nowhere else: `SO_SNDTIMEO` of
     * @ref send_timeout_ms, `TCP_NODELAY`, `SO_KEEPALIVE` with the idle/interval/count
     * tunables that make a peer vanishing without a FIN detectable at all (#957 — no way an
     * inbound session ends is a timer, and an LRU purge fires on socket exhaustion rather
     * than on idleness, so such a peer otherwise holds its slot and one unit of
     * `max_peers` forever), and a send
     * override so a SHORT write is seen. Nothing touches the server's configuration, so
     * REST sockets on the same instance keep their long bound and the owner's keepalive
     * setting — and, by the same token, this link does not depend on an adopted server
     * having enabled keepalive.
     */
    void bound_socket(int fd) const;

    /**
     * @brief Sample the httpd task's free-stack high-water mark and, ONCE, name a thin
     *        one (#955). Runs on the httpd task, at the session-claim edge only.
     *
     * The stand-in for a check of precondition 1 of the adopting ctor: nothing can read an
     * adopted server's configured `stack_size`, but a task's minimum-ever free stack is
     * readable. The mark is a running MINIMUM, so a sample taken at a claim already
     * reflects every deep delivery the task has served before it — one connection late,
     * which is the price of not paying an O(free-stack) scan per frame.
     *
     * It cannot prevent the overflow it is looking for; it converts the resulting
     * stack-protection reboot from an unrelated-looking flake into a named cause. Latched
     * after one report so a misconfigured node logs once rather than per connection —
     * and so the scan itself stops once its answer is known.
     */
    void check_httpd_stack();

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
     *
     * It judges the FRAME, not just the buffer (#951). `esp_http_server` writes one frame
     * as two calls to this function — the header, then the payload — so a write that puts
     * nothing on the wire loses a whole frame only when it is the frame's first. A failure
     * on the second leaves the peer holding a header promising bytes that never arrive,
     * which is the same unparseable stream a short write produces and is judged the same
     * way. The frame boundary comes from @ref tx_work, which brackets the send.
     */
    static int send_guarded(httpd_handle_t handle, int fd, const char* buf, std::size_t len,
                            int flags);

    /**
     * @brief Handle a detected stream desynchronisation on @p slot's socket: log it and
     *        close that session immediately, bypassing the streak (takes @ref peers_m_).
     *
     * Takes the SLOT, not the fd. @ref send_guarded is inside the write when it calls
     * this, on the httpd task, so the server's own session table is authoritative about
     * who owns that descriptor at that instant and hands the slot over directly — no
     * generation check is needed here, and no fd-keyed rescan either (#954).
     *
     * @param cause    Which shape it was, for the log: a short write, or a frame truncated
     *                 after its header reached the wire (#951). One verdict, two causes.
     * @param on_wire  Bytes of the frame the socket accepted before the failure.
     * @param lost     Bytes of it that never left, and never will.
     */
    void note_send_desync(session_t* slot, const char* cause, std::size_t on_wire,
                          std::size_t lost);

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

    /** @brief Take a slot out of service without freeing it: clear its fd/open and cut its
     *         resolution handle loose, so a callback or a directed send that outlives the
     *         link lands on valid, inert memory. Caller holds @ref peers_m_. Non-static
     *         since #1013 — the handle it cuts loose belongs to the LINK's pool, not to
     *         the slot. */
    void neutralise(session_t* slot);

    /**
     * @brief The server handle. ATOMIC because the TX path is documented as safe to run
     *        past the destructor (@ref claim_tx_slot's teardown gate) and the destructor
     *        writes this while a producer task may be reading it — a data race, and UB by
     *        the memory model, on every build until #963. Relaxed: this orders nothing,
     *        it only makes the concurrent access defined.
     */
    std::atomic<httpd_handle_t> handle_{nullptr};  // nullptr => the instance never started
    std::uint16_t port_;
    std::size_t max_peers_;
    /** @brief Opening-handshake admission predicate + its opaque ctx; null admits every
     *         peer (the default). Read on the httpd task, from @ref ws_pre_handshake —
     *         see @ref set_admission_cb. */
    admission_fn_t admission_fn_ = nullptr;
    /** @brief The three-valued form of the same predicate; at most one of the two is ever
     *         non-null (@ref set_admission_cb clears the other). */
    admission_verdict_fn_t admission_verdict_fn_ = nullptr;
    void* admission_ctx_ = nullptr;
    /**
     * @brief One socket that passed admission, whose entitlement is NOT YET KNOWN, and which
     *        has not yet claimed a session — the row of the pending-handshake ledger.
     *
     * One fact about that gap: when this un-spoken socket stops being worth holding (#1247).
     * It used to carry a second — the handshake's verdict (#1245) — and no longer does, because
     * a session whose verdict is already ADMIT_AUTHENTICATED never enters the ledger at all: it
     * claims its slot at the 101 (#1334, @ref claim_session).
     */
    struct pending_handshake_t {
        int fd = -1; /**< @brief The socket the 101 was written on. */
        /** @brief When @ref sweep_auth_deadlines must close it if it has still said nothing —
         *         `esp_timer_get_time()` at the pre-handshake plus `auth_deadline_us_`. */
        std::int64_t deadline_us = 0;
    };
    /**
     * @brief Sockets whose entitlement is NOT YET KNOWN and which have not yet claimed a
     *        session. Entries `[0, pending_n_)` are live.
     *
     * Written in @ref ws_pre_handshake, consumed at the lazy claim in @ref on_data_frame and
     * reaped by @ref sweep_auth_deadlines — all three on the httpd task, under @ref peers_m_.
     * FIXED-SIZE (@ref kMaxPendingHandshakes) on purpose: this is a handoff between two points
     * of the SAME connection's setup, not a table.
     *
     * WHAT IS NOT IN IT (#1334). A handshake answered @ref
     * admission_verdict_t::ADMIT_AUTHENTICATED is not a question awaiting an answer — the answer
     * arrived with the opening GET — so such a socket claims its session immediately and is
     * bounded by nothing here. Carrying it in the ledger was the #1334 defect: the row's only
     * retirement paths are the first-frame claim and the deadline sweep, and a peer entitled to
     * be silent takes neither, so the sweep closed a valid authenticated session at the deadline
     * and 24 idle pre-authenticated links would have filled the ledger and refused every further
     * upgrade. Both follow from the row existing at all; the fix is that it does not.
     *
     * IT IS ALSO THE ONLY THING THAT SEES A SILENT UPGRADE. `esp_http_server` answers the
     * handshake internally and this link claims a session lazily, so a peer that completes the
     * 101 and then sends NOTHING has no `session_t` and is invisible to a sweep of `slots_` —
     * the defect #1247 reported, against a reference text that named "open sockets, say
     * nothing" as the attack the deadline stops. The row is stamped here, where the socket is
     * first known, so the deadline bounds the socket rather than the session.
     *
     * POPULATED ONLY WHEN AN AUTH HOOK IS INSTALLED. Without one there is no deadline to
     * enforce and a silent upgraded socket is the HTTP server's business exactly as before —
     * which is also what keeps the overflow refusal below unreachable on such a link.
     *
     * A FULL LEDGER REFUSES THE HANDSHAKE; see @ref kMaxPendingHandshakes for why that, and
     * not an assert on a size argued to be unreachable.
     */
    std::array<pending_handshake_t, kMaxPendingHandshakes> pending_{};
    /** @brief How many entries of @ref pending_ are live. */
    std::size_t pending_n_ = 0;
    /** @brief Post-handshake auth hook + its opaque ctx; null serves every session at once
     *         (the default) — see @ref set_auth_cb. Read on the httpd task, unlocked. */
    auth_fn_t auth_fn_ = nullptr;
    void* auth_ctx_ = nullptr;
    /** @brief The unauthenticated-session bound in MICROseconds, resolved from the ctor's
     *         milliseconds once (0 there => @ref kDefaultAuthDeadlineMs). */
    std::int64_t auth_deadline_us_ = 0;
    std::atomic<std::uint32_t> auth_rejected_{0};
    std::atomic<std::uint32_t> auth_expired_{0};
    /**
     * @brief The periodic `esp_timer` that fires the deadline sweep, as an opaque pointer so
     *        this header names no `esp_timer` type; null when auth is not configured.
     *
     * Created by the first @ref set_auth_cb that installs a hook and deleted at the head of
     * the destructor, BEFORE @ref close_gate — so the join in close_gate is what proves no
     * sweep is still inside this link. Its callback touches the GATE and nothing else of
     * ours, for the same reason every other latched callback does.
     */
    void* auth_timer_ = nullptr;
    /**
     * @brief `esp_timer` callback (timer task): ask the httpd task to run a sweep.
     *
     * @p arg is the GATE, never the link — the timer may fire while a destructor is running
     * and the gate is the object designed to survive that. It does no work of its own beyond
     * one @ref httpd_queue_work; a refused enqueue is simply this tick skipped, and the next
     * one covers the same sessions (a deadline is a floor on how long a squatter lives, not
     * a promise about the instant it dies).
     */
    static void auth_timer_fired(void* arg);
    /** @brief `httpd_queue_work` fn: run @ref sweep_auth_deadlines on the httpd task,
     *         resolving the link through the gate @p arg. */
    static void auth_sweep_work(void* arg);
    /**
     * @brief Close every session, and every un-spoken upgraded SOCKET, whose authentication
     *        deadline has passed (httpd task only).
     *
     * TWO SETS, because the resource is claimed lazily and the scarce one is the socket
     * (#1247). `slots_` holds the peers that spoke and then stalled; @ref pending_ holds the
     * ones that completed the 101 and said nothing, which have no `session_t` to walk and were
     * therefore bounded by nothing at all. A pending row is closed on the raw descriptor —
     * there is no slot to condemn — after the server's own `httpd_ws_get_fd_info` confirms a
     * websocket still lives there, so a row left behind by a socket that hung up cannot shut
     * down whoever was accepted onto the recycled number.
     *
     * Snapshots the expired sessions under @ref peers_m_ and closes them with the lock
     * RELEASED, because @ref close_session takes it again — the same shape every other
     * close path here uses.
     */
    void sweep_auth_deadlines();
    /**
     * @brief Close every upgraded socket that has said NOTHING past its deadline (httpd task
     *        only; @ref peers_m_ must NOT be held). @p now is @ref sweep_auth_deadlines' clock
     *        reading, shared so both halves of one sweep judge against one instant.
     *
     * Takes @ref peers_m_ per row and RELEASES it around the close, the same shape the session
     * half uses. Each row is retired before its close runs, so a tick that overlaps the next
     * cannot sweep the same descriptor twice.
     */
    void sweep_pending_handshakes(std::int64_t now);
    /**
     * @brief Hand one whole message from an unauthenticated session to the auth hook and act
     *        on the verdict (httpd task only; @ref peers_m_ must NOT be held).
     *
     * The hook runs with no lock of this link held: it is foreign code of unbounded duration
     * and the mutex it would otherwise sit under is the one the send path needs.
     */
    void on_auth_message(session_t* slot, std::span<const std::byte> body);
    /**
     * @brief Open @p fd's row in the pending-handshake ledger: the deadline by which this
     *        socket must have spoken (httpd task; takes @ref peers_m_).
     *
     * Any stale row for the same descriptor is REPLACED rather than duplicated: a descriptor
     * is reused the moment the kernel frees it, and a leftover from a handshake whose peer
     * never spoke must not charge whoever lands on that number next the previous connection's
     * half-elapsed deadline.
     *
     * @return false when the ledger is FULL — the refusal the caller turns into `ESP_FAIL`,
     *         abandoning the upgrade. Answered by value rather than asserted; see @ref
     *         kMaxPendingHandshakes.
     */
    [[nodiscard]] bool note_pending_handshake(int fd);
    /**
     * @brief Consume @p fd's ledger row, if it has one (httpd task; @ref peers_m_ must
     *        ALREADY be held — this runs inside the claim's critical section).
     *
     * Consuming rather than reading is what keeps the ledger a handoff: a row cannot outlive
     * the claim it was written for, and a claimed session is bounded by its own @ref session_t
     * deadline from here on. It answers nothing, because since #1334 the row carries nothing to
     * answer — a pre-authenticated session never opened one.
     */
    void take_pending_handshake(int fd) noexcept;
    /**
     * @brief Drop @p fd's ledger row without answering its verdict (httpd task; @ref peers_m_
     *        must ALREADY be held). Used by the sweep, which has already decided to close it.
     */
    void drop_pending_handshake(std::size_t at) noexcept;
    /**
     * @brief Send @p payload to @p slot's socket RIGHT NOW rather than through the work
     *        queue (httpd task only; @ref peers_m_ must NOT be held).
     *
     * The auth exchange was the first send path that is already ON the httpd task and
     * already holds its session, so it needs neither the pool nor `httpd_queue_work` — the
     * queue exists to marshal sends from OTHER tasks onto this one. Since #1494 the
     * directed reply travels the same way (@ref send_in_call). Bracketed like @ref tx_work's
     * send so a short write is judged by the same rule (@ref send_guarded).
     *
     * @param on_wire Optional: receives the bracket's byte count for this frame. Non-zero
     *                after a failure is the DESYNC (#951) — the frame was announced and cut
     *                off, and the session has already been condemned inside the write — so
     *                a caller that logs an ordinary drop must not also log that one. The
     *                teardown callers pass nullptr: they are closing the session anyway.
     */
    esp_err_t send_now(session_t* slot, int fd, int ws_type, std::span<const std::byte> payload,
                       std::size_t* on_wire = nullptr);
    /**
     * @brief Send a WebSocket CLOSE with @p code and tear the session down (httpd task only;
     *        @ref peers_m_ must NOT be held).
     *
     * Order matters and is the whole reason this is not just @ref condemn: `condemn` is
     * `shutdown`, after which every write on the socket fails at once, so a close code
     * written afterwards never reaches the peer. The frame goes first, the shutdown second.
     */
    void close_session(session_t* slot, std::uint16_t code);
    /**
     * @brief Re-validate a close-work item's `(slot, gen)` identity and, if the named
     *        session is still the one holding the slot, run @ref close_session on it
     *        with @ref kCloseRevoked (httpd task only; @ref peers_m_ must NOT be held).
     *
     * The identity test is @ref live_fd's, made here rather than trusted from enqueue
     * time: the item drained arbitrarily later, and everything that can invalidate the
     * reference — the peer departing, the slot's reclaim, a new claim — runs on this
     * same task, so a reference that passes cannot go stale between the test and the
     * close. A reference that fails means the named session is already gone, which is
     * the outcome the caller asked for; nothing touches its successor (#954).
     */
    void close_ref(const session_ref_t& to);
    /** @brief The per-socket send bound applied at admission — see @ref send_timeout_ms. */
    std::uint32_t send_timeout_ms_ = 0;
    /** @brief Frames that never reached a socket, for this link's life — see @ref
     *         enqueue_drops. Relaxed: a diagnostic count, ordered against nothing. */
    std::atomic<std::uint32_t> enqueue_drops_{0};
    /**
     * @brief The rest of @ref stats_t. Relaxed like @ref enqueue_drops_ and for the same
     *        reason: each is a diagnostic tally ordered against nothing, and none of them
     *        sits on the per-frame path — every one is bumped on a refusal, a teardown, or
     *        a drop, so the steady-state send and receive legs are untouched (#953).
     */
    /** @brief Count+log an over-cap inbound frame, OUT OF LINE — see the definition for
     *         why the per-frame path must not carry a log call site (#994's lesson). */
    void note_rx_oversize(std::size_t len);
    /** @brief Count+log an RX payload allocation failure, out of line. */
    void note_rx_alloc_fail(std::size_t len);
    /** @brief Log a reassembly that would pass the cap, out of line. */
    void note_reassembly_over_cap(std::size_t had, std::size_t adding);
    std::atomic<std::uint32_t> tx_pool_misses_{0};
    std::atomic<std::uint32_t> tx_pool_waits_{0};
    /**
     * @brief `esp_timer_get_time()` value until which a full pool is taken as-is rather
     *        than waited on — the futility latch (#1187).
     *
     * Set when a wait expires without a slot freeing, i.e. when the httpd task has not
     * drained one send occupancy's worth. The wait bound is per SEND and a fan-out asks per
     * DESTINATION, so without this a wide sweep against a wedged task would park its
     * producer for one bound per edge; with it, the first expiry answers for the rest of the
     * pass and the frames go back to being dropped and counted immediately. Relaxed: it
     * orders nothing and a stale read costs at most one extra wait.
     */
    std::atomic<std::int64_t> tx_wait_futile_until_us_{0};
    std::atomic<std::uint32_t> tx_to_dead_peer_{0};
    std::atomic<std::uint32_t> peers_refused_{0};
    std::atomic<std::uint32_t> sessions_condemned_{0};
    std::atomic<std::uint32_t> rx_dropped_oversize_{0};
    std::atomic<std::uint32_t> rx_dropped_alloc_{0};
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
    /**
     * @brief The resolution-handle pool `peer_link` hands out of (#1013) — grown on
     *        demand, never shrunk, and recycled through @ref free_resolutions_.
     *
     * One entry per LIVE resolution plus @ref kResolutionSpare in quarantine, so the pool
     * is bounded by the socket budget and not by traffic: a handle is per RESOLUTION, but
     * a second resolution of the same live session reuses the first's, and every handle
     * comes back when its session ends. The entries are separately allocated for the
     * reason the slot shells are — the teardown that cannot retire the server's callbacks
     * LEAKS them rather than freeing memory a queued send may still address.
     */
    std::vector<std::unique_ptr<peer_resolution_t>> resolutions_;
    /** @brief Retired handles, oldest first: `peer_link` recycles from the HEAD and
     *         @ref retire_resolution appends at the TAIL, so a handle just retired is the
     *         LAST one handed back out (see @ref kResolutionSpare). */
    peer_resolution_t* free_resolutions_ = nullptr;
    peer_resolution_t* free_resolutions_tail_ = nullptr; /**< @brief Its tail (peers_m_). */
    std::size_t free_resolutions_n_ = 0;                 /**< @brief Its length (peers_m_). */
    /**
     * @brief Retired handles kept in quarantine before any is recycled.
     *
     * A recycled handle is restamped for a DIFFERENT session, so a caller still holding
     * it would be back where #1013 started. The quarantine is what makes that unreachable
     * in practice rather than merely unlikely: `peer_link` grows the pool until this many
     * retired handles are queued ahead of the one it would reuse, so a caller would have
     * to be suspended between its resolve and its send across this many further session
     * departures on the whole link — the contract on `bus_link_t::peer_link` is resolve
     * per use, and one forward hop is what sits between the two.
     */
    static constexpr std::size_t kResolutionSpare = 4;
    /** @brief Once-allocated RX scratch (httpd-task-only, so lock-free by construction):
     *         a frame that fits reads here instead of a per-frame allocation. */
    std::unique_ptr<std::byte[]> rx_scratch_;
    /** @brief Set once @ref check_httpd_stack has reported a thin stack — it then stops
     *         sampling. Plain `bool`: written and read only on the httpd task, like
     *         @ref rx_scratch_ and the reassembly buffer. */
    bool stack_named_ = false;
    /** @brief Effective RX scratch capacity — the ctor's `rx_scratch_bytes` resolved
     *         against @ref kDefaultRxScratchBytes, and zeroed if the allocation failed so
     *         the size and the pointer can never disagree. */
    std::size_t rx_scratch_bytes_ = 0;
    /** @brief Once-allocated TX work-slot pool: claimed lock-free by sending tasks,
     *         released by the httpd task as each send drains. */
    std::unique_ptr<tx_slot_t[]> tx_pool_;
    /**
     * @brief The slots' inline payload storage, one flat block of
     *        `tx_slots_total_ * tx_inline_bytes_`.
     *
     * Separate from @ref tx_pool_ because the inline capacity is a CONSTRUCTOR argument
     * since #1160 and a C++ array member cannot carry a runtime extent. One block rather
     * than one per slot: the pool is allocated once per link and never grows, so a single
     * allocation is both fewer heap headers and the shape that lets the teardown's
     * leak-instead-of-free arm abandon the slots and their payloads together.
     */
    std::unique_ptr<std::byte[]> tx_inline_;
    /** @brief Effective inline capacity of one slot — the ctor's `tx_inline_bytes`. */
    std::size_t tx_inline_bytes_ = 0;
    /** @brief Effective sender-claimable pool depth — the ctor's `tx_pool_slots`, what
     *         @ref tx_slot_capacity reports. */
    std::size_t tx_pool_slots_ = 0;
    /** @brief Slots ALLOCATED: @ref tx_pool_slots_ plus the in-call reserve. Cached
     *         because every pool sweep walks it. */
    std::size_t tx_slots_total_ = 0;
    /**
     * @brief The handler-admission gate registered as the URI's `user_ctx` — every
     *        dispatch resolves this link through it (see @ref close_gate).
     *
     * A raw pointer, and deliberately so: in adopted mode it must OUTLIVE this link,
     * because the adopted server keeps routing to it until each latched session is
     * deleted and no API of ours can force that. The destructor frees it only when it
     * can prove nothing still holds it (owning mode, after `httpd_stop`).
     */
    /** @brief The latched-callback gate. ATOMIC for the reason on @ref handle_ — the
     *         destructor stores nullptr here while a producer may be reading it (#963). */
    std::atomic<gate_t*> gate_{nullptr};
};

}  // namespace tr::net
