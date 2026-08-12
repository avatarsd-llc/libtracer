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
 * the linux virtual board (glibc sockets); the two are selected by the build.
 *
 * Since #947 the selection is EXCLUSIVE and nothing rests on `--gc-sections`: on a
 * chip target `core/src/transport_ws.cpp` is not compiled, so this link is the only
 * `ws` dialer there. Relying on the collector never worked anyway — the built-in
 * transport catalog REGISTERED the portable factory, which kept 46 of its symbols
 * reachable in a measured chip image. Excluding the TU is the fix, because on lwIP
 * the portable pair is not just unreachable but unusable: its scatter-gather egress
 * asks `sendmsg` for `MSG_NOSIGNAL`, `lwip_sendmsg` rejects it with `EOPNOTSUPP`,
 * and every data frame is dropped in silence (#948).
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
 *   - EVERY DIAL INPUT IS SETTLED BEFORE THE RECV THREAD EXISTS. The ctor spawns a thread
 *     that dials as soon as it is armed (immediately, unless `defer_recv` holds the first
 *     dial for @ref start_receiving — see below), so this type has NO
 *     post-construction dial-configuration
 *     surface: host, port, `ws_path` and the handshake headers are all constructor
 *     arguments and all `const` thereafter — read by the recv thread, written by nobody.
 *     (The rx/tx buffers are the same rule in a weaker spelling: sized once in the ctor and
 *     never resized. The vectors are not `const` because their BYTES are scratch, but no
 *     one may make their extent settable after the spawn either.)
 *     It used to have one such surface — `set_handshake_headers()` assigned an
 *     unsynchronized `std::string` that `connect_once()` was already reading (#959). That
 *     assignment is concurrent with the read, so it is a data race: a reallocating
 *     assignment can hand `esp_transport_ws_set_config` a `cfg.headers` that is already
 *     freed. And nothing in the API decided which side won, so whether the FIRST dial
 *     carried the token was UNDEFINED — a dial without one is refused by a peer that gates
 *     admission, costing a `kReconnectBackoffMs` flap before the next dial, which is what
 *     made first-dial liveness nondeterministic by construction. (Which side actually wins
 *     is a scheduler property, not an API one. It was never measured on chip; on the host
 *     fake, a rebuilt pre-fix ordering had the write win 20 of 20 measured runs — on a
 *     reconstruction whose write-to-read window is NARROWER than the setter it stands in
 *     for, so read that rate as an upper bound; the scope note in
 *     `esp_ws_client_dial_test.cpp`'s file header says what it is and is not evidence for.
 *     So a tokenless first dial is what the old surface PERMITTED rather than an observed
 *     behaviour.) "Set it before the link first dials"
 *     was not a contract the API could express, only one it could ask for; a ctor argument
 *     makes it structural. A knob that must be applied before the thread starts belongs in
 *     the ctor, which is the same rule that made `recv_stack` reach `esp_pthread_set_cfg`
 *     (#900) rather than be discarded after the spawn.
 *   - THE PRE-SINK WINDOW IS CLOSED BY NOT DIALLING (ADR-0081, #1102). The receiver sink
 *     is installed by the owner AFTER construction (`provide_link` → the creating write →
 *     `fwd_router_t::add_child`), and our own connect is what provokes the peer's
 *     push-on-connect — so a link that dials from its ctor can deliver a message into an
 *     empty `receiver_slot_t`, a silent loss with no counter moving. Constructed with
 *     `defer_recv`, the recv thread parks BEFORE its first dial until
 *     @ref start_receiving releases it: nothing can arrive before the sink exists because
 *     nothing is connected, which is ADR-0081's "hold in the transport's native window"
 *     stated for a link whose window IS its connection. The latch is ONE-SHOT,
 *     deliberately: every re-dial happens on a link that was already armed, the router
 *     keeps the receiver installed across a `link_down`, so a latch that re-closed per
 *     connection would hold bytes against a sink that is already standing. Default off —
 *     the historical dial-at-once contract — because there is no `ws` factory on a chip
 *     target to arm it: the embedder that stages the link with `provide_link` is the one
 *     that must opt in (and then MUST issue the creating write, or arm the link itself —
 *     see the ctor's `defer_recv` note).
 *   - A single recv thread (spawned in the ctor) owns the read side: once armed (see
 *     above; immediately unless `defer_recv`) it dials
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
 *     positive poll is prompt; the WRITE is not — IDF spends the caller's timeout on
 *     up to three legs inside one `esp_transport_write` — so that mutex IS held
 *     blocking, for a bound derived from the task-watchdog period rather than the
 *     4 s literal it used to be (#952: 3 x 4 s against a 5 s watchdog). The reads run
 *     otherwise lock-free on the one recv thread; the graph/router lock-stripes
 *     below are unchanged — this link adds no lock beyond that syscall serializer.
 *   - Teardown is a BARRIER, not an assumption. The destructor disarms the link
 *     (`stop_`), wakes the recv thread's backoff, joins it, then destroys the handles
 *     UNDER the same mutex a sender holds across its write, and finally waits out
 *     every sender that had already ANNOUNCED itself on the in-flight tally — raised at
 *     the top of `send()`, before it queues on that mutex. Before #952 it destroyed the
 *     handles with that mutex held nowhere, on the false premise that the joined recv
 *     thread was the only handle user — so a sender queued behind a stalled write woke
 *     up owning a destroyed handle. The tally, not `send()`'s first instruction, is the
 *     covered boundary, and it is drawn there because no barrier inside an object can
 *     draw it earlier: a caller that has not reached the tally when the destructor reads
 *     it for the last time is NOT waited out — whether it is a `send()` that STARTS
 *     after the destructor returns, or one that entered `send()` and is still short of
 *     the tally (a few instructions). The embedder owns the link's lifetime against its
 *     own callers; this type bounds what happens once a caller is on the tally.
 *   - The handles themselves are published by `connected_`, not by the mutex: the recv
 *     thread rebuilds `ws_`/`tcp_` on every re-dial holding NO lock, and releases them
 *     with the `connected_` store. So a handle is only ever read behind an ACQUIRE that
 *     observed `true` — a check ahead of that gate races the rebuild, whatever lock it
 *     holds.
 *   - The DEPARTURE SEAM is reported (#957). The recv loop's "not connected" arm is
 *     downstream of all three sites that clear `connected_` — `drop()` (peer CLOSE, a
 *     poll error, a read error), `send()`'s failed/short-write arm, and the destructor —
 *     so it fires `notify_down()` once per connection that was up, with no transport lock
 *     held, before it re-dials. A LOCAL teardown reports nothing (core's `stop_` rule,
 *     and the third of those three sites). It used to fire from nowhere, on
 *     the premise that a blip's subscriber edges should survive the reconnect; that
 *     premise only holds for a blip the far side also survives, and the link cannot tell
 *     one from a peer that rebooted and forgot every subscription and label it issued.
 *     So a reconnect is a NEW session to the routing plane: the router evicts this
 *     child's edges and label bindings and both sides re-establish, instead of this node
 *     producing into a session that no longer exists.
 *   - The link also has to NOTICE a peer that vanishes without a FIN (#957). Nothing in
 *     the loop does: `esp_transport_poll_read` reports "no data" forever, so `ok()` would
 *     answer true for a dead peer indefinitely on an idle link. Every dialed socket
 *     therefore gets `SO_KEEPALIVE` plus the idle/interval/count tunables ESP-IDF
 *     documents for `esp_http_server`'s own keepalive — the same three the server sibling
 *     applies to accepted sockets — so a vanished peer surfaces as an ordinary poll/read
 *     error within tens of seconds and takes the drop path above.
 *   - `send()` COPIES the frame into a reusable tx scratch before writing, because
 *     `esp_transport_write` masks IN-PLACE (RFC 6455 client rule) and a delivered
 *     frame may be shared with the concurrent server link — so the caller's (const,
 *     possibly shared) bytes must not be transiently mutated. RX is zero-copy; the
 *     one TX copy is the irreducible cost of client masking on shared bytes.
 *   - NO CALLBACK INTO THE EMBEDDER EVER RUNS UNDER `write_m_` or `st_m_`. Deliveries
 *     are made with both released; the counter bumps and @ref stats only touch this
 *     object's own fields. That is the invariant a host relies on when it holds its
 *     own lock across a `stats()` call — it keeps the lock order acyclic, so keep
 *     any future work under either mutex strictly local.
 *   - The counters live under their OWN mutex (`st_m_`), not the write serializer.
 *     `write_m_` is held across a transport write for up to `kWriteTimeoutMs`, so
 *     `stats()` under it would block for seconds on a stalled peer and recv delivery
 *     would queue behind slow sends. `st_m_` is only ever held for a bump or a copy,
 *     which is what makes `stats()` safe to call from a periodic task.
 *
 * NO per-frame heap: the rx/tx buffers are allocated ONCE at construction (bounded,
 * tunable); steady-state send/recv touch neither the global heap nor a per-frame
 * allocation. One recv thread per dialed peer (each blocks on its own connection),
 * mirroring `transport_ws_client`.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "esp_transport.h"
#include "libtracer/transport.hpp"
#include "libtracer_esp/link_stats.hpp"

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
     * @brief Start the recv thread, which dials `ws://<host>:<port><ws_path>` — at once
     *        by default, or only after @ref start_receiving when `defer_recv` is set.
     *        The ctor does NOT block on the connection (the dial runs on the recv
     *        thread, with reconnect), so a slow/unreachable peer never stalls the
     *        caller — confirm readiness with @ref ok.
     *
     * @param host     The peer's IPv4 dotted-quad or hostname (the graph plane's WS host).
     * @param port     The peer's TCP port (its :80 esp_http_server, for the /ws mount).
     * @param ws_path  The WS URI to request in the handshake (default "/ws", matching
     *                 the httpd_ws_link_t server mount).
     * @param handshake_headers Extra HTTP header lines appended to the opening-handshake
     *                 request, each `Name: value\r\n`-terminated (`esp_transport_ws` emits
     *                 them verbatim); empty leaves the field null, so the handshake is
     *                 byte-for-byte the historical one. The client counterpart to
     *                 @ref httpd_ws_link_t::set_admission_cb: a board-to-board dial carries
     *                 no browser session cookie, so a token header here is how a dialing
     *                 node authenticates itself to a peer whose graph WS gates admission.
     *                 It is a CONSTRUCTOR argument and not a setter because the recv thread
     *                 dials as soon as it exists — see the threading note (#959). Applied
     *                 to the first dial and to every re-dial.
     * @param rx_bytes Reusable receive-buffer size (one inbound message must fit); our
     *                 control TLVs are small, so the default is modest.
     * @param tx_bytes Reusable send-scratch size (one outbound frame must fit). An
     *                 outbound frame larger than this is DROPPED whole — counted on
     *                 `stats().c.tx_drops` and LOGGED with both sizes (#959). The log is
     *                 what makes the ceiling nameable: the same `tx_drops` is bumped when
     *                 the peer is down and on a short write, so the counter alone does not
     *                 say which drop happened, let alone which knob was too small.
     * @param recv_stack Recv-thread stack in bytes, HONORED (#900): the recv thread runs
     *                   in-call delivery through the graph's on_write seam, which is the
     *                   deep path, so a node that knows its delivery depth sizes it here.
     *                   0 = the platform pthread default.
     * @param defer_recv Hold the FIRST dial until @ref start_receiving (ADR-0081's
     *                   defer-the-dial arm, #1102). The recv thread is spawned but parks
     *                   before its first connect, so a peer's push-on-connect cannot land
     *                   before the owner has installed the receiver sink — the recipe is
     *                   construct with this set, `provide_link`, issue the creating write
     *                   (whose `make_connection` installs the sink and then arms every
     *                   link unconditionally). The COST is deliberate and documented: an
     *                   unarmed link never dials, so @ref ok stays false and the
     *                   reconnect/keepalive/departure machinery is all off until armed —
     *                   an embedder that sets this and never arms the link has a link
     *                   that never connects. Default false, the historical dial-at-once
     *                   contract: there is no `ws` factory on a chip target to pass this
     *                   flag (the `tcp`/core-`ws` factories pass their `defer_recv`
     *                   themselves), so opting in is the embedder's move.
     */
    explicit esp_ws_client_link_t(std::string host, std::uint16_t port, std::string ws_path = "/ws",
                                  std::string handshake_headers = {}, std::size_t rx_bytes = 2048,
                                  std::size_t tx_bytes = 2048, std::size_t recv_stack = 0,
                                  bool defer_recv = false);

    /**
     * @brief Stop the recv thread, then close + destroy the esp_transport handles.
     *
     * Bounded and ordered (#952): the link is disarmed first, the recv thread's
     * reconnect backoff is woken rather than waited out, the handles are destroyed
     * under the send serializer, and every sender that has ANNOUNCED itself on the
     * in-flight tally is waited out before this object's own members go. The tally is
     * raised at the top of `send()`, before it queues on that serializer — so what is
     * guaranteed is "a caller on the tally finishes first", NOT "a caller that has
     * entered `send()` finishes first": a caller between `send()`'s entry checks and
     * that raise is not waited out, exactly as a `send()` starting after this
     * destructor returns is not. Both are the embedder's lifetime problem; nothing
     * inside the object can close them. A teardown that lands mid-dial still costs one
     * dial timeout — `esp_transport_connect` takes no cancellation — which is why that
     * timeout is derived from the task-watchdog period.
     */
    ~esp_ws_client_link_t() override;

    esp_ws_client_link_t(const esp_ws_client_link_t&) = delete;
    esp_ws_client_link_t& operator=(const esp_ws_client_link_t&) = delete;

    /**
     * @brief Send @p frame as one masked BINARY WebSocket message to the peer. Drops
     *        (best-effort, like the UDP path) when not currently connected.
     *
     * Blocking, and BOUNDED: a peer whose TCP window has closed costs the calling task
     * one write budget (a quarter of the task-watchdog period, split across the three
     * legs IDF spends the timeout on) and then drops the frame, tearing the connection
     * down for the recv thread to re-dial. It used to cost up to three times a 4 s
     * literal, which is a watchdog panic rather than a dropped frame (#952).
     */
    void send(std::span<const std::byte> frame) override;

    /**
     * @brief Open this link's delivery gate: release the `defer_recv` dial latch, so the
     *        recv thread makes its first dial (ADR-0081, #1102).
     *
     * The second phase of the two-phase bring-up the base contract describes
     * (`transport_t::start_receiving`): construct with `defer_recv`, install the sinks,
     * then call this. IDEMPOTENT and SAFE ON A LINK THAT NEVER CONNECTED —
     * `transport_vertex_t::make_connection` arms every link unconditionally once the
     * receiver is installed, and on a link constructed without `defer_recv` (or armed
     * already) this finds the latch set and does nothing. ONE-SHOT: a reconnect never
     * re-closes the gate, because every re-dial happens on a link that was already armed
     * and the router keeps the receiver installed across a `link_down`.
     */
    void start_receiving() override;

    /** @brief Span delivery: the router services each inbound frame in-call, so no
     *         frame outlives its callback. */
    [[nodiscard]] bool delivers_ropes() const override { return false; }

    /** @brief Point-to-point link — no bus/peer-enumeration facet. */
    [[nodiscard]] bus_link_t* bus() override { return nullptr; }

    /**
     * @brief True once the opening handshake has completed (and while connected).
     *
     * A POLL, not a notification — and bounded in staleness only by what the stack can
     * detect: a peer that vanishes without a FIN is noticed by the keepalive probes the
     * dial arms (#957), so this answers `true` for a dead peer for at most that window.
     * A layer that needs the transition rather than the state takes the departure seam
     * (`transport_t::set_down_notifier`), which this link now fires on every death path.
     */
    [[nodiscard]] bool ok() const noexcept { return connected_.load(std::memory_order_acquire); }

    /**
     * @brief Inbound MESSAGES dropped by the receive path since construction — the
     *        `dropped_rx()` convention core's transports already spell this way.
     *
     * Two causes, both of which used to be logs-only (#953): a message that does not fit
     * `rx_bytes` (dropped whole, never truncated — its remaining fragments are consumed
     * and discarded too, #901), and a stray CONTINUATION arriving with no message open.
     * A frame lost to a connection drop is NOT counted: that is the link going down, not
     * the receive path refusing a message.
     */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept {
        return dropped_rx_.load(std::memory_order_relaxed);
    }

    /**
     * @brief One coherent snapshot of this link's passive counters plus the two
     *        dial facts only the link can know.
     *
     * `reconnects` counts COMPLETED handshakes since construction (the first dial
     * included), which is the only externally-observable signal that a transient
     * drop happened at all: @ref drop deliberately does NOT fire `notify_down`, so
     * the routing plane keeps the peer's subscriber edges across a blip and nothing
     * upstream ever sees the transition. That suppression is load-bearing and stays;
     * the counter is the observational replacement for it.
     *
     * `connect_ms` is the duration of the LAST successful `esp_transport_connect`,
     * i.e. the TCP connect plus the RFC 6455 opening handshake. It is an UPPER BOUND
     * on roughly two round-trips, sampled only at (re)dial — it can be days stale and
     * it is NOT an RTT measurement. A host surfacing it should label it "handshake".
     *
     * `c.rx_drops` is NOT a second tally: it is read straight from @ref dropped_rx, so
     * this link has exactly one inbound-drop truth and both spellings always agree.
     */
    struct stats_t {
        link_counters_t c;            /**< @brief Traffic counters (see link_stats.hpp). */
        std::uint32_t reconnects = 0; /**< @brief Completed handshakes since construction. */
        std::uint32_t connect_ms = 0; /**< @brief Last handshake duration, ms (0 = never). */
        bool up = false;              /**< @brief @ref ok at the instant of the snapshot. */
    };

    /**
     * @brief Copy out @ref stats_t under a brief hold of the counter mutex.
     *
     * Callable from ANY task. It takes `st_m_` only for the struct copy — no syscall, no
     * allocation, and it NEVER calls back into the embedder, which is what lets a host
     * hold its own lock across this call without introducing a cycle (a downstream
     * publisher holds its own dial mutex across this call).
     */
    [[nodiscard]] stats_t stats() const;

   private:
    /** @brief The recv thread body: (re)connect, then poll+read+deliver until stopped. */
    void recv_loop();
    /** @brief One dial attempt: (lazily) build the tcp+ws transports, connect, apply the
     *         per-socket policy (Nagle off, bounded write, keepalive probes), mark up.
     *         @return true on a completed handshake. */
    bool connect_once();
    /** @brief Close the connection and mark down; the next recv_loop turn reports the
     *         departure and re-dials. The report is NOT made here — this runs under the
     *         syscall serializer, and the notifier re-enters the routing plane. */
    void drop();

    const std::string host_;
    const std::uint16_t port_;
    const std::string ws_path_;
    /** @brief Extra CRLF-terminated handshake header lines, empty = none. `const`, and that
     *         is the fix (#959): it is written once by the ctor, BEFORE the recv thread that
     *         reads it on every (re)dial exists, so there is no cross-thread write to order
     *         and the first dial carries it. */
    const std::string handshake_headers_;

    // Both handles are written ONLY by the recv thread (connect_once rebuilds them on
    // every dial, holding no lock) and PUBLISHED by the connected_ release store below;
    // any other thread must observe that store before it may read either one.
    esp_transport_handle_t tcp_ = nullptr;  // parent TCP transport (owned)
    esp_transport_handle_t ws_ = nullptr;   // WS transport over tcp_ (owned)

    std::vector<std::byte> rx_buf_;  // reusable RX fill (zero-copy read target)
    std::vector<std::byte> tx_buf_;  // reusable TX scratch (masked in-place under write_m_)

    // Serializes the esp_transport read/write SYSCALLS (the write is bounded). It does
    // NOT cover the re-dial's rebuild of the handles above — assuming it did is the
    // premise the pre-#952 destructor was written on.
    std::mutex write_m_;
    /** @brief Senders that have RAISED this tally and not yet returned from @ref send.
     *         Raised BEFORE the sender queues on `write_m_`, so the destructor's drain
     *         covers exactly the callers that could already be parked on that mutex —
     *         and, by the same token, no earlier than the raise itself (#952). */
    std::atomic<std::uint32_t> senders_{0};
    /** @brief Guards the recv thread's two parked waits — the reconnect backoff and the
     *         `defer_recv` dial latch — both of which the destructor has to be able to
     *         cut short, hence a condition variable rather than `sleep_for` (#952).
     *         Never taken with `write_m_` held. */
    std::mutex backoff_m_;
    std::condition_variable backoff_cv_; /**< @brief Signalled by the destructor and by
                                          *          @ref start_receiving. */
    /**
     * @brief Guards the counter block below, and NOTHING else.
     *
     * Deliberately not `write_m_`: that one is held across `esp_transport_write` for
     * up to `kWriteTimeoutMs` (4 s) on a stalled socket, so counters riding it would
     * make @ref stats block for seconds on a peer with a zero window — and a host
     * that holds its own mutex across `stats()` would drag that lock into the wait
     * too. This mutex is only ever taken for a bump or the snapshot copy, so it is
     * uncontended and syscall-free from any task. Nesting is always
     * `write_m_ -> st_m_`, never the reverse. `mutable` for the const snapshot.
     */
    mutable std::mutex st_m_;
    /** @brief Passive traffic counters — st_m_. `rx_drops` is filled from `dropped_rx_`
     *         at snapshot time and is never bumped here. See @ref stats. */
    link_counters_t st_;
    /** @brief Completed handshakes since construction — st_m_. */
    std::uint32_t reconnects_ = 0;
    /** @brief Last successful dial's handshake duration in ms — st_m_. */
    std::uint32_t connect_ms_ = 0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    /** @brief The ONE-SHOT dial latch (ADR-0081, #1102): the recv thread parks before its
     *         FIRST dial until this is true. Starts true unless the ctor's `defer_recv`;
     *         set by @ref start_receiving and never cleared — every re-dial happens on a
     *         link that was already armed, so reconnects are not re-gated. */
    std::atomic<bool> armed_{true};
    /** @brief Receive-path message drops — see @ref dropped_rx. Written only by the recv
     *         thread, read by anyone, so relaxed is enough (it is a statistic). */
    std::atomic<std::uint64_t> dropped_rx_{0};
    std::thread recv_thread_;
};

}  // namespace tr::net
