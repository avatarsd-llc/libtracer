/**
 * @file
 * @brief `httpd_ws_link_t` implementation — see include/libtracer_esp/httpd_ws_link.hpp.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Chip-target-only TU (needs esp_http_server + lwIP BSD sockets), selected by the
 * component CMakeLists — never an in-source #ifdef, the same rule twai_link.cpp
 * follows. The linux virtual board keeps core's raw-socket transport_ws_server.
 */

#include "libtracer_esp/httpd_ws_link.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tr::net {

namespace {

constexpr const char* kTag = "httpd_ws";

/**
 * @brief The adopted-mode teardown drain: turns of @ref kDrainSliceMs each.
 *
 * The ONE bound the destructor spends waiting on the adopted server's task — first for
 * the session detach, then for the in-flight TX slots (#815 established this pair of
 * numbers; naming them keeps the two drains on one bound rather than inventing a
 * second). Nothing depends on it for CORRECTNESS: both expiries are memory-safe by
 * construction (leak the sessions / leak the pool) and are logged, so the bound only
 * trades how long a teardown blocks against how often that leak is taken. An adopted
 * server we do not own exposes no timeout of its own to derive it from — the send/recv
 * wait it was configured with belongs to the caller's `httpd_config_t`, not to us.
 *
 * DELIBERATELY FIXED (#1160). It is reached only on a teardown of an ADOPTED server, it
 * costs no RAM, and both outcomes are already safe and logged — so the only thing a knob
 * would let an embedder choose is how long their own destructor blocks before taking a
 * leak they are told about. That is not a resource trade, and it is not on any hot path;
 * a node that finds this bound wrong has a wedged server task, which is the thing to fix.
 */
constexpr int kDrainTurns = 200;

/** @brief One drain turn, milliseconds — see @ref kDrainTurns. Fixed for the same reason,
 *         and it is only the resolution at which that one bound is sampled. */
constexpr int kDrainSliceMs = 5;

/** @brief The calling task's identity, as the opaque token `server_task_` latches. */
[[nodiscard]] void* current_task() noexcept {
    return static_cast<void*>(xTaskGetCurrentTaskHandle());
}

/**
 * @brief The stack size the deep in-call path was measured OVERFLOWING, bytes.
 *
 * The other half of the measurement @ref httpd_ws_link_t::kRequiredHttpdStack carries
 * (F2b, 2026-07-09): the /unit batch apply overflowed 8 KB and needed ~12 KB. It is named
 * here so @ref kStackHeadroomFloor is derived from the two figures that one measurement
 * produced rather than from a fresh number nobody measured.
 */
constexpr std::size_t kMeasuredOverflowStack = 8192;

/**
 * @brief Free-stack headroom below which @ref httpd_ws_link_t::check_httpd_stack names
 *        the cause, bytes.
 *
 * The margin the required figure buys over the size that was measured to overflow. On a
 * task sized as this link asks, free headroom under it means the deepest point the task
 * has ever reached is already past the depth that overflowed 8 KB — the next batch apply
 * is the stack-protection panic. On a task sized SMALLER (the adopted-server case, where
 * nothing can read the configured size) it trips sooner, which is the misconfiguration
 * this exists to name.
 *
 * NOT a tunable, and not a synthetic limit: both inputs are the measurement's own two
 * numbers, so there is no third number to justify and no knob to get wrong.
 */
constexpr std::size_t kStackHeadroomFloor =
    httpd_ws_link_t::kRequiredHttpdStack - kMeasuredOverflowStack;

/**
 * @brief Upper bound on a single inbound message (one frame, or a reassembly).
 *
 * A borrowed-delivery transport heap-allocates the frame's bytes per receive, so
 * an unbounded length is a heap-exhaustion lever. Graph control-plane TLVs are far
 * smaller; a frame or reassembly past this is treated as abuse and the peer is
 * dropped (or the message discarded).
 *
 * DELIBERATELY FIXED, and not one of #1160's constructor arguments. It costs NO RAM — it
 * is the ceiling on a per-frame nothrow allocation, not a buffer — so there is nothing to
 * trade, and what it bounds is an attack rather than a workload: an embedder who raises it
 * is not tuning a node, they are widening a heap-exhaustion lever on the one path a
 * stranger controls. A deployment that legitimately needs larger control TLVs has outgrown
 * the assumption behind the number, which is a change to this file with its reasoning, not
 * a knob turn.
 */
constexpr std::size_t kMaxFrameBytes = 32768;

/**
 * @brief Sockets reserved beyond the peer cap: httpd's internal working sockets
 *        plus headroom so the (cap+1)th peer is still ACCEPTED and can be refused
 *        cleanly in the handshake handler rather than held in the SYN backlog.
 *
 * DELIBERATELY FIXED (#1160): it is a fact about `esp_http_server`'s own socket
 * bookkeeping plus the one spare that makes a refusal a clean 1-frame close instead of a
 * SYN-backlog stall — not a budget of ours. Raising it would buy an embedder nothing they
 * cannot get by raising `max_peers`, which is already an argument; lowering it breaks the
 * refusal path this component promises.
 */
constexpr std::size_t kInternalSockSlack = 3;

/**
 * @brief Consecutive failed SENDS to one peer that mark its session broken (then close).
 *
 * A single failed send — the peer's window full for one bounded write — is transient
 * backpressure: dropping that one frame is the lean response, and the next send that
 * completes resets the count. But a session whose sends keep failing with no success in
 * between is not riding out a burst: its peer is silently missing frames while the
 * socket looks open. Three in a row distinguishes the two — one drop is noise, two can
 * straddle a burst, three consecutive means the drain isn't keeping up at all. This is a
 * brokenness detector, not a tunable, so it is a named constant and NOT a config knob.
 * #1160 asked the question again, knob by knob, and the answer did not change: this is the
 * only thing standing between one silently-stalled peer and the httpd task being parked
 * for the whole watchdog window, so an embedder who could raise it could disable the
 * protection that keeps the node alive — and one who could lower it to 1 would condemn
 * peers for a single burst. It is also a DIVISOR of the send bound below
 * (@ref derive_send_timeout_ms), so a knob over it would silently re-time every socket.
 *
 * The trichotomy is unchanged by #835; what changed is WHOSE evidence feeds it. It is
 * now the failed sends — which name their destination by construction — and no longer
 * the refused enqueues, which name only the shared control queue (see
 * @ref httpd_ws_link_t::note_enqueue_drop). Bounding the send also bounds the total
 * stall one broken peer can impose before teardown: three send bounds, not three of the
 * server's whole send_wait_timeout.
 */
constexpr std::uint8_t kMaxConsecutiveTxDrops = 3;

/**
 * @brief How many times IDF spends the send bound inside ONE `httpd_ws_send_frame_async`.
 *
 * `SO_SNDTIMEO` is a per-`send` property, and IDF writes a WS frame in TWO calls to the
 * session's send function — the header (`components/esp_http_server/src/httpd_ws.c:447`)
 * and then the payload (`:455`). So the caller's number is a per-LEG bound, never a
 * per-frame one, and a single stalled peer parks the httpd task for twice what the
 * derivation used to claim (#956).
 *
 * The sibling constant on the client link is `kIdfWriteLegs` in `esp_ws_client_link.cpp`,
 * which is 3 because `_ws_write` spends it on a poll plus both writes (#952). Same class
 * of fact, different function, so each states its own — never a shared constant.
 *
 * This term disappears if the send path ever becomes a single write (the direction #949
 * and the frame-atomicity work point at); it is a fact about the API in use, not a policy.
 */
constexpr std::uint32_t kIdfWsWriteLegs = 2;

/**
 * @brief The task-watchdog period, seconds — the numerator of the send bound.
 *
 * Not a number of ours: `CONFIG_ESP_TASK_WDT_TIMEOUT_S` is the system's own normative
 * statement of how long a task may go unfed, and it is literally the tripwire #835
 * observed firing. IDF defines it through `sdkconfig.h` (pulled in by FreeRTOS.h); the
 * fallback is IDF's own Kconfig default for that symbol, for a build with the task
 * watchdog compiled out and for the host test build, so the derivation has one
 * provenance on every target.
 */
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
constexpr std::uint32_t kTaskWdtSeconds = CONFIG_ESP_TASK_WDT_TIMEOUT_S;
#else
constexpr std::uint32_t kTaskWdtSeconds = 5;
#endif

/**
 * @brief Peer cap assumed when the caller passes `max_peers == 0` (unbounded): the
 *        shared lwIP socket pool is small, so an unbounded cap still needs a finite
 *        socket budget — and the same finite number is the send bound's divisor.
 *
 * DELIBERATELY FIXED (#1160), because the knob for it already exists and is better: pass
 * `max_peers`. This number is only ever consulted when the caller declined to state one,
 * so a second knob would be a configurable value for the case "the embedder configured
 * nothing" — and the adopting constructor already LOGS a warning when it has to fall back
 * to it, naming the figure it assumed.
 */
constexpr std::size_t kDefaultPeerCap = 4;

/**
 * @brief TCP keepalive policy for an upgraded WS socket: idle seconds before the first
 *        probe, seconds between probes, and probes before the stack declares the
 *        connection dead (#957).
 *
 * Not numbers of ours. They are the defaults ESP-IDF documents for this very server's
 * own keepalive (`esp_http_server.h`, `httpd_config_t`: `keep_alive_idle` "Default is 5
 * (second)", `keep_alive_interval` "Default is 5 (second)", `keep_alive_count` "Default
 * is 3 counts") — so the policy applied per WS socket is the host server's own, stated
 * where this link can guarantee it rather than where its owner may have left it off.
 * `esp_ws_client_link_t` states the same fact for dialed sockets, so a board-to-board
 * pair declares a peer dead at the same age from either end. A shared FACT, never a
 * shared constant.
 *
 * lwIP takes `TCP_KEEPIDLE`/`TCP_KEEPINTVL` in SECONDS (`lwip/sockets.h`) and IDF
 * compiles lwIP with `LWIP_TCP_KEEPALIVE == 1` unconditionally
 * (`lwip/port/include/lwipopts.h`), so the three tunables exist on every chip target
 * this link builds for; Linux takes the same three in the same units, which is what
 * makes the host suite representative of the option seam.
 */
constexpr int kKeepIdleSeconds = 5;
constexpr int kKeepIntervalSeconds = 5; /**< @brief Seconds between probes. */
constexpr int kKeepProbes = 3;          /**< @brief Unanswered probes before death. */

/**
 * @brief Derive the per-socket send bound for @p peer_cap peers, milliseconds.
 *
 * The sends serialize on the single httpd task, so the quantity that must fit inside one
 * watchdog window is the WHOLE stall a stalled peer set can impose before the link is rid
 * of it — the failure #835 observed. That is not one fan-out round. A peer is not
 * condemned on its first failed send: the brokenness detector wants
 * @ref kMaxConsecutiveTxDrops consecutive failures, and every one of them costs a full
 * bound. So the worst case is every peer in the cap, each timing out its full streak:
 *
 *     peers * kMaxConsecutiveTxDrops * kIdfWsWriteLegs * bound  <=  one watchdog window
 *
 * Dividing by the peer count alone (what this did before) makes one ROUND fill the window
 * exactly, and the streak then carries the task `kMaxConsecutiveTxDrops` windows past the
 * tripwire — measured downstream at 4959 ms of a 5000 ms budget, i.e. 99% consumed with a
 * panic decided by noise (#840, residual 1). The strike cap is the missing divisor, and
 * it is the same constant the teardown path already reasons with: this file's own
 * @ref kMaxConsecutiveTxDrops note says the bound is what makes one broken peer cost
 * "three send bounds" — that three has to be IN the derivation, not merely acknowledged
 * by it.
 *
 * `kIdfWsWriteLegs` is the third divisor and the same kind of correction (#956): the bound
 * is a per-`send_fn` property, and IDF spends it TWICE per frame, so a per-frame stall was
 * double what this returned.
 *
 * All four inputs are facts already in hand (@ref kTaskWdtSeconds, the caller's peer cap,
 * @ref kMaxConsecutiveTxDrops, @ref kIdfWsWriteLegs), so there is still no knob and no
 * millisecond literal.
 *
 * The floor is a CORRECTNESS clamp, not a policy: `SO_SNDTIMEO = 0` means *block forever*
 * — precisely the failure this derivation exists to remove — and a large-but-legal
 * `peer_cap` divides the window to zero (#956). One millisecond is not a useful bound and
 * is not meant to be; it is the statement that no configuration may reach "unbounded" by
 * arithmetic. A deployment landing on it has a peer cap its watchdog window cannot cover,
 * which is a wiring problem the clamp makes survivable rather than silent.
 */
[[nodiscard]] constexpr std::uint32_t derive_send_timeout_ms(std::size_t peer_cap) noexcept {
    const std::size_t peers = peer_cap != 0 ? peer_cap : kDefaultPeerCap;
    const std::size_t legs = peers * kMaxConsecutiveTxDrops * kIdfWsWriteLegs;
    const auto derived = static_cast<std::uint32_t>(kTaskWdtSeconds * 1000U / legs);
    return derived != 0 ? derived : 1;
}

/** @brief Clamp @p want to the server's own per-socket send bound, seconds — the value
 *         REST sockets keep. A WS socket may be bounded tighter, never looser. */
[[nodiscard]] constexpr std::uint32_t clamp_send_timeout_ms(std::uint32_t want,
                                                            int send_wait_timeout_s) noexcept {
    const std::uint32_t ceiling = static_cast<std::uint32_t>(send_wait_timeout_s) * 1000U;
    return want < ceiling ? want : ceiling;
}

/**
 * @brief TX work slots held back for sends issued ON the httpd task — ADDITIONAL to the
 *        configured pool depth, never carved out of it (#1218).
 *
 * A send issued on the httpd task (a request's reply, serviced in-call) is the one sender
 * that cannot wait for a slot: the task that would free one is the task that is asking, so
 * waiting there waits on its own stack frame (#814). Without a reserve, a delivery burst and
 * a subscribe ack raced through the same slots and the ack lost — the requester then timed
 * out against an edge that was already delivering (#1187's secondary effect). One slot is
 * enough for the shape that produced it: one reply per serviced request.
 *
 * That the reserve is EXTRA is the whole of #1218. #1187 implemented it by shortening the
 * scan an off-task claimer was allowed — which cost every producer a slot permanently,
 * whether or not any request was in flight, and turned a fan-out of exactly
 * @ref httpd_ws_link_t::tx_slot_capacity (which had always fit, at the offered rate) into
 * one that had to wait for the drain on every single pass. The reserve is not free — it
 * costs one slot of RAM per link (a shell plus @ref httpd_ws_link_t::tx_inline_bytes) —
 * and that price is the honest one: a guarantee for a claimer that cannot wait has to be a
 * slot nobody else can take, not a narrower bound for everybody else.
 *
 * DELIBERATELY FIXED, and not a ctor argument like the three sizes around it (#1160). It
 * is not a depth an embedder can trade: the shape it answers is "one reply per serviced
 * request", and esp_http_server services requests one at a time on one task, so a second
 * reserved slot could never be claimed and a zeroth would reinstate the #1187 ack loss.
 * Its RAM cost travels with `tx_inline_bytes`, which IS configurable.
 */
constexpr std::size_t kTxReplySlots = 1;

/**
 * @brief One turn a WAITING send spends off the CPU before re-trying the pool,
 *        milliseconds.
 *
 * A send that finds the pool full and is NOT running on the httpd task sleeps in turns of
 * this until a slot frees or the bound below expires (@ref
 * httpd_ws_link_t::claim_tx_slot_waiting). Sleeping — not spinning, and not
 * `taskYIELD` — is what makes it work on the unicore target the defect was found on: the
 * httpd task is the only task that can free a slot, and a producer at a higher priority
 * yields nothing to it by spinning. One tick is the smallest amount of "let the drain
 * run" the scheduler can express; the granularity is the tick period, so this is a floor
 * on the turn, never a promise about it. The BOUND is what is derived
 * (@ref tx_wait_bound_us) — the turn is only how finely that bound is sampled.
 *
 * The waiting frame is the CALLER'S, still in the caller's memory: nothing is copied, no
 * queue of ours grows, and a frame that does not make it is dropped and counted exactly
 * as before. So this stays inside ADR-0081 §1 (never park ingress or egress in a
 * library-owned buffer) — it is the producer's own call that waits, which is the same
 * "hold it in the layer that already owns the memory" answer §2 gives.
 */
constexpr std::uint32_t kTxWaitSliceMs = 1;

/**
 * @brief The longest a send may wait for a TX slot, microseconds — derived, never chosen.
 *
 * One slot is held for exactly as long as the httpd task needs to put its frame on a
 * socket, and that duration already has a bound in hand: the per-socket send bound this
 * link applies at admission, spent once per write leg (@ref kIdfWsWriteLegs). Past it the
 * oldest queued send has either completed or timed out — either way its slot is back, or
 * its peer is accruing the strike that condemns it. Waiting longer than one such
 * occupancy therefore cannot learn anything new; waiting less would give up while the
 * drain is still healthy.
 *
 * It is a per-SEND bound, and the futility latch (@ref
 * httpd_ws_link_t::tx_wait_futile_until_us_) is what keeps a whole fan-out pass from
 * paying it once per destination against a task that is not draining at all.
 */
[[nodiscard]] constexpr std::int64_t tx_wait_bound_us(std::uint32_t send_timeout_ms) noexcept {
    return static_cast<std::int64_t>(send_timeout_ms) * kIdfWsWriteLegs * 1000;
}

/**
 * @brief Destinations one fan-out chunk holds — the ON-STACK snapshot a broadcast walks
 *        its peer set through, and the reason a broadcast allocates nothing.
 *
 * The snapshot must not be a `std::vector`, and for the reason
 * @ref httpd_ws_link_t::tx_work_t already records: under `-fno-exceptions` the vector's
 * THROWING allocator turns a failed growth into `abort()` via the bad_alloc stub, which
 * is the same defeat of a nothrow guard that once crashed this link on a reply-sized
 * copy. Here it sat on the FAN-OUT path (#961) — reached by every broadcast, and reached
 * BEFORE any nothrow fallback could apply, so a fan-out landing in a heap trough rebooted
 * the node instead of dropping a frame. Nothing counted it and nothing logged it.
 *
 * A fixed chunk plus a RESUMABLE scan removes the allocation rather than moving it: there
 * is no heap arm left to fail, so no drop to account for and no sizing policy for the
 * unbounded-`max_peers` case. @ref kDefaultPeerCap is the size because it is already this
 * file's answer to "how many peers does a link budget for" — the finite socket budget an
 * unbounded link is given, and the divisor @ref derive_send_timeout_ms uses. At or under
 * it a broadcast takes exactly the one `peers_m_` hold it always did; past it the scan
 * resumes where it stopped, costing one more uncontended acquisition per chunk.
 */
constexpr std::size_t kFanoutChunk = kDefaultPeerCap;

/**
 * @brief How often the unauthenticated-session sweep runs, as a fraction of the deadline.
 *
 * A sweep period equal to the deadline would let a squatter live up to twice it (it can
 * expire the instant after a tick), so the period is half — which bounds the real lifetime
 * at 1.5x the configured window. Finer costs wakeups on a chip that is otherwise idle
 * between frames and buys nothing: the deadline is a bound on a resource, not an SLA on the
 * close, and the resource it protects (the peer cap) is additionally defended
 * synchronously — @ref httpd_ws_link_t::on_data_frame reaps expired sessions before it tests
 * the cap, so an expired squatter can never be the reason a real peer is refused, whatever
 * the timer is doing.
 */
constexpr std::int64_t kAuthSweepDivisor = 2;
/** @brief Floor on the sweep period: a sub-100 ms deadline must not turn the timer task
 *         into a spinner. */
constexpr std::int64_t kMinAuthSweepUs = 100000;

/**
 * @brief Resolve the constructor's `auth_deadline_ms` to microseconds, substituting
 *        @ref httpd_ws_link_t::kDefaultAuthDeadlineMs for 0 — the same "0 means derive"
 *        idiom `send_timeout_ms` uses.
 */
[[nodiscard]] constexpr std::int64_t resolve_auth_deadline_us(std::uint32_t ms) {
    const std::int64_t chosen = ms != 0 ? ms : httpd_ws_link_t::kDefaultAuthDeadlineMs;
    return chosen * 1000;
}

/**
 * @brief Resolve one of the three buffer-sizing constructor arguments (#1160): @p want,
 *        or @p fallback when the caller left it 0.
 *
 * The SAME "0 means take the default" idiom `send_timeout_ms` and `auth_deadline_ms`
 * already use on these constructors, so an embedder learns one convention for the whole
 * argument list. There is no clamp and no ceiling: this component does not know the
 * target's heap, and inventing an upper bound here would be exactly the synthetic limit
 * the RAM-bearing three were made arguments to avoid. A value too large simply fails to
 * allocate, and @ref httpd_ws_link_t::alloc_buffers already reports that as the link
 * dropping every send on the counted path.
 */
[[nodiscard]] constexpr std::size_t resolve_size(std::size_t want, std::size_t fallback) noexcept {
    return want != 0 ? want : fallback;
}

/**
 * @brief Bytes a stored `<ip>:<port>` needs, NUL included — the exact bound, not a guess.
 *
 * `INET6_ADDRSTRLEN` already counts the longest textual address and its terminator; the
 * `:` and up to five port digits are what this adds. Sizing it from the platform's own
 * constant is what keeps it a fact rather than a magic number: a stack that widened its
 * address text would widen this with it. With `CONFIG_LWIP_IPV6` off, lwIP defines
 * neither the v6 constant nor `sockaddr_in6` at all, and every accepted socket is
 * AF_INET — so the v4 constant is the exact bound, not a truncation. The v6
 * constant's own presence is the discriminator (not the Kconfig symbol): the host
 * test build compiles this TU with no sdkconfig against a dual-stack libc, and it
 * must keep the v6 arm the way an IPv6-on chip build does.
 */
#if defined(INET6_ADDRSTRLEN)
constexpr std::size_t kAddrChars = INET6_ADDRSTRLEN;
#else
constexpr std::size_t kAddrChars = INET_ADDRSTRLEN;
#endif
constexpr std::size_t kEndpointChars = kAddrChars + 6;

/**
 * @brief The peer's routable name for slot @p idx — `p<slot>` (ADR-0073 §2).
 *
 * Two measured decisions, both against an A/A null of 0 on this TU's `.text`:
 *
 * `noinline` for the SAME reason @ref format_endpoint is. Folded into
 * @ref httpd_ws_link_t::on_data_frame — which reaches this once per session and is
 * otherwise the per-FRAME hot path — it cost **+1962 B in that one function**. Out of line
 * it costs a call on the claim edge and leaves the frame path's code layout the size it was.
 *
 * The digits are written here rather than by `std::to_string` because that instantiation is
 * **+1456 B** of this TU, and it is NOT shared with the one @ref format_endpoint already pulls in:
 * the body is duplicated per call site, so the second use pays full price again (`int` and
 * `std::size_t` overloads both, measured). A slot index is a small non-negative integer, so
 * the general path buys nothing here. Core's `slot_server_t` keeps `std::to_string`
 * (`core/src/posix_endpoint.cpp:406`) — it is host code with no image budget; the STRING is
 * identical either way, which is all the two have to agree on.
 */
[[gnu::noinline]] [[nodiscard]] std::string slot_name(std::size_t idx) {
    char buf[24];
    char* p = buf + sizeof(buf);
    do {
        *--p = static_cast<char>('0' + (idx % 10));
        idx /= 10;
    } while (idx != 0);
    *--p = 'p';
    return std::string(p, static_cast<std::size_t>(buf + sizeof(buf) - p));
}

/**
 * @brief `<ip>:<port>` of the far side of @p fd — the peer's PHYSICAL address, kept for
 *        diagnostics only (ADR-0073 §2). Falls back to `fd<n>`.
 *
 * The address family is read from what `getpeername` actually filled, never assumed. With
 * `CONFIG_LWIP_IPV6` on — the default on this target — `esp_http_server` binds its
 * listener `PF_INET6` (httpd_main.c), so every accepted WS socket is an AF_INET6 one and
 * a `sockaddr_in6` is what comes back. Decoding that as a `sockaddr_in` reads the port
 * correctly (same offset) and then reads `sin6_flowinfo` as the address — which is zero,
 * so every peer on the node was named `0.0.0.0:<port>`. That is what the on-silicon run
 * saw in the strike log, and it made the strike unattributable to a peer at exactly the
 * moment the attribution mattered. A v4-mapped v6 address (`::ffff:a.b.c.d`) is unwrapped
 * so a dual-stack node keeps reporting its IPv4 peers the way the census always has.
 * With `CONFIG_LWIP_IPV6` off the whole v6 arm is compiled out — lwIP defines neither
 * `sockaddr_in6` nor `INET6_ADDRSTRLEN` then (the constant's absence is the compile-time
 * discriminator), and the listener binds `PF_INET`, so AF_INET is the only family
 * `getpeername` can report.
 *
 * This string is NOT the peer's graph name and has not been since #994: it carries `.` and
 * `:`, both rejected by `graph::valid_segment`, so a session named with it could be listed
 * and never addressed. @ref httpd_ws_link_t::session_t::name holds the routable `p<slot>`;
 * this is what an operator needs to tell which physical client that slot is.
 *
 * Writes into the caller's buffer rather than returning a `std::string`, which is what made
 * #994 a NET SHRINK. Measured against an A/A null of 0 on this TU's `.text`: the naming
 * change on its own cost **+976 B**, and dropping the `std::string` + `std::to_string` build
 * from this body returned about **6.6 KB**, for **−5648 B net**. It also removed a heap
 * allocation from every session claim — the old return value outlived the call as the slot's
 * name, so each session held a heap chunk for its whole life. `snprintf` is already linked
 * here (every `ESP_LOG` uses it), so the formatting is paid for once for the whole image.
 *
 * `noinline` because it runs ONCE per connection — on the claim edge inside
 * @ref httpd_ws_link_t::on_data_frame, which is otherwise the per-FRAME hot path. It is a
 * large body (`getpeername` + `inet_ntop` + formatting), and with the opening-GET claim site
 * gone it has a single caller, which is precisely the shape that invites the inliner to fold
 * it into that hot function. Keeping the call is what keeps the per-frame path's code layout
 * the size it was.
 */
[[gnu::noinline]] void format_endpoint(int fd, char (&out)[kEndpointChars]) {
    sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        char ip[kAddrChars] = {};
#if defined(INET6_ADDRSTRLEN)
        if (addr.ss_family == AF_INET6) {
            const auto& a6 = reinterpret_cast<const sockaddr_in6&>(addr);
            if (IN6_IS_ADDR_V4MAPPED(&a6.sin6_addr)) {
                in_addr v4 = {};
                std::memcpy(&v4, reinterpret_cast<const std::uint8_t*>(&a6.sin6_addr) + 12,
                            sizeof(v4));
                ::inet_ntop(AF_INET, &v4, ip, sizeof(ip));
            } else {
                ::inet_ntop(AF_INET6, &a6.sin6_addr, ip, sizeof(ip));
            }
            (void)std::snprintf(out, sizeof(out), "%s:%u", ip, (unsigned)ntohs(a6.sin6_port));
            return;
        }
#endif
        if (addr.ss_family == AF_INET) {
            const auto& a4 = reinterpret_cast<const sockaddr_in&>(addr);
            ::inet_ntop(AF_INET, &a4.sin_addr, ip, sizeof(ip));
            (void)std::snprintf(out, sizeof(out), "%s:%u", ip, (unsigned)ntohs(a4.sin_port));
            return;
        }
    }
    (void)std::snprintf(out, sizeof(out), "fd%d", fd);
}

/**
 * @brief Nothrow fragment-reassembly buffer: grows by exact-size `new (std::nothrow)`
 *        reallocation, so heap exhaustion drops the in-flight message instead of
 *        aborting the node.
 *
 * `std::vector` is unusable here: under `-fno-exceptions` its throwing allocator
 * turns a failed growth into `abort()` via the bad_alloc stub — and the appended
 * chunk is peer-controlled up to kMaxFrameBytes, so reassembly growth MUST be
 * failure-capable (the same backpressure contract as the tx queue). Fragmentation
 * is the rare path (the SPA sends one whole TLV per unfragmented frame) and the
 * total is capped by kMaxFrameBytes, so exact-size regrow-and-copy is the lean
 * choice over capacity doubling.
 */
struct asm_buf_t {
    /** @brief True when no reassembly is in progress. */
    [[nodiscard]] bool empty() const noexcept { return len_ == 0; }
    /** @brief Assembled length so far, bytes. */
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    /** @brief The assembled bytes so far (valid until the next append/clear). */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {bytes_.get(), len_}; }
    /** @brief Release the storage (post-deliver / slot-reclaim / drop reset). */
    void clear() noexcept {
        bytes_.reset();
        len_ = 0;
    }
    /**
     * @brief Move the assembled message OUT, leaving this buffer empty.
     *
     * The delivery of a completed reassembly must not be followed by a write back into
     * the slot: the app callback the delivery runs can tear this link down in-call
     * (#814), and the slot is gone by the time deliver() returns. Taking the bytes into
     * a caller-local buffer first makes the post-delivery `clear()` unnecessary, so the
     * RX path touches NOTHING owned by the link once it has delivered.
     */
    [[nodiscard]] asm_buf_t take() noexcept {
        asm_buf_t out;
        out.bytes_ = std::move(bytes_);
        out.len_ = len_;
        len_ = 0;
        return out;
    }
    /**
     * @brief Append @p chunk, nothrow.
     * @retval false Allocation failed — the buffer is cleared (the partial message is
     *               unrecoverable) and the caller drops the message (backpressure).
     */
    [[nodiscard]] bool append(std::span<const std::byte> chunk) noexcept {
        if (chunk.empty()) return true;
        std::unique_ptr<std::byte[]> grown(new (std::nothrow) std::byte[len_ + chunk.size()]);
        if (grown == nullptr) {
            clear();
            return false;
        }
        if (len_ != 0) std::memcpy(grown.get(), bytes_.get(), len_);
        std::memcpy(grown.get() + len_, chunk.data(), chunk.size());
        bytes_ = std::move(grown);
        len_ += chunk.size();
        return true;
    }

   private:
    std::unique_ptr<std::byte[]> bytes_; /**< @brief Owned storage (exact-sized). */
    std::size_t len_ = 0;                /**< @brief Assembled length, bytes. */
};

}  // namespace

/**
 * @brief The handler-admission gate: the ONE object the adopted server may still hold a
 *        pointer to after this link is gone, and the barrier the destructor joins the
 *        in-flight URI handler on.
 *
 * It exists because `esp_http_server` LATCHES the WebSocket route into the session, not
 * into the URI table: `httpd_uri.c` copies `uri->handler` and `uri->user_ctx` into
 * `sock_db::ws_handler` / `ws_user_ctx` when it answers the handshake, and only deleting
 * the session clears them. Unregistering the URI therefore stops NEW handshakes and
 * nothing else — every already-upgraded peer keeps dispatching its data frames straight
 * into the handler, with the registered `user_ctx`, for as long as its session lives.
 * A destructor cannot outrun that: it can neither enumerate those sessions (a peer that
 * upgraded but never sent a frame is invisible to this link) nor force them deleted
 * (`httpd_sess_trigger_close` is itself a queued request that can be refused).
 *
 * So the registered `user_ctx` is this gate, never the link. The handler resolves the
 * link THROUGH it under @ref m, which makes two things true at once:
 *   - after @ref httpd_ws_link_t::close_gate, `link` is null and every later dispatch
 *     is refused before it can touch a single link member (httpd then closes that
 *     socket, so the stale sessions reap themselves);
 *   - while a handler frame IS inside the link, `depth` is non-zero, and the destructor
 *     blocks until it leaves — the barrier that the URI unregister never was.
 *
 * Its lifetime is deliberately longer than the link's in adopted mode: since the set of
 * sessions still holding the pointer is unknowable, the gate is LEAKED there (one small
 * block, teardown-only — the same leak-rather-than-free discipline #815 established for
 * the TX pool). Owning mode frees it: `httpd_stop` deletes every session first.
 *
 * A FIFTH entry point resolves the link through this gate and is NOT one of the latched
 * four: @ref httpd_ws_link_t::ws_pre_handshake, the `ws_pre_handshake_cb` both constructors
 * register (#958). The asymmetry is the URI table's: `httpd_uri.c` reads that callback out
 * of the registration on every handshake and frees it with the entry on unregister, where
 * `handler`/`user_ctx` were copied into each session and survive. So it needs the gate for
 * the SAME reason — it dereferences the link, and only the barrier makes that safe against
 * a concurrent destructor — but unlike the handler it cannot outlive the URI, so it is not
 * part of what makes the gate's lifetime longer than the link's.
 *
 * LOCK ORDER, recorded because it was previously established only by code (#960):
 * `m` may be taken while holding nothing, and `peers_m_` may be taken under it — never
 * the reverse. NO callback installed from outside this link runs while `m` is held —
 * neither the routing plane's departure notifier nor an app sink. (The IDF calls
 * @ref httpd_ws_link_t::condemn makes under it are not an exception to that: they go
 * *into* the server, are bounded by it, and re-enter nothing of ours.) That is a rule,
 * not an accident: `m` is what each of the four callbacks the server latched resolves the
 * link through — @ref httpd_ws_link_t::ws_handler, @ref httpd_ws_link_t::on_session_closed,
 * @ref httpd_ws_link_t::send_guarded, @ref httpd_ws_link_t::tx_work — and what a
 * destructor blocks on, so an ordering edge from it into the routing plane's locks would
 * make any `graph → m` path a hard ABBA. Work that must outlive the lock — a bounded send,
 * an unbounded departure notification — registers on @ref depth and runs with `m`
 * released. That keeps the destructor's join intact without giving the mutex an ordering
 * constraint on a foreign lock. It does NOT make destroying a link from under a lock its
 * in-flight work needs safe: that deadlocks on the join, here as in the URI-handler case
 * and as in `transport_ws_server`, whose destructor joins its poll thread for the same
 * reason.
 */
struct httpd_ws_link_t::gate_t {
    std::mutex m;                    /**< @brief Guards every member below. */
    std::condition_variable cv;      /**< @brief Signalled as @ref depth falls. */
    httpd_ws_link_t* link = nullptr; /**< @brief The link, or null once it is going. */
    /**
     * @brief Frames the destructor must join: live URI-handler frames, plus a departure
     *        notification in flight with `m` released (@ref
     *        httpd_ws_link_t::on_session_closed).
     */
    unsigned depth = 0;
    /**
     * @brief The socket whose request the in-flight handler frame is servicing, or -1.
     *
     * Read by a destructor nested inside that frame (#814): `httpd_sess_set_ctx` takes a
     * REQUEST-SCOPED branch for exactly this session and detaching it is impossible
     * there (see @ref httpd_ws_link_t::detach_sessions).
     */
    int serving_fd = -1;
};

/**
 * @brief One peer slot: a single inbound WebSocket client's connection state.
 *
 * Slots are recycled in place across connections (never freed before the link), so a
 * @ref peer_resolution_t may name one for as long as the link lives — it compares the
 * generation it captured rather than trusting the slot's current one. Threading:
 * `fd`/`open`/`name` are read cross-thread (peer_link / enumerate_peers / a send's fd
 * snapshot) and written by the httpd task (accept/close) — all under `peers_m_`.
 * `asm_buf` is touched only on the httpd task (RX reassembly). `gate` is set once at
 * creation and never changes.
 */
struct httpd_ws_link_t::session_t {
    /**
     * @brief The owning link's gate — how @ref on_session_closed reaches the link.
     *
     * Never the link itself: this slot may be LEAKED past the link's death (a teardown
     * that could not retire the server's `free_ctx` pointer), and the gate is the one
     * object guaranteed to still be there to say so.
     */
    gate_t* gate = nullptr;
    int fd = -1; /**< @brief Peer socket; -1 => free slot. */
    /**
     * @brief Bumped every time this slot is CLAIMED — the half of a session's identity a
     *        descriptor number cannot supply (peers_m_).
     *
     * A socket NUMBER does not identify a session over time and neither does this slot's
     * ADDRESS: lwIP hands a closed descriptor's number straight back to the next accept,
     * and both claim sites reuse the first slot with `fd < 0`, so the departed peer's slot
     * object is exactly the one the new peer lands in. A queued send that resolved its
     * destination before the swap would therefore match on fd AND on slot pointer, and be
     * written to a stranger — which is why the identity carried through the TX path is the
     * pair (slot, gen) and not either half alone (#954). Compared, never interpreted.
     *
     * Wrapping is not a hazard worth spending a wider counter on: aliasing would need
     * 2^32 reconnections onto this one slot while a single work item sits in the control
     * queue, and that item drains within one pass of the httpd task's loop.
     */
    std::uint32_t gen = 0;
    /**
     * @brief This session's identity HANDLE, `(slot index, gen)` — what the peer-receiver
     *        seam tags every inbound frame with (#1294).
     *
     * The same pair @ref httpd_ws_link_t::session_ref_t carries, published to the routing
     * plane instead of kept private to the TX path: a session ref is one SUPPLIER of a
     * handle, not the handle itself (#1294 ruling 1), and this is where this link supplies
     * one. Stamped at the claim beside @ref name and RETIRED (generation 0) by
     * @ref httpd_ws_link_t::reclaim_slot, so a handle minted against a departed tab never
     * matches whoever lands on the slot next. Guarded by `peers_m_`, like the name.
     */
    peer_handle_t handle;
    bool open = false; /**< @brief True between handshake and close. */
    /**
     * @brief The peer's ROUTABLE name — `p<slot>` (ADR-0073 §2, #426, #994).
     *
     * The slot index, and nothing else, because this string is what the graph spells: a
     * peer-named link's synthesized `:children[]` lists it and a `dst` path addresses the
     * session back through it. `graph::valid_segment` rejects both `.` and `:`
     * (`core/include/libtracer/path.hpp:56`), so the `<ip>:<port>` this used to hold could
     * be ENUMERATED and never ADDRESSED — the enumerable⇒addressable invariant broken on
     * every node, which is the precondition RFC-0020 §6 states for the NAME-hop rejection.
     * The physical address did not disappear with it; it moved to @ref endpoint_str.
     *
     * A pure function of the slot's position, so a recycled slot gets the SAME name back
     * (@ref httpd_ws_link_t::reclaim_slot moves the old string out for the eviction seam),
     * exactly as core's own bus servers do it (`core/src/posix_endpoint.cpp:406`). It also
     * fits every libstdc++ small-string buffer, so a claim no longer heap-allocates a name.
     */
    std::string name;
    /**
     * @brief The peer's `<ip>:<port>` — DIAGNOSTICS ONLY, never a path segment.
     *
     * ADR-0073 §2's other half: the address string leaves the graph but not the operator's
     * hands, because `p3` alone cannot be matched to a physical client. Reported through
     * @ref httpd_ws_link_t::peer_stats_t::endpoint_str and named in the strike log.
     *
     * A fixed array rather than a `std::string`: it is written once per claim and read on
     * a stalled-send path, so the array costs no heap chunk for the session's whole life
     * and cannot fragment the small-heap targets this link is built for. Stamped at the
     * claim edge — where the socket is still healthy — because `getpeername` is least
     * likely to answer at the moment the strike log needs the name.
     */
    char endpoint_str[kEndpointChars] = {};
    asm_buf_t asm_buf;         /**< @brief RFC 6455 fragment reassembly (nothrow). */
    std::uint8_t tx_drops = 0; /**< @brief Consecutive failed sends (peers_m_). */
    /**
     * @brief This peer has been condemned — no frame may reach its socket again (peers_m_).
     *
     * Set at the INSTANT the link decides the session is broken (the strike cap, or one
     * short write), not when a close eventually runs. That distinction is the whole of
     * #835's second round: the decision is local and immediate, the close is not, and
     * everything the link does between them has to already be behaving as if the peer
     * were gone. It gates three things — @ref httpd_ws_link_t::queue_send refuses new
     * frames, @ref httpd_ws_link_t::tx_work skips frames already queued, and both
     * accounting paths stop treating this session as evidence about anything.
     *
     * Cleared with the rest of the slot in @ref httpd_ws_link_t::reclaim_slot. Slots are
     * recycled in place and lwIP hands a descriptor NUMBER straight back, so a mark that
     * outlived its session would mute an unrelated peer; the reclaim runs from `free_ctx`
     * on the httpd task, before that task can accept anything onto the number.
     */
    bool dead = false;
    /**
     * @brief A frame write is OPEN on this session: bytes landed since @ref open_tx_frame
     *        belong to a frame the peer has been promised in full (httpd task only).
     *
     * `esp_http_server` writes ONE WebSocket frame as TWO calls to the session's send
     * function — the header, then the payload (httpd_ws.c) — so the override cannot tell,
     * from one buffer, whether a failure loses a whole frame or truncates one already
     * announced on the wire. That distinction is the difference between the two policies
     * this link runs (#951), and nothing IDF hands the override carries it. So the frame
     * boundary is marked here, by the one caller that knows it: @ref
     * httpd_ws_link_t::tx_work brackets the async send.
     *
     * Not under `peers_m_`, unlike every other cross-thread member of this slot, and not
     * atomic: all four touch points — the two brackets and the override's accumulate and
     * verdict — run on the httpd task, and `httpd_ws_send_frame_async` is synchronous, so
     * the window cannot interleave with another frame's. Same confinement `asm_buf` has
     * on the receive side.
     */
    bool tx_frame_open = false;
    /**
     * @brief Bytes of that frame the socket has already accepted (httpd task only).
     *
     * Zero with @ref tx_frame_open set means the frame's FIRST write is the one being
     * judged, which is exactly the case #481's "drop the frame, keep the socket" was
     * written for: nothing of it reached the peer.
     */
    std::size_t tx_frame_bytes = 0;
    /** @brief Open a frame-write window — the bracket @ref httpd_ws_link_t::tx_work opens. */
    void open_tx_frame() noexcept {
        tx_frame_bytes = 0;
        tx_frame_open = true;
    }
    /**
     * @brief Close it, and report how many bytes of that frame reached the wire.
     *
     * Non-zero on a FAILED send is the whole finding: the peer is holding a header whose
     * payload will never arrive.
     */
    [[nodiscard]] std::size_t close_tx_frame() noexcept {
        tx_frame_open = false;
        return tx_frame_bytes;
    }
    /**
     * @brief This session has not authenticated yet — it exists, and it is served NOTHING
     *        (peers_m_).
     *
     * False for every session on a link with no auth hook, which is what makes the whole
     * feature inert until one is installed. While true the session is skipped by
     * `enumerate_peers`, `enumerate_peer_stats`, `peer_link` and the `send` fan-out, and its
     * inbound messages go to the hook instead of the graph — the four gates that together
     * mean "admitted at the transport level, serving nothing".
     */
    bool auth_pending = false;
    /**
     * @brief `esp_timer_get_time()` value at which an @ref auth_pending session is closed
     *        with `kCloseAuthTimeout` (peers_m_); 0 when there is no deadline to keep.
     *
     * Stamped ONCE, at the claim, and deliberately never extended by the peer's own traffic:
     * a scheme that needs several frames gets the whole window for all of them, and a peer
     * that could refresh the deadline by sending anything at all would have no deadline.
     */
    std::int64_t auth_deadline_us = 0;
    /**
     * @brief The identity the hook bound on ACCEPT, NUL-terminated (peers_m_).
     *
     * A fixed array for the same reason @ref endpoint_str is one: written once per session,
     * read on a diagnostic path, and never worth a heap chunk on a small-heap target.
     */
    char subject[httpd_ws_link_t::kMaxSubjectChars + 1] = {};
    /**
     * @brief The resolution handle currently naming THIS session, or null (peers_m_).
     *
     * Not an endpoint OF the slot — the slot no longer owns one (#1013). It is a cache of
     * the handle `peer_link` minted for the session in it, so a second resolution between
     * two claims reuses the first's instead of consuming another pool entry: both
     * resolutions saw the same generation, so one object answers for both. Retired to the
     * link's free list the moment the session ends (@ref httpd_ws_link_t::retire_resolution),
     * which is what bounds the pool to the peer population.
     */
    peer_resolution_t* resolution = nullptr;
    /**
     * @brief Passive traffic counters for the session currently holding this slot
     *        (peers_m_, like everything else cross-thread here) — see link_stats.hpp.
     *
     * Zeroed on the CLAIM edge, not on reclaim: a reclaimed slot keeps its last
     * session's numbers until something takes it over, and enumeration skips closed
     * slots anyway, so there is nothing to hide and one less write on the close path.
     */
    link_counters_t st;
};

/**
 * @brief A destination the TX path can still verify when it finally runs: WHICH session,
 *        not which descriptor.
 *
 * The whole reason the TX path stopped carrying a bare `int fd` (#954). `send()` snapshots
 * its destinations under @ref peers_m_ and releases the lock before enqueueing, and
 * `httpd_queue_work` runs the item later still — one control message per pass of the
 * server loop, while accept and close proceed at full speed in that same pass. So between
 * resolving a destination and writing to it, the peer can hang up and an unrelated client
 * can be accepted onto the recycled descriptor. Everything the old path could ask at that
 * point answered the wrong question: `httpd_ws_get_fd_info` says only "SOME websocket
 * lives at this number", and a slots_ scan for `s->fd == fd` finds whoever holds it now.
 * The result was one peer's frames written into another peer's socket, and one peer's send
 * failures charged to a stranger's strike counter.
 *
 * The pair is minted under @ref peers_m_ at the instant the sender resolves the peer, and
 * both halves are load-bearing:
 *   - `slot` alone is NOT enough. Slots are recycled IN PLACE, so the address a departed
 *     peer's send recorded is the address the next peer is handed — which is also why
 *     comparing the server's session ctx pointer (what @ref detach_req_t does, correctly,
 *     for a teardown that admits no new peers) does not close this hole on the live path.
 *   - `gen` alone is NOT enough: two different slots are routinely at the same generation.
 *
 * Together they are unique for as long as any work item can run, because slot addresses
 * are stable for the link's life (grown, never shrunk; the abandon path leaks rather than
 * frees precisely so that stays true) and a claim always bumps the generation. Validated
 * at every TX site through @ref httpd_ws_link_t::live_fd, and a reference that has gone
 * stale SINCE IT WAS MINTED always FAILS the check rather than resolving to whoever now
 * holds the descriptor.
 *
 * @note The guarantee is exactly as good as the moment of MINTING, which is why the
 *       directed path no longer mints here at all. A caller that resolved a peer, was
 *       preempted, and sent afterwards used to mint from the slot's CURRENT generation,
 *       so the check was self-satisfying for whoever held the slot by then (#1013). The
 *       generation is now captured at RESOLVE time and carried by
 *       @ref httpd_ws_link_t::peer_resolution_t, and this pair is COPIED out of it — so
 *       a directed send can never observe a newer generation than the one its caller
 *       resolved against. The broadcast producer still mints, correctly: it resolves its
 *       destinations and enqueues them inside one call, with no window between.
 */
struct httpd_ws_link_t::session_ref_t {
    session_t* slot = nullptr; /**< @brief The peer slot; compared, never dereferenced blind. */
    std::uint32_t gen = 0;     /**< @brief @ref session_t::gen at the moment of minting. */
};

/**
 * @brief What @ref httpd_ws_link_t::peer_link hands back: ONE RESOLUTION of a peer name,
 *        carrying the session identity it resolved against (#1013).
 *
 * `send()` writes a BINARY frame to that SESSION's socket only (via the owning link's
 * httpd send queue), and is a counted no-op once that session has departed.
 *
 * The type this replaced was one object per SLOT for the link's life, so it had nothing
 * of its own to check a send against and re-read the slot's generation when the send
 * finally ran. That read is the whole of #1013: between a caller resolving a peer and
 * that caller sending, the httpd task can close the session and accept another onto the
 * same descriptor — a browser reload — and a generation minted after the swap describes
 * the STRANGER, so the check passed and one authenticated session's directed reply was
 * written into another's socket. Stamping the shared per-slot object at resolve time was
 * explicitly rejected as the fix: two callers resolving one slot at different generations
 * overwrite each other's stamp, which narrows the window while reading like a closure.
 *
 * So the identity is captured HERE, at the resolve, and @ref gen_ is never re-read from
 * the slot. A send validates the slot against the generation the CALLER saw; a session
 * that departed in between fails that test and the frame is dropped and counted, exactly
 * as a send to a peer that had already gone is.
 *
 * Lifetime: handles come from @ref httpd_ws_link_t::resolutions_, are recycled through
 * its free list when their session ends, and — like the peer slots — are pointer-valid
 * for the link's life, because the teardown that cannot retire the server's callbacks
 * leaks them rather than freeing them.
 *
 * It keeps the base `link_up()` (`true`) DELIBERATELY, and that is the one place in this
 * component where the #1059 liveness question is answered by the default rather than by
 * state: a session's `open`/`dead` flags live under `peers_m_` as plain bools, so an
 * honest answer here would mean either taking that mutex inside a `noexcept` poll (which
 * the httpd task can already be holding) or mirroring the flags into an atomic that could
 * drift from them. Nothing polls this handle — it is a private type handed to the routing
 * plane purely to SEND, and a departed peer is reported to that plane by the eviction
 * seam, not by a poll — so the mirror would buy an unused answer with a new divergence.
 * Revisit if a puller ever appears (#1203).
 */
class httpd_ws_link_t::peer_resolution_t final : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override;
    /** @brief Directed scatter-gather send: gathered once into the nothrow tx work buffer
     *         (no intermediate flatten temporary — see the owning link's iovec
     *         @ref httpd_ws_link_t::send). */
    void send(std::span<const std::span<const std::byte>> iov) override;

   private:
    friend class httpd_ws_link_t;
    httpd_ws_link_t* owner_ = nullptr; /**< @brief The resolving link; null once retired
                                        *          into inert memory by a teardown. */
    session_t* slot_ = nullptr;        /**< @brief The slot this resolution named. */
    /**
     * @brief @ref session_t::gen AT THE RESOLVE — the field that closes #1013.
     *
     * Written once per stamping, under `peers_m_`, and only ever COMPARED afterwards.
     * Nothing re-reads the slot's generation into it, which is the entire difference
     * between this type and the per-slot endpoint it replaced.
     */
    std::uint32_t gen_ = 0;
    /** @brief Free-list link while retired; meaningless while stamped (peers_m_). */
    peer_resolution_t* free_next_ = nullptr;
};

/**
 * @brief One queued outbound frame: the payload is gather-copied ONCE, nothrow, so
 *        it outlives the send() caller's spans until the httpd task drains the work
 *        item.
 *
 * ONE storage shape since #949: the work item IS a @ref tx_slot_t member, and `payload`
 * points at the slot's inline buffer — or at `owned` for the one frame shape that outgrows
 * it, which keeps the pooled shell and takes a nothrow heap payload. There is no
 * shell-on-the-heap arm any more: a send that finds the pool exhausted is dropped and
 * counted, so the number of items this link can have outstanding in the control queue is
 * the pool size and nothing else. Never a `std::vector` for the copy — the vector's
 * THROWING allocator inside a braced initializer defeated the `new (std::nothrow)` guard on
 * the shell: under `-fno-exceptions` a reply-sized copy hitting heap exhaustion aborted the
 * node (the browser-session crash).
 */
struct httpd_ws_link_t::tx_work_t {
    httpd_handle_t handle = nullptr; /**< @brief Owning httpd instance. */
    /**
     * @brief The owning link's gate — how @ref httpd_ws_link_t::tx_work reaches the link
     *        to record the send's outcome, and never the link itself.
     *
     * Same reasoning as @ref httpd_ws_link_t::session_t::gate: this item can drain on the
     * adopted server's task after the link is gone, so the ONE pointer it may follow is
     * the object designed to outlive it. A null link behind the gate means the teardown
     * shut it and the outcome has nobody left to inform.
     */
    gate_t* gate = nullptr;
    /**
     * @brief Destination SESSION — re-validated at drain time, never a bare descriptor.
     *
     * The item is enqueued on one task and written on another, arbitrarily later, so the
     * fd it was gathered for may by then belong to somebody else entirely. Carrying the
     * session reference is what makes a late item skippable instead of misdeliverable;
     * @ref httpd_ws_link_t::tx_work resolves it back to a socket through
     * @ref httpd_ws_link_t::live_fd and sends nothing when that fails (#954).
     */
    session_ref_t to;
    std::byte* payload = nullptr; /**< @brief Gathered frame bytes (slot-inline or `owned`). */
    std::size_t len = 0;          /**< @brief Frame length, bytes. */
    /**
     * @brief The slot this item lives inside — bound once in @ref
     *        httpd_ws_link_t::alloc_buffers and never rewritten, so it is stable for the
     *        link's life and never null on a queued item.
     */
    tx_slot_t* slot = nullptr;
    std::unique_ptr<std::byte[]> owned; /**< @brief Heap payload (oversize frames only). */
};

/**
 * @brief One pre-allocated TX work slot: claimed lock-free (a CAS on @ref busy) by
 *        any sending task in @ref claim_tx_slot, released by the httpd task once
 *        its send drains (@ref release_tx_work) — so a steady-state send allocates
 *        nothing. The pool (`tx_slots_total_` of these — `tx_pool_slots_` claimable by any
 *        sender plus the in-call reserve) is allocated once per link.
 *
 * A single flag is the WHOLE lifetime, and that is a statement about the ESP-IDF floor
 * rather than about this file. A claimed slot is released only by the work item that was
 * enqueued for it, so the flag is sound exactly while "`httpd_queue_work` returned ESP_OK"
 * implies "this item will run". Below ESP-IDF 5.5.5 it did not: the call was a bare
 * non-blocking `sendto` on the loopback control socket, and an enqueue past
 * CONFIG_LWIP_UDP_RECVMBOX_SIZE was discarded inside lwIP with ESP_OK still returned, so
 * the item never ran and its slot was pinned for the rest of the boot (#944). From 5.5.5
 * the mbox slot is reserved through a counting semaphore BEFORE the `sendto`
 * (esp_http_server `httpd_main.c`), so a full queue is an `ESP_FAIL` the caller sees, and
 * the enqueue that succeeds silently and never runs does not exist. `idf_component.yml`
 * requires that floor, which is what let the four-state lifetime, its age stamp and its
 * sweep be deleted rather than kept for a condition that cannot arise (#949).
 *
 * @note The close path's `shutdown` (@ref httpd_ws_link_t::condemn) is NOT this deletion's
 *       business and stays exactly as it is: it exists because a close asked of a FULL
 *       control queue does not land, and a fail-fast refusal is still a close that did not
 *       land. Only the silent-success case went away.
 */
struct httpd_ws_link_t::tx_slot_t {
    std::atomic<bool> busy{false};   /**< @brief Claimed flag (acquire/release). */
    tx_work_t work;                  /**< @brief The slot's embedded work item. */
    std::byte* inline_buf = nullptr; /**< @brief This slot's slice of the link's inline
                                      *          payload block (@ref httpd_ws_link_t::tx_inline_),
                                      *          bound once in alloc_buffers. */
};

/**
 * @brief The teardown session-detach work item — everything @ref detach_work needs, and
 *        NOTHING that belongs to the link.
 *
 * Deliberately self-contained (a server handle and a snapshot of fds): the item may be
 * sitting in the adopted server's control queue when the destructor gives up on it, so
 * touching the link from the work function would reintroduce exactly the
 * use-after-free this fixes. Ownership is settled by @ref released — the destructor and
 * the work function each exchange it once, and the SECOND one to do so deletes the
 * item; a work item the server never runs is a single small leak, never a double free.
 *
 * The fds are paired with the ctx pointer each one was carrying at snapshot time,
 * because a socket NUMBER does not identify a session over time: `httpd_sess_get`
 * resolves purely by fd, so an item that drains late — after the peer hung up and the
 * shared server accepted an unrelated client onto the recycled descriptor — would
 * otherwise run a stranger's `free_ctx` mid-life and force its session closed. The
 * ctx pointers are COMPARED, never dereferenced, and the abandon path leaks every slot
 * precisely so those addresses stay unique for as long as this item can run.
 */
struct httpd_ws_link_t::detach_req_t {
    httpd_handle_t handle = nullptr;   /**< @brief The adopted server (still running). */
    std::unique_ptr<int[]> fds;        /**< @brief Snapshot of the open peers' sockets. */
    std::unique_ptr<void*[]> ctxs;     /**< @brief The ctx each fd carried (identity only). */
    std::size_t n = 0;                 /**< @brief Entries in @ref fds / @ref ctxs. */
    std::atomic<bool> done{false};     /**< @brief Set once every fd has been detached. */
    std::atomic<bool> released{false}; /**< @brief Ownership handshake (see the brief). */
};

/**
 * @brief The @ref httpd_ws_link_t::close_peer work item: the session identity to close,
 *        and the gate to reach the link through — NOTHING that belongs to the link.
 *
 * Heap-allocated per call and freed by @ref httpd_ws_link_t::close_work, which is
 * affordable because close_peer is an administrative action, never a data-path one — the
 * no-allocation discipline protects the per-frame paths, and a revocation is not one.
 * Nothrow, like every allocation here: a failed `new` is a `false` to the caller, not an
 * abort. The gate rather than the link for the same reason @ref
 * httpd_ws_link_t::tx_work_t carries one: the item can drain on the adopted server's task
 * after the link is gone, and the gate is the one object designed to outlive it.
 */
struct httpd_ws_link_t::close_req_t {
    gate_t* gate = nullptr; /**< @brief The owning link's gate (see the brief). */
    session_ref_t to;       /**< @brief The session to close, identified as (slot, gen). */
};

httpd_ws_link_t::httpd_ws_link_t(std::uint16_t bind_port, std::size_t max_peers, bool peer_named,
                                 std::uint32_t send_timeout_ms, std::uint32_t auth_deadline_ms,
                                 std::size_t rx_scratch_bytes, std::size_t tx_pool_slots,
                                 std::size_t tx_inline_bytes)
    : port_(bind_port),
      max_peers_(max_peers),
      auth_deadline_us_(resolve_auth_deadline_us(auth_deadline_ms)),
      peer_named_(peer_named),
      rx_scratch_bytes_(resolve_size(rx_scratch_bytes, kDefaultRxScratchBytes)),
      tx_inline_bytes_(resolve_size(tx_inline_bytes, kDefaultTxInlineBytes)),
      tx_pool_slots_(resolve_size(tx_pool_slots, kDefaultTxPoolSlots)),
      tx_slots_total_(tx_pool_slots_ + kTxReplySlots) {
    if (!open_gate()) return;  // ok() stays false; nothing was registered
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = bind_port;
    // A SECOND httpd instance must not share the first's control UDP port — the SPA
    // httpd (web_server.c on :80) keeps the default, so offset ours by one or
    // httpd_start fails to bind the control socket.
    cfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    cfg.stack_size = kRequiredHttpdStack;
    // Room for `max_peers` clients plus slack (see kInternalSockSlack). 0 = unbounded:
    // pick a sane finite socket budget (the shared lwIP pool is small).
    const std::size_t peers = max_peers != 0 ? max_peers : kDefaultPeerCap;
    cfg.max_open_sockets = static_cast<std::uint16_t>(peers + kInternalSockSlack);
    // Do NOT LRU-evict an existing client: at the cap we refuse the NEW peer in the
    // handshake handler, never drop a live graph peer mid-stream (transport_ws_server's
    // admission contract). lru_purge would silently sever an in-flight subscriber.
    cfg.lru_purge_enable = false;
    // The bound WS sockets get at admission — never the server's, which still governs
    // REST responses on this same instance (see bound_socket).
    send_timeout_ms_ = clamp_send_timeout_ms(
        send_timeout_ms != 0 ? send_timeout_ms : derive_send_timeout_ms(peers),
        cfg.send_wait_timeout);

    httpd_handle_t started = nullptr;
    if (httpd_start(&started, &cfg) != ESP_OK) {
        handle_.store(nullptr, std::memory_order_relaxed);  // ok() stays false
        return;
    }
    // Publish only once httpd_start has filled it. `handle_` is atomic since #963, so it
    // cannot be handed to the API by address any more — and that is the better shape
    // regardless: no producer can observe a half-initialised handle.
    handle_.store(started, std::memory_order_relaxed);
    uri_ = "/";            // owns_httpd_ stays true; the dtor stops the server, but keep uri_
                           // coherent with the adopting path (both register the same handler).
    httpd_uri_t uri = {};  // zero-init, then set fields by name (the struct's tail members
    uri.uri = "/";         // sit behind Kconfig, so positional init would not be stable)
    uri.method = HTTP_GET;
    uri.handler = &httpd_ws_link_t::ws_handler;
    // The GATE, never `this` — esp_http_server latches this pointer into every upgraded
    // session and keeps dispatching through it until that session is deleted, which can
    // be long after this link is gone. See gate_t.
    uri.user_ctx = gate_.load(std::memory_order_relaxed);
    uri.is_websocket = true;
    // The ONE call that sees the opening GET: the server answers the handshake itself and
    // returns without invoking `handler` for it, so the admission predicate has to sit
    // here or it never sees a peer's credentials at all. Present unconditionally — the
    // component's Kconfig selects CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT.
    uri.ws_pre_handshake_cb = &httpd_ws_link_t::ws_pre_handshake;
    uri.handle_ws_control_frames = false;  // httpd answers PING/PONG and tracks CLOSE
    if (httpd_register_uri_handler(handle_.load(std::memory_order_relaxed), &uri) != ESP_OK) {
        httpd_stop(handle_.load(std::memory_order_relaxed));
        handle_.store(nullptr, std::memory_order_relaxed);
        return;
    }
    alloc_buffers();
}

httpd_ws_link_t::httpd_ws_link_t(httpd_handle_t external, const char* uri_pattern,
                                 std::size_t max_peers, bool peer_named,
                                 std::uint32_t send_timeout_ms, std::uint32_t auth_deadline_ms,
                                 std::size_t rx_scratch_bytes, std::size_t tx_pool_slots,
                                 std::size_t tx_inline_bytes)
    : max_peers_(max_peers),
      auth_deadline_us_(resolve_auth_deadline_us(auth_deadline_ms)),
      peer_named_(peer_named),
      rx_scratch_bytes_(resolve_size(rx_scratch_bytes, kDefaultRxScratchBytes)),
      tx_inline_bytes_(resolve_size(tx_inline_bytes, kDefaultTxInlineBytes)),
      tx_pool_slots_(resolve_size(tx_pool_slots, kDefaultTxPoolSlots)),
      tx_slots_total_(tx_pool_slots_ + kTxReplySlots) {
    if (!open_gate()) return;  // ok() stays false; nothing was registered
    // The adopted server's httpd_config_t belongs to the caller and esp_http_server
    // exposes no reader for it, so the clamp uses IDF's default send_wait_timeout — the
    // value that server has unless its owner tightened it, in which case the socket
    // option we set at admission is the tighter of the two anyway.
    const httpd_config_t defaults = HTTPD_DEFAULT_CONFIG();
    send_timeout_ms_ = clamp_send_timeout_ms(
        send_timeout_ms != 0 ? send_timeout_ms : derive_send_timeout_ms(max_peers),
        defaults.send_wait_timeout);
    // `max_peers == 0` means two different things to two readers, and ONLY here can they
    // disagree without limit (#956): to admission it means UNBOUNDED (`on_data_frame`
    // guards the cap with `if (max_peers_ != 0)`), while the derivation above substitutes
    // kDefaultPeerCap. The owning constructor is self-consistent — the same substitution
    // also sets its socket budget, so the assumption is enforced — but an adopted server's
    // real ceiling is the caller's `max_open_sockets`, and esp_http_server exposes no
    // reader for it. So the mismatch cannot be detected, only DECLARED: a node whose
    // adopted server admits more peers than this gets a bound derived for a population it
    // does not limit, and nothing else would say so. Whether 0 should stay a legal value
    // in adopted mode at all is an API question this does not decide.
    if (max_peers == 0 && send_timeout_ms == 0)
        ESP_LOGW(kTag,
                 "max_peers=0 on an ADOPTED server: send bound derived against an assumed "
                 "cap of %u (%u ms), but admission is unbounded — pass the server's real "
                 "max_open_sockets, or an explicit send_timeout_ms",
                 (unsigned)kDefaultPeerCap, (unsigned)send_timeout_ms_);
    // Adopt an already-running server (the firmware's :80 SPA httpd): register the WS URI
    // as one more handler on it rather than standing up a second esp_http_server. We do
    // NOT own the server, so port_ is 0 (no bind of ours) and the dtor must never
    // httpd_stop it — only unregister the URI. No cfg / ctrl_port / httpd_start here: with
    // one server the control-UDP-port clash the owning ctor guards against cannot arise.
    handle_.store(external, std::memory_order_relaxed);
    owns_httpd_ = false;
    port_ = 0;
    uri_ = uri_pattern;

    httpd_uri_t uri = {};    // zero-init, then set fields by name (the struct's tail members
    uri.uri = uri_.c_str();  // sit behind Kconfig, so positional init would not be stable)
    uri.method = HTTP_GET;
    uri.handler = &httpd_ws_link_t::ws_handler;
    uri.user_ctx = gate_.load(std::memory_order_relaxed);  // the GATE, never `this` — see
                                                           // gate_t
    uri.is_websocket = true;
    // Same admission point as the owning ctor, and per-URI: registering it here gates the
    // handshake for THIS link's WS URI only, leaving the adopted server's other routes
    // exactly as its owner configured them.
    uri.ws_pre_handshake_cb = &httpd_ws_link_t::ws_pre_handshake;
    uri.handle_ws_control_frames = false;  // httpd answers PING/PONG and tracks CLOSE
    if (httpd_register_uri_handler(external, &uri) != ESP_OK) {
        handle_.store(nullptr, std::memory_order_relaxed);  // ok() stays false; do NOT httpd_stop —
                                                            // we do not own the server
        return;
    }
    alloc_buffers();
}

bool httpd_ws_link_t::open_gate() {
    // Nothrow, and load-bearing: the gate is what makes the registered handler safe to
    // dispatch after this link dies, so a link that could not allocate one must not
    // register a handler at all. Both constructors bail to ok() == false on failure.
    gate_t* const g = new (std::nothrow) gate_t;
    if (g == nullptr) return false;
    g->link = this;
    gate_.store(g, std::memory_order_relaxed);
    return true;
}

void httpd_ws_link_t::close_gate() {
    if (gate_.load(std::memory_order_relaxed) == nullptr) return;
    // A destructor reached from INSIDE the URI handler — an app teardown driven by the
    // very frame being serviced (#814) — IS the in-flight frame, so waiting for `depth`
    // to reach zero would wait on a stack frame below this one. Running on the server's
    // task is the proof of that, and equally the proof that no OTHER frame can be in
    // flight: esp_http_server dispatches every request from that one task.
    const bool on_server_task = server_task_.load(std::memory_order_relaxed) == current_task();
    std::unique_lock lock(gate_.load(std::memory_order_relaxed)->m);
    // From here no dispatch can enter the link — ws_handler refuses (httpd closes that
    // socket) and on_session_closed is inert. Both were reachable a moment ago through
    // pointers the adopted server latched and no API of ours can revoke.
    gate_.load(std::memory_order_relaxed)->link = nullptr;
    // Unbounded BY DESIGN, unlike the two drains: there is no leak-instead-of-free
    // fallback for the link itself, so a handler frame still reading peers_m_ /
    // rx_scratch_ / slots_ has to be joined, not out-waited. It is bounded in practice by
    // the app callback the delivery runs, and the one way it could wait on itself is the
    // case skipped above. Since #960 a departure notification in flight is joined here
    // too: it dereferences the link and hands the routing plane a name, and it runs with
    // `m` released precisely so this wait — not the mutex — is what holds it.
    if (!on_server_task)
        gate_.load(std::memory_order_relaxed)->cv.wait(lock, [this] {
            return gate_.load(std::memory_order_relaxed)->depth == 0;
        });
}

void httpd_ws_link_t::alloc_buffers() {
    // Both are once-per-link and nothrow, and they fail DIFFERENTLY since #949. RX stays
    // optional — a frame just takes the per-frame nothrow buffer it already takes when it
    // outgrows the scratch. TX is not optional any more: the pool is the only place an
    // outbound frame can be gathered, so a link that could not allocate one drops every
    // send on the counted enqueue-drop path (@ref note_enqueue_drop) instead of quietly
    // moving a hot publish path onto the global heap.
    rx_scratch_.reset(new (std::nothrow) std::byte[rx_scratch_bytes_]);
    // The size and the pointer must never disagree: on_data_frame decides "does this frame
    // fit the scratch" from rx_scratch_bytes_, so a failed allocation has to zero it or a
    // frame would be memcpy'd into nothing.
    if (rx_scratch_ == nullptr) rx_scratch_bytes_ = 0;
    tx_pool_.reset(new (std::nothrow) tx_slot_t[tx_slots_total_]);
    tx_inline_.reset(new (std::nothrow) std::byte[tx_slots_total_ * tx_inline_bytes_]);
    // Two allocations, one pool: a slot with no payload storage behind it is not a usable
    // slot, so the pair fails together. Dropping the array is what puts every send on the
    // counted enqueue-drop path (@ref note_enqueue_drop) rather than leaving a claimable
    // slot whose inline_buf is null.
    if (tx_pool_ == nullptr || tx_inline_ == nullptr) {
        tx_pool_.reset();
        tx_inline_.reset();
        return;
    }
    // Bind each slot to its embedded work item ONCE, here, and never again. The
    // back-pointer is a property of the slot, not of the claim: the work item is how the
    // httpd task finds the slot to release, and a claimer re-storing the same value into it
    // while that release is in flight would be a plain data race for no gain. The slot's
    // slice of the inline block is bound on the same terms and for the same reason.
    for (std::size_t i = 0; i < tx_slots_total_; ++i) {
        tx_pool_[i].work.slot = &tx_pool_[i];
        tx_pool_[i].inline_buf = tx_inline_.get() + i * tx_inline_bytes_;
    }
}

httpd_ws_link_t::tx_slot_t* httpd_ws_link_t::claim_tx_slot(bool in_call) {
    // Teardown gate: once the dtor is draining, new sends must stop claiming slots or the
    // drain never converges (unregistering the URI stops RX only — subscription pushers
    // keep sending until the router detaches the transport). They are dropped and counted,
    // which is the same answer an exhausted pool gets, so nothing runs past the dtor.
    if (tx_pool_ == nullptr || stopping_.load(std::memory_order_relaxed)) return nullptr;
    const auto take = [this](std::size_t i) {
        bool expected = false;
        return tx_pool_[i].busy.compare_exchange_strong(expected, true, std::memory_order_acquire);
    };
    // The reserve sits PAST the pool (kTxReplySlots, #1218), and an in-call send takes it
    // FIRST. The asymmetry is the point: a send issued on the httpd task — a request's reply,
    // serviced in-call — cannot wait for a drain the asking task is itself supposed to
    // perform, while an off-task producer can, so a slot it does not take costs it latency
    // and never a frame. Reaching for the reserve before the pool is what keeps that
    // guarantee from being paid for out of the fan-out width: a reply in flight leaves all
    // tx_pool_slots_ claimable, so a producer sweep that fits the pool still fits it.
    if (in_call)
        for (std::size_t i = tx_pool_slots_; i < tx_slots_total_; ++i)
            if (take(i)) return &tx_pool_[i];
    for (std::size_t i = 0; i < tx_pool_slots_; ++i)
        if (take(i)) return &tx_pool_[i];
    return nullptr;  // every slot in flight this instant — the caller waits, or drops
}

httpd_ws_link_t::tx_slot_t* httpd_ws_link_t::claim_tx_slot_waiting() {
    // "Am I the drain?" is the whole fork. esp_http_server runs every queued send on ONE
    // task, so a send issued on it can only be served after the current handler frame
    // returns: waiting there would wait on this stack frame, which is the #814 shape and a
    // guaranteed deadlock for the full send bound. In-call sends take their reserve, or the
    // pool as they find it, and drop when both are full — they never wait.
    const bool in_call = server_task_.load(std::memory_order_relaxed) == current_task();
    if (tx_slot_t* const slot = claim_tx_slot(in_call); slot != nullptr) return slot;
    const std::int64_t bound_us = tx_wait_bound_us(send_timeout_ms_);
    if (in_call || bound_us <= 0) return nullptr;
    const std::int64_t started_us = esp_timer_get_time();
    // The futility latch. The bound below is per SEND, and a fan-out asks per DESTINATION —
    // so against an httpd task that is not draining at all, a 12-edge sweep would pay it
    // twelve times over and park its producer for twelve send bounds. Once ONE wait has
    // expired, the answer for the rest of that sweep is already known, so the pool is taken
    // as-is (drop and count) until the latch lapses. A pass therefore costs at most one
    // bound of latency before it goes back to being honest, counted loss.
    if (started_us < tx_wait_futile_until_us_.load(std::memory_order_relaxed)) return nullptr;
    tx_pool_waits_.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t deadline_us = started_us + bound_us;
    do {
        // Off the CPU, never a spin: on a unicore target the httpd task is the only task
        // that can free a slot, and it does not run while this one is runnable.
        std::this_thread::sleep_for(std::chrono::milliseconds(kTxWaitSliceMs));
        // A destructor that started while this send was asleep is drained on a bound of its
        // own, so give up the moment it does rather than extending it by ours.
        if (stopping_.load(std::memory_order_relaxed)) return nullptr;
        if (tx_slot_t* const slot = claim_tx_slot(false); slot != nullptr) return slot;
    } while (esp_timer_get_time() < deadline_us);
    tx_wait_futile_until_us_.store(esp_timer_get_time() + bound_us, std::memory_order_relaxed);
    return nullptr;  // the drain is not keeping up — the caller drops and counts it
}

void httpd_ws_link_t::release_tx_work(tx_work_t* work) {
    // Every work item is a pool slot's own member since #949 — there is no heap shell to
    // free, and `slot` is bound once in alloc_buffers, so it is never null here.
    work->owned.reset();  // drop an oversize heap payload before the slot recycles
    work->slot->busy.store(false, std::memory_order_release);
}

httpd_ws_link_t::~httpd_ws_link_t() {
    // Owning mode: stop the task first so no handler / queued work touches slots being
    // freed. On device the node leaks this object (recv path lives for the process), so
    // this only runs in a host teardown — but keep it correct. Adopted mode: only
    // unregister our WS URI and leave the caller's server running — never stop a server
    // this link did not start.
    // Suppress departure notifications for the session closes THIS teardown provokes
    // (httpd_stop closes every session, re-entering on_session_closed) — the routing
    // plane the notifier targets may be tearing down alongside us.
    stopping_.store(true, std::memory_order_relaxed);
    // Retire the deadline sweep BEFORE the gate is shut, so the two steps compose: stopping
    // the timer means no NEW tick can be raised, and the close_gate below then joins the one
    // that may already be inside the barrier (either in the timer callback or in the sweep
    // work item the httpd task is running). Doing it the other way round would leave a
    // window where a tick starts against a link whose members are already being freed.
    //
    // A tick that was queued and not yet drained is safe to leave behind: auth_sweep_work
    // resolves the link through the gate and finds it null, exactly as every other latched
    // callback does.
    if (auth_timer_ != nullptr) {
        auto* const timer = static_cast<esp_timer_handle_t>(auth_timer_);
        (void)esp_timer_stop(timer);
        (void)esp_timer_delete(timer);
        auth_timer_ = nullptr;
    }
    // Shut the handler gate FIRST and join whatever frame is inside it. Unregistering the
    // URI does NOT do this: esp_http_server latched the route into each upgraded session,
    // so frames keep arriving at the handler regardless (see
    // gate_t). Once close_gate returns, no dispatch can reach a
    // single member of this link — which is also what makes the session snapshot below COMPLETE,
    // since no handler can still be about to claim a slot behind it.
    close_gate();
    if (handle_.load(std::memory_order_relaxed) != nullptr) {
        if (owns_httpd_) {
            httpd_stop(handle_.load(std::memory_order_relaxed));
        } else {
            // Courtesy only, now that the gate is shut: it stops new handshakes from
            // latching the route, so the population of stale sessions cannot grow.
            httpd_unregister_uri_handler(handle_.load(std::memory_order_relaxed), uri_.c_str(),
                                         HTTP_GET);
            detach_sessions();
        }
    }
    // Adopted mode: the caller's server keeps running after our URI is unregistered,
    // so a queued tx_work may still execute on its task while it references a pool
    // slot — wait (bounded) for the in-flight slots to drain before tx_pool_ dies.
    // Owning mode needs no wait: httpd_stop has halted the task, so nothing can touch
    // the pool afterwards (an undrained work item is simply never run).
    if (!owns_httpd_ && tx_pool_ != nullptr) {
        bool busy = true;
        for (int turn = 0; turn < kDrainTurns && busy; ++turn) {
            busy = false;
            for (std::size_t i = 0; i < tx_slots_total_; ++i)
                if (tx_pool_[i].busy.load(std::memory_order_acquire)) busy = true;
            if (busy) std::this_thread::sleep_for(std::chrono::milliseconds(kDrainSliceMs));
        }
        if (busy) {
            // The drain bound expired with a send still in flight. That is not a
            // corner case to power through: httpd_ws_send_frame_async can sit in
            // SO_SNDTIMEO for several seconds on one large frame, and a dtor running
            // on the adopting server's OWN task can never see its queued work drain
            // at all (the work runs on the task that is sleeping here). The in-flight
            // tx_work still reads the slot's payload and release-stores its busy flag,
            // so freeing the pool now is a use-after-free — leak it instead: a
            // bounded, teardown-only loss that the drained path never pays. tx_work
            // touches only the work item and the caller's still-running server handle,
            // never this link, so the leaked pool is the one allocation that must outlive
            // us — BOTH halves of it since #1160: the slot array and the inline payload
            // block the slots point into, which an in-flight send is still reading from.
            ESP_LOGW(kTag, "tx pool leaked at teardown: a queued send outlived the drain bound");
            (void)tx_pool_.release();
            (void)tx_inline_.release();
        }
    }
    // The gate outlives the link exactly when the server does. Owning mode: httpd_stop
    // has deleted every session, so nothing holds the pointer any more and it is freed.
    // Adopted mode: the set of sessions that latched it is unknowable (a peer that
    // upgraded and never sent a frame is invisible here) and no API deletes them on
    // demand, so it is LEAKED — one small block, teardown-only, and the price of a
    // handler that stays safe to dispatch forever. Nothing was registered when the
    // constructor failed, so that case frees it too.
    if (owns_httpd_ || handle_.load(std::memory_order_relaxed) == nullptr) {
        delete gate_.load(std::memory_order_relaxed);
    } else {
        ESP_LOGD(kTag, "handler gate leaked at teardown: the adopted server still routes to it");
    }
    gate_.store(nullptr, std::memory_order_relaxed);
    handle_.store(nullptr, std::memory_order_relaxed);
}

void httpd_ws_link_t::detach_work(void* req_arg) {
    auto* const req = static_cast<detach_req_t*>(req_arg);
    // Runs ON the adopted server's task (httpd_queue_work's whole contract), so the
    // session table is ours to touch for the duration — the one context in which it is.
    for (std::size_t i = 0; i < req->n; ++i) {
        const int fd = req->fds[i];
        if (fd < 0) continue;
        // IDENTITY gate, and the reason the snapshot carries ctx pointers at all. This
        // item can drain arbitrarily late — a full control queue delays it, and a
        // destructor that gave up on its bound leaves it queued for a server task that
        // may only recover minutes later. By then the peer may have hung up and the
        // SHARED server (the firmware's :80 SPA httpd serves other routes too) may have
        // accepted an unrelated client onto the recycled descriptor. httpd_sess_get
        // resolves by fd alone, so detaching blind would run that stranger's free_ctx
        // mid-life and force its live session closed. The stored ctx is compared, never
        // dereferenced; a mismatch means this is not our session any more, and the only
        // correct action on someone else's session is none.
        if (httpd_sess_get_ctx(req->handle, fd) != req->ctxs[i]) continue;
        // httpd_sess_set_ctx(.., nullptr, nullptr) is the detach: on a ctx CHANGE it runs
        // the session's current free_ctx there and then and stores the new pair. Both
        // being null is what makes the session inert forever after: httpd's own close
        // path early-outs on a null ctx. Our on_session_closed may fire from inside this
        // call; it resolves the link through the gate, so it is inert if the destructor
        // has already shut it, and harmless if it has not.
        httpd_sess_set_ctx(req->handle, fd, nullptr, nullptr);
        // Now that nothing can call back, closing is what finally retires the session's
        // latched WS route as well — the peer's socket would otherwise sit open on a URI
        // that no longer exists. Asynchronous, and we deliberately do NOT drain it: with
        // the gate shut, neither the close nor any frame that beats it can reach a link.
        (void)httpd_sess_trigger_close(req->handle, fd);
    }
    req->done.store(true, std::memory_order_release);
    if (req->released.exchange(true, std::memory_order_acq_rel)) delete req;
}

void httpd_ws_link_t::detach_sessions() {
    // The one session that CANNOT be detached: the request a URI handler frame this
    // destructor is nested inside is servicing (#814). For it, httpd_sess_set_ctx takes
    // its request-scoped branch — it edits the in-flight httpd_req_t, leaves the socket
    // table's ctx/free_ctx untouched, and explicitly does not run the outgoing callback
    // (httpd_req_cleanup does, after the handler returns, i.e. after this destructor and
    // this slot are gone). Calling it there would arm exactly the use-after-free this
    // path exists to remove, so that slot is neutralised-and-leaked instead and its
    // session left as it is: with the gate shut, the callback it still holds is inert.
    if (gate_.load(std::memory_order_relaxed) != nullptr) {
        int serving_fd = -1;
        {
            // `depth` counts departure notifications too since #960, so it alone no longer
            // implies a request scope — but `serving_fd` is set and cleared by @ref
            // ws_handler under this same lock and is -1 for every other holder, so the
            // pair still names exactly the request-scoped session and nothing else.
            const std::lock_guard lock(gate_.load(std::memory_order_relaxed)->m);
            if (gate_.load(std::memory_order_relaxed)->depth != 0)
                serving_fd = gate_.load(std::memory_order_relaxed)->serving_fd;
        }
        if (serving_fd >= 0) abandon_session(serving_fd);
    }

    std::size_t open_n = 0;
    {
        const std::lock_guard lock(peers_m_);
        for (const auto& s : slots_)
            if (s->open) ++open_n;
    }
    if (open_n == 0) return;  // no slot armed => no ctx of ours left to retire

    // Nothrow throughout: a teardown that cannot allocate must still be memory-safe, so
    // an OOM here takes the neutralise-and-leak path rather than skipping the detach.
    std::unique_ptr<detach_req_t> req(new (std::nothrow) detach_req_t);
    std::unique_ptr<int[]> fds(new (std::nothrow) int[open_n]);
    std::unique_ptr<void*[]> ctxs(new (std::nothrow) void*[open_n]);
    if (req == nullptr || fds == nullptr || ctxs == nullptr) {
        abandon_sessions();
        return;
    }
    {
        const std::lock_guard lock(peers_m_);
        std::size_t i = 0;
        for (const auto& s : slots_)
            if (s->open && i < open_n) {
                fds[i] = s->fd;
                ctxs[i] = s.get();  // the ctx this fd was armed with — identity only
                ++i;
            }
        req->n = i;
    }
    req->handle = handle_.load(std::memory_order_relaxed);
    req->fds = std::move(fds);
    req->ctxs = std::move(ctxs);

    detach_req_t* raw = req.release();
    bool detached = false;
    // Relaxed both ways: a match can only be observed by the task that stored it, and a
    // stale MISS on any other task is the safe direction (queue the work and wait, which
    // is what a task that is not the server must do anyway).
    if (server_task_.load(std::memory_order_relaxed) == current_task()) {
        // We ARE the server task (the dtor was reached from a work item or a handler on
        // it). Queued work could only run after we return, so waiting for it would
        // deadlock by construction — the #814 lesson. Being that task is exactly the
        // permission detach_work needs, so run it right here instead.
        detach_work(raw);
        detached = true;
    } else if (httpd_queue_work(handle_.load(std::memory_order_relaxed),
                                &httpd_ws_link_t::detach_work, raw) != ESP_OK) {
        ESP_LOGE(kTag, "session detach could not be queued (ctrl queue full)");
        delete raw;  // never queued => nobody else can own it
        raw = nullptr;
    } else {
        for (int turn = 0; turn < kDrainTurns && !detached; ++turn) {
            detached = raw->done.load(std::memory_order_acquire);
            if (!detached) std::this_thread::sleep_for(std::chrono::milliseconds(kDrainSliceMs));
        }
    }
    if (raw != nullptr && !detached)
        ESP_LOGE(kTag, "session detach did not run on the httpd task within the drain bound");
    if (raw != nullptr && raw->released.exchange(true, std::memory_order_acq_rel)) delete raw;
    if (!detached) abandon_sessions();
}

void httpd_ws_link_t::abandon_sessions() {
    std::size_t leaked = 0;
    {
        const std::lock_guard lock(peers_m_);
        // EVERY slot, not just the still-open ones. A closed slot's address is also in
        // the detach snapshot the server may still be holding, and that snapshot's
        // fd-reuse guard compares ctx POINTERS: freeing a shell here would let an
        // unrelated allocation land on its address and be mistaken for ours. Leaking the
        // whole set keeps those addresses unique for as long as the item can run, and
        // this path is already the loudly-logged, teardown-only loss.
        for (auto& s : slots_) {
            neutralise(s.get());
            (void)s.release();
            ++leaked;
        }
        slots_.clear();
        // The resolution handles go the same way, and for the same reason (#1013): the
        // routing plane may still hold one, and `neutralise` has just made every one of
        // them inert, so leaking the pool is what keeps that inert object at a valid
        // address. Freeing it here would turn a no-op send into a use-after-free — the
        // precedent the leaked slot shells beside it set (#815).
        for (auto& r : resolutions_) (void)r.release();
        resolutions_.clear();
        free_resolutions_ = nullptr;
        free_resolutions_tail_ = nullptr;
        free_resolutions_n_ = 0;
    }
    ESP_LOGW(kTag,
             "%u session slot(s) leaked at teardown: the adopted server still holds "
             "their close callback",
             (unsigned)leaked);
}

void httpd_ws_link_t::abandon_session(int fd) {
    const std::lock_guard lock(peers_m_);
    for (auto it = slots_.begin(); it != slots_.end(); ++it) {
        if ((*it)->fd != fd) continue;
        neutralise(it->get());
        (void)it->release();
        slots_.erase(it);
        ESP_LOGW(kTag, "session slot fd=%d leaked at teardown: it is the request in flight", fd);
        return;
    }
}

void httpd_ws_link_t::neutralise(session_t* slot) {
    // Take the slot out of service without freeing it. The server may still hold it as a
    // session ctx and WILL run free_ctx on it eventually; the gate already makes that
    // call inert, and leaving the shell allocated is what makes it land on valid memory.
    // Clearing `open`/`fd` also keeps it out of any later snapshot, and the resolution
    // handle's own null-owner case covers a directed send that outlives the link.
    slot->open = false;
    slot->fd = -1;
    slot->asm_buf.clear();
    retire_resolution(slot, /*inert=*/true);
}

// ---------------------------------------------------------------------------
// RX — runs on the esp_http_server task.
// ---------------------------------------------------------------------------

esp_err_t httpd_ws_link_t::ws_handler(httpd_req_t* req) {
    // req->user_ctx is the GATE (see gate_t), because
    // esp_http_server keeps dispatching through this pointer for as long as the SESSION lives —
    // unregistering the URI does not revoke it, and the link may be long gone. Resolving the link
    // through the gate is the admission test AND the barrier registration.
    auto* const gate = static_cast<gate_t*>(req->user_ctx);
    if (gate == nullptr) return ESP_FAIL;
    const int fd = httpd_req_to_sockfd(req);
    httpd_ws_link_t* self = nullptr;
    {
        const std::lock_guard lock(gate->m);
        // Null link => the destructor has shut the gate. Refusing makes httpd close this
        // socket, which is exactly what should happen to a session still routed at a
        // link that no longer exists — and it is the only reason it is safe to leave
        // those sessions behind.
        if (gate->link == nullptr) return ESP_FAIL;
        self = gate->link;
        ++gate->depth;  // the destructor blocks until this frame leaves
        gate->serving_fd = fd;
    }
    // Latch the server's task identity here, where we are provably running on it. Relaxed:
    // the only reader is a destructor asking whether it is itself that task — and then the
    // store and the load are on one task, so no ordering is at stake.
    self->server_task_.store(current_task(), std::memory_order_relaxed);
    // Data frames, and only data frames. esp_http_server answers the opening GET itself
    // and returns from httpd_uri() BEFORE `uri->handler`, so the handshake never arrives
    // here; the opening GET is serviced by ws_pre_handshake instead. A request that does
    // reach this handler with method GET is therefore a plain HTTP request on the WS URI,
    // not a peer — httpd_ws_recv_frame answers ESP_ERR_INVALID_STATE on a socket with no
    // handshake done, which fails the handler and closes it. That is the right answer and
    // it costs no branch of ours.
    const esp_err_t err = self->on_data_frame(req);
    // `self` may be DESTROYED by now: the delivery above runs the app in-call and the app
    // may tear this link down (#814). Only `gate`, which deliberately outlives it, may be
    // touched from here on.
    {
        const std::lock_guard lock(gate->m);
        --gate->depth;
        gate->serving_fd = -1;
    }
    gate->cv.notify_all();
    return err;
}

esp_err_t httpd_ws_link_t::ws_pre_handshake(httpd_req_t* req) {
    // The admission point. esp_http_server calls this from httpd_uri() with the opening
    // GET fully parsed — method, URI and every header readable through the ordinary
    // httpd_req_get_* accessors — and BEFORE it writes the 101 or latches the WS route
    // into the session. Answering anything but ESP_OK abandons the upgrade and closes the
    // socket, so a refusal costs the peer one HTTP request and this link nothing: no slot,
    // no session ctx, no socket policy, no entry in the peer set.
    //
    // It is registered with the same `user_ctx` the handler is — the GATE, never `this`
    // (see gate_t) — and resolves the link through it the same way,
    // because the ONLY thing that makes it safe to dereference the link here is the barrier the
    // gate supplies: a concurrent destructor takes `m` to null the link and then joins on `depth`,
    // so either this call finds a null link and refuses, or the destructor waits for the predicate
    // to return.
    auto* const gate = static_cast<gate_t*>(req->user_ctx);
    if (gate == nullptr) return ESP_FAIL;
    admission_fn_t fn = nullptr;
    admission_verdict_fn_t verdict_fn = nullptr;
    void* ctx = nullptr;
    {
        const std::lock_guard lock(gate->m);
        if (gate->link == nullptr) return ESP_FAIL;
        httpd_ws_link_t* const self = gate->link;
        // Latch the server's task identity for the same reason ws_handler does: a
        // predicate is a foreign callback and may tear this link down in-call, and
        // close_gate needs to recognise that it is running ON the task whose frame it
        // would otherwise wait for.
        self->server_task_.store(current_task(), std::memory_order_relaxed);
        fn = self->admission_fn_;
        verdict_fn = self->admission_verdict_fn_;
        ctx = self->admission_ctx_;
        // Register on the barrier for the duration of the predicate. `serving_fd` stays
        // -1 deliberately: it names the session an in-flight handler frame is servicing
        // so detach_sessions can skip it, and there is no session here yet — nothing to
        // detach, nothing to abandon.
        ++gate->depth;
    }
    // The predicate runs with `m` RELEASED — it is host code, bounded by nothing this
    // link owns, and the gate mutex is the one every latched callback resolves through
    // (the lock-order rule recorded on gate_t).
    //
    // At most one of the two forms is installed (each setter clears the other), so this is a
    // choice between three shapes and never a merge of two answers: the three-valued
    // predicate, the two-valued one, or no predicate at all — which admits every peer, the
    // historical open-graph behavior.
    const admission_verdict_t verdict =
        verdict_fn != nullptr ? verdict_fn(ctx, req)
        : fn != nullptr ? (fn(ctx, req) ? admission_verdict_t::ADMIT : admission_verdict_t::REFUSE)
                        : admission_verdict_t::ADMIT;
    bool admit = verdict != admission_verdict_t::REFUSE;
    if (!admit) ESP_LOGW(kTag, "peer refused by admission hook (fd=%d)", httpd_req_to_sockfd(req));
    // Open the ledger row BEFORE the 101 goes out, so the peer's first data frame — which may
    // arrive on another httpd select round the instant the upgrade completes — can never race
    // the row it has to consume. `peers_m_` nests UNDER `m`, which is the permitted order (see
    // gate_t), and the link is re-resolved through the gate the way every other latched
    // callback does.
    //
    // The row carries BOTH facts learned here: the handshake's verdict (#1245) and the instant
    // this socket stops being worth holding if it never speaks (#1247). It is the only record
    // of a silent upgrade that exists — esp_http_server answers the handshake itself and the
    // session is claimed lazily on the first frame — so without it the deadline bounds a
    // session that stalled mid-conversation and nothing at all bounds a socket that opened and
    // said nothing, which is the case the reference names as the attack.
    //
    // Only on a link with an auth hook: without one there is no deadline to enforce, the
    // verdict changes nothing about how a session is served, and a silent socket is the HTTP
    // server's own business exactly as it always was. That is also what keeps the refusal
    // below unreachable on such a link.
    if (admit) {
        const std::lock_guard lock(gate->m);
        httpd_ws_link_t* const self = gate->link;
        if (self != nullptr && self->auth_fn_ != nullptr &&
            !self->note_pending_handshake(httpd_req_to_sockfd(req),
                                          verdict == admission_verdict_t::ADMIT_AUTHENTICATED)) {
            // Refused BY VALUE and turned into the same clean abandon a REFUSE verdict gets:
            // ESP_FAIL out of the pre-handshake, no 101, no session, no latched route. The
            // alternative — admitting a socket the ledger cannot hold — would be admitting one
            // nothing can ever close.
            admit = false;
        }
    }
    {
        const std::lock_guard lock(gate->m);
        // Count the refusal here (#953) rather than beside its log line: this is a STATIC
        // callback, so the link is only reachable through the gate, and only under `m`.
        // The raised `depth` above already guarantees the link outlives the predicate, but
        // re-resolving under the lock is the idiom every other latched callback uses and
        // costs nothing on a mutex this function was taking anyway.
        if (!admit && gate->link != nullptr)
            gate->link->peers_refused_.fetch_add(1, std::memory_order_relaxed);
        --gate->depth;
    }
    gate->cv.notify_all();
    return admit ? ESP_OK : ESP_FAIL;
}

esp_err_t httpd_ws_link_t::on_data_frame(httpd_req_t* req) {
    httpd_ws_frame_t frame = {};
    // Pass 1 (max_len 0): read the header only — fills frame.len / frame.type. The
    // payload is NOT consumed off the socket here; pass 2 below does that.
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;  // socket error => httpd closes the session
    if (frame.len > kMaxFrameBytes) {
        // Counted and NAMED (#953). No session is charged: the cap is deliberately applied
        // before the slot lookup, so there is nothing to charge yet — which is also why
        // this drop was invisible from every published surface until now.
        note_rx_oversize(frame.len);
        return ESP_FAIL;  // abusive frame => drop the peer
    }

    // Pass 2: ALWAYS drain the payload — even a frame type we ignore must be consumed,
    // or its bytes stay in the stream and the next recv reads them as a frame header
    // (TCP-stream misalignment). Only then decide what to do with it. Fast path: a
    // frame that fits reads into the once-allocated rx scratch — no per-frame heap.
    // The scratch is safe to reuse per frame because this handler is the only RX
    // path (httpd task) and delivery below is synchronous; reassembly copies out of
    // `body` before returning. An oversized frame falls back to an exact-size nothrow
    // buffer (frame.len is peer-controlled up to kMaxFrameBytes; a throwing
    // std::vector would abort the node on heap exhaustion under -fno-exceptions);
    // on OOM the payload cannot be drained, so fail the handler — httpd closes just
    // this session (backpressure), never the whole node.
    std::unique_ptr<std::byte[]> heap_payload;
    std::byte* payload = nullptr;
    if (frame.len != 0) {
        if (rx_scratch_ != nullptr && frame.len <= rx_scratch_bytes_) {
            payload = rx_scratch_.get();
        } else {
            heap_payload.reset(new (std::nothrow) std::byte[frame.len]);
            if (heap_payload == nullptr) {
                note_rx_alloc_fail(frame.len);
                return ESP_FAIL;
            }
            payload = heap_payload.get();
        }
        frame.payload = reinterpret_cast<std::uint8_t*>(payload);
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) return err;
    }
    const std::span<const std::byte> body(payload, frame.len);
    // Only data frames carry a TLV (control frames are httpd's —
    // handle_.load(std::memory_order_relaxed)ws_control_frames is off); a stray TEXT/PONG is now
    // drained and ignored.
    if (frame.type != HTTPD_WS_TYPE_BINARY && frame.type != HTTPD_WS_TYPE_CONTINUE) return ESP_OK;

    const int fd = httpd_req_to_sockfd(req);
    // Resolve the slot (peer name for the bus tag + the reassembly buffer). Copy the
    // name out under the lock — the deliver below is synchronous, so a local string
    // outlives the whole in-call servicing.
    // esp_http_server responds the WS handshake INTERNALLY and does NOT call the URI
    // handler for the opening GET, so this is the ONE claim site: the peer is claimed
    // LAZILY, on its first data frame. An existing slot for `fd` is reused, which is what
    // makes every later frame of the session free of this block. Admission is decided
    // earlier and elsewhere — ws_pre_handshake, before the upgrade — so a peer that
    // reaches here has already been admitted.
    session_t* slot = nullptr;
    std::string peer;
    peer_handle_t handle;
    bool newly_claimed = false;
    bool pending = false;  // this session has not authenticated yet — see session_t::auth_pending
    // Reap expired unauthenticated sessions BEFORE the cap is tested below. The periodic
    // sweep is what bounds a squatter's lifetime, but it must not be what decides whether a
    // real peer gets in: a tick that has not fired yet would otherwise let a session which is
    // already past its deadline consume the one unit of `max_peers` this claim needs. Doing
    // it here makes the cap answer from live sessions only, at the one instant that matters,
    // whatever the timer is doing. Costs nothing on a link with no auth hook (the sweep
    // returns on the null-hook test) and one slot walk per NEW peer otherwise.
    if (auth_fn_ != nullptr) sweep_auth_deadlines();
    {
        const std::lock_guard lock(peers_m_);
        for (const auto& s : slots_)
            if (s->open && s->fd == fd) {
                slot = s.get();
                break;
            }
        if (slot == nullptr) {
            // Consume the pending-handshake row HERE, ahead of the cap test, because this
            // socket has now spoken and the ledger only ever bounded silent ones (#1247).
            // Consuming it also HANDS THE BOUND OVER: past this point the connection is either
            // a session with its own `auth_deadline_us` or a refusal httpd is about to close,
            // and in neither case may the sweep still see it as an un-spoken socket — it would
            // otherwise be closed twice, on two different arguments, and counted twice.
            // ...unless the HANDSHAKE authenticated it (#1245). A peer that presented a
            // credential the admission predicate accepted has already answered the question
            // the auth hook asks, and asking it again in-band is a question a native dialer
            // cannot answer at all — it has no way to send an authentication frame, so a link
            // serving both browsers and dialers would close every dialer at the deadline.
            const bool preauthenticated = take_pending_handshake(fd);
            // No teardown test: the gate admitted this frame, so the barrier has not
            // snapshotted yet and a session armed here is still caught.
            // Admission cap: refuse cleanly (ESP_FAIL => httpd closes the socket).
            if (max_peers_ != 0) {
                std::size_t open_n = 0;
                // `!dead` for the reason #963 gave enumerate_peers and peer_link: a condemned
                // session is one the link has already refused to carry another frame for, and
                // the reclaim that frees its slot is httpd's to schedule (a select round away
                // when the server is healthy, and unbounded when it is not — which is exactly
                // when condemn() gets used). Counting it as occupancy makes the cap answer
                // from the session TABLE while every other question answers from
                // REACHABILITY, and the peer it turns away is a live one being refused on
                // behalf of a dead one. This is what makes the auth deadline's reap effective
                // in the same call rather than one server pass later (#1184).
                for (const auto& s : slots_)
                    if (s->open && !s->dead) ++open_n;
                if (open_n >= max_peers_) {
                    peers_refused_.fetch_add(1, std::memory_order_relaxed);
                    ESP_LOGW(kTag, "peer refused: at max_peers=%u", (unsigned)max_peers_);
                    return ESP_FAIL;
                }
            }
            // The index is carried out of the search, not recomputed: it IS the peer's
            // routable name below, so losing it here is what cost every ESP node its
            // addressability (#994).
            std::size_t idx = 0;
            for (std::size_t i = 0; i < slots_.size(); ++i)
                if (slots_[i]->fd < 0) {
                    slot = slots_[i].get();
                    idx = i;
                    break;
                }  // reuse a departed slot
            if (slot == nullptr) {
                auto s = std::make_unique<session_t>();
                slot = s.get();
                slot->gate = gate_.load(std::memory_order_relaxed);
                slots_.push_back(std::move(s));
                idx = slots_.size() - 1;
            }
            // Belt and braces: every path that frees a slot retires its handle first, so
            // this is a no-op — but a slot must NEVER carry a handle stamped at a
            // generation it is about to leave behind, and that invariant belongs at the
            // claim, next to the bump that would break it.
            retire_resolution(slot, /*inert=*/false);
            // ADR-0073 §2: the routable NAME is the slot index, legal by construction, and
            // the `<ip>:<port>` goes to the diagnostics field instead of into the graph.
            slot->name = slot_name(idx);
            format_endpoint(fd, slot->endpoint_str);
            slot->asm_buf.clear();
            slot->fd = fd;
            ++slot->gen;  // a NEW session in a recycled slot: fail every stale ref (#954)
            // The seam's handle is minted from that same bump (#1294) — one identity for
            // the TX path and the routing plane, never two that could disagree. `gen` is
            // never 0 after the increment on a claim, so the handle is always valid.
            slot->handle = peer_handle_t{static_cast<std::uint32_t>(idx), slot->gen};
            slot->open = true;
            // The opening GET never reaches this handler, so THIS lazy first-frame claim
            // is the establish edge — the only honest place to stamp "connected at" and to
            // start this connection's counters.
            slot->st = {};
            slot->st.connected_at_us = esp_timer_get_time();
            // A link with an auth hook claims every session UNAUTHENTICATED, and the
            // deadline is stamped from the same clock reading the session is dated by. The
            // subject is cleared with it: slots are recycled in place, so a stale identity
            // left here would be published for whoever landed on the slot next. ...unless the
            // HANDSHAKE authenticated it (#1245): `preauthenticated` came out of the ledger row
            // consumed above, ahead of the cap test.
            slot->auth_pending = auth_fn_ != nullptr && !preauthenticated;
            slot->auth_deadline_us =
                slot->auth_pending ? slot->st.connected_at_us + auth_deadline_us_ : 0;
            slot->subject[0] = '\0';
            newly_claimed = true;
        }
        peer = slot->name;
        handle = slot->handle;
        pending = slot->auth_pending;
    }
    // Reclaim the slot on close — armed once, when first claimed (the free_ctx fires on the
    // httpd task at close). Outside peers_m_ so no httpd lock nests under ours.
    if (newly_claimed) {
        httpd_sess_set_ctx(req->handle, fd, slot, &httpd_ws_link_t::on_session_closed);
        // The claim edge is also where the per-socket policy is applied — once per
        // session, on the socket that just became a peer's.
        bound_socket(fd);
        // ...and so is the stack sample (#955). This is now the ONLY claim edge — the
        // opening-GET one this used to share with was removed, so nothing samples twice.
        check_httpd_stack();
        // ...and so is the arrival seam (#1223). Here, not inside the peers_m_ hold above:
        // the notifier re-enters the routing plane and takes graph locks, which is the same
        // precondition notify_departed carries. Announced even while `auth_pending`: an
        // anchor is invisible to enumeration, resolution and fan-out (it is not an address),
        // so it grants a pending session nothing the auth narrowing withholds — and a
        // session closed at the auth deadline is torn down through the ordinary departure
        // seam, which retires it.
        notify_arrived(handle, peer);
    }

    // Reassembly — asm_buf is httpd-task-only, so no lock. The SPA sends one whole TLV
    // per unfragmented BINARY frame (the fast path); a fragmented message chains here.
    if (frame.type == HTTPD_WS_TYPE_BINARY && frame.final && slot->asm_buf.empty()) {
        note_rx_message(slot, body.size());  // BEFORE the delivery — see note_rx_message
        // An unauthenticated session's message is a CREDENTIAL, never a TLV: it goes to the
        // auth hook and the graph never sees it. This is the gate that makes "admitted but
        // served nothing" true for reads and writes; the other three (enumeration,
        // resolution, fan-out) close the same door from the outbound side.
        if (pending)
            on_auth_message(slot, body);
        else
            deliver(handle, body);  // unfragmented: deliver borrowed, no extra copy
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_CONTINUE && slot->asm_buf.empty())
        return ESP_OK;  // stray CONTINUE with no assembly open — drop
    if (frame.type == HTTPD_WS_TYPE_BINARY)
        slot->asm_buf.clear();  // a BINARY mid-assembly discards the stale partial
    if (slot->asm_buf.size() + body.size() > kMaxFrameBytes) {
        slot->asm_buf.clear();
        note_rx_drop(slot);
        // The peer counter above already had this; what was missing is any way to see it
        // without polling (#953). Logged at the same level as its sibling alloc failure
        // just below, which was the only one of the two that ever said anything.
        note_reassembly_over_cap(slot->asm_buf.size(), body.size());
        return ESP_OK;  // reassembly would exceed the cap — drop the message
    }
    if (!slot->asm_buf.append(body)) {
        note_rx_drop(slot);
        ESP_LOGW(kTag, "reassembly alloc failed - message dropped");
        return ESP_OK;  // nothrow growth failed: drop the message, keep the peer
    }
    if (frame.final) {
        // Take the message OUT of the slot before delivering it. Delivery runs the app
        // in-call and the app may destroy this link (#814) — after which `slot` and every
        // other member is freed, so the old clear()-after-deliver was a use-after-free on
        // the fragmented path. Nothing owned by the link is touched past this point.
        const asm_buf_t message = slot->asm_buf.take();
        note_rx_message(slot, message.bytes().size());  // last touch of `slot` — see below
        // Same fork as the unfragmented path: a credential may arrive fragmented (a Noise
        // message can outgrow a browser's frame budget), and it must reach the hook by the
        // same route rather than being the one shape that leaks into the graph.
        if (pending)
            on_auth_message(slot, message.bytes());
        else
            deliver(handle, message.bytes());
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Post-handshake authentication (#1184) — the in-band credential path a browser
// can use, since the WebSocket API cannot set a request header.
// ---------------------------------------------------------------------------

void httpd_ws_link_t::set_auth_cb(auth_fn_t fn, void* ctx) noexcept {
    auth_fn_ = fn;
    auth_ctx_ = ctx;
    // The sweep is armed ONCE, by the first hook installed, and never re-armed: the deadline
    // is a constructor fact, so a later set_auth_cb cannot change the period, and clearing
    // the hook leaves a timer whose sweep returns immediately on the null-hook test.
    if (fn == nullptr || auth_timer_ != nullptr) return;
    esp_timer_create_args_t args = {};
    args.callback = &httpd_ws_link_t::auth_timer_fired;
    // The GATE, never `this` — a timer can fire at any point in a destructor's run, and the
    // gate is the object built to survive that (see gate_t). Null when the ctor failed to
    // allocate one, which the callback handles.
    args.arg = gate_.load(std::memory_order_relaxed);
    args.name = "tr_ws_auth";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK) {
        // Reported, and NOT fatal to the hook. Refusing to authenticate because a timer was
        // unavailable would fail OPEN — the one outcome an admission seam must never have —
        // so the credential check stands and only its bound is missing. The synchronous reap
        // in on_data_frame still keeps an expired session from consuming the peer cap.
        ESP_LOGE(kTag, "auth deadline sweep unavailable: esp_timer_create failed");
        return;
    }
    const std::int64_t period = auth_deadline_us_ / kAuthSweepDivisor;
    if (esp_timer_start_periodic(timer, static_cast<std::uint64_t>(
                                            period > kMinAuthSweepUs ? period : kMinAuthSweepUs)) !=
        ESP_OK) {
        (void)esp_timer_delete(timer);
        ESP_LOGE(kTag, "auth deadline sweep unavailable: esp_timer_start_periodic failed");
        return;
    }
    auth_timer_ = timer;
}

void httpd_ws_link_t::auth_timer_fired(void* arg) {
    auto* const gate = static_cast<gate_t*>(arg);
    if (gate == nullptr) return;
    // Resolve the link through the gate and register on its barrier, the idiom every latched
    // callback here uses: either this finds a null link and returns, or a concurrent
    // destructor waits in close_gate for the depth to fall. The handle is read under the
    // same hold, so it cannot be the stale value of a server already stopped.
    httpd_handle_t h = nullptr;
    {
        const std::lock_guard lock(gate->m);
        if (gate->link == nullptr) return;
        h = gate->link->handle_.load(std::memory_order_relaxed);
        ++gate->depth;
    }
    // Nothing is swept HERE. This runs on the esp_timer task, and every close the sweep can
    // reach touches the session table — `shutdown` on a descriptor whose lifetime belongs to
    // the httpd task, plus the slot bookkeeping that task owns. Marshalling the work over is
    // the same discipline the send path follows, and for the same reason.
    if (h != nullptr) (void)httpd_queue_work(h, &httpd_ws_link_t::auth_sweep_work, gate);
    {
        const std::lock_guard lock(gate->m);
        --gate->depth;
    }
    gate->cv.notify_all();
}

void httpd_ws_link_t::auth_sweep_work(void* arg) {
    auto* const gate = static_cast<gate_t*>(arg);
    if (gate == nullptr) return;
    httpd_ws_link_t* self = nullptr;
    {
        const std::lock_guard lock(gate->m);
        // A tick queued before the teardown can drain after it. A null link is the whole
        // answer: there is nothing left to sweep and nothing left to touch.
        if (gate->link == nullptr) return;
        self = gate->link;
        ++gate->depth;
    }
    self->sweep_auth_deadlines();
    {
        const std::lock_guard lock(gate->m);
        --gate->depth;
    }
    gate->cv.notify_all();
}

void httpd_ws_link_t::sweep_auth_deadlines() {
    if (auth_fn_ == nullptr) return;
    const std::int64_t now = esp_timer_get_time();
    // Same chunked, resumable scan the fan-out uses (see send), and for the same two
    // reasons: the snapshot is a FIXED on-stack array rather than a container sized to the
    // peer set — no allocation on a path that runs on a timer tick — and the closes below
    // must happen with `peers_m_` released, because close_session takes it again and the
    // frame it writes must not go out under this link's own lock.
    //
    // Resuming at `next` after releasing the lock is sound for the reason recorded on send():
    // a slot's INDEX never moves while the link is serving.
    std::size_t next = 0;
    for (bool more = true; more;) {
        session_t* expired[kFanoutChunk];
        std::size_t n = 0;
        {
            const std::lock_guard lock(peers_m_);
            while (next < slots_.size() && n < kFanoutChunk) {
                const auto& s = slots_[next++];
                if (s->open && !s->dead && s->auth_pending && s->auth_deadline_us != 0 &&
                    now >= s->auth_deadline_us)
                    expired[n++] = s.get();
            }
            more = next < slots_.size();
        }
        for (std::size_t i = 0; i < n; ++i) {
            auth_expired_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGW(kTag, "session closed: no credential within the auth deadline");
            close_session(expired[i], kCloseAuthTimeout);
        }
    }
    sweep_pending_handshakes(now);
}

void httpd_ws_link_t::sweep_pending_handshakes(std::int64_t now) {
    // The other half of the same deadline, and the half `slots_` cannot show (#1247). A peer
    // that completed the 101 and sent NOTHING never reached the lazy claim, so it has no
    // session, no `auth_pending` flag and nothing for the walk above to find — it was bounded
    // only by whatever keepalive policy the embedder's HTTP server happened to carry. Its row
    // in the ledger is the record that makes it reachable at all.
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    for (;;) {
        int fd = -1;
        {
            const std::lock_guard lock(peers_m_);
            std::size_t at = pending_n_;
            for (std::size_t i = 0; i < pending_n_; ++i)
                if (now >= pending_[i].deadline_us) {
                    at = i;
                    break;
                }
            if (at == pending_n_) break;
            fd = pending_[at].fd;
            // Retire the row BEFORE the close, and while still holding the lock: the close
            // below runs unlocked, and a row left behind would be swept again on the next tick
            // against a descriptor this link no longer has any claim on.
            drop_pending_handshake(at);
        }
        // The server's own verdict on the descriptor, taken exactly as tx_work takes it. There
        // is no session_t here to carry an identity, so this is the ONLY thing standing between
        // a row whose socket already hung up and a `shutdown` on whoever was accepted onto the
        // recycled number. A row that fails it is simply dropped: the socket it named is gone,
        // which is the outcome the deadline wanted anyway.
        if (h == nullptr || fd < 0 || httpd_ws_get_fd_info(h, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
            continue;
        auth_expired_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(kTag, "socket closed: upgraded and sent nothing within the auth deadline");
        // The close code first and the shutdown second — close_session's order, for
        // close_session's reason (after `shutdown` every write fails at once, so a code written
        // afterwards never reaches the peer). Done inline rather than through close_session
        // because there is no slot: the frame is written on the bare descriptor, which is all
        // this connection ever cost us.
        const std::byte payload[2] = {static_cast<std::byte>((kCloseAuthTimeout >> 8) & 0xFF),
                                      static_cast<std::byte>(kCloseAuthTimeout & 0xFF)};
        (void)send_now(nullptr, fd, HTTPD_WS_TYPE_CLOSE, payload);
        condemn(fd);
    }
}

esp_err_t httpd_ws_link_t::send_now(session_t* slot, int fd, int ws_type,
                                    std::span<const std::byte> payload) {
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    if (h == nullptr || fd < 0) return ESP_FAIL;
    // Straight to the socket, with neither a pool slot nor `httpd_queue_work` in the way.
    // The queue exists to marshal sends from OTHER tasks onto the httpd task; every caller
    // of this one is already ON it (the frame handler, or the sweep work item the timer
    // posted there), so queueing would buy a copy, a slot and a round through the control
    // socket to arrive at the same call. It also means the auth exchange cannot be starved
    // by a fan-out that has the pool busy — which matters, because the frame it is trying to
    // write is the one that decides whether this session is served at all.
    httpd_ws_frame_t f = {};
    f.final = true;
    f.fragmented = false;
    f.type = static_cast<httpd_ws_type_t>(ws_type);
    // `httpd_ws_frame_t::payload` is non-const in IDF and the send does not write through
    // it; the cast is confined to this one call rather than pushed onto the callers.
    f.payload = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(payload.data()));
    f.len = payload.size();
    // Bracket it exactly as tx_work does, so the send override judges a short write on an
    // auth frame by the same rule it judges every other frame by (#951) — a truncated CLOSE
    // desynchronises the stream just as thoroughly as a truncated reply.
    if (slot != nullptr) slot->open_tx_frame();
    const esp_err_t err = httpd_ws_send_frame_async(h, fd, &f);
    if (slot != nullptr) (void)slot->close_tx_frame();
    return err;
}

void httpd_ws_link_t::close_session(session_t* slot, std::uint16_t code) {
    int fd = -1;
    {
        const std::lock_guard lock(peers_m_);
        if (slot == nullptr || !slot->open || slot->dead) return;
        fd = slot->fd;
    }
    if (fd < 0) return;
    // RFC 6455 §5.5.1: a CLOSE payload opens with the 2-byte status code, network order.
    // Sending it BEFORE the shutdown is the whole reason this is not a bare condemn():
    // `condemn` is `shutdown`, after which every write on the socket fails at once, so a
    // code written afterwards would never reach the peer — and a peer that cannot tell a
    // refused credential from an expired deadline is exactly what this feature set out to
    // avoid. A failed send is not worth acting on: the session is going either way.
    const std::byte payload[2] = {static_cast<std::byte>((code >> 8) & 0xFF),
                                  static_cast<std::byte>(code & 0xFF)};
    (void)send_now(slot, fd, HTTPD_WS_TYPE_CLOSE, payload);
    {
        // Mark the verdict the instant it is reached, before the close it provokes has run —
        // the same immediacy `session_t::dead` exists for on the strike path. From here every
        // send path refuses this session rather than racing the reap.
        const std::lock_guard lock(peers_m_);
        slot->dead = true;
    }
    condemn(fd);
}

bool httpd_ws_link_t::note_pending_handshake(int fd, bool preauthenticated) {
    if (fd < 0) return false;
    const std::lock_guard lock(peers_m_);
    const std::int64_t deadline = esp_timer_get_time() + auth_deadline_us_;
    // REPLACE a stale row for the same descriptor rather than adding a second. A descriptor is
    // reused as soon as the kernel frees it, so a leftover from a handshake whose peer never
    // sent a frame must not pre-authenticate the NEXT connection to land on that number (the
    // one way the verdict half could fail open), nor charge it a deadline that has already
    // half elapsed (the one way the deadline half could close a peer that just arrived).
    for (std::size_t i = 0; i < pending_n_; ++i) {
        if (pending_[i].fd != fd) continue;
        pending_[i].deadline_us = deadline;
        pending_[i].preauthenticated = preauthenticated;
        return true;
    }
    if (pending_n_ == pending_.size()) {
        // FULL, and the answer is a refusal by value — the caller turns this into ESP_FAIL and
        // esp_http_server abandons the upgrade, which is the same clean close a REFUSE verdict
        // gets. Not an assert on a size argued to be unreachable (#1247): the row carries this
        // socket's deadline, so evicting one to make room would hand back an UNBOUNDED socket —
        // precisely the resource this ledger exists to bound — and an abort() on an MCU is the
        // one answer to exhaustion this project does not give anywhere else. Reaching this at
        // all means more upgrades are in flight, un-spoken, than kMaxPendingHandshakes; the
        // sweep frees rows at the deadline, so it is self-clearing.
        ESP_LOGW(kTag, "pending-handshake ledger full (%u) - handshake refused (fd=%d)",
                 static_cast<unsigned>(pending_.size()), fd);
        return false;
    }
    pending_[pending_n_++] = pending_handshake_t{fd, deadline, preauthenticated};
    return true;
}

bool httpd_ws_link_t::take_pending_handshake(int fd) noexcept {
    if (fd < 0) return false;
    for (std::size_t i = 0; i < pending_n_; ++i) {
        if (pending_[i].fd != fd) continue;
        const bool preauthenticated = pending_[i].preauthenticated;
        drop_pending_handshake(i);
        return preauthenticated;
    }
    return false;
}

void httpd_ws_link_t::drop_pending_handshake(std::size_t at) noexcept {
    if (at >= pending_n_) return;
    pending_[at] = pending_[pending_n_ - 1];  // order carries no meaning
    --pending_n_;
}

void httpd_ws_link_t::on_auth_message(session_t* slot, std::span<const std::byte> body) {
    const auth_fn_t fn = auth_fn_;
    void* const ctx = auth_ctx_;
    if (fn == nullptr) return;
    int fd = -1;
    {
        const std::lock_guard lock(peers_m_);
        if (slot == nullptr || !slot->open || slot->dead) return;
        fd = slot->fd;
    }
    // The hook runs with NO lock of this link held — foreign code of unbounded duration,
    // and `peers_m_` is the mutex the whole send path needs. Same placement the admission
    // predicate gets in ws_pre_handshake, for the same reason.
    const auth_result_t res = fn(ctx, body);
    // The reply goes out FIRST, whatever the verdict. On CONTINUE it is the handshake's next
    // message; on REJECT it lets a scheme say why in its own language, ahead of the close
    // code; on ACCEPT it is the confirmation a client may be waiting for before it starts
    // sending TLVs. Empty (a bearer token needing no answer) sends nothing.
    if (!res.reply.empty()) (void)send_now(slot, fd, HTTPD_WS_TYPE_BINARY, res.reply);
    switch (res.verdict) {
        case auth_verdict_t::ACCEPT: {
            const std::lock_guard lock(peers_m_);
            if (!slot->open || slot->dead) return;  // departed inside the hook
            const std::size_t n =
                res.subject.size() < kMaxSubjectChars ? res.subject.size() : kMaxSubjectChars;
            if (n != 0) std::memcpy(slot->subject, res.subject.data(), n);
            slot->subject[n] = '\0';
            // The session becomes a peer HERE, and everything that was closed to it opens at
            // once: enumeration, resolution, fan-out and the graph. Clearing the deadline
            // with it is what keeps a served session out of the sweep's reach forever after.
            slot->auth_pending = false;
            slot->auth_deadline_us = 0;
            break;
        }
        case auth_verdict_t::CONTINUE:
            // Nothing changes — deliberately including the deadline, which is NOT extended.
            // A multi-frame scheme is given the whole window for all of its frames; a peer
            // that could push the deadline out by sending anything at all would have none.
            break;
        case auth_verdict_t::REJECT:
            auth_rejected_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGW(kTag, "session closed: credential refused by the auth hook");
            close_session(slot, kCloseAuthFailed);
            break;
    }
}

void httpd_ws_link_t::on_session_closed(void* ctx) {
    auto* const slot = static_cast<session_t*>(ctx);
    if (slot == nullptr || slot->gate == nullptr) return;
    gate_t* const gate = slot->gate;
    httpd_ws_link_t* owner = nullptr;
    std::string departed;
    peer_handle_t departed_handle;
    {
        // Resolve the link through the gate. That is what makes this safe against a
        // concurrent teardown: a destructor can only shut the gate by taking this same
        // lock, so either the reclaim completes first with the link provably alive, or it
        // finds a null link and is inert. A slot reached here after its link is gone is
        // one a teardown deliberately leaked (@ref abandon_sessions) — landing on valid,
        // inert memory rather than a freed shell.
        const std::lock_guard lock(gate->m);
        owner = gate->link;
        if (owner == nullptr) return;
        departed = owner->reclaim_slot(slot, departed_handle);
        if (departed.empty()) return;  // nothing owed to the routing plane
        // A departure IS owed, and it is fired below with `m` RELEASED (#960). The mutex
        // is the wrong instrument to hold it under: it is the one each of the server's
        // four latched callbacks resolves this link through and the one a destructor
        // blocks on, while the notifier is a foreign callback that re-enters router →
        // graph and is bounded by nothing this link owns. Holding it across that is also
        // what `bus_link_t::notify_peer_down` documents must not happen ("with none of its
        // internal locks held"), and what the same reasoning already keeps `tx_work` from
        // doing across a send.
        //
        // What must NOT be dropped with it is the LIFETIME guarantee holding it supplied:
        // `owner` is dereferenced below, and the notifier's ctx is the routing plane the
        // teardown may be dismantling. So register on the gate's existing barrier instead
        // — the same `depth`/`cv` pair a URI-handler frame uses — and the destructor's
        // @ref close_gate still cannot return while this notification is in flight. The
        // wait moves from the mutex to the condition variable; it does not disappear.
        ++gate->depth;
    }
    owner->notify_departed(departed_handle, departed);
    {
        // `owner` may be DESTROYED by now, exactly as at the tail of @ref ws_handler: the
        // notifier can drive an app teardown, and the barrier above is what let it start.
        // Only `gate`, which deliberately outlives the link, may be touched from here on.
        const std::lock_guard lock(gate->m);
        --gate->depth;
    }
    gate->cv.notify_all();
}

std::string httpd_ws_link_t::reclaim_slot(session_t* slot, peer_handle_t& handle) {
    std::string departed;
    handle = peer_handle_t{};
    bool was_open;
    bool was_pending;
    {
        const std::lock_guard lock(peers_m_);
        was_open = slot->open;
        was_pending = slot->auth_pending;
        // The auth state is recycled with everything else, and the subject especially: a
        // slot handed to a new peer must not publish its predecessor's identity for the
        // window between the claim and the next ACCEPT. Same rule as the dead mark above.
        slot->auth_pending = false;
        slot->auth_deadline_us = 0;
        slot->subject[0] = '\0';
        departed = std::move(slot->name);
        // RETIRE the seam's handle with the name (#1294): "valid until depart" means
        // nothing this slot carries afterwards may be stamped with the departed session's
        // identity. `gen` itself survives, so the next claim mints a fresh one.
        handle = slot->handle;
        slot->handle = peer_handle_t{};
        slot->open = false;
        slot->fd = -1;
        slot->name.clear();
        // The session this slot carried is over, so the handle that NAMED that session is
        // spent: back to the pool, where it waits out the quarantine before it can be
        // restamped (#1013). Anyone still holding it now fails the generation test rather
        // than reaching whoever lands here next — which is the whole point of retiring it
        // instead of re-pointing it.
        retire_resolution(slot, /*inert=*/false);
        // The address goes with the name. Unobservable today (the claim rewrites it before
        // `open`, and only open slots are reported), but it is the same hygiene the TX
        // verdicts below get, and for the same reason: a recycled slot must carry nothing
        // of its predecessor's into a log that a human will read as being about the peer
        // now in it.
        slot->endpoint_str[0] = '\0';
        slot->asm_buf.clear();
        slot->tx_drops = 0;
        // Slots are RECYCLED in place, so both TX verdicts about the departed peer must
        // be cleared with it. The dead mark especially: lwIP hands a descriptor NUMBER
        // straight back, so a mark left standing would refuse every frame to whichever
        // unrelated peer next landed on that number. This runs from `free_ctx` on the
        // httpd task — before that task can accept anything onto it — so the clear
        // strictly precedes any reuse.
        slot->dead = false;
        // The frame bracket goes with them (#951). No send can be in flight here — both
        // halves of the bracket are in ONE tx_work call with a synchronous send between
        // them, on this same task — so this is hygiene for a recycled slot rather than a
        // live hazard: a slot handed to a new peer carries no TX state of its
        // predecessor's, the rule the dead mark above is here for having broken twice. Left
        // standing, an open bracket would let the FIRST failed write to the next peer read
        // as a truncation of a frame that peer was never sent.
        (void)slot->close_tx_frame();
    }
    // Departure seam (RFC-0009 §D extended to peer departure): a browser tab that hung up
    // leaves its subscriber edges behind, so the routing plane's eviction hook
    // (fwd_router_t::link_down via the installed notifier) is owed one call. It is NOT
    // fired here — the name is handed back to @ref on_session_closed, which fires it with
    // BOTH of this link's mutexes released (#960). Only a session that completed its
    // handshake (open) can have flowed subscribes. A TX-failure-triggered close (tx_work /
    // note_tx_result) arrives here through the same free_ctx path, so the departed peer's
    // subscriber edges are evicted too; was_open (flipped under peers_m_ on the first
    // pass) keeps the notification single-fire per session. No teardown test is needed to
    // suppress it: the caller resolved this link through the gate, so reaching here at all
    // means the destructor has not shut it, and the barrier it registers on keeps the
    // routing plane the notifier targets standing for the call.
    if (!was_open) return {};
    // An UNAUTHENTICATED session is owed no departure either, and for a stronger reason than
    // the `was_open` test beside it: it was never announced. It was absent from
    // `enumerate_peers`, unreachable through `peer_link` and skipped by every fan-out, so the
    // routing plane holds no edge, no subscription and no name for it — and `notify_peer_down`
    // for a peer that never came up is a foreign call into router → graph that can only be
    // noise, or worse if a name it does hold happens to match a recycled slot's.
    if (was_pending) return {};
    return departed;
}

void httpd_ws_link_t::notify_departed(peer_handle_t handle, std::string_view peer) {
    // Peer-named mode evicts just the departed peer's edges. A FLAT link has ONE routing
    // identity for every peer it carries — the registered child NAME — so its only seam is
    // the whole link, and firing that on a MID-LIFE close would evict the surviving tabs'
    // edges along with the departed one's. It waits for the last open session (#889) —
    // the same fork slot_server_t::teardown_slot takes, for the same reason.
    if (peer_named_) {
        notify_peer_down(handle, peer);
    } else if (!any_open_session()) {
        notify_down();
    }
}

void httpd_ws_link_t::notify_arrived(peer_handle_t handle, std::string_view peer) {
    // Peer-named only, the same fork notify_departed takes and for the same reason: a flat
    // link has ONE routing identity for every tab it carries, so there is no per-session
    // identity to anchor. There is no flat-mode counterpart to notify_down() here — link-up
    // is the transport's own state, not a per-session event.
    if (peer_named_) notify_peer_up(handle, peer);
}

bool httpd_ws_link_t::any_open_session() const {
    // reclaim_slot cleared the departing slot's `open` under this same lock before handing
    // the name up, so the departed session is not counted. A peer admitted between this
    // answer and the notification would be evicted with edges it has not had time to grow
    // — nothing to lose — which is why the two need not be one critical section (and must
    // not be: notify_down runs with no lock of this link's held).
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_t>& s : slots_)
        if (s->open) return true;
    return false;
}

void httpd_ws_link_t::note_rx_message(session_t* slot, std::size_t bytes) {
    const std::lock_guard lock(peers_m_);
    ++slot->st.rx_frames;
    slot->st.rx_bytes += static_cast<std::uint32_t>(bytes);
    slot->st.last_rx_us = esp_timer_get_time();
}

void httpd_ws_link_t::note_rx_drop(session_t* slot) {
    const std::lock_guard lock(peers_m_);
    ++slot->st.rx_drops;
}

/**
 * @brief Count and name an over-cap inbound frame.
 *
 * `noinline` deliberately, and it is #994's lesson applied before it cost anything:
 * `on_data_frame` runs on every inbound FRAME, and letting a log call site — format string,
 * argument marshalling, the call setup — inline into it grew that function by 350 bytes at
 * `-Os` for branches that are almost never taken. Measured, not assumed.
 */
[[gnu::noinline]] void httpd_ws_link_t::note_rx_oversize(std::size_t len) {
    rx_dropped_oversize_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW(kTag, "rx frame over the abuse cap (len=%u > %u) - dropping the peer", (unsigned)len,
             (unsigned)kMaxFrameBytes);
}

/** @brief Count and name an RX payload allocation failure. `noinline` for the reason on
 *         @ref note_rx_oversize. */
[[gnu::noinline]] void httpd_ws_link_t::note_rx_alloc_fail(std::size_t len) {
    rx_dropped_alloc_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW(kTag, "rx alloc failed (len=%u) - closing session", (unsigned)len);
}

/** @brief Name a reassembly that would pass the cap — the per-peer counter is bumped by the
 *         caller, which holds the slot; this adds only the log the site never had.
 *         `noinline` for the reason on @ref note_rx_oversize. */
[[gnu::noinline]] void httpd_ws_link_t::note_reassembly_over_cap(std::size_t had,
                                                                 std::size_t adding) {
    ESP_LOGW(kTag, "reassembly over the cap (%u + %u > %u) - message dropped", (unsigned)had,
             (unsigned)adding, (unsigned)kMaxFrameBytes);
}

void httpd_ws_link_t::deliver(peer_handle_t peer, std::span<const std::byte> frame) {
    // Peer-named bus tag when the facet is on (each tab its own return route); the flat
    // point-to-point sink otherwise — matching what fwd_router_t::add_child installed.
    if (peer_named_)
        peer_rx_.deliver_borrowed(peer, frame);
    else
        rx_.deliver_borrowed(frame);
}

// ---------------------------------------------------------------------------
// TX — every send is marshalled onto the httpd task (the async-send pattern).
// ---------------------------------------------------------------------------

void httpd_ws_link_t::queue_send(const session_ref_t& to,
                                 std::span<const std::span<const std::byte>> iov) {
    // ONE load of each member, into a local (#963). The plain reads these replace raced
    // the destructor's writes outright — UB by the memory model — and re-reading them
    // across the gather let one send act on two different values of the same member.
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    gate_t* const g = gate_.load(std::memory_order_relaxed);
    if (h == nullptr) return;
    // Re-resolve the destination HERE rather than trusting the fd the caller looked at:
    // a departed or condemned peer takes no more frames, and is refused at this end of the
    // queue rather than the far one. Queueing to it would be worse than useless: the
    // control socket is the scarce resource under this failure (one small UDP mbox, shared
    // with the close the link is trying to land), so every frame accepted for a doomed
    // session is a slot the rest of the node does not get. Both production callers below
    // already skip dead slots from the lock they were holding anyway; this is the locus
    // that makes it a property of the seam rather than of its callers, and it costs one
    // uncontended mutex on a path that is about to memcpy a frame and do a syscall.
    const int fd = live_fd(to);
    if (fd < 0) {
        // Counted (#953). Normally benign — a fan-out that snapshotted its destinations
        // racing a departure — but silent, and a silent drop here reads exactly like a
        // delivery from outside.
        tx_to_dead_peer_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Gather-copy the payload ONCE: httpd_queue_work is asynchronous, so the caller's
    // spans are gone by the time the httpd task runs tx_work. The destination is a
    // pre-allocated pool slot claimed lock-free (CAS), gathered straight into its inline
    // buffer — no allocation at all. The single remaining arm that allocates is a frame
    // past tx_inline_bytes_, which keeps its pooled shell and takes a nothrow heap payload.
    //
    // A pool that has nothing free is where this used to grow a heap work item and post it
    // anyway, which turned a bounded, observable condition into an unbounded, invisible
    // one: the outstanding-send count was then capped only by the heap, against a control
    // mbox of six drained one message per server pass. It is now the drop it should always
    // have been — the pool IS this link's outstanding-send bound, and exceeding it is
    // counted (#949, and the exhaustion policy ADR-0039 §4 / ADR-0042 §2 already ruled).
    //
    // Nothrow END TO END on the one arm that can still allocate — never a std::vector for
    // the copy: its THROWING allocator once defeated the `new (std::nothrow)` guard on the
    // shell, aborting the node under -fno-exceptions on a reply-sized copy.
    std::size_t total = 0;
    for (const auto& part : iov) total += part.size();
    tx_work_t* work = nullptr;
    std::byte* dst = nullptr;
    if (tx_slot_t* const slot = claim_tx_slot_waiting(); slot != nullptr) {
        work = &slot->work;
        work->handle = h;
        work->gate = g;
        work->to = to;
        work->len = total;
        if (total <= tx_inline_bytes_) {
            dst = slot->inline_buf;
        } else {
            work->owned.reset(new (std::nothrow) std::byte[total]);
            dst = work->owned.get();
            if (dst == nullptr) {  // oversize-payload OOM: recycle the slot, drop below
                release_tx_work(work);
                work = nullptr;
            }
        }
        if (work != nullptr) work->payload = dst;
    } else {
        // The pool had nothing free, AND (off the httpd task) it still had nothing free
        // after a full drain bound — see claim_tx_slot_waiting. Still an enqueue_drops below
        // — the total is unchanged — but recorded separately, because it is evidence about
        // THIS link's outstanding depth against tx_slot_capacity, while the other two causes
        // name the shared control queue and the heap (#953). Since #1187 it is also evidence
        // about the DRAIN and not merely about the depth: a miss now means the httpd task
        // did not free a slot within one send occupancy, which `tx_pool_waits` separates
        // from the far commoner "waited briefly and was served".
        tx_pool_misses_.fetch_add(1, std::memory_order_relaxed);
    }
    bool queued = false;
    if (work != nullptr) {  // work == nullptr => no slot, or OOM: drop this frame
        std::byte* p = dst;
        for (const auto& part : iov) {
            if (!part.empty()) std::memcpy(p, part.data(), part.size());
            p += part.size();
        }
        queued = httpd_queue_work(h, &httpd_ws_link_t::tx_work, work) == ESP_OK;
        // A refused enqueue is the only enqueue failure that exists above the ESP-IDF floor
        // this component requires, and it hands the slot straight back: the item was never
        // posted, so nothing else can be reading it (see tx_slot_t).
        if (!queued) release_tx_work(work);
    }
    // A frame that never reached the queue is charged to the LINK, not to this peer: a
    // refused enqueue is evidence about the shared control queue, and under #835's shape
    // the peer that saturated it is precisely the one NOT sending here. An exhausted pool
    // is evidence about the link's own in-flight depth, which is no more this peer's fault.
    // The session streak is fed from tx_work, where the result names its destination.
    if (!queued) note_enqueue_drop(fd, total);
}

void httpd_ws_link_t::queue_send(const session_ref_t& to, std::span<const std::byte> frame) {
    // One-span sugar over the gather form — the single copy/backpressure locus.
    const std::span<const std::byte> one[] = {frame};
    queue_send(to, std::span<const std::span<const std::byte>>(one));
}

httpd_ws_link_t::stats_t httpd_ws_link_t::stats() const noexcept {
    // Relaxed throughout, and deliberately NOT a consistent cut: each field is an
    // independent tally and taking a lock to align them would put a diagnostic read in
    // front of the send path. Two snapshots differenced is the intended use (#953).
    stats_t s;
    s.enqueue_drops = enqueue_drops_.load(std::memory_order_relaxed);
    s.tx_pool_misses = tx_pool_misses_.load(std::memory_order_relaxed);
    s.tx_pool_waits = tx_pool_waits_.load(std::memory_order_relaxed);
    s.tx_to_dead_peer = tx_to_dead_peer_.load(std::memory_order_relaxed);
    s.peers_refused = peers_refused_.load(std::memory_order_relaxed);
    s.sessions_condemned = sessions_condemned_.load(std::memory_order_relaxed);
    s.rx_dropped_oversize = rx_dropped_oversize_.load(std::memory_order_relaxed);
    s.rx_dropped_alloc = rx_dropped_alloc_.load(std::memory_order_relaxed);
    s.auth_rejected = auth_rejected_.load(std::memory_order_relaxed);
    s.auth_expired = auth_expired_.load(std::memory_order_relaxed);
    return s;
}

std::size_t httpd_ws_link_t::tx_slots_busy() const noexcept {
    if (tx_pool_ == nullptr) return 0;
    std::size_t busy = 0;
    for (std::size_t i = 0; i < tx_slots_total_; ++i)
        if (tx_pool_[i].busy.load(std::memory_order_relaxed)) ++busy;
    return busy;
}

std::size_t httpd_ws_link_t::tx_slot_capacity() const noexcept { return tx_pool_slots_; }

std::size_t httpd_ws_link_t::rx_scratch_bytes() const noexcept { return rx_scratch_bytes_; }

std::size_t httpd_ws_link_t::tx_inline_bytes() const noexcept { return tx_inline_bytes_; }

std::size_t httpd_ws_link_t::buffer_bytes() const noexcept {
    // What was actually allocated, not what was asked for: rx_scratch_bytes_ is zeroed on a
    // failed RX allocation, and a link whose pool failed reports no pool cost at all.
    const std::size_t pool =
        tx_pool_ != nullptr ? tx_slots_total_ * (sizeof(tx_slot_t) + tx_inline_bytes_) : 0;
    return rx_scratch_bytes_ + pool;
}

std::size_t httpd_ws_link_t::tx_reply_reserve() noexcept { return kTxReplySlots; }

void httpd_ws_link_t::note_enqueue_drop(int fd, std::size_t bytes) {
    const std::uint32_t total = enqueue_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
    ESP_LOGW(kTag, "tx enqueue drop (queue refused / pool exhausted / OOM) fd=%d len=%u total=%u",
             fd, (unsigned)bytes, (unsigned)total);
}

int httpd_ws_link_t::live_fd(const session_ref_t& to) const {
    const std::lock_guard lock(peers_m_);
    if (to.slot == nullptr) return -1;
    // The identity test, and the reason this function exists rather than a slots_ scan for
    // `s->fd == fd`: that scan finds whoever holds the descriptor NOW, which under reuse is
    // a different peer than the one this frame was gathered for. Dereferencing the slot is
    // safe without any further proof — slots are recycled in place and never freed while
    // the link lives (the abandon path leaks them for exactly this reason), and every
    // caller reached us through the gate, which is what establishes that the link does.
    if (to.slot->gen != to.gen) return -1;  // the session departed; this slot has moved on
    if (!to.slot->open || to.slot->dead) return -1;
    return to.slot->fd;
}

void httpd_ws_link_t::condemn(int fd) {
    // Counted (#953): this is the ONLY place the link kills a session of its own accord,
    // so it is the whole difference between a peer that left and one that was torn down —
    // a distinction an embedder could not previously make from any published surface.
    sessions_condemned_.fetch_add(1, std::memory_order_relaxed);
    // The close that does NOT ride the control socket, and the reason this round exists.
    //
    // `httpd_sess_trigger_close` is `httpd_queue_work(httpd_sess_close, sd)`
    // (httpd_sess.c:476-481, release/v5.5): the SAME loopback control socket, drained by
    // the SAME single httpd task that is currently working through this fd's queued
    // sends. So it is strictly FIFO behind the very backlog it exists to clear, and every
    // entry ahead of it costs a full send bound on a stalled socket. Worse, on the
    // default non-blocking path `httpd_queue_work` is a bare `sendto` to that socket
    // (httpd_main.c) — an enqueue past the receiver's UDP mbox is dropped inside lwIP
    // while `sendto`, and therefore `httpd_queue_work`, still returns success. An ESP_OK
    // from trigger_close is not evidence that anything was queued. On silicon the close
    // was asked for repeatedly and never took effect; the fd kept failing for two minutes.
    //
    // `shutdown` answers instead, because it is not a request of the server at all:
    //   - it costs one syscall, taking effect before this function returns;
    //   - lwIP raises NETCONN_EVT_RCVPLUS on a shut socket (api_msg.c
    //     lwip_netconn_do_shutdown), so the fd comes back readable-at-EOF from the very
    //     next `select` in `httpd_server` and httpd reaps the session through its OWN
    //     path — the one arm of that loop with no control message on it;
    //   - every later write on the socket fails AT ONCE instead of waiting out the bound.
    //
    // It does NOT close the descriptor — `shutdown` never does — so httpd remains the
    // sole owner of the fd's lifetime and its own `close` stays correct. Calling it is
    // safe against that lifetime because EVERY caller runs ON the httpd task — a tx_work
    // item, a send override invoked from one, close_session (the auth verdicts and
    // close_peer), and the pending-handshake sweep (#1247) — the single task that accepts
    // and closes sockets, so the fd cannot be recycled underneath this call.
    //
    // The sweep is the one caller with no session_t behind its descriptor, so it is also
    // the one that cannot lean on live_fd for identity; it tests the server's own
    // `httpd_ws_get_fd_info` before reaching here instead. See sweep_pending_handshakes.
    if (::shutdown(fd, SHUT_RDWR) != 0) ESP_LOGW(kTag, "shutdown failed fd=%d (%d)", fd, errno);
    // Best-effort belt: if the control socket does have room, this reaps the session a
    // select cycle sooner. Its return is deliberately not trusted — see above — so a
    // failure is logged at debug and nothing depends on it.
    if (httpd_sess_trigger_close(handle_.load(std::memory_order_relaxed), fd) != ESP_OK)
        ESP_LOGD(kTag, "trigger_close not queued fd=%d (the shutdown carries the close)", fd);
}

void httpd_ws_link_t::note_tx_skip(const session_ref_t& to) {
    const std::lock_guard lock(peers_m_);
    if (to.slot == nullptr) return;
    // The same identity test @ref live_fd makes, for the same reason. A skip fires
    // precisely when the peer just departed, which is exactly when lwIP recycles its
    // descriptor number and this slot is reclaimed for someone else — so charging by fd,
    // or by slot pointer alone, hands a brand-new healthy session the departed one's lost
    // frame, and a drop that appears is supposed to be a signal (#954). Dereferencing
    // needs no further proof: slots are recycled in place and never freed while the link
    // lives, and reaching here through the gate is what establishes that it does.
    if (to.slot->gen != to.gen) return;  // the session departed; nobody left to charge
    ++to.slot->st.tx_drops;
}

void httpd_ws_link_t::note_tx_result(const session_ref_t& to, bool sent, std::size_t bytes) {
    bool close_now = false;
    std::string peer;
    char addr[kEndpointChars] = {};
    int fd = -1;
    {
        const std::lock_guard lock(peers_m_);
        session_t* const slot = to.slot;
        if (slot == nullptr) return;
        // The result belongs to the session the frame was gathered for or to nobody. The
        // old fd-keyed scan handed it to whichever session held the descriptor by the time
        // the send drained, so a departed peer's failures accrued against a stranger and
        // kMaxConsecutiveTxDrops of them closed a session that had failed nothing (#954).
        if (slot->gen != to.gen) return;  // that session is gone; this is not its successor's
        if (!slot->open) return;          // departed between the snapshot and now
        if (slot->dead) return;           // already condemned — no further evidence is wanted
        if (sent) {
            ++slot->st.tx_frames;
            slot->st.tx_bytes += static_cast<std::uint32_t>(bytes);
            slot->tx_drops = 0;  // a failure streak is CONSECUTIVE — any success resets it
            return;
        }
        // Two counts, two questions. The cumulative one below answers "did anything get
        // lost toward this peer?"; the streak byte answers "is this peer broken RIGHT
        // NOW?" and its #835 3-strike semantics are untouched by the addition.
        ++slot->st.tx_drops;
        if (slot->tx_drops < kMaxConsecutiveTxDrops) ++slot->tx_drops;
        close_now = slot->tx_drops >= kMaxConsecutiveTxDrops;
        peer = slot->name;
        std::memcpy(addr, slot->endpoint_str, sizeof(addr));
        fd = slot->fd;
        // Condemn UNDER the same lock that reached the verdict. Any later moment is a
        // window in which the fan-out enqueues more frames for a peer already known to be
        // broken, and each of those costs the httpd task a whole send bound.
        if (close_now) slot->dead = true;
    }
    if (!close_now) return;  // the drop itself is already logged by tx_work
    // BOTH names, because after #994 neither one alone identifies the peer: `p<slot>` is
    // what an operator can address the session by, and `<ip>:<port>` is what tells them
    // which physical client that slot is. Both come from the slot, filled at admission.
    // Never `getpeername` here: the socket is by definition the one that is not working,
    // and naming a peer at the moment it is being struck must not depend on that socket
    // answering — the reason the address is STORED rather than recomputed.
    ESP_LOGW(kTag, "%u consecutive failed sends peer=%s (%s) fd=%d len=%u - closing session",
             (unsigned)kMaxConsecutiveTxDrops, peer.c_str(), addr, fd, (unsigned)bytes);
    // At the streak cap the session is broken, not bursty: close it so the peer's onclose
    // fires and it reconnects, instead of silently missing frames forever. This is what
    // aims the teardown at the peer that is actually stalled — and the dead mark set above
    // is what makes the teardown REACHABLE, by emptying the queue between here and it.
    //
    // Consistent with lru_purge_enable=false: that admission contract forbids evicting a
    // LIVE peer to make room for a new one, and a peer that has failed
    // kMaxConsecutiveTxDrops bounded sends with no success between is not live mid-stream
    // by the transport's own definition.
    condemn(fd);
}

void httpd_ws_link_t::bound_socket(int fd) const {
    // Per-fd, on the UPGRADED socket only. The alternative — lowering the server's
    // config.send_wait_timeout — would shorten HTTP responses too; a WS frame and a SPA
    // asset have nothing in common but the server. Both calls are best-effort: a link
    // that cannot bound a socket is the pre-#835 behaviour for that one peer, never a
    // reason to refuse it.
    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(send_timeout_ms_ / 1000U);
    tv.tv_usec = static_cast<suseconds_t>((send_timeout_ms_ % 1000U) * 1000U);
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
        ESP_LOGW(kTag, "SO_SNDTIMEO not applied fd=%d (%u ms)", fd, (unsigned)send_timeout_ms_);
    // Disable Nagle on the UPGRADED socket. A libtracer WS frame is a small,
    // self-contained TLV whose reply the peer is already waiting on — the exact
    // request-reply shape Nagle + delayed-ACK stalls, adding tens of ms of pure
    // latency to every round-trip. WS frames carry their own length, so there is
    // nothing for Nagle to coalesce that the framing does not already batch. REST
    // responses on this server are unaffected: they ride sockets this link never
    // upgrades. Best-effort like the timeout above: a peer that cannot take the
    // option is the pre-patch latency for that one peer, never a reason to refuse it.
    const int nodelay = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0)
        ESP_LOGW(kTag, "TCP_NODELAY not applied fd=%d", fd);
    // Notice a peer that vanishes WITHOUT a FIN — a Wi-Fi drop, a power cut, a killed
    // browser tab, a NAT rebind (#957). Enumerating the ways an inbound session can end
    // gives peer CLOSE/FIN, a handler returning ESP_FAIL, the TX failure streak, the
    // short-write condemn, and httpd_stop — there is no timer among them. Nor does
    // esp_http_server's LRU purge stand in for one: the owning ctor sets
    // `lru_purge_enable = false` outright, and in adopting mode the flag belongs to the
    // server's owner and IDF exposes no reader for it — but a purge fires on socket
    // exhaustion, never on idleness, so an idle peer is not reclaimed either way while
    // sockets remain. The TX streak cannot stand in: `note_tx_result` is fed only from
    // `tx_work`, so a session this link never sends to accrues no evidence at all.
    // Without keepalive probes such a peer holds its slot and one unit of `max_peers` for
    // the life of the process; with them the stack fails the socket, httpd closes the
    // session, and the ordinary free_ctx → reclaim_slot → notify_peer_down path runs.
    //
    // Applied HERE and not left to the server's own `keep_alive_enable`, for the same
    // reason the send bound is: an adopted server's config belongs to its owner and this
    // link must not depend on the owner having got it right — and in owning mode
    // HTTPD_DEFAULT_CONFIG leaves keepalive off. The three tunables are the ones IDF
    // documents as the defaults for that same server config (esp_http_server.h:
    // keep_alive_idle / _interval / _count), so the policy is IDF's, stated per WS
    // socket. Best-effort, and only behind the enable: without SO_KEEPALIVE the tunables
    // mean nothing, so a stack that refuses it keeps the pre-#957 behaviour for that one
    // peer rather than half a policy.
    const int keepalive = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) != 0) {
        ESP_LOGW(kTag, "SO_KEEPALIVE not applied fd=%d (a silent peer death is undetected)", fd);
    } else {
        const int idle = kKeepIdleSeconds;
        const int intvl = kKeepIntervalSeconds;
        const int cnt = kKeepProbes;
        // Each attempted independently: a stack that refuses one tunable still gets the
        // others, and `||` would stop at the first refusal.
        int refused = ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0;
        refused += ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) != 0;
        refused += ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) != 0;
        if (refused != 0)
            ESP_LOGW(kTag, "%d keepalive tunable(s) refused fd=%d (stack defaults apply)", refused,
                     fd);
    }
    // The short-write guard is not optional decoration: a BOUNDED write is exactly the
    // one that can expire mid-buffer, so shortening the bound raises the rate of the case
    // esp_http_server reports as success. See send_guarded.
    if (httpd_sess_set_send_override(handle_.load(std::memory_order_relaxed), fd,
                                     &httpd_ws_link_t::send_guarded) != ESP_OK)
        ESP_LOGW(kTag, "send override not installed fd=%d (short writes unguarded)", fd);
}

void httpd_ws_link_t::check_httpd_stack() {
    // One report per link, then never again: the answer does not change, and the sample
    // itself is not free — FreeRTOS computes the mark by scanning the untouched fill
    // pattern, so it costs a pass over the FREE part of the stack. That is why this sits
    // on the session-claim edge (once per connection, next to the five setsockopts
    // admission already pays) and not on the per-frame delivery path.
    if (stack_named_) return;
    // BYTES. ESP-IDF's uxTaskGetStackHighWaterMark deliberately diverges from vanilla
    // FreeRTOS here and its own task.h says so ("in bytes (as opposed to words in the
    // standard FreeRTOS documentation)"), so the comparison below needs no conversion —
    // and IDF's FreeRTOSConfig.h defines INCLUDE_uxTaskGetStackHighWaterMark to 1
    // unconditionally, so the call is available on every target this TU builds for.
    const std::size_t free_bytes = uxTaskGetStackHighWaterMark(nullptr);
    if (free_bytes >= kStackHeadroomFloor) return;
    stack_named_ = true;
    // ERROR, not warning: the failure this predicts is a stack-protection panic on the
    // batch-apply path — a reboot, and one that reads as an unrelated flake because it
    // needs that one workload to appear. Owning mode configured the stack itself, so a
    // report here means the measured figure is no longer enough (bump
    // kRequiredHttpdStack); adopting mode means the server this link was handed was
    // started with too small a stack, which nothing but this sample can observe.
    ESP_LOGE(kTag,
             "httpd task free stack %u B < %u B: the in-call graph delivery is close to "
             "overflowing it%s (needs stack_size >= %u B)",
             (unsigned)free_bytes, (unsigned)kStackHeadroomFloor,
             owns_httpd_ ? "" : " - this link ADOPTED the server, so its stack is yours to size",
             (unsigned)kRequiredHttpdStack);
}

int httpd_ws_link_t::send_guarded(httpd_handle_t handle, int fd, const char* buf, std::size_t len,
                                  int flags) {
    (void)handle;
    // The write esp_http_server would have done, with its own error mapping — a plain
    // BSD send, which this TU already owns the socket layer for. It is NOT delegated to
    // the server's default send function: httpd_socket_send routes back through THIS
    // override (that is what an override is), and the default is private to the
    // component. So the write happens here and the return value is inspected here.
    if (buf == nullptr) return HTTPD_SOCK_ERR_INVALID;
    // The session this socket belongs to, read BEFORE the write: it carries the frame
    // bracket tx_work opened, which is the only way this override can tell which write of
    // a frame it is on. Resolved the same way every other latched callback resolves its
    // state — through the server's own session table, on the httpd task, so the answer is
    // authoritative for that instant and needs no generation check (#954). It costs one
    // session-table lookup per write instead of per detected fault, which is not a new
    // order of cost on this path: `httpd_socket_send` performs the IDENTICAL lookup (a
    // scan of at most `max_open_sockets` descriptors) immediately before every call it
    // makes to this function, and the bounded `::send` below dominates both.
    auto* const slot = static_cast<session_t*>(httpd_sess_get_ctx(handle, fd));
    const int ret = static_cast<int>(::send(fd, buf, len, flags));
    // A failed write that is NOT the frame's first one leaves the peer holding a header
    // promising bytes that never arrive (#951). esp_http_server writes one frame as two
    // calls to this function — header, then payload — and reports either failure as one
    // `ESP_FAIL`, so the caller cannot tell "the frame never started" from "the frame was
    // announced and then truncated". Only the second is a lost frame the peer can retry;
    // the first is the same unparseable stream a short write produces, and the peer will
    // consume every later frame as this one's missing payload.
    const bool truncated =
        ret < 0 && slot != nullptr && slot->tx_frame_open && slot->tx_frame_bytes != 0;
    if (ret >= 0 && slot != nullptr && slot->tx_frame_open)
        slot->tx_frame_bytes += static_cast<std::size_t>(ret);
    // A SHORT write: the bound expired with SOME of this buffer already on the wire.
    // esp_http_server checks only `ret < 0`, so returning the partial count here would
    // report a half-written WebSocket frame as a delivered one and leave the peer parsing
    // the remainder of the frame as the next frame's header — silent stream corruption,
    // for as long as the socket lives.
    const bool short_write = ret >= 0 && static_cast<std::size_t>(ret) < len;
    if (!truncated && !short_write) {
        if (ret >= 0) return ret;
        const int err = errno;
        return err == EAGAIN || err == EWOULDBLOCK || err == EINTR ? HTTPD_SOCK_ERR_TIMEOUT
                                                                   : HTTPD_SOCK_ERR_FAIL;
    }
    // One verdict for both shapes, because they are one fault: the peer's byte stream no
    // longer parses. Resolve the link the same way every other latched callback does —
    // through the session's slot and its gate, both of which deliberately outlive the
    // link — and never while the write is in flight.
    if (slot != nullptr && slot->gate != nullptr) {
        const std::lock_guard lock(slot->gate->m);
        if (httpd_ws_link_t* const owner = slot->gate->link; owner != nullptr)
            owner->note_send_desync(
                slot, truncated ? "frame truncated" : "short write",
                slot->tx_frame_open ? slot->tx_frame_bytes : static_cast<std::size_t>(ret),
                truncated ? len : len - static_cast<std::size_t>(ret));
    }
    return HTTPD_SOCK_ERR_FAIL;  // and report the failure IDF would otherwise have missed
}

void httpd_ws_link_t::note_send_desync(session_t* slot, const char* cause, std::size_t on_wire,
                                       std::size_t lost) {
    std::string peer;
    int fd = -1;
    {
        const std::lock_guard lock(peers_m_);
        // The slot came from the server's own session table, resolved inside the write on
        // the httpd task, so it IS the current owner of that socket — no fd scan (which
        // would ask the aliasing question again) and no generation check (there is no
        // elapsed time for a swap to happen in). Not open => not a peer of ours to condemn.
        if (slot == nullptr || !slot->open) return;
        if (slot->dead) return;  // already condemned — this stream is going
        slot->dead = true;
        peer = slot->name;
        fd = slot->fd;
    }
    // Named from the slot, not from the socket — see note_tx_result.
    ESP_LOGE(kTag,
             "%s peer=%s fd=%d (%u B of the frame on the wire, %u B lost) - closing: stream "
             "desynchronised",
             cause, peer.c_str(), fd, (unsigned)on_wire, (unsigned)lost);
    // NOT the streak: a different fault class. The streak means "this peer keeps missing
    // whole frames"; this means "the byte stream is no longer parseable", and one
    // occurrence is already proof. Dropping the frame and keeping the socket — the #481
    // response — is only sound when ZERO bytes of it reached the wire, which is the
    // precondition @ref send_guarded now ESTABLISHES per frame rather than assumes (#951):
    // a header that went out with its payload lost fails it just as a short write does.
    //
    // This close is ONE-SHOT by construction (the mark above makes a second call return
    // early), so it may not be an operation that can silently fail to happen — which the
    // control queue's is. @ref condemn is the one that cannot.
    condemn(fd);
}

void httpd_ws_link_t::tx_work(void* arg) {
    auto* const work = static_cast<tx_work_t*>(arg);
    // This item runs exactly once per successful enqueue and owns its slot for the whole
    // call: the claim marked the slot busy before a byte was written into it, and nothing
    // clears that flag but the release at the end of this function. `work->slot` is stable
    // for the link's life (bound once in alloc_buffers; the abandon path leaks the pool
    // rather than freeing it, exactly so this stays true).
    //
    // Resolve the destination SESSION back to a socket, and refuse to invent one. This is
    // the checkpoint the old bare-fd path had no way to pass: it asked
    // `httpd_ws_get_fd_info(handle, fd)`, which answers "some websocket lives at this
    // number" and cannot answer "the session this frame was gathered for" — IDF's session
    // lookup is purely fd-keyed. A frame queued for a peer that hung up was therefore
    // written into whichever client had since been accepted onto the recycled descriptor
    // (#954). live_fd fails that reference instead, and the same call carries the two
    // skips that were already here: a departed peer, and one condemned after this frame
    // was queued (attempting that costs the full send bound on a socket already known to
    // be broken, while the queue behind it holds the close). The gate is taken briefly and
    // released before the send: holding it across the write would block a concurrent
    // destructor for the whole bound. Lock order is gate->m then peers_m_, the same order
    // on_session_closed and the accounting below use.
    int fd = -1;
    bool refresh_lru = false;
    if (work->gate != nullptr) {
        const std::lock_guard lock(work->gate->m);
        if (httpd_ws_link_t* const owner = work->gate->link; owner != nullptr) {
            fd = owner->live_fd(work->to);
            // Adopted mode only. In owning mode the purge this defends against is off by
            // construction (the ctor sets lru_purge_enable = false on the cfg it starts
            // the server with), so the scan below would be measurable cost buying a
            // counter nobody reads. Read here because this is the one place the link is
            // already resolved; owns_httpd_ is ctor-set and never written again.
            refresh_lru = !owner->owns_httpd_;
        }
    }
    // The server's own verdict, on the socket the LINK just vouched for — kept as the
    // second opinion it always was (a peer mid-CLOSE is a websocket the link still has
    // open), never as the identity test it was being asked to be.
    const bool live =
        fd >= 0 && httpd_ws_get_fd_info(work->handle, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
    esp_err_t err = ESP_OK;
    if (live) {
        httpd_ws_frame_t f = {};
        f.final = true;
        f.fragmented = false;
        f.type = HTTPD_WS_TYPE_BINARY;
        f.payload = reinterpret_cast<std::uint8_t*>(work->payload);
        f.len = work->len;
        // Bracket the send: everything the override lands between these two calls belongs
        // to THIS frame, which is what lets it tell a frame that never started from one it
        // announced and could not finish (#951). Only the caller knows that boundary —
        // esp_http_server splits the frame into a header write and a payload write and
        // tells the override nothing about the split. The slot is the one live_fd just
        // vouched for, so no further identity test is owed here, and both calls run on the
        // httpd task with the synchronous send between them.
        session_t* const slot = work->to.slot;
        if (slot != nullptr) slot->open_tx_frame();
        err = httpd_ws_send_frame_async(work->handle, fd, &f);
        if (err == ESP_OK && refresh_lru) {
            // Tell the ADOPTED server this session is not idle (#955). Apart from this very
            // API, IDF advances a session's LRU counter in one place — the tail of
            // httpd_sess_process, i.e. inbound request processing — so a server-initiated
            // push does not touch it. A graph peer that subscribes and thereafter only
            // RECEIVES therefore ages toward the lowest counter no matter how much this link
            // is pushing at it, and httpd_accept_conn's victim search (which excludes only
            // sessions marked for_async_req, never a WS one) picks the lowest. The pure
            // subscriber was the preferential victim precisely BECAUSE the push stream is
            // invisible to the counter; refreshing here removes that inversion.
            //
            // It is NOT immunity, and must not be described as any: at the host's socket
            // ceiling some session is still closed, and an evicted peer reaches this link
            // as an ordinary departure. The inbound direction needs nothing — httpd_sess_process
            // already covers it, and this handler runs inside it.
            //
            // Safe on this task: the function walks hd->hd_sd and bumps hd->lru_counter with
            // no lock of its own, and tx_work runs on the httpd task (httpd_queue_work put it
            // there), which is the task that owns that table. The result is dropped on
            // purpose — ESP_ERR_NOT_FOUND only says the session left between the send and
            // now, which is neither this frame's business nor actionable.
            (void)httpd_sess_update_lru_counter(work->handle, fd);
        }
        const std::size_t on_wire = slot != nullptr ? slot->close_tx_frame() : 0;
        // `on_wire != 0` is the OTHER failure, and it is not this one's: the frame was
        // announced and then cut off, the stream no longer parses, and the session has
        // already been condemned inside the write (note_send_desync logs it at ERROR with
        // the byte counts). It must not also be reported below — the two used to produce
        // the same line, which is why a desynchronised session looked benign in the field.
        if (err != ESP_OK && on_wire == 0) {
            // DROP the frame; do NOT close the session on THIS failure alone (#481). One
            // failed async send means the peer missed ONE frame it can retry — not that
            // the socket is dead. The load-bearing case: a large reply (e.g. the
            // composed-root snapshot, ~12.7 KB) whose one contiguous WS frame exhausts
            // its send bound while the SAME socket still delivers small frames fine.
            // Closing on it tore the whole session down and killed the peer's follow-on
            // small requests ("transport closed") — the dead-web-ui churn. Dropping keeps
            // the socket alive, so the peer's next (small) request succeeds and its own
            // deadline/retry recovers the missed reply.
            //
            // #835 supersedes the OTHER half of that comment, not this one: the result is
            // now fed to the destination's streak below, so an interleaved success still
            // resets it (the #481 shape is untouched) while a peer failing
            // kMaxConsecutiveTxDrops sends in a row — which is the doc's own definition of
            // a broken session — is finally the one that gets torn down. It used to be
            // the refused ENQUEUES that closed sessions, and those name the shared control
            // queue rather than any peer (see note_enqueue_drop).
            //
            // #951 supersedes nothing here either, and narrows the GUARD instead: "the peer
            // missed one frame it can retry" is only true while none of it reached the
            // wire, and that is now a measured precondition (`on_wire == 0`) rather than a
            // property assumed of every ESP_FAIL.
            ESP_LOGW(kTag, "ws send failed (%s) fd=%d len=%u - frame dropped", esp_err_to_name(err),
                     fd, (unsigned)work->len);
        }
    }
    // Copy out everything the accounting needs BEFORE the slot goes back to the pool:
    // once released, another task may claim it and overwrite the work item.
    gate_t* const gate = work->gate;
    const session_ref_t to = work->to;
    const std::size_t len = work->len;
    release_tx_work(work);  // recycle the pool slot (and any oversize heap payload)
    // A skipped send is not evidence for the STREAK: no result. Every skip qualifies — the
    // peer departed, a different session now holds its slot, or it was condemned and the
    // verdict is already in. It is still a frame the peer never got, so the CUMULATIVE
    // tally takes it (note_tx_skip): the streak asks "is this session broken?", the tally
    // asks "how much has this session lost?", and a skip answers only the second.
    if (!live) {
        if (gate != nullptr) {
            const std::lock_guard lock(gate->m);
            if (httpd_ws_link_t* const owner = gate->link; owner != nullptr)
                owner->note_tx_skip(to);
        }
        return;
    }
    if (gate == nullptr) return;
    // Resolve the link through the gate — the same contract on_session_closed uses. Held
    // for the whole call, so a concurrent destructor either waits here or finds the gate
    // already shut and this outcome has nobody to inform.
    const std::lock_guard lock(gate->m);
    if (httpd_ws_link_t* const owner = gate->link; owner != nullptr)
        owner->note_tx_result(to, err == ESP_OK, len);
}

void httpd_ws_link_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void httpd_ws_link_t::send(std::span<const std::span<const std::byte>> iov) {
    // Broadcast: snapshot the destinations under the lock, then enqueue unlocked — the
    // per-session drop accounting inside queue_send takes peers_m_ itself. Overriding
    // the iovec entry point means a rope reply is gathered ONCE per peer, straight
    // into the queued work buffer — the base default's gather-into-a-temporary would
    // double-buffer a large reply (flatten temp + tx copy live simultaneously), the
    // heap spike behind the on-device OOM abort.
    //
    // What the snapshot holds is a SESSION reference per peer, not a bare fd: the lock is
    // released before the first enqueue and the frames are written later still, so a
    // descriptor read here can belong to somebody else by the time it is used (#954). The
    // reference is minted here, under the same lock that read the peer's liveness.
    //
    // And it is a FIXED on-stack chunk with a resumable scan, never a container sized to
    // the peer set (#961) — see kFanoutChunk for why a `std::vector` here was an abort
    // waiting for a heap trough. Resuming at `next` after releasing the lock is sound
    // because a slot's INDEX never moves while the link is serving: the only in-service
    // mutation of `slots_` is the APPEND at the single claim site (the push_back in
    // on_data_frame), and the two sites that remove entries —
    // abandon_sessions' clear() and abandon_session's erase() — are reachable only through
    // detach_sessions(), which nothing but the destructor calls. So no peer can be visited
    // twice, and a departed slot is a hole the scan steps over. A peer that ARRIVES
    // mid-fan-out lands past `next` and is simply included, which a broadcast (already
    // non-atomic: every enqueue happens outside the lock, and the writes later still) has
    // never promised either way.
    session_ref_t targets[kFanoutChunk];
    std::size_t next = 0;
    for (bool more = true; more;) {
        std::size_t n = 0;
        {
            const std::lock_guard lock(peers_m_);
            // A condemned peer is skipped from the lock the snapshot already holds, so the
            // fan-out never even offers it a frame (queue_send would refuse it anyway).
            while (next < slots_.size() && n < kFanoutChunk) {
                const auto& s = slots_[next++];
                // `!auth_pending` (#1184): a broadcast is how a subscription push reaches its
                // peers, so including an unauthenticated session would leak vertex VALUES to
                // a peer that has presented no credential — the most direct possible defeat
                // of the gate, and the one that needs no lookup at all.
                if (s->open && !s->dead && !s->auth_pending)
                    targets[n++] = session_ref_t{s.get(), s->gen};
            }
            more = next < slots_.size();
        }
        for (std::size_t i = 0; i < n; ++i) queue_send(targets[i], iov);
    }
}

void httpd_ws_link_t::peer_resolution_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void httpd_ws_link_t::peer_resolution_t::send(std::span<const std::span<const std::byte>> iov) {
    // The directed reply path (fwd_router hands the reply rope's iovec here): one
    // nothrow gather into the tx work item, no intermediate flatten temporary.
    //
    // ONE load into a local, for the reason queue_send records (#963): a teardown may be
    // writing this member while a producer reads it.
    httpd_ws_link_t* const owner = owner_;
    if (owner == nullptr) return;  // retired into inert memory by a teardown
    // The identity test #1013 exists for. `gen_` was captured when the CALLER resolved this
    // peer, and is only ever COMPARED here — never re-read from the slot, which is what the
    // shared per-slot endpoint this replaced had to do. Between a resolve and its send the
    // httpd task can close the session and accept another onto the same descriptor AND the
    // same recycled slot — a browser reload — and a generation minted after that swap
    // describes the stranger, so the check passed and the reply was written into the wrong
    // socket, on a link whose whole point is that a reply reaches only the tab that asked
    // (#954).
    session_ref_t to;
    {
        const std::lock_guard lock(owner->peers_m_);
        session_t* const slot = slot_;
        if (slot == nullptr || !slot->open || slot->dead || slot->gen != gen_) {
            // Counted HERE as well as at queue_send's head (#953), and it has to be: this
            // early-out is the one a directed reply actually takes, so a counter only at
            // the queue_send locus would read zero for the very path most likely to race a
            // departure. Same field either way — it is one event with two detection sites,
            // and each drops exactly one frame.
            owner->tx_to_dead_peer_.fetch_add(1, std::memory_order_relaxed);
            return;  // departed, condemned, or SUPERSEDED => no-op
        }
        to = session_ref_t{slot, gen_};
    }
    owner->queue_send(to, iov);
}

// ---------------------------------------------------------------------------
// bus_link_t facet — peer enumeration / resolution (cross-thread reads).
// ---------------------------------------------------------------------------

void httpd_ws_link_t::enumerate_peers(const peer_visitor_t& visit) const {
    const std::lock_guard lock(peers_m_);
    // `!dead` as well as `open` (#963): condemn() shuts the socket and marks the slot
    // dead, then WAITS for httpd's select loop to notice EOF and run free_ctx. Through
    // that gap every sending path already refuses the peer — send()'s snapshot filters
    // !dead, queue_send re-resolves through live_fd, tx_work skips a doomed slot — so a
    // census that still lists it makes the link answer "this peer exists" and "this peer
    // is unreachable" at the same instant. The facet reports REACHABILITY, not table
    // occupancy. The gap is one select round when the server is healthy, and condemn()
    // exists precisely for when it is not.
    //
    // `!auth_pending` on top of both (#1184): a session that has not presented a credential
    // is admitted at the transport level and served nothing, and "not discoverable" is part
    // of what serving nothing means — a peer that could be enumerated could be addressed by
    // whoever read the census, which is the enumerable⇒addressable invariant working against
    // the gate. Same reasoning as the `!dead` filter beside it: the facet reports peers this
    // link will actually carry frames for.
    for (const auto& s : slots_)
        if (s->open && !s->dead && !s->auth_pending && !s->name.empty()) visit(s->name);
}

void httpd_ws_link_t::enumerate_peer_stats(const peer_stats_visitor_t& visit) const {
    const std::lock_guard lock(peers_m_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto& s = slots_[i];
        if (!s->open) continue;  // a reclaimed slot keeps stale numbers — never report it
        // Unauthenticated sessions are absent here for the reason recorded on
        // enumerate_peers: they are not peers yet. Their existence is still observable, but
        // through the LINK's counters (stats_t::auth_rejected / auth_expired), which is the
        // right altitude for "something is knocking" — a per-session census of things that
        // have not identified themselves is a census of an attacker's socket count.
        if (s->auth_pending) continue;
        // The counters are COPIED into the visitor's argument; `name`, `endpoint_str` and
        // `subject` borrow, and only for the duration of the call (same contract as
        // enumerate_peers).
        visit(peer_stats_t{s->name, i, s->gen, s->st, s->endpoint_str, s->subject});
    }
}

transport_t* httpd_ws_link_t::peer_link(std::string_view peer) {
    const std::lock_guard lock(peers_m_);
    // `!dead` for the reason on enumerate_peers (#963): the endpoint this used to hand
    // out for a condemned slot was a guaranteed no-op, so a router lookup succeeded and
    // every frame through it was silently discarded. Refusing here turns that into an
    // honest "no such peer" the caller can act on.
    // `!auth_pending` (#1184): resolving an unauthenticated session would hand the routing
    // plane a working endpoint for a peer that has presented nothing, which is the whole
    // gate defeated by one lookup — a directed FWD reply does not consult the census.
    for (const auto& s : slots_)
        if (s->open && !s->dead && !s->auth_pending && s->name == peer)
            return acquire_resolution(s.get());
    return nullptr;
}

std::string_view httpd_ws_link_t::peer_name(peer_handle_t peer, std::span<char> scratch) const {
    // Positional by construction (ADR-0073 §2): the name a slot is stamped with at the
    // claim IS 'p' + its index, so the inverse is that formatting and needs neither
    // `peers_m_` nor the slot vector. Written out by hand for the reason @ref slot_name
    // is — `std::to_string` is +1456 B of this TU per call site — and into the CALLER's
    // scratch, so it heap-allocates nothing on the per-frame path.
    if (!peer.valid() || scratch.size() < 2) return {};
    char buf[24];
    char* p = buf + sizeof(buf);
    std::uint32_t idx = peer.index;
    do {
        *--p = static_cast<char>('0' + (idx % 10));
        idx /= 10;
    } while (idx != 0);
    *--p = 'p';
    const auto len = static_cast<std::size_t>(buf + sizeof(buf) - p);
    if (len > scratch.size()) return {};
    for (std::size_t i = 0; i < len; ++i) scratch[i] = p[i];
    return std::string_view(scratch.data(), len);
}

httpd_ws_link_t::peer_resolution_t* httpd_ws_link_t::acquire_resolution(session_t* slot) {
    // A second resolution of the SAME live session answers with the first's handle. Both
    // saw the same generation — that is what makes sharing safe here and what made the
    // per-SLOT endpoint unsafe: the hazard was ever sharing one object ACROSS generations.
    // It is also what bounds the pool by the peer population instead of by traffic, so a
    // forward hop's resolve allocates nothing in steady state.
    if (slot->resolution != nullptr) return slot->resolution;
    peer_resolution_t* got = nullptr;
    // Grow before recycling, until the quarantine is stocked (kResolutionSpare). A handle
    // is restamped for a DIFFERENT session when it comes back out, so handing back the one
    // most recently retired would reproduce #1013 for a caller still holding it; the free
    // list is FIFO and this keeps a depth of retirements between the two events.
    if (free_resolutions_n_ > kResolutionSpare) {
        got = free_resolutions_;
        free_resolutions_ = got->free_next_;
        if (free_resolutions_ == nullptr) free_resolutions_tail_ = nullptr;
        --free_resolutions_n_;
        got->free_next_ = nullptr;
    } else if (resolutions_.size() < slots_.size() + kResolutionSpare) {
        // Same shape as the slot claim's own growth a few lines up: one small object per
        // peer past the high-water mark, never per frame.
        auto r = std::make_unique<peer_resolution_t>();
        got = r.get();
        resolutions_.push_back(std::move(r));
    } else if (free_resolutions_ != nullptr) {
        // At the cap with a shallow quarantine: recycle rather than refuse. Reachable only
        // when the pool is already sized for every slot, i.e. when the retirements ahead of
        // this one are the link's whole peer population.
        got = free_resolutions_;
        free_resolutions_ = got->free_next_;
        if (free_resolutions_ == nullptr) free_resolutions_tail_ = nullptr;
        --free_resolutions_n_;
        got->free_next_ = nullptr;
    }
    // Nothing to hand out => an honest "no such peer" (the caller's frame is dropped and
    // counted at ITS end), never a handle that could name the wrong session. FAIL CLOSED is
    // the whole point of the change; a misdelivery is not a lesser failure than a drop.
    if (got == nullptr) return nullptr;
    got->owner_ = this;
    got->slot_ = slot;
    got->gen_ = slot->gen;  // the CAPTURE — read once, here, under peers_m_ (#1013)
    slot->resolution = got;
    return got;
}

void httpd_ws_link_t::retire_resolution(session_t* slot, bool inert) {
    peer_resolution_t* const r = slot->resolution;
    if (r == nullptr) return;
    slot->resolution = nullptr;
    if (inert) {
        // The teardown fork: this slot's shell is about to be LEAKED rather than freed, so
        // the handle must not keep a pointer into it. Unbinding is what makes a send that
        // outlives the link land on valid, inert memory — the same rule `neutralise` keeps
        // for the slot itself.
        r->owner_ = nullptr;
        r->slot_ = nullptr;
    }
    // Append at the TAIL: the free list is FIFO so that the handle just retired is the LAST
    // one handed back out (see kResolutionSpare).
    r->free_next_ = nullptr;
    if (free_resolutions_tail_ != nullptr) {
        free_resolutions_tail_->free_next_ = r;
    } else {
        free_resolutions_ = r;
    }
    free_resolutions_tail_ = r;
    ++free_resolutions_n_;
}

bool httpd_ws_link_t::close_peer(std::string_view peer) {
    // A FLAT link has no per-peer identity to close BY: every tab it carries answers to the
    // one registered child NAME, so the only honest answer is "this kind cannot close one
    // peer" — the base's own `false`. Same fork notify_departed takes (#889).
    if (!peer_named_) return false;
    // ONE load of each member into a local, for the reason queue_send records (#963): the
    // plain re-reads these replace raced the destructor's writes outright.
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    gate_t* const g = gate_.load(std::memory_order_relaxed);
    if (h == nullptr || g == nullptr) return false;
    session_ref_t to;
    {
        const std::lock_guard lock(peers_m_);
        // The SAME visibility filter peer_link and enumerate_peers apply, and it has to be
        // the same one: a name this link refuses to resolve and refuses to enumerate is a
        // name it must also refuse to close, or `close_peer` becomes a probe that answers
        // `true` for sessions the caller was told do not exist (an unauthenticated one, or
        // one already condemned and awaiting its reap).
        for (const auto& s : slots_) {
            if (!s->open || s->dead || s->auth_pending || s->name != peer) continue;
            // Mint the identity HERE, under the lock that resolved it (#954). The close runs
            // on the httpd task an arbitrary time later, by which point this slot may have
            // been reclaimed and re-earned the very same positional name — `p3` is a pure
            // function of the slot index, so the NAME cannot survive as the token. The
            // generation can.
            to = session_ref_t{s.get(), s->gen};
            break;
        }
    }
    if (to.slot == nullptr) return false;  // no served session answers to that name
    // Heap, nothrow, one per call: this is an administrative action, not a data-path one,
    // so it does not draw on the TX pool whose whole purpose is to bound the FRAME path's
    // in-flight depth. A revocation competing with the fan-out for slots would be exactly
    // backwards — it is most needed when that pool is under pressure.
    auto* const req = new (std::nothrow) close_req_t{g, to};
    if (req == nullptr) return false;
    // The honest half of the contract, and the whole reason #1146 was blocked on a transport
    // fact rather than on effort. Above the component's ESP-IDF floor (>=5.5.5) this verdict
    // means what it says: `httpd_queue_work` reserves the control mbox slot through a
    // counting semaphore BEFORE its `sendto`, so ESP_OK implies the item will run and a full
    // queue is a visible ESP_FAIL. Below that floor the enqueue was a bare non-blocking
    // `sendto` whose datagram lwIP could bin while still reporting success (#944/#949) — on
    // which a `true` here would have been a security-relevant lie, told precisely when the
    // queue is fullest, which is when a stalling peer most needs revoking.
    if (httpd_queue_work(h, &httpd_ws_link_t::close_work, req) != ESP_OK) {
        delete req;
        return false;  // refused: nothing was initiated, and the caller may retry
    }
    return true;
}

void httpd_ws_link_t::close_work(void* req_arg) {
    // Runs ON the httpd task (httpd_queue_work's whole contract) — the task that owns
    // accept, close and therefore the descriptor's lifetime, which is what makes the
    // `shutdown` inside close_session safe here and unsafe from the caller's task (#954's
    // recycled-fd hazard, and the precondition condemn() documents).
    const std::unique_ptr<close_req_t> req(static_cast<close_req_t*>(req_arg));
    if (req->gate == nullptr) return;
    httpd_ws_link_t* owner = nullptr;
    {
        // Resolve the link through the gate, exactly as tx_work and on_session_closed do:
        // a destructor can only shut the gate under this same lock, so either this item
        // finds a link that is provably alive or it finds null and is inert. Registering on
        // the barrier is what lets the close itself run with `m` RELEASED — close_session
        // takes `peers_m_` and calls into the server, and holding the gate across that
        // would block a concurrent destructor for the whole send bound (the rule gate_t's
        // lock-order note states).
        const std::lock_guard lock(req->gate->m);
        owner = req->gate->link;
        if (owner == nullptr) return;  // the link is gone; the session is not ours to close
        ++req->gate->depth;
    }
    owner->close_ref(req->to);
    {
        // `owner` may be DESTROYED by now — the departure this close provokes reaches the
        // routing plane through free_ctx and an app teardown can follow it in-call (#814),
        // the same tail on_session_closed carries. Only `gate` may be touched from here.
        const std::lock_guard lock(req->gate->m);
        --req->gate->depth;
    }
    req->gate->cv.notify_all();
}

void httpd_ws_link_t::close_ref(const session_ref_t& to) {
    {
        const std::lock_guard lock(peers_m_);
        if (to.slot == nullptr) return;
        // live_fd's identity test, made again HERE and not trusted from enqueue time. The
        // reference was minted on another task and this item drained arbitrarily later, so
        // the named session may have departed on its own in between — and its slot may have
        // been reclaimed and re-claimed by a stranger who inherited both the slot address
        // and the positional name. A failed test means the caller's peer is already gone,
        // which is the outcome it asked for; the successor is not touched (#954).
        if (to.slot->gen != to.gen) return;
        if (!to.slot->open || to.slot->dead) return;
    }
    // No further identity test is owed below: everything that could invalidate the
    // reference — a departure's reclaim_slot, a fresh claim — runs on THIS task, so it
    // cannot interleave between the check above and the close.
    //
    // close_session, not condemn: the peer is told WHY in the one language a WebSocket
    // client can read without a protocol of its own. A revoked controller that sees a bare
    // socket teardown reads it as a network fault and reconnects forever, which is the
    // behaviour the distinct close codes exist to prevent. The CLOSE frame goes out first
    // and the `shutdown` second — the order close_session exists to keep, since after the
    // shutdown no write can reach the peer at all.
    close_session(to.slot, kCloseRevoked);
}

}  // namespace tr::net
