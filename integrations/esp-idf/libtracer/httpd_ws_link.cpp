/**
 * @file
 * @brief `httpd_ws_link_t` implementation — see include/libtracer_esp/httpd_ws_link.hpp.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Chip-target-only TU (needs esp_http_server + lwIP BSD sockets), selected by the
 * component CMakeLists — never an in-source #ifdef, the same rule twai_link.cpp
 * follows. The linux virtual board keeps core's raw-socket ws_server_transport_t.
 */

#include "libtracer_esp/httpd_ws_link.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "libtracer/egress.hpp"

namespace tr::net {

// `httpd_ws_link_t` derives from `bus_link_t` unconditionally — it serves many inbound
// browser sessions and names each one — so it is exactly the module a `kBusLinks = false`
// target declared it does not carry, and it has no flat mode to be demoted to. Refused at
// BUILD time, the way `transport_can.cpp` refuses CAN (#1670, compile-time-by-default): the
// component compiles this TU only under CONFIG_LIBTRACER_WS_SERVER, which SELECTS
// CONFIG_LIBTRACER_BUS_LINKS, and core's host suites compile it only in a build that binds the
// bus tier (LIBTRACER_TEST_ESP_WS_SERVER). So the assert is actionable wherever it can fire.
static_assert(kBusLinks,
              "httpd_ws_link.cpp is the WebSocket SERVER link, a bus by construction, and this "
              "build binds kBusLinks = false (the default since v0.17.0). ESP-IDF: enable "
              "CONFIG_LIBTRACER_WS_SERVER, which selects CONFIG_LIBTRACER_BUS_LINKS. Otherwise "
              "bind `static constexpr bool kBusLinks = true;` in libtracer/config_override.hpp, "
              "or do not compile this TU.");

// stats_t's layout is what a dashboard or a pinned integrator reads (#2195). Sixteen
// 32-bit tallies, in this order, a new one only ever appended (#2198's peers_frozen); a
// field added, moved or widened fails here first.
static_assert(sizeof(httpd_ws_link_t::stats_t) == 16 * sizeof(std::uint32_t));
static_assert(offsetof(httpd_ws_link_t::stats_t, enqueue_drops) == 0);
static_assert(offsetof(httpd_ws_link_t::stats_t, tx_conflated) == 4);
static_assert(offsetof(httpd_ws_link_t::stats_t, rx_drain_waits) == 56);
static_assert(offsetof(httpd_ws_link_t::stats_t, peers_frozen) == 60);

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
 * @brief The link whose WebSocket handler frame THIS task is currently inside, or nullptr
 *        — the "this send is a reply being serviced in-call" predicate (#1494).
 *
 * Deliberately NOT `server_task_ == current_task()`, which is the weaker question
 * @ref httpd_ws_link_t::close_gate asks (there it only has to know whether waiting for
 * the depth would wait on this very stack frame). Two things a bare task
 * identity cannot tell apart, and the reply fork needs both:
 *   - a work item the httpd task is draining (the auth sweep, a close) is ON the task but
 *     inside no handler frame, so nothing holds the gate's `depth` and nothing pins the
 *     session the frame names — the guarantees the in-call fork leans on;
 *   - on the host, a suite that delivers a frame and then sends from the SAME thread is on
 *     the "httpd task" forever after, which would make the pool suites' subject depend on
 *     which thread they happen to run on rather than on the link.
 *
 * A pointer in `.tbss` (trivial type, no `__cxa_thread_atexit` registration): a handful of
 * bytes in each task's TLS block and a single load to read.
 */
thread_local const httpd_ws_link_t* t_serving_link = nullptr;

/** @brief RAII: mark the calling task as being inside @p link's handler frame for the
 *         duration of the scope, and restore what it was before.
 *
 * Restoring the PREVIOUS value rather than clearing, and never touching @p link in the
 * destructor, is what makes this safe on the one path that matters: the delivery inside
 * the handler runs the app in-call and the app may destroy the link (#814), so by the time
 * this scope unwinds `link` may be gone. Only the saved pointer is written back. */
class serving_scope_t {
   public:
    /** @brief Enter @p link's handler frame on this task. */
    explicit serving_scope_t(const httpd_ws_link_t* link) noexcept : prev_(t_serving_link) {
        t_serving_link = link;
    }
    /** @brief Leave it, restoring whatever the task was serving before. */
    ~serving_scope_t() { t_serving_link = prev_; }
    serving_scope_t(const serving_scope_t&) = delete;
    serving_scope_t& operator=(const serving_scope_t&) = delete;

   private:
    const httpd_ws_link_t* prev_; /**< @brief What this task was serving on entry. */
};

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

/** @brief Whether this build bounds a drain at all: either budget non-zero (ADR-0085). */
constexpr bool kRxDrainPaced = kRxDrainFrames != 0 || kRxDrainBytes != 0;

/**
 * @brief Client sockets an adopted teardown can list (`httpd_get_client_list`) when it asks
 *        whether its handler gate is still reachable (#2202). A server whose list does not
 *        fit refuses it, and the gate is then kept: 64 B of the httpd task's stack, not one
 *        int per lwIP socket.
 */
constexpr std::size_t kMaxClientFds = 16;

/**
 * @brief lwIP's per-connection segment queue and send buffer, the two limits a drain's
 *        in-call replies run into (ADR-0085 §7).
 *
 * Both are the board's lwIP configuration (`lwipopts.h`, from `CONFIG_LWIP_TCP_SND_BUF_DEFAULT`
 * and lwIP's own `TCP_SND_QUEUELEN` formula), reached through the socket headers this file
 * includes. The host build has no lwIP; it binds lwIP's values at IDF's defaults so the
 * host test exercises the same arithmetic.
 */
#if defined(TCP_SND_QUEUELEN)
constexpr std::size_t kTcpSndQueueLen = TCP_SND_QUEUELEN;
#else
constexpr std::size_t kTcpSndQueueLen = 16;  // (4 * 5760 + 1439) / 1440
#endif
#if defined(TCP_SND_BUF)
constexpr std::size_t kTcpSndBuf = TCP_SND_BUF;
#else
constexpr std::size_t kTcpSndBuf = 5760;
#endif

/**
 * @brief One core's IDLE GATE: how many times its idle task has run, and the semaphore a
 *        link whose drain budget is spent waits on until it runs again (ADR-0085).
 *
 * The idle task is the one task that runs only when nothing else on its core wants to, so
 * "it has run" is the event a starved core is missing — no clock, no timer, no tick
 * arithmetic. `epoch` has ONE writer, that core's idle hook, which is why it advances by a
 * load and a store rather than an RMW (a libatomic call on a core without one, every idle
 * iteration). `waiters` is the hook's test for "is anybody parked", so a gate nobody waits
 * on costs the idle loop one store and one load.
 *
 * The pair is a Dekker handshake, both sides `seq_cst`: a waiter counts itself in, then
 * re-reads `epoch`; the hook advances `epoch`, then reads `waiters`. Whichever goes second
 * sees the other, so a waiter either finds the epoch already moved or is given the
 * semaphore. A give nobody takes saturates the binary semaphore at one, and the waiter's
 * loop re-tests the epoch, so a stale give costs one extra pass and never a missed wake.
 *
 * Two links (or two servers) parked on one core share the binary semaphore: one give wakes
 * one of them, and the other wakes on the next idle pass, which comes once the first has
 * read its budget and parked again. Liveness holds; fairness is one drain late.
 *
 * The idle hook is not the only giver. A producer that has just posted egress work to the
 * httpd task (@ref httpd_ws_link_t::egress_posted) gives the same semaphore, with the same
 * handshake against `waiters`: the httpd task is the one drain of its own control queue, so
 * a drain parked for idle must not hold the sends and closes it alone can run. That wake
 * reads one frame and parks again (see @ref httpd_ws_link_t::pace_rx).
 */
struct idle_gate_t {
    std::atomic<std::uint32_t> epoch{0};   /**< @brief Idle-task iterations, mod 2^32. */
    std::atomic<std::uint32_t> waiters{0}; /**< @brief Links parked on @ref sem right now. */
    std::atomic<bool> hooked{false};       /**< @brief The idle hook is installed. */
    StaticSemaphore_t sem_storage{};       /**< @brief @ref sem's storage: no heap, no failure. */
    SemaphoreHandle_t sem = nullptr;       /**< @brief Given by the hook while a link waits. */
};

/** @brief One gate per core: a starved core is a per-core fact (portNUM_PROCESSORS). */
std::array<idle_gate_t, portNUM_PROCESSORS> g_idle_gates;

/**
 * @brief Core @p Core's idle hook: advance its epoch and wake a parked link.
 *
 * Runs ON the idle task, which must never block: `xSemaphoreGive` does not. Answers true,
 * so it never holds the core out of its low-power wait.
 */
template <std::size_t Core>
bool on_core_idle() {
    idle_gate_t& gate = g_idle_gates[Core];
    gate.epoch.store(gate.epoch.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst);
    if (gate.waiters.load(std::memory_order_seq_cst) != 0) (void)xSemaphoreGive(gate.sem);
    return true;
}

/** @brief The hook of every core, indexed by core (the IDF hook takes no context). */
template <std::size_t... Core>
constexpr std::array<esp_freertos_idle_cb_t, sizeof...(Core)> idle_hooks(
    std::index_sequence<Core...> /*cores*/) {
    return {&on_core_idle<Core>...};
}

/**
 * @brief Install every core's idle hook, once per process.
 *
 * Called from each link constructor; the first one installs, the rest find it done. The
 * hooks are never removed: one store and one load per idle iteration is what a process
 * that once served a link keeps paying, and removing them would race a link being torn
 * down on one core against a hook running on another. A core whose hook table is full
 * (IDF holds eight per core) is reported once and left UNPACED — a link must never wait
 * on an idle hook that will not run.
 */
void install_idle_hooks() {
    if constexpr (!kRxDrainPaced) return;
    static const bool installed = [] {
        constexpr auto hooks = idle_hooks(std::make_index_sequence<portNUM_PROCESSORS>{});
        for (std::size_t core = 0; core < hooks.size(); ++core) {
            idle_gate_t& gate = g_idle_gates[core];
            gate.sem = xSemaphoreCreateBinaryStatic(&gate.sem_storage);
            const bool ok = esp_register_freertos_idle_hook_for_cpu(
                                hooks[core], static_cast<UBaseType_t>(core)) == ESP_OK;
            if (!ok)
                ESP_LOGE(kTag, "idle hook not installed on core %u: ingress unpaced",
                         (unsigned)core);
            gate.hooked.store(ok, std::memory_order_release);
        }
        return true;
    }();
    (void)installed;
}

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
 * @brief Frames a drain reads back to back before the session's socket is switched from
 *        TCP_NODELAY to Nagle, so the drain's remaining replies coalesce (ADR-0085 §7).
 *
 * Why the socket must change under a flood, and only then. The receive context is the
 * task that writes the in-call replies, and while it holds unread ingress lwIP holds the
 * peer's data segments as REFUSED (the socket's recv mbox is full) and drops every further
 * data-bearing segment from that peer before processing it, ACK numbers included
 * (`tcp_in.c`, the `refused_data` test at the top of `tcp_input`'s per-pcb path). The
 * peer keeps piggybacking its ACKs on data for as long as it has data to send, so the
 * board's own reply segments stay unacknowledged until the board READS. Under
 * TCP_NODELAY every reply is two segments (header and payload, @ref kIdfWsWriteLegs), so
 * a drain of @ref kRxDrainFrames small replies queues 2x that many against
 * @ref kTcpSndQueueLen; the write that finds the queue full waits for an ACK only the
 * waiting task can release, spends the send bound, and fails: one reply refused, or a
 * header on the wire with its payload lost, and three in a row condemn a healthy session.
 *
 * Nagle is TCP's own answer: with unacknowledged data outstanding, small writes gather in
 * one unsent segment until it fills or an ACK arrives, so a drain's replies cost one or two
 * segments, never the queue. It is wrong for the interactive case (a lone reply's payload
 * leg would wait a delayed-ACK behind its header leg), so it is applied only once a drain
 * has shown the flood shape, this many frames without the core idling, and held until the
 * peer's next LIGHT drain, one that ends by idle before reaching it. A peer that flooded
 * once and then only receives never produces that drain and keeps paying one delayed ACK
 * per push burst until it sends lightly again or reconnects. The threshold is half the
 * queue in frames: what the drain wrote under TCP_NODELAY before switching can never fill
 * it.
 */
constexpr std::size_t kRxDrainNagleFrames = kTcpSndQueueLen / (2 * kIdfWsWriteLegs);

/**
 * @brief In-call reply bytes a drain may write before it ends, whatever its frame count:
 *        a quarter of lwIP's send buffer (ADR-0085 §7).
 *
 * The byte half of @ref kRxDrainNagleFrames. Coalesced or not, the replies of a drain stay
 * unacknowledged until the next drain's reads let the peer's ACKs through, so the replies
 * of about two drains are outstanding at once. Bounding a drain's reply bytes to a quarter
 * of @ref kTcpSndBuf keeps that total under half of it, and a reply write never waits for
 * send-buffer space the writer itself is withholding. Frames with small replies never
 * reach it; a drain of large replies ends sooner and parks more often: the receiver pays.
 *
 * In-call replies only. Pushes posted by other tasks and sent from inside the park
 * (@ref httpd_ws_link_t::flush_egress) are outside this bound, and need none since #2095:
 * they are written without waiting, so a send buffer they fill holds the peer's next push
 * back (conflated, counted) instead of failing it. A single in-call reply larger than the
 * send buffer still waits out the send bound (pre-existing).
 */
constexpr std::size_t kRxDrainTxBytes = kTcpSndBuf / 4;

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

/** @brief Longest RFC 6455 §5.2 server frame header: 2 bytes plus a 64-bit length. */
constexpr std::size_t kWsHeaderMax = 10;

/**
 * @brief Encode the server frame header for one unfragmented BINARY frame of
 *        @p payload_len bytes (RFC 6455 §5.2: FIN|BINARY, unmasked, 7/16/64-bit length).
 *
 * The same bytes `httpd_ws_send_frame_async` writes ahead of a gathered frame. The link
 * writes them itself where IDF cannot: a RETAINED frame (RFC-0028 §6.9), and the rest of a
 * frame a non-blocking write left part-way (#2095).
 * @return The header length: 2, 4 or 10.
 */
[[nodiscard]] std::size_t encode_ws_header(std::size_t payload_len,
                                           std::array<std::byte, kWsHeaderMax>& out) noexcept {
    out[0] = std::byte{0x82};
    if (payload_len < 126) {
        out[1] = static_cast<std::byte>(payload_len);
        return 2;
    }
    if (payload_len <= 0xFFFF) {
        out[1] = std::byte{126};
        out[2] = static_cast<std::byte>((payload_len >> 8) & 0xFF);
        out[3] = static_cast<std::byte>(payload_len & 0xFF);
        return 4;
    }
    out[1] = std::byte{127};
    for (int i = 0; i < 8; ++i)
        out[2 + i] = static_cast<std::byte>(
            (static_cast<std::uint64_t>(payload_len) >> (8 * (7 - i))) & 0xFF);
    return kWsHeaderMax;
}

/**
 * @brief Destinations one fan-out chunk holds — the ON-STACK snapshot a broadcast walks
 *        its peer set through, and the reason a broadcast allocates nothing.
 *
 * The snapshot must not be a `std::vector`, and for the reason
 * @ref httpd_ws_link_t::tx_frame_t already records: under `-fno-exceptions` the vector's
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

/** @brief An auth record no liveness check has seen yet (#2257): the first check stamps
 *         its `now`, and `now` is never past it, so an unseen record never expires. */
constexpr std::uint64_t kAuthUnseen = ~std::uint64_t{0};

/**
 * @brief Copy @p reason into @p out, cut to @ref httpd_ws_link_t::kMaxCloseReasonBytes and
 *        backed off to a UTF-8 character boundary so the close frame never carries half a
 *        character (#1857).
 * @return The bytes copied.
 */
[[nodiscard]] std::uint8_t copy_close_reason(
    std::string_view reason, std::array<char, httpd_ws_link_t::kMaxCloseReasonBytes>& out) {
    std::size_t n = reason.size();
    if (n > out.size()) {
        n = out.size();
        // A continuation byte (10xxxxxx) at the cut means the character starts earlier.
        while (n > 0 && (static_cast<unsigned char>(reason[n]) & 0xC0U) == 0x80U) --n;
    }
    std::copy_n(reason.data(), n, out.data());
    return static_cast<std::uint8_t>(n);
}

/**
 * @brief The session destructor a refused-after-upgrade socket carries (#1857). Its ctx is
 *        the link's gate, which the session does not own, so there is nothing to free — and a
 *        null destructor would not do, because httpd answers a null one with `free(ctx)`.
 */
void keep_refusal_mark(void* /*gate*/) {}

/**
 * @brief Resolve one of the three buffer-sizing constructor arguments (#1160): @p want,
 *        or @p fallback when the caller left it 0.
 *
 * The SAME "0 means take the default" idiom `send_timeout_ms` already uses on these constructors,
 * so an embedder learns one convention for the whole argument list. There is no clamp and no
 * ceiling: this component does not know the target's heap, and inventing an upper bound here would
 * be exactly the synthetic limit the RAM-bearing three were made arguments to avoid. A value too
 * large simply fails to allocate, and @ref httpd_ws_link_t::alloc_buffers already reports that as
 * the link dropping every send on the counted path.
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
 * @brief Write the session name of @p peer — `p<slot>~<gen>` (ADR-0073 §2) — and its NUL
 *        into @p out, which holds at least `kSlotNameChars`; returns the name's length.
 *
 * The tenancy generation is part of the name, so a reused slot gets a fresh one: a route
 * another node still holds for the previous session names nobody, its delivery is refused,
 * and that refusal reaps the stale edge.
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
 * `std::size_t` overloads both, measured). A slot index and a generation are non-negative
 * integers, so the general path buys nothing here. Core's `slot_server_t` keeps
 * `std::to_chars` — it is host code with no image budget; the STRING is identical either
 * way, which is all the two have to agree on.
 */
[[gnu::noinline]] [[nodiscard]] std::uint8_t format_slot_name(peer_handle_t peer, char* out) {
    char buf[24];
    char* p = buf + sizeof(buf);
    // Written back to front: the generation and its separator, then the slot and its `p`.
    const std::pair<std::uint32_t, char> parts[] = {{peer.generation, kTenancySeparator},
                                                    {peer.index, 'p'}};
    for (auto [v, lead] : parts) {
        do {
            *--p = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        *--p = lead;
    }
    const auto len = static_cast<std::size_t>(buf + sizeof(buf) - p);
    std::copy_n(p, len, out);
    out[len] = '\0';
    return static_cast<std::uint8_t>(len);
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
 * and never addressed. @ref httpd_ws_link_t::session_t::name holds the routable `p<slot>~<gen>`;
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
 * @brief Failable fragment-reassembly buffer: grows geometrically from the link's
 *        `memory.state` (#1880), so a refusal drops the in-flight message instead of
 *        aborting the node.
 *
 * `std::vector` is unusable here: under `-fno-exceptions` its throwing allocator
 * turns a failed growth into `abort()` via the bad_alloc stub — and the appended
 * chunk is peer-controlled up to kMaxFrameBytes, so reassembly growth MUST be
 * failure-capable (the same backpressure contract as the tx queue).
 *
 * The work per message is linear in its bytes. The block doubles from 8 bytes, clamped at
 * kMaxFrameBytes, so a message of n bytes regrows O(log n) times and copies under 2n
 * bytes however it is cut, and never holds a block past the cap. Each fragment is
 * charged its bytes plus
 * @ref kFragmentHeaderBytes against kMaxFrameBytes, so the cap bounds the fragment count
 * as well as the bytes: an empty CONTINUE costs what its header cost on the wire.
 */
struct asm_buf_t {
    /** @brief The smallest RFC 6455 frame header (§5.2): what one fragment costs on the wire
     *         before any payload, charged with it against kMaxFrameBytes. */
    static constexpr std::size_t kFragmentHeaderBytes = 2;

    /** @brief An empty buffer that will draw its storage from @p src (`memory.state`). */
    explicit asm_buf_t(mem::block_source_t& src) noexcept : bytes_(src) {}
    /** @brief True when no reassembly is in progress. */
    [[nodiscard]] bool empty() const noexcept { return charged_ == 0; }
    /** @brief What the message has been charged so far: its bytes plus a header per fragment. */
    [[nodiscard]] std::size_t charged() const noexcept { return charged_; }
    /** @brief Whether a fragment of @p chunk bytes would take the charge past kMaxFrameBytes. */
    [[nodiscard]] bool over_cap(std::size_t chunk) const noexcept {
        return charged_ + chunk + kFragmentHeaderBytes > kMaxFrameBytes;
    }
    /** @brief The assembled bytes so far (valid until the next append/clear). */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return mem::as_span(bytes_); }
    /** @brief Release the storage (post-deliver / slot-reclaim / drop reset). */
    void clear() noexcept {
        bytes_ = mem::bytes_t(bytes_.source());
        charged_ = 0;
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
        asm_buf_t out(bytes_.source());
        out.bytes_ = std::move(bytes_);
        out.charged_ = std::exchange(charged_, 0);
        return out;
    }
    /**
     * @brief Append @p chunk and charge it, nothrow. Precondition: `!over_cap(chunk.size())`.
     * @retval false The store refused — the buffer is cleared (the partial message is
     *               unrecoverable) and the caller drops the message (backpressure).
     */
    [[nodiscard]] bool append(std::span<const std::byte> chunk) noexcept {
        const std::size_t want = bytes_.size() + chunk.size();
        const std::size_t doubled = std::max({want, std::size_t{8}, 2 * bytes_.capacity()});
        const bool room =
            want <= bytes_.capacity() || bytes_.reserve(std::min(doubled, kMaxFrameBytes));
        if (!room || !bytes_.append(chunk.data(), chunk.size())) {
            clear();
            return false;
        }
        charged_ += chunk.size() + kFragmentHeaderBytes;
        return true;
    }

   private:
    mem::bytes_t bytes_;      /**< @brief Owned storage, doubled up to kMaxFrameBytes. */
    std::size_t charged_ = 0; /**< @brief Bytes plus a header per fragment, against the cap. */
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
 * Its lifetime can be longer than the link's in adopted mode. Owning mode frees it:
 * `httpd_stop` deletes every session first. Adopted mode hands it to the teardown's detach
 * item, which frees it on the server's task, after the unregister and behind every item
 * queued before it, when the server lists no WebSocket client at all (#2202): then no
 * session holds the pointer. It is still LEAKED (one small block, teardown-only, the
 * leak-rather-than-free discipline #815 established for the TX path) when any WebSocket
 * client is open — a session still closing, a peer that upgraded and never sent a frame,
 * a client of another route on a shared server — when the destructor runs on the
 * server's own task, when the server's client list does not fit, or when the item is
 * refused or never runs.
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
 * and as in `ws_server_transport_t`, whose destructor joins its poll thread for the same
 * reason.
 */
struct httpd_ws_link_t::gate_t {
    /** @brief Where this gate and every work item queued through it were drawn
     *         (`memory.state`); set once, before the gate is published. */
    mem::block_source_t* src = nullptr;
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
 * @brief One outbound frame's bytes, gathered ONCE per send and shared by every peer it goes
 *        to (#2095): a reference count, the gathered bytes, and a retained frame's value.
 *
 * A broadcast gathers its iovec into one of these and every destination's slot takes a
 * reference, so a fan-out to N peers costs one copy and one block, not N. Drawn from
 * `memory.io` with its bytes trailing it in the same block, or with them in a buffer
 * borrowed from the declared LARGE size class where the frame lands in its band (#1566,
 * bounded, fails closed). The last reference frees it, from whichever task drops it, through
 * the source recorded in it. Never a `std::vector` for the copy — the vector's THROWING
 * allocator once defeated a nothrow guard here, and under `-fno-exceptions` a reply-sized
 * copy hitting heap exhaustion aborted the node (the browser-session crash).
 */
struct httpd_ws_link_t::tx_block_t {
    mem::block_source_t* src = nullptr; /**< @brief Where this block was drawn. */
    std::size_t alloc_bytes = 0;        /**< @brief Its size there, for the release. */
    std::atomic<std::uint32_t> refs{1}; /**< @brief Slots (and a builder) holding it. */
    std::byte* payload = nullptr;       /**< @brief Gathered bytes: trailing, or large-class. */
    std::size_t len = 0;                /**< @brief Bytes at @ref payload. */
    /**
     * @brief A RETAINED frame's value (RFC-0028 §6.9): @ref payload then holds the frame's
     *        encoded WebSocket header and head, and these bytes follow them on the wire,
     *        written straight from the published block. Empty for a gathered frame.
     */
    graph::value_ref_t value;
    /** @brief The LARGE-class claimed flag this block holds, or null (#1566). The flag and
     *         not an index, so the last release needs no link to give it back. */
    std::atomic<bool>* large_busy = nullptr;
    /** @brief Bytes the frame accounts for: the gathered payload, or a retained frame's
     *         header, head and value. */
    [[nodiscard]] std::size_t total() const noexcept {
        return len + (value ? value->total_length() : 0);
    }
};

std::size_t httpd_ws_link_t::tx_block_header() noexcept {
    return (sizeof(tx_block_t) + alignof(std::max_align_t) - 1) / alignof(std::max_align_t) *
           alignof(std::max_align_t);
}

std::size_t httpd_ws_link_t::large_stride() const noexcept {
    return tx_block_header() + (tx_large_bytes_ + alignof(std::max_align_t) - 1) /
                                   alignof(std::max_align_t) * alignof(std::max_align_t);
}

/**
 * @brief One peer's reference to an outbound frame, held until its socket takes it (#2095).
 *
 * Each session holds one for the frame on the wire (@ref session_t::tx) and one per frame
 * waiting behind it, with the lane it came on (#2195). The bytes live in the shared @ref
 * tx_block_t; what is per peer is the reference, the session it was staged for and how far the
 * socket has taken it — a few words, whatever the frame size.
 */
struct httpd_ws_link_t::tx_frame_t {
    tx_block_t* blk = nullptr; /**< @brief The shared bytes; null = the frame is empty. */
    /**
     * @brief The session generation this frame was staged for.
     *
     * A producer resolves its destination, then stores, and the slot can be reclaimed and
     * claimed again between the two; the generation is what makes a frame stored late for
     * a departed session skippable at the flush rather than written to its successor (#954).
     */
    std::uint32_t gen = 0;
    /**
     * @brief Wire bytes of this frame the socket has already taken — its WebSocket header
     *        included (httpd task only).
     *
     * Non-zero on a held frame is a frame part-way on the wire: a non-blocking write took
     * what the socket had room for and the rest goes on the session's next flush. Nothing
     * else may be written to that socket in between (@ref httpd_ws_link_t::finish_wire).
     */
    std::uint32_t sent = 0;
    /**
     * @brief What a full lane does with a newer frame (#2195), chosen once from the lane at
     *        stage time and kept in the two low bits of @ref lane.
     */
    enum policy_t : std::uintptr_t {
        kRefuse = 0,  /**< @brief Refuse the newer frame: a reliable STREAM, the control lane. */
        kReplace = 1, /**< @brief The newer frame takes the waiting one's place: a SAMPLE. */
        kShed = 2,    /**< @brief Shed the oldest waiting frame: a best-effort STREAM. */
    };
    /**
     * @brief The delivery lane (#2195): its key (the subscription edge, so per session the
     *        path; null for a frame from no vertex: a reply, an event, an ANNOUNCE) with its
     *        @ref policy_t in the two low bits.
     *
     * The key names an edge, an object aligned to at least 4 bytes, so its two low bits are
     * free, and keeping the policy there costs the reference no word of its own. Compared,
     * never dereferenced. An edge that ends while its frame waits can come back at the same
     * address as a NEW subscription's edge; if both are SAMPLEs, the new one's frame then
     * replaces the ended one's waiting frame (counted as `tx_conflated`). Two live edges never
     * share an address, so a live path's frame is never taken by another path's.
     */
    std::uintptr_t lane = kRefuse;
    /** @brief True while a frame is held. */
    [[nodiscard]] bool held() const noexcept { return blk != nullptr; }
    /** @brief True if @p o is a newer frame that may replace this one: a SAMPLE of the same
     *         path for the same session. Nothing else is ever replaced (#2195). */
    [[nodiscard]] bool conflates(const tx_frame_t& o) const noexcept {
        return lane == o.lane && gen == o.gen && (lane & 3) == kReplace;
    }
};

/**
 * @brief One frame waiting behind a session's wire frame (#2195): the session's own spare,
 *        or a node drawn from the link's egress store while the frame waits.
 */
struct httpd_ws_link_t::tx_wait_t {
    tx_frame_t frame;
    tx_wait_t* next = nullptr;
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
    /** @brief A free slot whose reassembly buffer draws from @p src (`memory.state`). */
    explicit session_t(mem::block_source_t& src) noexcept : asm_buf(src) {}
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
     * @brief The peer's ROUTABLE name — `p<slot>~<gen>` (ADR-0073 §2, #426, #994).
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
     * (@ref httpd_ws_link_t::reclaim_slot hands the old one out for the eviction seam),
     * exactly as core's own bus servers do it (`core/src/posix_endpoint.cpp:406`). Held in
     * place, so a claim allocates nothing for it (#1880).
     */
    slot_name_t name;
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
    /**
     * @brief The open window's writes do not wait (#2095): @ref httpd_ws_link_t::send_guarded
     *        adds `MSG_DONTWAIT`, and a write the socket has no room for, or room for only
     *        part of, ends the frame's turn instead of failing it (httpd task only).
     */
    bool tx_nonblock = false;
    /** @brief A non-blocking write in the open window found the socket full: the frame is
     *         held, not failed (httpd task only). */
    bool tx_frame_blocked = false;
    /**
     * @brief Open a frame-write window — the bracket every socket write of a frame sits in.
     * @param nonblock  Write without waiting (a held frame's flush) or within the send bound
     *                  (an in-call reply, a close, an auth frame).
     * @param on_wire   Bytes of this frame already on the wire: non-zero resumes a frame a
     *                  non-blocking write left part-way, so a failure is a truncation.
     */
    void open_tx_frame(bool nonblock = false, std::size_t on_wire = 0) noexcept {
        tx_frame_bytes = on_wire;
        tx_frame_open = true;
        tx_nonblock = nonblock;
        tx_frame_blocked = false;
    }
    /**
     * @brief Close it, and report how many bytes of that frame reached the wire.
     *
     * Non-zero on a FAILED send is the whole finding: the peer is holding a header whose
     * payload will never arrive.
     */
    [[nodiscard]] std::size_t close_tx_frame() noexcept {
        tx_frame_open = false;
        tx_nonblock = false;
        return tx_frame_bytes;
    }
    /**
     * @brief This peer's frame on the wire (httpd task only, #2095).
     *
     * Behind it wait up to @ref httpd_ws_link_t::kTxWaitDepth newer frames, in order
     * (@ref tx_head), AT MOST ONE per SAMPLE path (#2195): a newer frame of the same path
     * REPLACES the waiting one in place, and the peer's stats count the replaced frame
     * (`tx_conflated`). Frames of other paths, STREAM frames and frames from no vertex are
     * never replaced: a frame from one vertex is not a newer value of another, and an event
     * or an ANNOUNCE is owed to the peer once. Each lane is bounded by its depth
     * (`kEgressControlDepth` for frames from no vertex): a best-effort STREAM at its depth or
     * at the queue's cap sheds its own oldest frame (counted as `tx_conflated`), and a full
     * reliable or control lane, a full queue or a refused node draw is the link's counted
     * refusal. So a slow peer holds at
     * most its own few frames and never a work item or the httpd task that another peer
     * needs. The httpd task moves the oldest waiting frame onto the wire, under @ref tx_m,
     * when the wire frame is empty.
     */
    tx_frame_t tx;
    tx_wait_t* tx_head = nullptr; /**< @brief The oldest waiting frame (tx_m). */
    tx_wait_t* tx_tail = nullptr; /**< @brief The newest waiting frame (tx_m). */
    /** @brief The node the first waiting frame takes, so a peer with one frame waiting costs
     *         no draw (tx_m). */
    tx_wait_t tx_spare;
    bool tx_spare_used = false; /**< @brief @ref tx_spare is in the queue (tx_m). */
    /** @brief Guards the waiting frames. Held for a gather or a flip, never across a socket
     *         write, and never with `peers_m_` taken under it. */
    std::mutex tx_m;
    /**
     * @brief Refused large draws while this session pinned the class and its last write
     *        moved no byte (tx_m).
     *
     * Bumped by @ref httpd_ws_link_t::note_large_pinned for a peer pinning a large block
     * after such a write (@ref tx_refused), and by nothing else; cleared by any write that
     * moves a byte. A frame refused at the full queue never bumps it: a healthy peer
     * subscribed to many vertices fills its queue in one burst (#2195). At
     * @ref httpd_ws_link_t::kTxStallStrikes the flush condemns the session through the
     * strike path: a peer whose socket never drains holds no seat for ever.
     */
    std::uint32_t tx_stalls = 0;
    /** @brief The last write of this session's wire frame took zero bytes (tx_m). */
    bool tx_refused = false;
    /** @brief A byte of this session's frames went out since the last liveness check
     *         (#2198, tx_m). Set by the flush, taken by `close_frozen_peers`. */
    bool tx_moved = true;
    /** @brief The liveness check found this session frozen; the next flush condemns it
     *         (#2198, tx_m). */
    bool tx_frozen = false;
    /** @brief The application's `now` at the last liveness check that found this session
     *         moving or owing nothing (#2198, tx_m). */
    std::uint64_t live_since = 0;
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
     * @brief The application's `now` at the first liveness check that saw this session
     *        (#2257, peers_m_); @ref kAuthUnseen until one has.
     *
     * Stamped ONCE and never extended by the peer's own traffic: a scheme that needs several
     * frames gets the whole window for all of them, and a peer that could refresh its record
     * by sending anything at all would have no deadline. While @ref auth_pending, a record
     * older than the check's `deadline` closes the session with `kCloseAuthTimeout`.
     */
    std::uint64_t auth_since = kAuthUnseen;
    /**
     * @brief The identity the hook bound on ACCEPT, NUL-terminated (peers_m_).
     *
     * A fixed array for the same reason @ref endpoint_str is one: written once per session,
     * read on a diagnostic path, and never worth a heap chunk on a small-heap target.
     */
    char subject[httpd_ws_link_t::kMaxSubjectChars + 1] = {};
    /** @brief The length of @ref subject, bytes (peers_m_); 0 = no subject bound. The caller
     *         is built from this, never by scanning for the terminator. */
    std::uint8_t subject_len = 0;
    static_assert(httpd_ws_link_t::kMaxSubjectChars <= 0xFF, "subject_len is one byte");
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
    /** @brief Directed RETAINED send (RFC-0028 §6.9): queued as head bytes plus one
     *         reference to @p value (see the owning link's retained @ref httpd_ws_link_t::send);
     *         in-call it is the gathered in-call write. */
    void send(std::span<const std::span<const std::byte>> head, const graph::value_t& value,
              const graph::delivery_lane_t& lane) override;
    /**
     * @brief The owning LINK's shed-frame counters, projected through this handle (#1494).
     *
     * The base default is all-zero, and inheriting it here was a genuine hole rather than an
     * honest answer: a caller that holds only this `transport_t*` — which is precisely what
     * the routing plane holds for a directed peer — read three zeros while every frame this
     * handle dropped was being counted correctly one level up, on the link. `transport_t::send`
     * is best-effort by contract and `drop_stats()` is the ONLY feedback it offers, so a
     * handle that answers zero converts "counted shed" back into silent loss for the one
     * caller shape that cannot reach past it.
     *
     * The numbers are the LINK's, not this session's, and that is stated rather than papered
     * over: this component keeps no per-session tallies (a dead-peer send bumps the link's
     * `tx_to_dead_peer` whichever session it was aimed at), so a per-handle block would have to
     * be invented — a fabricated number, which the base's own doc forbids more strongly than it
     * forbids an unattributed one. A monitor reads this as "the link this peer resolves
     * against is shedding", then attributes with the link's richer @ref httpd_ws_link_t::stats.
     *
     * A retired handle (`owner_ == nullptr`, teardown) answers all-zero, which is the base's
     * honest default and correct here: there is no longer a link whose counters these are.
     */
    [[nodiscard]] tr::net::transport_drop_stats_t drop_stats() const noexcept override;

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
    mem::block_source_t* src = nullptr; /**< @brief Where this item came from (`memory.state`). */
    httpd_handle_t handle = nullptr;    /**< @brief The adopted server (still running). */
    fixed_array_t<int> fds;             /**< @brief Snapshot of the open peers' sockets. */
    fixed_array_t<void*> ctxs;          /**< @brief The ctx each fd carried (identity only). */
    std::size_t n = 0;                  /**< @brief Entries in @ref fds / @ref ctxs. */
    std::atomic<bool> done{false};      /**< @brief Set once every fd has been detached. */
    std::atomic<bool> released{false};  /**< @brief Ownership handshake (see the brief). */
    /** @brief The handler gate, handed over by the destructor: freed here when the server
     *         lists no WebSocket client, kept otherwise (#2202). Null = not handed over. */
    gate_t* gate = nullptr;
};

/**
 * @brief The @ref httpd_ws_link_t::close_peer work item: the session identity to close,
 *        and the gate to reach the link through — NOTHING that belongs to the link.
 *
 * Drawn per call from `memory.state` and returned by @ref httpd_ws_link_t::close_work, which
 * is affordable because close_peer is an administrative action, never a data-path one — the
 * no-allocation discipline protects the per-frame paths, and a revocation is not one.
 * Failable, like every allocation here: a refused draw is a `false` to the caller, not an
 * abort. The gate rather than the link for the same reason @ref
 * httpd_ws_link_t::tx_work posts one: the item can drain on the adopted server's task
 * after the link is gone, and the gate is the one object designed to outlive it.
 */
struct httpd_ws_link_t::close_req_t {
    gate_t* gate = nullptr; /**< @brief The owning link's gate (see the brief). */
    session_ref_t to;       /**< @brief The session to close, identified as (slot, gen). */
};

/**
 * @brief One @ref httpd_ws_link_t::queue_refusal work item: the socket to close and the gate
 *        to reach the link through.
 *
 * Drawn per refusal from `memory.state` and returned by @ref httpd_ws_link_t::refusal_work, like
 * @ref close_req_t and for the same reasons: it is not a frame-path allocation, and the gate is the
 * one object that outlives the link. It lives from the handshake until the close is written, and it
 * is the only thing the refusal costs that is not the httpd session itself.
 */
struct httpd_ws_link_t::refusal_req_t {
    gate_t* gate = nullptr; /**< @brief The owning link's gate. */
    int fd = -1;            /**< @brief The refused socket. */
};

httpd_ws_link_t::httpd_ws_link_t(std::uint16_t bind_port, const httpd_ws_config_t& config)
    : port_(bind_port),
      max_peers_(config.max_peers),
      refusal_code_(config.refusal_close_code != 0 ? config.refusal_close_code
                                                   : kCloseTryAgainLater),
      refusal_reason_len_(copy_close_reason(config.refusal_close_reason, refusal_reason_)),
      peer_named_(config.peer_named),
      state_src_(&config.memory.state_or_default()),
      io_src_(&config.memory.io_or_default()),
      uri_(*state_src_),
      slots_(*state_src_),
      resolutions_(*state_src_),
      rx_backend_(config.memory.rx),
      rx_scratch_bytes_(resolve_size(config.rx_scratch_bytes, kDefaultRxScratchBytes)),
      tx_inline_bytes_(resolve_size(config.tx_inline_bytes, kDefaultTxInlineBytes)),
      tx_large_bytes_(config.tx_large.bytes),
      tx_large_slots_(config.tx_large.slots) {
    const std::size_t max_peers = config.max_peers;
    const std::uint32_t send_timeout_ms = config.send_timeout_ms;
    install_idle_hooks();  // before any handler can run (ADR-0085)
    open_gate();
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
    // handshake handler, never drop a live graph peer mid-stream (ws_server_transport_t's
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
    // owns_httpd_ stays true and `uri_` stays empty: the dtor stops the server instead of
    // unregistering the URI, so nothing ever reads it on this path.
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
                                 const httpd_ws_config_t& config)
    : max_peers_(config.max_peers),
      refusal_code_(config.refusal_close_code != 0 ? config.refusal_close_code
                                                   : kCloseTryAgainLater),
      refusal_reason_len_(copy_close_reason(config.refusal_close_reason, refusal_reason_)),
      peer_named_(config.peer_named),
      state_src_(&config.memory.state_or_default()),
      io_src_(&config.memory.io_or_default()),
      uri_(*state_src_),
      slots_(*state_src_),
      resolutions_(*state_src_),
      rx_backend_(config.memory.rx),
      rx_scratch_bytes_(resolve_size(config.rx_scratch_bytes, kDefaultRxScratchBytes)),
      tx_inline_bytes_(resolve_size(config.tx_inline_bytes, kDefaultTxInlineBytes)),
      tx_large_bytes_(config.tx_large.bytes),
      tx_large_slots_(config.tx_large.slots) {
    const std::size_t max_peers = config.max_peers;
    const std::uint32_t send_timeout_ms = config.send_timeout_ms;
    install_idle_hooks();  // before any handler can run (ADR-0085)
    open_gate();
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
    if (!uri_.assign(uri_pattern))
        mem::exhausted_at_init(*state_src_, "httpd_ws_link_t: the registered URI");

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

void httpd_ws_link_t::open_gate() {
    // Load-bearing: the gate is what makes the registered handler safe to dispatch after
    // this link dies, so a link that could not draw one must not register a handler at all.
    // It is drawn at setup, so a refusal is a sizing bug (ADR-0083), not a runtime state.
    gate_t* const g = mem::make_in<gate_t>(*state_src_);
    if (g == nullptr) mem::exhausted_at_init(*state_src_, "httpd_ws_link_t: the handler gate");
    g->src = state_src_;
    g->link = this;
    gate_.store(g, std::memory_order_relaxed);
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
    // Once per link, at the sizes the constructor resolved, through the allocation seam
    // (#1880): the RX scratch is connection state, the in-call scratch frame is egress. Both
    // are sized by the integrator's own knobs, so a store that cannot hold them is a sizing
    // bug and aborts here, naming it (ADR-0083) — never a link that comes up and then drops
    // every frame. No per-peer TX buffer exists at all: a frame's bytes are gathered once per
    // send into a block every destination shares (make_block, #2095).
    if (!rx_scratch_.assign(*state_src_, rx_scratch_bytes_))
        mem::exhausted_at_init(*state_src_, "httpd_ws_link_t: the RX scratch");
    if (!tx_scratch_block_.assign(*io_src_, tx_inline_bytes_))
        mem::exhausted_at_init(*io_src_, "httpd_ws_link_t: the TX reply scratch");
    alloc_tx_large();
}

void httpd_ws_link_t::alloc_tx_large() {
    // The large size class is the integrator's declaration and nothing else (#1566): unset,
    // this returns having allocated nothing and every frame past tx_inline_bytes_ keeps the
    // per-frame nothrow heap arm it has always taken. There is no default to fall back to
    // on purpose — the other three sizes have one because every deployment needs SOME value
    // of them, while this is extra RAM bought for a payload band only the integrator can
    // describe.
    //
    // A declaration is rejected here rather than clamped, and it is INERT rather than fatal:
    // a class no wider than the inline capacity would never be reached (the band is empty),
    // and a class of zero buffers is a bound of zero, which is a link that drops every large
    // frame instead of sending it. Neither is worth failing a node's construction over, and
    // neither may be silently rounded into something the operator did not ask for — so the
    // effective values are zeroed, the reason is logged ONCE at construction, and
    // tx_large_bytes()/tx_large_slot_capacity() report the class the link is really running
    // (#1160: report the ceiling, never the constant that was asked for).
    if (tx_large_bytes_ == 0 && tx_large_slots_ == 0) return;  // nothing declared: the norm
    if (tx_large_bytes_ <= tx_inline_bytes_ || tx_large_slots_ == 0) {
        ESP_LOGW(kTag, "tx large class ignored (bytes=%u slots=%u, inline=%u) - heap tail kept",
                 (unsigned)tx_large_bytes_, (unsigned)tx_large_slots_, (unsigned)tx_inline_bytes_);
        tx_large_bytes_ = 0;
        tx_large_slots_ = 0;
        return;
    }
    // A VALID declaration is sized by the integrator exactly as the pool is, so a store that
    // cannot hold it is the same sizing bug and aborts the same way (#1880): the pair is
    // drawn together, and each claimed flag starts clear (value-initialized).
    // Each buffer is a whole frame block — its header and its bytes — so a frame in the band
    // draws nothing at all, not even the block header.
    if (!tx_large_.assign(*io_src_, tx_large_slots_ * large_stride() / sizeof(std::max_align_t)) ||
        !tx_large_busy_.assign(*io_src_, tx_large_slots_))
        mem::exhausted_at_init(*io_src_, "httpd_ws_link_t: the TX large class");
}

void* httpd_ws_link_t::claim_tx_large(std::atomic<bool>*& flag) {
    // Undeclared, rejected or failed: the caller's band test already excluded that case
    // (tx_large_bytes_ is 0 and no frame is <= 0 bytes past the inline capacity), so this is
    // belt and braces for a future caller rather than a live arm.
    if (tx_large_busy_ == nullptr) return nullptr;
    for (std::size_t i = 0; i < tx_large_slots_; ++i) {
        bool expected = false;
        if (!tx_large_busy_[i].compare_exchange_strong(expected, true, std::memory_order_acquire))
            continue;
        flag = &tx_large_busy_[i];
        note_tx_large_peak();
        return reinterpret_cast<std::byte*>(tx_large_.data()) + i * large_stride();
    }
    return nullptr;  // every buffer of the class in flight — the caller drops and counts
}

void httpd_ws_link_t::note_large_pinned() {
    // The class is spent, and a peer whose socket took nothing may be what holds it: its frame
    // pins a block no later large frame can have, and a full queue never strikes (#2195).
    // Each refused draw therefore counts against every peer that holds a block of the class and
    // whose last write moved no byte (tx_refused, cleared only by a write that moves one). Reaching
    // kTxStallStrikes posts the flush that strikes it, which hands the block back. Same chunked
    // walk as the flush; no timer, no clock.
    session_t* batch[kFanoutChunk];
    std::size_t next = 0;
    bool strike = false;
    for (bool more = true; more;) {
        std::size_t n = 0;
        {
            const std::lock_guard lock(peers_m_);
            while (next < slots_.size() && n < kFanoutChunk) batch[n++] = slots_[next++];
            more = next < slots_.size();
        }
        for (std::size_t i = 0; i < n; ++i) {
            session_t* const s = batch[i];
            const std::lock_guard lock(s->tx_m);
            const auto pins = [](const tx_frame_t& f) {
                return f.held() && f.blk->large_busy != nullptr;
            };
            bool pinned = pins(s->tx);
            for (const tx_wait_t* w = s->tx_head; w != nullptr && !pinned; w = w->next)
                pinned = pins(w->frame);
            if (!s->tx_refused || !pinned) continue;
            if (++s->tx_stalls >= kTxStallStrikes) strike = true;
        }
    }
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    if (strike && h != nullptr) (void)post_flush(h);
}

void httpd_ws_link_t::note_tx_large_peak() {
    // The high-water mark of the class's occupancy, which is the number an operator SIZES
    // `tx_large_slots` with — and the one thing a scan-on-demand accessor cannot
    // reconstruct after the fact, so it is sampled here, at the claim that could have set it.
    //
    // This is the file's one counter update on a success arm, and it is confined to the
    // large band by construction: a frame that fits `tx_inline_bytes_` never reaches this
    // function, so the steady-state send the counting doctrine protects (core/STYLE.md
    // §counting doctrine ¶1) gains no instruction at all. The scan is over a handful of
    // flags — the class is a small pool by declaration.
    //
    // Relaxed throughout, and not a consistent cut: a claim racing this one may or may not
    // be seen, so the peak is a lower bound on the true occupancy. That is the ordinary
    // snapshot-coherence clause (core/STYLE.md §Introspection) and it is the right trade —
    // an exact peak would need the claim path to take a lock in order to serve a diagnostic.
    const std::uint32_t now = static_cast<std::uint32_t>(tx_large_in_use());
    std::uint32_t peak = tx_large_peak_.load(std::memory_order_relaxed);
    while (peak < now && !tx_large_peak_.compare_exchange_weak(peak, now, std::memory_order_relaxed,
                                                               std::memory_order_relaxed)) {
    }
}

httpd_ws_link_t::tx_block_t* httpd_ws_link_t::make_block(
    std::span<const std::byte> prefix, std::span<const std::span<const std::byte>> iov,
    const graph::value_t* value) {
    std::size_t len = prefix.size();
    for (const auto& part : iov) len += part.size();
    // The LARGE band (#1566), and it exists only where an integrator declared it: with no
    // class, tx_large_bytes_ is 0 and no frame satisfies this test. Exhausted, it fails
    // CLOSED, never onto the egress store: falling back would hand back the unbounded
    // footprint the class was declared to remove, under exactly the load that motivated the
    // declaration — counted, so an operator can size `tx_large_slots` against it.
    std::atomic<bool>* large = nullptr;
    void* raw = nullptr;
    std::size_t bytes = 0;
    if (len > tx_inline_bytes_ && len <= tx_large_bytes_) {
        // A whole block of the class: its header leads the buffer (large_stride).
        raw = claim_tx_large(large);
        if (raw == nullptr) {
            tx_large_dropped_.fetch_add(1, std::memory_order_relaxed);
            note_large_pinned();
            return nullptr;
        }
    } else {
        // Every other frame: one exact-size block from the egress store, the bytes trailing
        // the header, failable end to end. A refusal is the caller's counted drop.
        bytes = tx_block_header() + len;
        raw = io_src_->try_alloc(bytes, alignof(std::max_align_t));
        if (raw == nullptr) return nullptr;
    }
    auto* const b = new (raw) tx_block_t;
    b->src = large != nullptr ? nullptr : io_src_;
    b->alloc_bytes = bytes;
    b->large_busy = large;
    b->payload = static_cast<std::byte*>(raw) + tx_block_header();
    b->len = len;
    if (value != nullptr) {
        b->value = graph::value_ref_t::keep(*value);
        if (!b->value) {  // a caller-owned value whose links could not be kept
            drop_block(b);
            return nullptr;
        }
    }
    std::byte* p = b->payload;
    if (!prefix.empty()) std::memcpy(p, prefix.data(), prefix.size());
    p += prefix.size();
    for (const auto& part : iov) {
        if (!part.empty()) std::memcpy(p, part.data(), part.size());
        p += part.size();
    }
    return b;
}

void httpd_ws_link_t::drop_block(tx_block_t* b) {
    if (b == nullptr || b->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    // The last reference: whichever task dropped it frees it, through what it recorded, so
    // this needs no link. The value reference first (RFC-0028 §6.9), then the large-class
    // buffer, then the block.
    b->value.reset();
    mem::block_source_t* const src = b->src;
    const std::size_t bytes = b->alloc_bytes;
    std::atomic<bool>* const large = b->large_busy;
    b->~tx_block_t();
    if (large != nullptr)
        large->store(false, std::memory_order_release);  // a block of the class: hand it back
    else
        src->release(b, bytes, alignof(std::max_align_t));
}

void httpd_ws_link_t::release_frame(tx_frame_t& frame) {
    drop_block(frame.blk);
    frame = tx_frame_t{};
}

void httpd_ws_link_t::free_waiting(session_t& s, tx_wait_t* w) {
    // A waiting node goes back where stage took it from — the session's own spare, or the
    // egress store. Its frame has been moved out or released already: this frees no block.
    if (w == &s.tx_spare) {
        s.tx_spare_used = false;
        return;
    }
    w->~tx_wait_t();
    io_src_->release(w, sizeof(tx_wait_t), alignof(tx_wait_t));
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
    // No TX drain to wait for (#2095). The one TX work item, the flush, carries the GATE and
    // nothing of the link's: it reaches the sessions and their frames only through
    // `gate->link`, which close_gate above has nulled, and it registers on the gate's depth
    // while it runs, which close_gate has joined. An item still in an adopted server's queue
    // finds the link gone and does nothing, so neither a wait nor a leak is owed here.
    // The gate outlives the link exactly when something may still reach it. Owning mode:
    // httpd_stop has deleted every session, so nothing holds the pointer any more and it
    // is freed. Adopted mode: detach_sessions handed it to the detach item, which frees it
    // on the server's task when no WebSocket client is left (#2202) and otherwise LEAKS it —
    // one small block, teardown-only, and the price of a handler that stays safe to dispatch
    // forever. From here this destructor never touches it. Nothing was registered when the
    // constructor failed, so that case frees it here too.
    if (owns_httpd_ || handle_.load(std::memory_order_relaxed) == nullptr)
        mem::drop_in(*state_src_, gate_.load(std::memory_order_relaxed));
    gate_.store(nullptr, std::memory_order_relaxed);
    handle_.store(nullptr, std::memory_order_relaxed);
    // The session table and the resolution handles it owns. A teardown that abandoned them
    // (@ref abandon_sessions) emptied both tables without returning a block, so this returns
    // exactly what nothing can still reach.
    for (session_t* const slot : slots_) mem::drop_in(*state_src_, slot);
    for (peer_resolution_t* const r : resolutions_) mem::drop_in(*state_src_, r);
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
    // The handed-over gate (#2202). The route is unregistered, so no handshake can latch it
    // again, and this runs behind every item queued before it, the flush that carries the
    // gate among them. What may still reach it is a session that latched it earlier, and
    // each of those is a WebSocket client in this list: a session still closing, a peer that
    // never sent a frame and a client of another route look the same, so any one keeps it.
    // A list the server refuses (more clients than fit) keeps it too.
    std::array<int, kMaxClientFds> fds{};
    std::size_t n = fds.size();
    std::size_t ws = httpd_get_client_list(req->handle, &n, fds.data()) != ESP_OK;
    for (std::size_t i = 0; i < n; ++i)
        ws += httpd_ws_get_fd_info(req->handle, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET;
    if (ws == 0) mem::drop_in(*req->src, req->gate);  // null: nothing handed over
    req->done.store(true, std::memory_order_release);
    if (req->released.exchange(true, std::memory_order_acq_rel)) mem::drop_in(*req->src, req);
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
    // Queued even with no slot armed, since the item also carries the gate (detach_work).
    // With none armed there is nothing to wait for: the destructor queues it and goes on.

    // Failable throughout, from `memory.state`: a teardown that cannot draw must still be
    // memory-safe, so a refusal here takes the neutralise-and-leak path rather than skipping
    // the detach.
    detach_req_t* raw = mem::make_in<detach_req_t>(*state_src_);
    if (raw == nullptr || !raw->fds.assign(*state_src_, open_n) ||
        !raw->ctxs.assign(*state_src_, open_n)) {
        mem::drop_in(*state_src_, raw);
        if (open_n != 0) abandon_sessions();  // the gate is kept either way
        return;
    }
    raw->src = state_src_;
    {
        const std::lock_guard lock(peers_m_);
        std::size_t i = 0;
        for (session_t* const s : slots_)
            if (s->open && i < open_n) {
                raw->fds[i] = s->fd;
                raw->ctxs[i] = s;  // the ctx this fd was armed with — identity only
                ++i;
            }
        raw->n = i;
    }
    raw->handle = handle_.load(std::memory_order_relaxed);
    raw->gate = gate_.load(std::memory_order_relaxed);

    bool detached = open_n == 0;
    // Relaxed both ways: a match can only be observed by the task that stored it, and a
    // stale MISS on any other task is the safe direction (queue the work and wait, which
    // is what a task that is not the server must do anyway).
    if (server_task_.load(std::memory_order_relaxed) == current_task()) {
        // We ARE the server task (the dtor was reached from a work item or a handler on
        // it). Queued work could only run after we return, so waiting for it would
        // deadlock by construction — the #814 lesson. Being that task is exactly the
        // permission detach_work needs, so run it right here instead. Items queued before
        // this frame have NOT run yet and one may carry the gate, so it is not handed over.
        raw->gate = nullptr;
        detach_work(raw);
        detached = true;
    } else if (httpd_queue_work(handle_.load(std::memory_order_relaxed),
                                &httpd_ws_link_t::detach_work, raw) != ESP_OK) {
        ESP_LOGE(kTag, "session detach could not be queued (ctrl queue full)");
        // Never queued: ours to drop, and the gate it carried is kept. With no slot armed
        // nothing is left to abandon, so `detached` keeps its `open_n == 0`.
        raw->released.store(true, std::memory_order_relaxed);
    } else {
        for (int turn = 0; turn < kDrainTurns && !detached; ++turn) {
            detached = raw->done.load(std::memory_order_acquire);
            if (!detached) std::this_thread::sleep_for(std::chrono::milliseconds(kDrainSliceMs));
        }
    }
    if (!detached) ESP_LOGE(kTag, "session detach did not run on the httpd task");
    if (raw->released.exchange(true, std::memory_order_acq_rel)) mem::drop_in(*state_src_, raw);
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
        for (session_t* const s : slots_) {
            neutralise(s);
            ++leaked;
        }
        slots_.clear();
        // The resolution handles go the same way, and for the same reason (#1013): the
        // routing plane may still hold one, and `neutralise` has just made every one of
        // them inert, so leaking the pool is what keeps that inert object at a valid
        // address. Freeing it here would turn a no-op send into a use-after-free — the
        // precedent the leaked slot shells beside it set (#815).
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
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i]->fd != fd) continue;
        neutralise(slots_[i]);
        slots_.erase_at(i);
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
    // Mark the frame: every send issued BENEATH this call is a reply serviced in-call, and
    // takes the fork that keeps it off the control queue (#1494 — see t_serving_link and
    // @ref httpd_ws_link_t::send_in_call). The scope restores the previous value on the way
    // out and never touches `self`, which the next comment explains may be gone by then.
    // An inbound frame is the one socket event ESP-IDF's server hands this link (it selects
    // for read only), so it is also when a frame held on a full socket — this peer's or any
    // other's — is offered its socket again (#2095). Before the app runs, while `self` is
    // provably alive, and only when something is held: one relaxed load otherwise.
    if (self->tx_held_.load(std::memory_order_relaxed)) (void)self->flush_egress();
    esp_err_t err = ESP_OK;
    {
        const serving_scope_t serving(self);
        err = self->on_data_frame(req);
    }
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
    bool admit = verdict == admission_verdict_t::ADMIT ||
                 verdict == admission_verdict_t::ADMIT_AUTHENTICATED;
    const int fd = httpd_req_to_sockfd(req);
    if (!admit) ESP_LOGW(kTag, "peer refused by admission hook (fd=%d)", fd);
    // A refusal that still lets the upgrade complete (#1857), so the peer reads a close code
    // instead of the bare 1006 a refused upgrade shows a browser. Nothing below runs for it:
    // `admit` is false, so it claims no slot and writes no ledger row, and it is counted with
    // every other refusal at the end. What it does get is a queued close and a mark on its
    // session; if either cannot be had, it is refused before the upgrade like REFUSE.
    const bool upgrade_refused =
        verdict == admission_verdict_t::REFUSE_AFTER_UPGRADE && queue_refusal(gate, req);
    // TWO SHAPES OF ADMISSION, and which one this is was decided by the predicate above.
    //
    // (a) ADMIT_AUTHENTICATED — CLAIM THE SESSION HERE (#1334). The ledger below exists to
    // carry an unanswered question across the gap between the 101 and a peer's first frame;
    // this peer's entitlement is not an open question, it was settled by the opening GET. It
    // used to take a ledger row anyway, and the row's only two retirement paths are the
    // first-frame claim and the DEADLINE SWEEP — so a peer that is entitled to be silent (a
    // subscriber that only receives, a link opened ahead of demand) took neither, and the
    // sweep closed it with kCloseAuthTimeout at the deadline, over and over. Exempting it from
    // the sweep instead would have left rows nothing ever retires, and 24 idle links would
    // then fill the fixed ledger and refuse every further upgrade — the same deployment, a
    // worse failure. Not carrying the row at all is what removes both.
    //
    // (b) everything else — open the ledger row, unchanged. It goes in BEFORE the 101 so the
    // peer's first data frame, which may arrive on another httpd select round the instant the
    // upgrade completes, can never race the row it has to consume. It is the only record of a
    // silent upgrade that exists — esp_http_server answers the handshake itself and such a
    // session is claimed lazily on its first frame — so without it nothing at all bounds a
    // socket that opened and said nothing, which is the case the reference names as the attack.
    //
    // Either way, only on a link with an auth hook: without one there is no deadline to
    // enforce, the verdict changes nothing about how a session is served, and a silent socket
    // is the HTTP server's own business exactly as it always was.
    //
    // LOCK ORDER. Both arms run under `gate->m` and the claim nests `peers_m_` under it, which
    // is the permitted order (see gate_t) and the one this path already holds. The link is
    // re-resolved through the gate the way every other latched callback does.
    session_t* claimed = nullptr;
    peer_handle_t claimed_handle;
    slot_name_t claimed_peer;
    if (admit) {
        const std::lock_guard lock(gate->m);
        httpd_ws_link_t* const self = gate->link;
        if (self != nullptr && self->auth_fn_ != nullptr) {
            if (verdict == admission_verdict_t::ADMIT_AUTHENTICATED) {
                const std::lock_guard peers(self->peers_m_);
                claimed = self->claim_session(fd, /*authenticated=*/true);
                if (claimed == nullptr) {
                    // At `max_peers`, and the answer is the SAME clean abandon a REFUSE verdict
                    // gets: ESP_FAIL out of the pre-handshake, no 101, no session, no latched
                    // route. Strictly better than the alternative this replaces — admitting the
                    // peer and then killing it at the deadline — and it is counted exactly
                    // once, by the `!admit` arm below, never here as well.
                    ESP_LOGW(kTag,
                             "peer refused at the handshake: at max_peers=%u or out of session "
                             "memory (fd=%d)",
                             (unsigned)self->max_peers_, fd);
                    admit = false;
                } else {
                    // Copied out under the lock: the announcement below runs with `peers_m_`
                    // released and the slot's name is recycled in place at the next claim.
                    claimed_handle = claimed->handle;
                    claimed_peer = claimed->name;
                }
            } else if (!self->note_pending_handshake(fd)) {
                // Refused BY VALUE and turned into the same clean abandon a REFUSE verdict gets.
                // The alternative — admitting a socket the ledger cannot hold — would be
                // admitting one nothing can ever close.
                admit = false;
            }
        }
    }
    if (claimed != nullptr) {
        // The claim edge's three side effects, run with EVERY lock of ours released and with
        // the link kept alive by the raised `depth` — the same three the first-frame claim runs
        // (see on_data_frame) and for the same reasons, so a session claimed at the 101 is
        // indistinguishable from one claimed at a frame from here on.
        //
        // The free_ctx FIRST, and it is the load-bearing one: `on_session_closed` is the only
        // path that ever reclaims a slot, so a session claimed here and never armed would leak
        // its slot for the life of the link the moment its peer disconnected without speaking —
        // which is precisely the peer this change exists to serve. The session already exists
        // at this point (esp_http_server creates it at accept, before it parses the request
        // this predicate is called from), so the ctx lands on the socket that is about to be
        // upgraded.
        httpd_sess_set_ctx(req->handle, fd, claimed, &httpd_ws_link_t::on_session_closed);
        httpd_ws_link_t* self = nullptr;
        {
            const std::lock_guard lock(gate->m);
            self = gate->link;
        }
        if (self != nullptr) {
            self->bound_socket(fd);
            self->check_httpd_stack();
            // The arrival seam (#1223), announced here rather than under either lock because
            // the notifier re-enters the routing plane and takes graph locks. This session is
            // authenticated, so unlike the pending case there is nothing the narrowing is
            // withholding from it.
            self->notify_arrived(claimed_handle, claimed_peer.view());
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
    return admit || upgrade_refused ? ESP_OK : ESP_FAIL;
}

bool httpd_ws_link_t::queue_refusal(gate_t* gate, httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    // From the gate's store (`memory.state`): this is a static callback, and the gate is the
    // one object that both reaches the link's source and outlives the link.
    auto* const item = mem::make_in<refusal_req_t>(*gate->src, gate, fd);
    if (item == nullptr) return false;
    // Queued from the httpd task, so it cannot run before this handshake returns and the
    // server writes the 101: the close always follows the upgrade on the wire.
    if (httpd_queue_work(req->handle, &httpd_ws_link_t::refusal_work, item) != ESP_OK) {
        mem::drop_in(*gate->src, item);
        return false;
    }
    // The mark: the gate as the session ctx. The session exists already (httpd seats it at
    // accept), and this is a request-scoped set that httpd copies into the session when the
    // handshake request completes, as the #1334 claim above relies on. The gate is compared,
    // never freed through this session — see keep_refusal_mark.
    httpd_sess_set_ctx(req->handle, fd, gate, &keep_refusal_mark);
    return true;
}

void httpd_ws_link_t::refusal_work(void* req_arg) {
    auto* const raw = static_cast<refusal_req_t*>(req_arg);
    const mem::block_ptr_t<refusal_req_t> req(*raw->gate->src, raw);
    httpd_ws_link_t* owner = nullptr;
    {
        // Resolved through the gate and held by `depth`, exactly as close_work does. A link
        // already gone leaves the socket to its server: its latched handler finds the gate
        // shut and fails the next frame, which closes it.
        const std::lock_guard lock(req->gate->m);
        owner = req->gate->link;
        if (owner == nullptr) return;
        ++req->gate->depth;
    }
    owner->refuse_upgraded(req->fd);
    {
        const std::lock_guard lock(req->gate->m);
        --req->gate->depth;
    }
    req->gate->cv.notify_all();
}

void httpd_ws_link_t::refuse_upgraded(int fd) {
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    // The identity test. The item was queued at the handshake and runs a select round or more
    // later; by then the peer may have hung up and its descriptor been reused. Only a socket
    // that is upgraded AND still carries this link's mark is the one refused: a stranger on
    // the reused number has another ctx, and a socket this already closed has none.
    if (h == nullptr || httpd_ws_get_fd_info(h, fd) != HTTPD_WS_CLIENT_WEBSOCKET ||
        httpd_sess_get_ctx(h, fd) != gate_.load(std::memory_order_relaxed))
        return;
    // Clear the mark first, so the other caller (a queued item after a frame-path refusal, or
    // the reverse) finds nothing to do.
    httpd_sess_set_ctx(h, fd, nullptr, &keep_refusal_mark);
    // RFC 6455 §5.5.1: the 2-byte code in network order, then the UTF-8 reason. Then the
    // shutdown, in that order for close_session's reason: after it no write gets out.
    std::array<std::byte, 2 + kMaxCloseReasonBytes> payload{};
    payload[0] = static_cast<std::byte>((refusal_code_ >> 8) & 0xFF);
    payload[1] = static_cast<std::byte>(refusal_code_ & 0xFF);
    std::memcpy(payload.data() + 2, refusal_reason_.data(), refusal_reason_len_);
    (void)send_now(nullptr, fd, HTTPD_WS_TYPE_CLOSE,
                   std::span<const std::byte>(payload.data(), 2 + refusal_reason_len_));
    condemn(fd);
    // Its frame went through the drain budget like any other, so it may be the socket a drain
    // put on Nagle (ADR-0085 §7). It has no session for on_session_closed to clear that from,
    // so it is cleared here: a recycled descriptor starts on TCP_NODELAY like every other.
    if (rx_nagle_fd_ == fd) rx_nagle_fd_ = -1;
}

httpd_ws_link_t::session_t* httpd_ws_link_t::claim_session(int fd, bool authenticated) {
    // ONE claim body, TWO edges (#1334) — the lazy first-frame one every ordinary session
    // takes, and the eager pre-handshake one an already-authenticated session takes. Factored
    // rather than duplicated because everything below is an INVARIANT of a claimed slot (the
    // generation bump and the handle minted from it, the retired resolution, the positional
    // name, the cleared subject); a second copy is a second place for one of them to rot.
    //
    // The caller holds `peers_m_`. It is not taken here, because both callers need a wider
    // critical section than the claim itself: the first-frame path consumes the ledger row and
    // reads the fresh name back under one hold, and the pre-handshake path nests this under
    // `gate->m` — the documented order (peers_m_ UNDER m; see gate_t), which is also the order
    // that path already holds when it gets here.
    //
    // Admission cap first, so a refusal costs no slot mutation at all.
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
        for (session_t* const s : slots_)
            if (s->open && !s->dead) ++open_n;
        if (open_n >= max_peers_) return nullptr;  // the CALLER counts and names it
    }
    // The index is carried out of the search, not recomputed: it IS the peer's
    // routable name below, so losing it here is what cost every ESP node its
    // addressability (#994).
    session_t* slot = nullptr;
    std::size_t idx = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i]->fd < 0) {
            slot = slots_[i];
            idx = i;
            break;
        }  // reuse a departed slot
    if (slot == nullptr) {
        // A new peer past the high-water mark: one session and one table entry from
        // `memory.state` (#1880). A refusal of either is this peer's to pay — it is refused
        // exactly like a peer past `max_peers`, and the CALLER counts it the same way.
        slot = mem::make_in<session_t>(*state_src_, *state_src_);
        if (slot == nullptr || !slots_.push_back(slot)) {
            mem::drop_in(*state_src_, slot);
            return nullptr;
        }
        slot->gate = gate_.load(std::memory_order_relaxed);
        idx = slots_.size() - 1;
    }
    // Belt and braces: every path that frees a slot retires its handle first, so
    // this is a no-op — but a slot must NEVER carry a handle stamped at a
    // generation it is about to leave behind, and that invariant belongs at the
    // claim, next to the bump that would break it.
    retire_resolution(slot, /*inert=*/false);
    ++slot->gen;  // a NEW session in a recycled slot: fail every stale ref (#954)
    // The seam's handle is minted from that same bump (#1294) — one identity for
    // the TX path and the routing plane, never two that could disagree. `gen` is
    // never 0 after the increment on a claim, so the handle is always valid.
    slot->handle = peer_handle_t{static_cast<std::uint32_t>(idx), slot->gen};
    // ADR-0073 §2: the routable NAME is the handle spelled out, `p<slot>~<gen>`, legal
    // by construction, and the `<ip>:<port>` goes to the diagnostics field instead of
    // into the graph. The generation makes a reused slot's name a fresh one.
    slot->name.len = format_slot_name(slot->handle, slot->name.text.data());
    format_endpoint(fd, slot->endpoint_str);
    slot->asm_buf.clear();
    slot->fd = fd;
    slot->open = true;
    // THIS claim is the establish edge, whichever of the two it is — the only honest place to
    // stamp "connected at" and to start this connection's counters. For an ordinary session
    // that is its first data frame, because the opening GET never reaches the URI handler; for
    // an already-authenticated one it is the pre-handshake, i.e. the 101 itself, which is
    // strictly the more accurate of the two instants (#1334).
    slot->st = {};
    slot->st.connected_at_us = esp_timer_get_time();
    // A link with an auth hook claims a session UNAUTHENTICATED unless the HANDSHAKE already
    // settled it (#1245/#1334), and its auth record starts unseen: the application's next
    // liveness check stamps it (#2257). An authenticated session is never judged by it,
    // because it has already answered the question the auth hook asks, and a
    // native dialer cannot answer it a second time in-band: it has no way to send an
    // authentication frame, so a deadline judged here would close every dialer that is entitled
    // to stay silent. The subject is cleared either way: slots are recycled in place, so a
    // stale identity left here would be published for whoever landed on the slot next.
    slot->auth_pending = auth_fn_ != nullptr && !authenticated;
    slot->auth_since = kAuthUnseen;
    slot->subject[0] = '\0';
    slot->subject_len = 0;
    return slot;
}

void httpd_ws_link_t::set_nodelay(int fd, bool on) {
    const int value = on ? 1 : 0;
    // Best-effort like bound_socket: a socket that cannot take the option keeps what it
    // had, which is the pre-ADR-0085 behaviour for that one peer.
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0)
        ESP_LOGD(kTag, "TCP_NODELAY=%d not applied fd=%d", value, fd);
}

void httpd_ws_link_t::pace_rx(int fd, std::size_t frame_bytes) {
    if constexpr (!kRxDrainPaced) return;
    const auto core = static_cast<std::size_t>(xPortGetCoreID());
    idle_gate_t& gate = g_idle_gates[core];
    if (!gate.hooked.load(std::memory_order_acquire)) return;
    std::uint32_t epoch = gate.epoch.load(std::memory_order_seq_cst);
    // An unpinned httpd task that migrated compares against a different core's epoch, which
    // means nothing; it starts a new drain on the core it is now on. The budget is therefore
    // approximate for such a task (ADR-0085), never a hang: it waits on the gate it is on.
    if (core != rx_drain_core_) {
        rx_drain_core_ = core;
        rx_drain_epoch_ = epoch - 1;  // any value but `epoch`: forces the reset below
    }
    // The drain is the run of frames since this core last idled; an epoch that moved since
    // the previous frame starts a new one. Otherwise a spent budget waits for the idle
    // task. While it waits it is still the httpd task, the one drain of this link's queued
    // sends, so a send posted to it by another task (a held frame) is RUN FROM HERE,
    // on this task, the socket write and all, and the park goes on. The frame stays unread:
    // egress wakes the park, ingress waits for idle, and a peer who can make this link
    // post a send per frame (a subscriber over another link, an app that replies off-task)
    // buys no ingress with it. The waiter counts itself in, THEN re-reads the epoch and the
    // pending count (the handshake on idle_gate_t, which egress_posted joins from the
    // producer's side), so neither an idle pass nor a post in between is slept through.
    // Returning instead, to let the server loop run the item, is not an option: IDF has
    // consumed the frame's first header byte before this handler is called
    // (httpd_parse.c httpd_req_new -> httpd_ws_get_frame_type), so a handler that returns
    // without the payload desynchronises the stream.
    // The third way a drain ends is on its own EGRESS: the in-call reply bytes it wrote
    // (kRxDrainTxBytes), so that what is unacknowledged never outgrows the send buffer
    // while this task is the one holding the ACKs back (see kRxDrainNagleFrames).
    const bool spent = (kRxDrainFrames != 0 && rx_drain_frames_ >= kRxDrainFrames) ||
                       (kRxDrainBytes != 0 && rx_drain_bytes_ >= kRxDrainBytes) ||
                       rx_drain_tx_bytes_ >= kRxDrainTxBytes;
    if (epoch == rx_drain_epoch_ && spent) {
        rx_drain_waits_.fetch_add(1, std::memory_order_relaxed);
        gate.waiters.fetch_add(1, std::memory_order_seq_cst);
        while (gate.epoch.load(std::memory_order_seq_cst) == epoch) {
            if (flush_egress()) continue;  // more may have been posted while those went out
            (void)xSemaphoreTake(gate.sem, portMAX_DELAY);
        }
        gate.waiters.fetch_sub(1, std::memory_order_seq_cst);
        epoch = gate.epoch.load(std::memory_order_seq_cst);
    }
    if (epoch != rx_drain_epoch_) {
        // A drain that ended by idle BEFORE the flood shape showed is a light load: the
        // session that was put on Nagle gets TCP_NODELAY back. One that reached the shape
        // keeps Nagle through the next drain, since its coalesced segments may still be
        // unacknowledged and the next drain's first replies would otherwise queue behind
        // them under TCP_NODELAY. Interactive peers never reach the shape and never flip.
        if (rx_nagle_fd_ >= 0 && rx_drain_frames_ < kRxDrainNagleFrames) {
            set_nodelay(rx_nagle_fd_, true);
            rx_nagle_fd_ = -1;
        }
        rx_drain_epoch_ = epoch;
        rx_drain_frames_ = 0;
        rx_drain_bytes_ = 0;
        rx_drain_tx_bytes_ = 0;
    }
    ++rx_drain_frames_;
    rx_drain_bytes_ += frame_bytes;
    // The flood shape: this many frames without the core idling. From here the drain's
    // replies coalesce. Switched on the session being drained, before its reply is written.
    if (rx_drain_frames_ >= kRxDrainNagleFrames && rx_nagle_fd_ != fd) {
        if (rx_nagle_fd_ >= 0) set_nodelay(rx_nagle_fd_, true);
        set_nodelay(fd, false);
        rx_nagle_fd_ = fd;
    }
}

void httpd_ws_link_t::egress_posted() noexcept {
    if constexpr (!kRxDrainPaced) return;
    // The producer's side of the idle_gate_t handshake, after it has stored its frame under
    // the session's tx_m: a drain that counted itself in before that store sees `waiters`
    // here and is given the semaphore; one that counts itself in after it takes the same
    // mutex in its flush and finds the frame before it sleeps. A give nobody takes saturates
    // the binary semaphore and costs the next waiter one extra pass. Every core's gate is
    // tested, because a producer does not know which core the httpd task is parked on; a
    // gate with no waiter costs one load.
    for (idle_gate_t& gate : g_idle_gates)
        if (gate.waiters.load(std::memory_order_seq_cst) != 0) (void)xSemaphoreGive(gate.sem);
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
    // The drain budget (ADR-0085). After the header, so the frame's size is known and is
    // charged whole; before the payload, so a spent budget leaves every byte of it — and
    // of whatever follows — in the socket while this task waits for its core to idle.
    pace_rx(httpd_req_to_sockfd(req), frame.len);

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
    fixed_array_t<std::byte> heap_payload;
    // The OWNING-RX segment (#1565), when an integrator named a source to draw it from. It
    // is the recv DESTINATION, not a copy of one: the frame lands in pool memory once and
    // travels from there by refcount, which is the entire saving the mode exists for. Empty
    // on a borrowed link, on a fragmented frame, and on a refusal — the three cases below.
    view::segment_ptr_t owned_seg;
    bool pool_refused = false;
    std::byte* payload = nullptr;
    if (frame.len != 0) {
        // ONE test in front of the two arms that were already here, against a member written
        // once by the constructor and never again. A borrowed link answers it false and
        // takes the scratch arm exactly as it always has — no allocation, no second read of
        // anything, and nothing at all added to `deliver` itself (the frame-rate seam this
        // deliberately stays out of; see deliver_owned).
        if (rx_backend_ != nullptr) {
            if (view::segment_t* const seg = rx_backend_->alloc(frame.len); seg != nullptr) {
                owned_seg = view::segment_ptr_t::adopt(seg);
                payload = owned_seg->bytes.data();
            } else {
                // The injected source refused: a bounded pool doing its job. Do NOT reach for
                // the heap — that would hand back the unbounded footprint the injection was
                // made to bound. The frame is dropped, but its BYTES must still leave the
                // stream or the next recv reads them as a header, so it is drained through
                // the scratch below and discarded after (see `pool_refused`).
                pool_refused = true;
            }
        }
        if (payload == nullptr) {
            if (rx_scratch_ != nullptr && frame.len <= rx_scratch_bytes_) {
                payload = rx_scratch_.data();
            } else if (pool_refused) {
                // A refused frame too large to drain through the scratch. Allocating a
                // drain buffer here would defeat the refusal, so the only bounded answer is
                // the one the sibling OOM arm already gives: fail the handler and let httpd
                // close THIS session. Counted first, as its own cause.
                note_rx_pool_refusal(frame.len);
                return ESP_FAIL;
            } else {
                if (!heap_payload.assign(*state_src_, frame.len)) {
                    note_rx_alloc_fail(frame.len);
                    return ESP_FAIL;
                }
                payload = heap_payload.data();
            }
        }
        frame.payload = reinterpret_cast<std::uint8_t*>(payload);
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) return err;
    }
    if (pool_refused) {
        // Drained, and now dropped: the stream stays framed, the peer stays connected, and
        // the loss is named rather than silent.
        note_rx_pool_refusal(frame.len);
        return ESP_OK;
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
    slot_name_t peer;
    peer_handle_t handle;
    bool newly_claimed = false;
    bool pending = false;  // this session has not authenticated yet — see session_t::auth_pending
    bool refused = false;  // refused after its upgrade (#1857) — see queue_refusal
    {
        const std::lock_guard lock(peers_m_);
        for (session_t* const s : slots_)
            if (s->open && s->fd == fd) {
                slot = s;
                break;
            }
        // A socket refused after its upgrade (#1857) that spoke before its queued close went
        // out. It has no slot, so this costs served sessions nothing; it gets the close here
        // instead, and its frame, already drained, is handled no further.
        refused = slot == nullptr &&
                  httpd_sess_get_ctx(req->handle, fd) == gate_.load(std::memory_order_relaxed);
        if (slot == nullptr && !refused) {
            // Consume the pending-handshake row HERE, ahead of the cap test, because this
            // socket has now spoken and the ledger only ever bounded silent ones (#1247).
            // Consuming it also HANDS THE BOUND OVER: past this point the connection is either
            // a session with its own `auth_since` record or a refusal httpd is about to close,
            // and in neither case may the sweep still see it as an un-spoken socket — it would
            // otherwise be closed twice, on two different arguments, and counted twice.
            take_pending_handshake(fd);
            // No teardown test: the gate admitted this frame, so the barrier has not
            // snapshotted yet and a session armed here is still caught.
            //
            // `authenticated=false`: reaching this claim at all means the handshake did NOT
            // authenticate this peer, because since #1334 one that did claimed its slot at the
            // 101 and would have been found by the fd search above.
            slot = claim_session(fd, /*authenticated=*/false);
            if (slot == nullptr) {
                // Admission cap: refuse cleanly (ESP_FAIL => httpd closes the socket). Counted
                // and named HERE rather than inside the claim, because the other claim edge
                // counts its refusal through the abandoned upgrade instead (#1334).
                peers_refused_.fetch_add(1, std::memory_order_relaxed);
                ESP_LOGW(kTag, "peer refused: at max_peers=%u or out of session memory",
                         (unsigned)max_peers_);
                return ESP_FAIL;
            }
            newly_claimed = true;
        }
        if (!refused) {
            peer = slot->name;
            handle = slot->handle;
            pending = slot->auth_pending;
        }
    }
    if (refused) {
        refuse_upgraded(fd);
        return ESP_OK;  // the shutdown above closes it, as an auth REJECT does
    }
    // Reclaim the slot on close — armed once, when first claimed (the free_ctx fires on the
    // httpd task at close). Outside peers_m_ so no httpd lock nests under ours.
    if (newly_claimed) {
        httpd_sess_set_ctx(req->handle, fd, slot, &httpd_ws_link_t::on_session_closed);
        // The claim edge is also where the per-socket policy is applied — once per
        // session, on the socket that just became a peer's.
        bound_socket(fd);
        // ...and so is the stack sample (#955). Twice per link now that #1334 added the
        // pre-handshake claim edge, but never twice per SESSION: the two edges are mutually
        // exclusive per connection (a session claimed at the 101 is found by the fd search
        // above and `newly_claimed` stays false for every frame it ever sends).
        check_httpd_stack();
        // ...and so is the arrival seam (#1223). Here, not inside the peers_m_ hold above:
        // the notifier re-enters the routing plane and takes graph locks, which is the same
        // precondition notify_departed carries. Announced even while `auth_pending`: an
        // anchor is invisible to enumeration, resolution and fan-out (it is not an address),
        // so it grants a pending session nothing the auth narrowing withholds — and a
        // session closed at the auth deadline is torn down through the ordinary departure
        // seam, which retires it. The arrival is also the slot's RE-TENANT edge (#1609): the
        // routing plane reclaims whatever a predecessor in this slot left filed under the
        // name, and this is before the frame below can carry the new session's first
        // SUBSCRIBE — so a name-keyed eviction never has two generations to tell apart.
        notify_arrived(handle, peer.view());
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
        else if (owned_seg)
            // OWNING (#1565): the bytes are already in the integrator's pool memory — this
            // hands the segment up, it does not copy it. The move empties `owned_seg`, so
            // the sink's rope is the only reference and nothing is released underneath it.
            deliver_owned(handle, view::view_t{std::move(owned_seg), 0, body.size()});
        else
            // Borrowed: no extra copy, and no owned segment either — the borrowed link, and
            // also the EMPTY frame on an owning one, which has no bytes to hand up. An empty
            // data frame carries no TLV and is rejected at decode wherever it lands, so a
            // rope sink that never sees it is told nothing it did not already know.
            deliver(handle, body);
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_CONTINUE && slot->asm_buf.empty())
        return ESP_OK;  // stray CONTINUE with no assembly open — drop
    if (frame.type == HTTPD_WS_TYPE_BINARY)
        slot->asm_buf.clear();  // a BINARY mid-assembly discards the stale partial
    if (slot->asm_buf.over_cap(body.size())) {
        // The peer counter has this; the log is the way to see it without polling (#953),
        // at the level of its sibling alloc failure just below.
        note_reassembly_over_cap(slot->asm_buf.charged(), body.size());
        slot->asm_buf.clear();
        note_rx_drop(slot);
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
        else if (rx_backend_ != nullptr)
            // The reassembled message cannot have been recv'd into pool memory — it was
            // built fragment by fragment in `asm_buf` before its total size was known — so
            // the owning mode pays ONE copy into a pool segment here. That is the price of
            // not starving a rope sink of the one message shape this link cannot hand it
            // directly.
            deliver_owned_copy(handle, message.bytes());
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
}

void httpd_ws_link_t::auth_sweep_work(void* arg) {
    auto* const gate = static_cast<gate_t*>(arg);
    if (gate == nullptr) return;
    httpd_ws_link_t* self = nullptr;
    {
        const std::lock_guard lock(gate->m);
        // A sweep queued before the teardown can drain after it. A null link is the whole
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
    // The verdict close_frozen_peers reached, on the caller's task, against its own `deadline`
    // (#2257). Records only ever move from unseen to a `now` no later than that, so a session
    // claimed after the check is never caught here.
    const std::uint64_t cutoff = auth_cutoff_.load(std::memory_order_relaxed);
    // Same chunked, resumable scan the fan-out uses (see send), and for the same two
    // reasons: the snapshot is a FIXED on-stack array rather than a container sized to the
    // peer set — no allocation on a path the httpd task runs — and the closes below
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
                session_t* const s = slots_[next++];
                if (s->open && !s->dead && s->auth_pending && s->auth_since < cutoff)
                    expired[n++] = s;
            }
            more = next < slots_.size();
        }
        for (std::size_t i = 0; i < n; ++i) {
            auth_expired_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGW(kTag, "session closed: no credential within the auth deadline");
            close_session(expired[i], kCloseAuthTimeout);
        }
    }
    sweep_pending_handshakes(cutoff);
}

void httpd_ws_link_t::sweep_pending_handshakes(std::uint64_t cutoff) {
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
                if (pending_[i].since < cutoff) {
                    at = i;
                    break;
                }
            if (at == pending_n_) break;
            fd = pending_[at].fd;
            // Retire the row BEFORE the close, and while still holding the lock: the close
            // below runs unlocked, and a row left behind would be swept again on the next check
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
                                    std::span<const std::byte> payload, std::size_t* on_wire) {
    if (on_wire != nullptr) *on_wire = 0;
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
    // A held frame part-way on this socket goes out first, or this frame's bytes would land
    // inside it (#2095). If it cannot be finished the session is condemned with it.
    if (slot != nullptr && !finish_wire(slot, fd)) return ESP_FAIL;
    // Bracket it exactly as write_frame does, so the send override judges a short write on
    // an auth frame by the same rule it judges every other frame by (#951) — a truncated
    // CLOSE desynchronises the stream just as thoroughly as a truncated reply.
    if (slot != nullptr) slot->open_tx_frame();
    const esp_err_t err = httpd_ws_send_frame_async(h, fd, &f);
    const std::size_t wrote = slot != nullptr ? slot->close_tx_frame() : 0;
    // The bracket's verdict, reported only to a caller that asked for it. A non-zero count
    // on a failed send is the OTHER failure (#951): the frame was announced and then cut
    // off, the session has already been condemned inside the write, and a caller that logs
    // an ordinary drop on top of it makes a desynchronised session look benign. The auth
    // and close callers pass nullptr — they are tearing the session down either way.
    if (on_wire != nullptr) *on_wire = wrote;
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

bool httpd_ws_link_t::note_pending_handshake(int fd) {
    if (fd < 0) return false;
    const std::lock_guard lock(peers_m_);
    // REPLACE a stale row for the same descriptor rather than adding a second. A descriptor is
    // reused as soon as the kernel frees it, so a leftover from a handshake whose peer never
    // sent a frame must not charge the NEXT connection to land on that number a deadline that
    // has already half elapsed — the one way this could close a peer that just arrived.
    for (std::size_t i = 0; i < pending_n_; ++i) {
        if (pending_[i].fd != fd) continue;
        pending_[i].since = kAuthUnseen;
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
        // application's liveness check frees rows past its deadline (#2257).
        ESP_LOGW(kTag, "pending-handshake ledger full (%u) - handshake refused (fd=%d)",
                 static_cast<unsigned>(pending_.size()), fd);
        return false;
    }
    pending_[pending_n_++] = pending_handshake_t{fd, kAuthUnseen};
    return true;
}

void httpd_ws_link_t::take_pending_handshake(int fd) noexcept {
    if (fd < 0) return;
    for (std::size_t i = 0; i < pending_n_; ++i) {
        if (pending_[i].fd != fd) continue;
        drop_pending_handshake(i);
        return;
    }
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
    const auth_result_t res = fn(ctx, fd, body);
    // The reply goes out FIRST, whatever the verdict. On CONTINUE it is the handshake's next
    // message; on REJECT it lets a scheme say why in its own language, ahead of the close
    // code; on ACCEPT it is the confirmation a client may be waiting for before it starts
    // sending TLVs. Empty (a bearer token needing no answer) sends nothing.
    if (!res.reply.empty()) (void)send_now(slot, fd, HTTPD_WS_TYPE_BINARY, res.reply);
    // A subject is stored whole or not at all: one longer than the field, or one carrying a
    // NUL, cannot be kept as the exact identity the hook named, so its ACCEPT is answered as
    // a REJECT (#1846). Never shortened to something that would compare as another subject.
    const bool subject_fits = res.subject.size() <= kMaxSubjectChars &&
                              (res.subject.empty() || std::memchr(res.subject.data(), '\0',
                                                                  res.subject.size()) == nullptr);
    const auth_verdict_t verdict = res.verdict == auth_verdict_t::ACCEPT && !subject_fits
                                       ? auth_verdict_t::REJECT
                                       : res.verdict;
    switch (verdict) {
        case auth_verdict_t::ACCEPT: {
            const std::lock_guard lock(peers_m_);
            if (!slot->open || slot->dead) return;  // departed inside the hook
            // The subject is the session's caller context from the next frame on (#1846):
            // @ref peer_subject answers it for this generation only, and the reclaim clears it.
            const std::size_t n = res.subject.size();  // fits: checked above
            if (n != 0) std::memcpy(slot->subject, res.subject.data(), n);
            slot->subject[n] = '\0';
            slot->subject_len = static_cast<std::uint8_t>(n);
            // The session becomes a peer HERE, and everything that was closed to it opens at
            // once: enumeration, resolution, fan-out and the graph. Clearing the flag is what
            // keeps a served session out of the sweep's reach forever after.
            slot->auth_pending = false;
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
    slot_name_t departed;
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
        // The socket on Nagle is gone with its session; a recycled descriptor starts on
        // TCP_NODELAY from bound_socket like every other.
        if (owner->rx_nagle_fd_ == slot->fd) owner->rx_nagle_fd_ = -1;
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
    owner->notify_departed(departed_handle, departed.view());
    {
        // `owner` may be DESTROYED by now, exactly as at the tail of @ref ws_handler: the
        // notifier can drive an app teardown, and the barrier above is what let it start.
        // Only `gate`, which deliberately outlives the link, may be touched from here on.
        const std::lock_guard lock(gate->m);
        --gate->depth;
    }
    gate->cv.notify_all();
}

httpd_ws_link_t::slot_name_t httpd_ws_link_t::reclaim_slot(session_t* slot, peer_handle_t& handle) {
    slot_name_t departed;
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
        slot->subject[0] = '\0';
        slot->subject_len = 0;
        departed = slot->name;
        // RETIRE the seam's handle with the name (#1294): "valid until depart" means
        // nothing this slot carries afterwards may be stamped with the departed session's
        // identity. `gen` itself survives, so the next claim mints a fresh one.
        handle = slot->handle;
        slot->handle = peer_handle_t{};
        slot->open = false;
        slot->fd = -1;
        slot->name = {};
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
    {
        // Every TX frame goes with the session (#2095): the one on the wire is this task's own
        // (no flush can be running — this is the httpd task, inside free_ctx), and the waiting
        // ones are producers', so they are taken under tx_m. A producer that resolved the
        // session before it left may still store one afterwards; it carries the departed
        // generation, and the next flush skips it.
        const std::lock_guard lock(slot->tx_m);
        release_frame(slot->tx);
        while (tx_wait_t* const w = slot->tx_head) {
            slot->tx_head = w->next;
            release_frame(w->frame);
            free_waiting(*slot, w);
        }
        slot->tx_tail = nullptr;
        slot->tx_stalls = 0;  // the stall count is the departed session's, like its frames
        slot->tx_refused = false;
        slot->tx_moved = true;  // and so is its liveness record (#2198)
        slot->tx_frozen = false;
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
    for (const session_t* const s : slots_)
        if (s->open) return true;
    return false;
}

void httpd_ws_link_t::note_rx_message(session_t* slot, std::size_t bytes) {
#ifdef CONFIG_LIBTRACER_LINK_TRAFFIC_STATS
    const std::lock_guard lock(peers_m_);
    ++slot->st.rx_frames;
    slot->st.rx_bytes += static_cast<std::uint32_t>(bytes);
    slot->st.last_rx_us = esp_timer_get_time();
#else
    // Traffic counters are the application's choice and it did not make one (#1663): a
    // delivered message takes neither the link-wide lock nor a timer read.
    static_cast<void>(slot);
    static_cast<void>(bytes);
#endif
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

/**
 * @brief Count and name an inbound frame the injected owning-RX source refused (#1565).
 *
 * `noinline` for the reason on @ref httpd_ws_link_t::note_rx_oversize: `on_data_frame` runs
 * per FRAME, and a log call site inlined into it costs a format string and its argument
 * setup on every frame that never needs one.
 *
 * The `len` is in the message because it is the number an operator sizes the injected pool
 * against — the refusal is either "the pool is full" or "no class of it is this wide", and
 * the two look identical from here but not from the pool's own census.
 */
[[gnu::noinline]] void httpd_ws_link_t::note_rx_pool_refusal(std::size_t len) {
    rx_dropped_pool_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW(kTag, "rx pool refused a segment (len=%u) - frame dropped", (unsigned)len);
}

/** @brief Name a reassembly that would pass the cap — the per-peer counter is bumped by the
 *         caller, which holds the slot; this adds only the log the site never had.
 *         `noinline` for the reason on @ref note_rx_oversize. */
[[gnu::noinline]] void httpd_ws_link_t::note_reassembly_over_cap(std::size_t had,
                                                                 std::size_t adding) {
    ESP_LOGW(kTag, "reassembly over the cap (%u charged + %u > %u) - message dropped",
             (unsigned)had, (unsigned)adding, (unsigned)kMaxFrameBytes);
}

void httpd_ws_link_t::deliver(peer_handle_t peer, std::span<const std::byte> frame) {
    // Peer-named bus tag when the facet is on (each tab its own return route); the flat
    // point-to-point sink otherwise — matching what fwd_router_t::add_child installed.
    if (peer_named_) {
        peer_rx_.deliver_borrowed(peer, frame);
    } else {
        rx_peer_ = peer;  // the flat arm's WHO, for @ref inbound_peer (#1846)
        rx_.deliver_borrowed(frame);
    }
}

void httpd_ws_link_t::deliver_owned(peer_handle_t peer, view::view_t frame) {
    // The same two-slot fork as @ref deliver, one tier up. `receiver_slot_t::deliver` is the
    // TIER SELECT: a rope sink — which is what fwd_router_t::add_child installed, because
    // delivers_ropes() answered true for this link — takes the frame owning and may pin it,
    // subrope it, or forward it; a span-only sink installed by some other embedder gets the
    // same bytes borrowed for the call. Either way this function is unreachable unless an
    // integrator named an rx_backend, so no borrowed link pays a single instruction of it.
    if (peer_named_) {
        peer_rx_.deliver(peer, std::move(frame));
    } else {
        rx_peer_ = peer;
        rx_.deliver(std::move(frame));
    }
}

void httpd_ws_link_t::deliver_owned_copy(peer_handle_t peer, std::span<const std::byte> body) {
    view::segment_t* const seg = rx_backend_->alloc(body.size());
    if (seg == nullptr) {
        note_rx_pool_refusal(body.size());
        return;  // named drop — never the heap, for the reason on rx_backend()
    }
    view::segment_ptr_t held = view::segment_ptr_t::adopt(seg);
    if (!body.empty()) std::memcpy(held->bytes.data(), body.data(), body.size());
    deliver_owned(peer, view::view_t{std::move(held), 0, body.size()});
}

// ---------------------------------------------------------------------------
// TX — every send is marshalled onto the httpd task (the async-send pattern).
// ---------------------------------------------------------------------------

void httpd_ws_link_t::queue_send(const session_ref_t& to,
                                 std::span<const std::span<const std::byte>> iov) {
    // One destination, one block: gathered here, referenced by the peer's slot, and this
    // builder's reference dropped once it is staged.
    std::size_t total = 0;
    for (const auto& part : iov) total += part.size();
    tx_block_t* const blk = make_block({}, iov, nullptr);
    if (stage(to, blk, total, graph::delivery_lane_t{})) ring_doorbell(blk, total);
    drop_block(blk);
}

bool httpd_ws_link_t::stage(const session_ref_t& to, tx_block_t* blk, std::size_t total,
                            const graph::delivery_lane_t& lane) {
    // ONE load of the handle, into a local (#963): the plain read this replaces raced the
    // destructor's write outright, and re-reading it across the store let one send act on
    // two different values of the same member.
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    if (h == nullptr) return false;
    // Re-resolve the destination HERE rather than trusting the fd the caller looked at: a
    // departed or condemned peer takes no more frames. Counted (#953): normally benign — a
    // fan-out that snapshotted its destinations racing a departure — but a silent drop here
    // reads exactly like a delivery from outside.
    const int fd = live_fd(to);
    if (fd < 0) {
        tx_to_dead_peer_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A frame with no block — the large class spent, the egress store refusing, or the value
    // not keepable — is a counted drop at the link: evidence about this link's memory, not
    // about the peer. And once the dtor is draining, nothing new is held (teardown gate).
    if (blk == nullptr || stopping_.load(std::memory_order_relaxed)) {
        note_enqueue_drop(fd, total);
        return false;
    }
    // Into THIS peer's queue, and nowhere else (#2095): a reference to the shared block,
    // never a copy, waiting by its lane (#2195). A SAMPLE of a path already waiting REPLACES
    // that path's frame in place and is counted: the newest value of a path is the one a slow
    // peer gets, and a replacement takes no node. Any other frame — another path, a STREAM, a
    // frame from no vertex — waits behind, never in place of one: a frame from one vertex is
    // not a newer value of another, and an event or an ANNOUNCE is owed to the peer once. A
    // best-effort STREAM lane at its depth, or at the peer's cap (kTxWaitDepth frames in all),
    // sheds its own oldest frame (counted) and reuses its node; a full reliable or control
    // lane, a full queue with nothing of this lane waiting, or a refused node draw refuses the
    // NEW frame — the link's counted drop. A slow peer holds no more
    // than its own few frames and no part of the httpd task that another peer needs. Neither
    // a replacement nor a refusal is a stall strike: a healthy peer subscribed to many
    // vertices fills its queue in one burst while its TCP window is shut for an RTT, and
    // striking it would loop it through reconnects (#2174 rules the same).
    session_t* const s = to.slot;
    // What a full lane does, decided once from the lane: the control lane and a reliable
    // STREAM refuse the newer frame, a SAMPLE replaces, a best-effort STREAM sheds its oldest.
    static constexpr tx_frame_t::policy_t kArm[2][2] = {
        {tx_frame_t::kReplace, tx_frame_t::kReplace},  // depth 1, either arm: a SAMPLE
        {tx_frame_t::kShed, tx_frame_t::kRefuse}};     // a STREAM or control: shed, refuse
    const bool control = lane.key == nullptr;
    const std::size_t cap = control ? kEgressControlDepth : std::max<std::uint16_t>(lane.depth, 1);
    const tx_frame_t::policy_t policy = kArm[cap > 1][lane.reliable | control];
    assert((reinterpret_cast<std::uintptr_t>(lane.key) & 3) == 0 &&
           "delivery_lane_t::key is 4-aligned");
    const tx_frame_t fresh{blk, to.gen, 0, reinterpret_cast<std::uintptr_t>(lane.key) | policy};
    tx_frame_t old;
    bool refused = false;
    bool full = false;
    {
        const std::lock_guard lock(s->tx_m);
        // This session's frames (a departed one's are skipped later), and the link to this
        // lane's oldest.
        std::size_t waiting = 0;
        std::size_t count = 0;
        tx_wait_t** at = nullptr;
        for (tx_wait_t** l = &s->tx_head; *l != nullptr; l = &(*l)->next) {
            if ((*l)->frame.gen != to.gen) continue;
            ++waiting;
            if ((*l)->frame.lane != fresh.lane || count++ != 0) continue;
            at = l;
        }
        // Append while the lane and the queue have room; else replace or shed the lane's
        // oldest (a SAMPLE in its place; a best-effort STREAM's node takes the new frame to
        // the tail, which keeps the STREAM's own arm even when it is the peer's queue that is
        // full); else refuse.
        tx_wait_t* node = nullptr;
        if (count < cap && waiting < kTxWaitDepth) {
            void* raw = &s->tx_spare;  // the in-slot spare: one waiting frame draws nothing
            if (s->tx_spare_used)
                raw = io_src_->try_alloc(sizeof(tx_wait_t), alignof(tx_wait_t));
            else
                s->tx_spare_used = true;
            node = raw != nullptr ? new (raw) tx_wait_t : nullptr;
            refused = node == nullptr;
        } else if (policy != tx_frame_t::kRefuse && at != nullptr) {
            tx_wait_t* const w = *at;
            old = std::exchange(w->frame, fresh);
            if (policy == tx_frame_t::kShed && w != s->tx_tail) {  // the tail stays in place
                *at = w->next;
                node = w;
            }
        } else {
            refused = full = true;
        }
        blk->refs.fetch_add(static_cast<std::uint32_t>(!refused), std::memory_order_relaxed);
        if (node != nullptr) {
            node->frame = fresh;
            node->next = nullptr;
            (s->tx_tail != nullptr ? s->tx_tail->next : s->tx_head) = node;
            s->tx_tail = node;
        }
    }
    // A refused frame still rings the doorbell, at most one flush per link (post_flush): a
    // socket that reopened is offered its held frames on this link's own traffic, and the
    // zero-byte write it may meet only arms tx_refused, which no refusal reads. A full lane or
    // queue is this peer's own pace, as a replacement is, so it is counted and not logged per
    // frame (a stalled peer at 100 updates/s would otherwise log 100 lines a second); a
    // refused node draw is the link's memory, and logs like every other refused enqueue.
    if (full)
        enqueue_drops_.fetch_add(1, std::memory_order_relaxed);
    else if (refused)
        note_enqueue_drop(fd, total);
    if (old.held()) note_tx_conflated(to);
    release_frame(old);
    return true;
}

void httpd_ws_link_t::note_tx_conflated(const session_ref_t& to) {
    tx_conflated_.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard lock(peers_m_);
    // The identity test live_fd makes: a replaced frame is charged to the session it was
    // gathered for or to nobody, never to whoever holds the slot now (#954).
    if (to.slot->gen == to.gen) ++to.slot->st.tx_conflated;
}

void httpd_ws_link_t::ring_doorbell(tx_block_t* blk, std::size_t total) {
    // The burst's ONE doorbell (#2146), rung after every destination's frame is stored: a
    // parked drain is woken once and the flush posted once, whatever the peer count. Rung
    // per frame, as it was, each store could hand the core to the httpd task (higher
    // priority than most producers) and back: a broadcast to 8 peers cost about 24 task
    // switches where it now costs about 3 (the post, the httpd task's flush of all 8, the
    // return to the producer).
    egress_posted();  // a parked drain flushes them now rather than hold them
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    if (h != nullptr && post_flush(h)) return;
    // The control queue refused the flush. The frames cannot go out until something else
    // posts one, so every one still holding this burst's block (none a flush already took)
    // is taken back and counted exactly as a refused enqueue always was — a fact about the
    // shared control queue, charged to the link and to no peer. The caller still holds its
    // reference, so @p blk names this burst's block and no other. Same chunked walk as the
    // fan-out.
    session_t* batch[kFanoutChunk];
    int fds[kFanoutChunk];
    std::size_t next = 0;
    for (bool more = true; more;) {
        std::size_t n = 0;
        {
            const std::lock_guard lock(peers_m_);
            for (; next < slots_.size() && n < kFanoutChunk; ++n) {
                batch[n] = slots_[next++];
                fds[n] = batch[n]->fd;
            }
            more = next < slots_.size();
        }
        for (std::size_t i = 0; i < n; ++i) {
            session_t* const s = batch[i];
            tx_frame_t back;
            {
                const std::lock_guard lock(s->tx_m);
                for (tx_wait_t *w = s->tx_head, *prev = nullptr; w != nullptr;
                     prev = w, w = w->next) {
                    if (w->frame.blk != blk) continue;
                    (prev != nullptr ? prev->next : s->tx_head) = w->next;
                    if (s->tx_tail == w) s->tx_tail = prev;
                    back = std::exchange(w->frame, tx_frame_t{});
                    free_waiting(*s, w);
                    break;
                }
            }
            if (!back.held()) continue;
            release_frame(back);
            note_enqueue_drop(fds[i], total);
        }
    }
}

bool httpd_ws_link_t::post_flush(httpd_handle_t h) {
    // ONE flush in the control queue per link, whatever the number of frames held: the flush
    // walks every session, so a second item would find nothing the first did not. The flag
    // is cleared by the flush BEFORE it walks (tx_work), so a frame stored after the walk
    // passed its session always posts a new one.
    if (flush_posted_.exchange(true, std::memory_order_seq_cst)) return true;
    if (httpd_queue_work(h, &httpd_ws_link_t::tx_work, gate_.load(std::memory_order_relaxed)) ==
        ESP_OK)
        return true;
    flush_posted_.store(false, std::memory_order_seq_cst);
    return false;
}

void httpd_ws_link_t::send_in_call(const session_ref_t& to,
                                   std::span<const std::span<const std::byte>> iov) {
    // The #1494 fork: a reply serviced ON the httpd task writes to the socket HERE, and the
    // control queue never hears about it.
    //
    // What the queue is for is marshalling a send from ANOTHER task onto the one task that
    // owns the descriptor. A reply is already on it, so every step of the queued path is
    // pure cost paid twice over: a gather-copy into a pool slot, a control message through
    // a six-deep UDP mbox that the httpd task drains ONE message per server pass, and a
    // second trip through the scheduler to arrive at the same `httpd_ws_send_frame_async`
    // this function calls directly. That mbox is the scarcest thing on the link — it is
    // shared with the closes @ref condemn wants landed — and the drain lag is what put
    // issue #1494's cliff exactly at the pool depth of the time: the in-call reserve slot
    // was claimable once per DRAIN, not once per request, so a pipelining
    // client is serviced again before the previous reply's slot comes back and the reply
    // falls into the general pool, where an in-call send never waits (the #814 deadlock
    // reason) and therefore drops at once. Off the queue, none of that arithmetic applies:
    // a reply cannot be lost to a full pool, to a full mbox, or to a fan-out's occupancy.
    //
    // It is also the shape that cannot strand a slot in ANY ESP-IDF, which is the other
    // half of #1494's report. Above this component's floor (`idf: ">=5.5.5"`) the enqueue
    // verdict is trustworthy — `httpd_queue_work` reserves its mbox slot through a counting
    // semaphore before the `sendto`, so ESP_OK means the item will run and release its slot
    // (#949; @ref condemn records the same floor, and the pre-floor ESP_OK-that-lies it
    // displaced). Below it, the silent bin was real, and a dropped work item stranded the
    // buffer it carried. A reply that holds no frame of the session's is immune to that
    // whether the verdict lies or not.
    //
    // The fan-out/push path is NOT rerouted here. It is issued from producer tasks, which
    // is precisely the case the queue exists for, and it is conflated per peer (#2095),
    // which a reply must never be: the requester is waiting for THIS frame.
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    if (h == nullptr) return;
    // The same re-resolve @ref queue_send opens with, for the same reason: a departed or
    // condemned peer takes no more frames, and the reply's own resolution was made before
    // the app was ever called.
    const int fd = live_fd(to);
    if (fd < 0) {
        tx_to_dead_peer_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The server's own verdict on the socket the link just vouched for — @ref tx_work's
    // second opinion, kept here so an in-call reply is judged by the same two tests a
    // queued frame is. A skip is not a send result: it feeds the cumulative tally, never
    // the streak (@ref note_tx_skip).
    if (httpd_ws_get_fd_info(h, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        note_tx_skip(to);
        return;
    }
    std::size_t total = 0;
    for (const auto& part : iov) total += part.size();
    // Teardown gate, the one @ref stage applies to every other send: once the dtor is
    // draining, nothing new goes out.
    if (stopping_.load(std::memory_order_relaxed)) {
        note_enqueue_drop(fd, total);
        return;
    }
    // One span is the reply's ordinary shape, and it needs NO buffer of ours at all: the
    // caller's memory is alive for this whole call (it is the caller's stack frame, one
    // level up), which is exactly what the asynchronous path could not assume and had to
    // gather-copy to escape. So the common reply now costs zero copies and zero slots.
    std::span<const std::byte> payload = iov.empty() ? std::span<const std::byte>() : iov[0];
    tx_block_t* big = nullptr;
    if (iov.size() > 1) {
        // A rope reply still has to reach the wire as ONE WebSocket frame, so it is gathered
        // — into the link's one reply scratch, never into a temporary of its own (a flatten
        // temp alongside the tx copy is the heap spike behind the on-device OOM abort). Only
        // this task uses it and only inside this call, so it is never contended. A rope past
        // the scratch takes a block, the same make_block a queued frame takes, and exhaustion
        // is the same counted drop.
        std::byte* p = tx_scratch_block_.data();
        if (total > tx_inline_bytes_) {
            big = make_block({}, iov, nullptr);
            if (big == nullptr) {
                note_enqueue_drop(fd, total);
                return;
            }
            payload = {big->payload, total};
        } else {
            payload = {p, total};
            for (const auto& part : iov) {
                if (!part.empty()) std::memcpy(p, part.data(), part.size());
                p += part.size();
            }
        }
    }
    std::size_t on_wire = 0;
    const esp_err_t err = send_now(to.slot, fd, HTTPD_WS_TYPE_BINARY, payload, &on_wire);
    // Released BEFORE the accounting, which takes peers_m_ and may close the session: the
    // block has done its job the instant the send returns.
    drop_block(big);
    // No `httpd_sess_update_lru_counter` here, unlike @ref tx_work: that call exists for a
    // push the adopted server's LRU cannot see, and this send is INSIDE `httpd_sess_process`
    // — the one place IDF advances the counter by itself.
    if (err != ESP_OK && on_wire == 0)
        ESP_LOGW(kTag, "ws reply failed (%s) fd=%d len=%u - frame dropped", esp_err_to_name(err),
                 fd, (unsigned)total);
    // Charged to the current drain (ADR-0085 §7): an in-call reply is written by the
    // receive context, on the httpd task, so this is the one writer the drain accounts.
    if (err == ESP_OK) rx_drain_tx_bytes_ += total;
    note_tx_result(to, err == ESP_OK, total);
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
    s.tx_conflated = tx_conflated_.load(std::memory_order_relaxed);
    s.tx_stall_strikes = tx_stall_strikes_.load(std::memory_order_relaxed);
    s.tx_large_dropped = tx_large_dropped_.load(std::memory_order_relaxed);
    s.tx_large_peak = tx_large_peak_.load(std::memory_order_relaxed);
    s.tx_to_dead_peer = tx_to_dead_peer_.load(std::memory_order_relaxed);
    s.tx_send_failed = tx_send_failed_.load(std::memory_order_relaxed);
    s.peers_refused = peers_refused_.load(std::memory_order_relaxed);
    s.sessions_condemned = sessions_condemned_.load(std::memory_order_relaxed);
    s.peers_frozen = peers_frozen_.load(std::memory_order_relaxed);
    s.rx_dropped_oversize = rx_dropped_oversize_.load(std::memory_order_relaxed);
    s.rx_dropped_alloc = rx_dropped_alloc_.load(std::memory_order_relaxed);
    s.rx_dropped_pool = rx_dropped_pool_.load(std::memory_order_relaxed);
    s.auth_rejected = auth_rejected_.load(std::memory_order_relaxed);
    s.auth_expired = auth_expired_.load(std::memory_order_relaxed);
    s.rx_drain_waits = rx_drain_waits_.load(std::memory_order_relaxed);
    return s;
}

std::size_t httpd_ws_link_t::rx_scratch_bytes() const noexcept { return rx_scratch_bytes_; }

std::size_t httpd_ws_link_t::tx_inline_bytes() const noexcept { return tx_inline_bytes_; }

std::size_t httpd_ws_link_t::tx_large_bytes() const noexcept { return tx_large_bytes_; }

std::size_t httpd_ws_link_t::tx_large_slot_capacity() const noexcept { return tx_large_slots_; }

std::size_t httpd_ws_link_t::tx_large_in_use() const noexcept {
    if (tx_large_busy_ == nullptr) return 0;
    std::size_t held = 0;
    for (std::size_t i = 0; i < tx_large_slots_; ++i)
        if (tx_large_busy_[i].load(std::memory_order_relaxed)) ++held;
    return held;
}

std::size_t httpd_ws_link_t::buffer_bytes() const noexcept {
    // What was actually allocated, not what was asked for: a large class that was rejected
    // or could not be allocated has had BOTH its numbers zeroed, so its term vanishes from
    // this sum without a test of its own. The frames in flight are not here: each send draws
    // one shared block that every peer references (@ref peer_tx_bytes).
    return rx_scratch_bytes_ + tx_scratch_block_.size() + tx_large_slots_ * large_stride();
}

std::size_t httpd_ws_link_t::peer_tx_bytes() const noexcept {
    return sizeof(session_t::tx) + sizeof(session_t::tx_head) + sizeof(session_t::tx_tail) +
           sizeof(session_t::tx_spare) + sizeof(session_t::tx_spare_used) +
           sizeof(session_t::tx_stalls);
}

void httpd_ws_link_t::note_enqueue_drop(int fd, std::size_t bytes) {
    const std::uint32_t total = enqueue_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
    ESP_LOGW(kTag,
             "tx enqueue drop (queue refused / large class spent / OOM) fd=%d len=%u total=%u", fd,
             (unsigned)bytes, (unsigned)total);
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
    // entry ahead of it costs a full send bound on a stalled socket. On silicon the close
    // was asked for repeatedly and never took effect; the fd kept failing for two minutes.
    //
    // The FIFO argument is the WHOLE of it: this component's floor is `idf: ">=5.5.5"`
    // (#949), where `httpd_queue_work` reserves its mbox slot through a counting semaphore
    // before the `sendto`, so a full queue is an ESP_FAIL the caller sees and an ESP_OK
    // means the item will run. The older reading — an ESP_OK that lies, because the bare
    // non-blocking `sendto` overflowed into lwIP's silent bin — describes the PRE-floor
    // server only, and an audit of #1494 that took it as current derived a permanently
    // stranded TX slot from a mechanism this component cannot reach. `shutdown` is the
    // right answer for the three reasons below, and not for that one.
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

void httpd_ws_link_t::note_tx_result(const session_ref_t& to, bool sent, std::size_t bytes,
                                     bool stalled) {
    // The link-wide tally first, before any test of the destination: a frame the socket
    // write refused is a frame the peer never got whether or not its session is still here
    // to carry the streak. Until this counter existed the only trace was the WARN line.
    if (!sent) tx_send_failed_.fetch_add(1, std::memory_order_relaxed);
    bool close_now = false;
    slot_name_t peer;
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
#ifdef CONFIG_LIBTRACER_LINK_TRAFFIC_STATS
            ++slot->st.tx_frames;
            slot->st.tx_bytes += static_cast<std::uint32_t>(bytes);
#endif
            slot->tx_drops = 0;  // a failure streak is CONSECUTIVE — any success resets it
            return;
        }
        // Two counts, two questions. The cumulative one below answers "did anything get
        // lost toward this peer?"; the streak byte answers "is this peer broken RIGHT
        // NOW?" and its #835 3-strike semantics are untouched by the addition.
        ++slot->st.tx_drops;
        if (slot->tx_drops < kMaxConsecutiveTxDrops) ++slot->tx_drops;
        // A peer whose socket took nothing through kTxStallStrikes replacements is the
        // streak spent in one verdict (#2095): the evidence is already that many frames deep.
        if (stalled) slot->tx_drops = kMaxConsecutiveTxDrops;
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
    // BOTH names, because after #994 neither one alone identifies the peer: `p<slot>~<gen>` is
    // what an operator can address the session by, and `<ip>:<port>` is what tells them
    // which physical client that slot is. Both come from the slot, filled at admission.
    // Never `getpeername` here: the socket is by definition the one that is not working,
    // and naming a peer at the moment it is being struck must not depend on that socket
    // answering — the reason the address is STORED rather than recomputed.
    if (stalled) {
        tx_stall_strikes_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(kTag, "%u frames replaced, socket took none peer=%s (%s) fd=%d - closing session",
                 (unsigned)kTxStallStrikes, peer.c_str(), addr, fd);
    } else {
        ESP_LOGW(kTag, "%u consecutive failed sends peer=%s (%s) fd=%d len=%u - closing session",
                 (unsigned)kMaxConsecutiveTxDrops, peer.c_str(), addr, fd, (unsigned)bytes);
    }
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
    // A held frame's flush writes WITHOUT WAITING (#2095): the httpd task serves every peer,
    // so a write that waited on this one's window would set every other peer's rate.
    const bool nonblock = slot != nullptr && slot->tx_frame_open && slot->tx_nonblock;
    const int ret = static_cast<int>(::send(fd, buf, len, nonblock ? flags | MSG_DONTWAIT : flags));
    if (nonblock && (ret >= 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        // A full socket, or room for only part of this buffer, is not a fault here: the
        // bracket keeps the count of what went out, the failure return stops IDF before it
        // writes the next part behind a partial one, and the link writes the rest on the
        // session's next flush (write_rest). A hard error falls through to the verdicts
        // below, exactly as a bounded write's does.
        if (ret > 0) slot->tx_frame_bytes += static_cast<std::size_t>(ret);
        if (ret >= 0 && static_cast<std::size_t>(ret) == len) return ret;
        slot->tx_frame_blocked = true;
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
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
    slot_name_t peer;
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
    // The control-queue item, and the ONE kind of TX work there is (#2095): "flush this
    // link". It carries the GATE, never a frame or a session, so an item that drains after
    // the link is gone finds a null link and does nothing — which is what lets the
    // destructor neither wait for TX nor leak a pool. It registers on the gate's depth while
    // it runs, so a concurrent destructor joins it instead of freeing the sessions under it.
    auto* const gate = static_cast<gate_t*>(arg);
    if (gate == nullptr) return;
    httpd_ws_link_t* owner = nullptr;
    {
        const std::lock_guard lock(gate->m);
        owner = gate->link;
        if (owner == nullptr) return;
        ++gate->depth;
    }
    // Cleared BEFORE the walk: a frame stored after the walk has passed its session sees the
    // flag clear and posts the next flush itself (post_flush).
    owner->flush_posted_.store(false, std::memory_order_seq_cst);
    (void)owner->flush_egress();
    {
        const std::lock_guard lock(gate->m);
        --gate->depth;
    }
    gate->cv.notify_all();
}

bool httpd_ws_link_t::flush_egress() {
    // Runs on the httpd task only — from tx_work, from a drain parked for idle (pace_rx), or
    // from the handler entry when a frame is held (ws_handler), where nothing of this task's
    // is mid-write. Every session is offered its turn; one whose socket is full returns at
    // once, so no peer's socket is waited on here and the slowest peer sets nobody's rate but
    // its own. The same fixed on-stack chunk and resumable scan the broadcast walks
    // (kFanoutChunk), for the same reasons.
    tx_held_.store(false, std::memory_order_relaxed);
    session_t* batch[kFanoutChunk];
    std::size_t next = 0;
    bool progress = false;
    for (bool more = true; more;) {
        std::size_t n = 0;
        {
            const std::lock_guard lock(peers_m_);
            while (next < slots_.size() && n < kFanoutChunk) batch[n++] = slots_[next++];
            more = next < slots_.size();
        }
        for (std::size_t i = 0; i < n; ++i) progress = flush_peer(batch[i]) || progress;
    }
    return progress;
}

bool httpd_ws_link_t::flush_peer(session_t* s) {
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    bool progress = false;
    // The frame on the wire, then at most one newer: a peer whose producer outpaces its
    // socket would otherwise keep this task on it for as long as the socket keeps up.
    for (int turn = 0; turn < 2; ++turn) {
        tx_frame_t* const f = &s->tx;
        tx_frame_t superseded;
        bool stalled = false;
        bool frozen = false;
        {
            // The oldest waiting frame moves onto the wire when the wire is free. A wire frame
            // not one byte of which has gone out yet is no more owed to the peer than a
            // waiting one, so a newer SAMPLE of its path takes its place and it is counted the
            // same way: a stalled peer that recovers gets the NEWEST value of a path, not the
            // one it stalled on. Never a frame of another path, and never a started one
            // (#2195). Under tx_m, the one moment a producer and this task touch the same
            // frames.
            const std::lock_guard lock(s->tx_m);
            for (tx_wait_t *w = s->tx_head, *prev = nullptr; w != nullptr; prev = w, w = w->next) {
                if (f->held() && (f->sent != 0 || !f->conflates(w->frame))) continue;
                (prev != nullptr ? prev->next : s->tx_head) = w->next;
                if (s->tx_tail == w) s->tx_tail = prev;
                superseded = std::exchange(*f, std::exchange(w->frame, tx_frame_t{}));
                free_waiting(*s, w);
                break;
            }
            if (!f->held()) return progress;
            stalled = s->tx_stalls >= kTxStallStrikes;
            frozen = std::exchange(s->tx_frozen, false);
        }
        if (superseded.held()) note_tx_conflated(session_ref_t{s, superseded.gen});
        release_frame(superseded);
        // The frame names the session it was staged for (#954), and the server's own verdict
        // is the second opinion. A skip is not a send result: it feeds the cumulative tally,
        // never the streak (note_tx_skip).
        const session_ref_t to{s, f->gen};
        const int fd = live_fd(to);
        if (h == nullptr || fd < 0 || httpd_ws_get_fd_info(h, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            release_frame(*f);
            note_tx_skip(to);
            progress = true;
            continue;
        }
        if (frozen) {
            // The application's liveness check found this peer owing bytes and taking none
            // past its deadline (#2198): condemned here, on the task that owns the fd.
            close_frozen(to);
            return true;
        }
        if (stalled) {
            // kTxStallStrikes large frames were refused while this peer pinned the class and
            // its socket took nothing: the peer is not draining, and a full socket under
            // MSG_DONTWAIT is never an error, so without this it would hold its seat and its slot
            // for ever. Through the strike path, as a peer whose writes fail is (note_tx_result).
            const std::size_t len = f->blk->total();
            release_frame(*f);
            note_tx_result(to, false, len, /*stalled=*/true);
            progress = true;
            continue;
        }
        const std::uint32_t before = f->sent;
        const tx_outcome_t out = write_frame(h, fd, *s, *f, /*nonblock=*/true);
        const bool moved = f->sent != before;
        {
            // What the stall count is about: a write that moved a byte clears it, one that moved
            // none arms the next refused large draw to count (note_large_pinned). A full queue
            // never counts: it is the peer's pace, and a burst fills it on a healthy socket.
            const std::lock_guard lock(s->tx_m);
            s->tx_moved = s->tx_moved || moved;
            if (moved || out != tx_outcome_t::HELD) {
                s->tx_stalls = 0;
                s->tx_refused = false;
            } else {
                s->tx_refused = true;
            }
        }
        if (out == tx_outcome_t::HELD) {
            // The socket is full: the frame stays where it is. ESP-IDF's server selects for
            // READ only, so there is no writable event to wait on; what moves a held frame on
            // is this link's own events, and no timer or clock read: a socket that took bytes
            // now is offered the rest on the next pass (the re-post below); otherwise the next
            // frame posted on this link, the next frame any of its peers sends (ws_handler), a
            // parked drain, or the application's flush_held().
            tx_held_.store(true, std::memory_order_relaxed);
            if (moved && h != nullptr) (void)post_flush(h);
            return progress || moved;
        }
        if (out == tx_outcome_t::SENT && !owns_httpd_) {
            // Tell an ADOPTED server this session is not idle (#955): IDF advances a session's
            // LRU counter only from inbound request processing, so a pure subscriber would
            // age toward being the purge victim however much this link pushes at it. Not
            // immunity; owning mode starts its server with the purge off.
            (void)httpd_sess_update_lru_counter(h, fd);
        }
        const std::size_t len = f->blk->total();
        if (out == tx_outcome_t::FAILED && f->sent == 0) {
            // DROP the frame, keep the session (#481): one refused write loses the peer one
            // frame it can recover from; three in a row condemn it (note_tx_result, #835).
            // A frame that failed part-way is the other fault (#951): send_guarded has
            // already condemned the session and logged it, so it is not reported twice.
            ESP_LOGW(kTag, "ws send failed fd=%d len=%u - frame dropped", fd, (unsigned)len);
        }
        release_frame(*f);
        note_tx_result(to, out == tx_outcome_t::SENT, len);
        progress = true;
    }
    // Two frames out and a third may be waiting: the other peers take their turn first, and
    // a re-posted flush brings this one back.
    bool more = false;
    {
        const std::lock_guard lock(s->tx_m);
        more = s->tx_head != nullptr;
    }
    if (more && h != nullptr) (void)post_flush(h);
    return progress;
}

std::size_t httpd_ws_link_t::close_frozen_peers(std::uint64_t now, std::uint64_t deadline) {
    // The verdict here, under each session's tx_m (never with peers_m_ under it); the close
    // on the httpd task, through one posted flush. No clock is read: `now` is the caller's.
    //
    // The same call bounds the auth handshake, the RX a session owes before it is served
    // (#2257): every unauthenticated session and silent upgrade is stamped with the first `now`
    // that sees it, and one recorded before @p deadline is closed with `kCloseAuthTimeout` by
    // the sweep this posts to the httpd task. Never extended by the peer's own traffic.
    session_t* batch[kFanoutChunk];
    std::size_t next = 0;
    std::size_t frozen = 0;
    std::size_t expired = 0;
    {
        const std::lock_guard lock(peers_m_);
        for (std::size_t i = 0; i < pending_n_; ++i) {
            pending_[i].since = std::min(pending_[i].since, now);
            expired += pending_[i].since < deadline;
        }
    }
    for (bool more = true; more;) {
        std::size_t n = 0;
        {
            const std::lock_guard lock(peers_m_);
            while (next < slots_.size() && n < kFanoutChunk) {
                session_t* const s = slots_[next++];
                s->auth_since = std::min(s->auth_since, now);
                expired += s->open && !s->dead && s->auth_pending && s->auth_since < deadline;
                batch[n++] = s;
            }
            more = next < slots_.size();
        }
        for (std::size_t i = 0; i < n; ++i) {
            session_t* const s = batch[i];
            const std::lock_guard lock(s->tx_m);
            const bool moved = std::exchange(s->tx_moved, false);
            const bool owed = s->tx.held() || s->tx_head != nullptr;
            if (!judge_frozen(!moved && owed, s->live_since, now, deadline)) continue;
            s->tx_frozen = true;
            s->live_since = now;
            ++frozen;
        }
    }
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    if (frozen != 0 && h != nullptr) (void)post_flush(h);
    auth_cutoff_.store(deadline, std::memory_order_relaxed);
    if (expired != 0 && h != nullptr)
        (void)httpd_queue_work(h, &httpd_ws_link_t::auth_sweep_work,
                               gate_.load(std::memory_order_relaxed));
    return frozen + expired;
}

void httpd_ws_link_t::close_frozen(const session_ref_t& to) {
    int fd = -1;
    {
        // The identity test live_fd makes, and the dead mark note_tx_result sets under the
        // same lock, so the fan-out stops feeding a peer already condemned.
        const std::lock_guard lock(peers_m_);
        session_t* const slot = to.slot;
        if (slot == nullptr || slot->gen != to.gen || !slot->open || slot->dead) return;
        slot->dead = true;
        fd = slot->fd;
    }
    peers_frozen_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW(kTag, "peer owed bytes past the application's deadline fd=%d - closing session", fd);
    condemn(fd);
}

void httpd_ws_link_t::flush_held() {
    // The application's own tick, for a link that has gone quiet with a frame still held on a
    // full socket: nothing else would offer it the socket again. No clock is read here; the
    // caller's cadence is the whole policy. One flush, as any post makes.
    if (!tx_held_.load(std::memory_order_relaxed)) return;
    const httpd_handle_t h = handle_.load(std::memory_order_relaxed);
    if (h != nullptr) (void)post_flush(h);
}

httpd_ws_link_t::tx_outcome_t httpd_ws_link_t::write_frame(httpd_handle_t h, int fd, session_t& s,
                                                           tx_frame_t& f, bool nonblock) {
    // Bracket the writes: everything the override lands between these two calls belongs to
    // THIS frame, which is what lets it tell a frame that never started from one it
    // announced and could not finish (#951), and — non-blocking — a full socket from a
    // broken one (#2095). Only the caller knows that boundary.
    s.open_tx_frame(nonblock, f.sent);
    esp_err_t err = ESP_OK;
    const tx_block_t& b = *f.blk;
    if (f.sent == 0 && !b.value) {
        httpd_ws_frame_t wf = {};
        wf.final = true;
        wf.fragmented = false;
        wf.type = HTTPD_WS_TYPE_BINARY;
        wf.payload = reinterpret_cast<std::uint8_t*>(b.payload);
        wf.len = b.len;
        err = httpd_ws_send_frame_async(h, fd, &wf);
    } else {
        err = write_rest(h, fd, f);
    }
    const bool blocked = s.tx_frame_blocked;
    f.sent = static_cast<std::uint32_t>(s.close_tx_frame());
    if (err == ESP_OK) return tx_outcome_t::SENT;
    return blocked ? tx_outcome_t::HELD : tx_outcome_t::FAILED;
}

esp_err_t httpd_ws_link_t::write_rest(httpd_handle_t h, int fd, const tx_frame_t& f) {
    // The frame as one byte sequence — its WebSocket header, its gathered bytes, then a
    // retained value's links straight from the published block — written from `f.sent` on.
    // A gathered frame's header is the one httpd_ws_send_frame_async would have written
    // (encode_ws_header); a retained frame carries its own in `payload`. Every write goes
    // through the session's send override inside write_frame's bracket, so a part the socket
    // takes only some of ends this call with the rest still owed.
    const tx_block_t& b = *f.blk;
    std::array<std::byte, kWsHeaderMax> hdr{};
    const std::size_t hdr_len = b.value ? 0 : encode_ws_header(b.len, hdr);
    std::size_t skip = f.sent;
    const auto write_part = [&](const std::byte* p, std::size_t n) {
        if (skip >= n) {
            skip -= n;
            return true;
        }
        p += skip;
        n -= skip;
        skip = 0;
        const int ret = httpd_socket_send(h, fd, reinterpret_cast<const char*>(p), n, 0);
        return ret >= 0 && static_cast<std::size_t>(ret) == n;
    };
    if (!write_part(hdr.data(), hdr_len) || !write_part(b.payload, b.len)) return ESP_FAIL;
    if (b.value)
        for (const view::view_t& l : b.value->links())
            if (!write_part(l.bytes().data(), l.bytes().size())) return ESP_FAIL;
    return ESP_OK;
}

bool httpd_ws_link_t::finish_wire(session_t* slot, int fd) {
    // A frame a non-blocking flush left part-way on this socket must be finished before any
    // other frame is written to it, or the peer parses the new frame's bytes as the rest of
    // the old one. Called on the httpd task by send_now, ahead of an in-call reply, a close
    // or an auth frame, so the rest is written within the send bound like the frame it
    // precedes: that is THIS peer's own request being served, so the wait is its own. A frame
    // not yet started is no hazard and stays held.
    tx_frame_t& f = slot->tx;
    if (!f.held() || f.sent == 0) return true;
    const session_ref_t to{slot, f.gen};
    const tx_outcome_t out =
        write_frame(handle_.load(std::memory_order_relaxed), fd, *slot, f, /*nonblock=*/false);
    const std::size_t len = f.blk->total();
    release_frame(f);
    note_tx_result(to, out == tx_outcome_t::SENT, len);
    return out == tx_outcome_t::SENT;
}

httpd_ws_link_t::tx_block_t* httpd_ws_link_t::make_retained(
    std::span<const std::span<const std::byte>> head, const graph::value_t& value) {
    // The server frame header, encoded here because the retained frame writes its own: the
    // block holds header and head, and the value's links follow them on the wire straight
    // from the published block, by reference.
    std::size_t head_len = 0;
    for (const auto& part : head) head_len += part.size();
    std::array<std::byte, kWsHeaderMax> ws_hdr{};
    const std::size_t ws_len = encode_ws_header(head_len + value.total_length(), ws_hdr);
    return make_block(std::span<const std::byte>(ws_hdr.data(), ws_len), head, &value);
}

void httpd_ws_link_t::queue_send_retained(const session_ref_t& to,
                                          std::span<const std::span<const std::byte>> head,
                                          const graph::value_t& value,
                                          const graph::delivery_lane_t& lane) {
    tx_block_t* const blk = make_retained(head, value);
    if (stage(to, blk, value.total_length(), lane)) ring_doorbell(blk, value.total_length());
    drop_block(blk);
}

void httpd_ws_link_t::send(std::span<const std::span<const std::byte>> head,
                           const graph::value_t& value, const graph::delivery_lane_t& lane) {
    // The broadcast's fan-out with the retained block in place of the gathered one, waiting
    // in each peer's queue by its lane: per path, never across paths (#2195).
    fan_out([&] { return make_retained(head, value); }, value.total_length(), lane);
}

void httpd_ws_link_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void httpd_ws_link_t::send(std::span<const std::span<const std::byte>> iov) {
    // A broadcast: the iovec gathered ONCE for every peer, straight into the shared block
    // (see fan_out). Overriding the iovec entry point is what keeps the base default's
    // gather-into-a-temporary away: it would double-buffer a large reply (flatten temp + tx
    // copy live simultaneously), the heap spike behind the on-device OOM abort.
    std::size_t total = 0;
    for (const auto& part : iov) total += part.size();
    fan_out([&] { return make_block({}, iov, nullptr); }, total, graph::delivery_lane_t{});
}

void httpd_ws_link_t::fan_out(function_ref_t<tx_block_t*()> build, std::size_t total,
                              const graph::delivery_lane_t& lane) {
    // Broadcast: snapshot the destinations under the lock, then enqueue unlocked — the
    // per-session drop accounting inside stage takes peers_m_ itself — and ring the
    // doorbell ONCE, after every destination's frame is stored (#2146).
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
    tx_block_t* blk = nullptr;
    bool built = false;
    bool staged = false;
    for (bool more = true; more;) {
        std::size_t n = 0;
        {
            const std::lock_guard lock(peers_m_);
            // A condemned peer is skipped from the lock the snapshot already holds, so the
            // fan-out never even offers it a frame (queue_send would refuse it anyway).
            while (next < slots_.size() && n < kFanoutChunk) {
                session_t* const s = slots_[next++];
                // `!auth_pending` (#1184): a broadcast is how a subscription push reaches its
                // peers, so including an unauthenticated session would leak vertex VALUES to
                // a peer that has presented no credential — the most direct possible defeat
                // of the gate, and the one that needs no lookup at all.
                if (s->open && !s->dead && !s->auth_pending)
                    targets[n++] = session_ref_t{s, s->gen};
            }
            more = next < slots_.size();
        }
        // ONE block for the whole fan-out (#2095), gathered on the first destination: every
        // peer's slot takes a reference, so N peers cost one copy and one block.
        if (n != 0 && !built) {
            blk = build();
            built = true;
        }
        for (std::size_t i = 0; i < n; ++i) staged = stage(targets[i], blk, total, lane) || staged;
    }
    if (staged) ring_doorbell(blk, total);
    drop_block(blk);
}

tr::net::transport_drop_stats_t httpd_ws_link_t::peer_resolution_t::drop_stats() const noexcept {
    // ONE load into a local, for the reason `send` records (#963): a teardown may be writing
    // this member while a reader is here. A null owner is a retired handle — no link, no
    // counters, and all-zero is then the base's honest default rather than a fabrication.
    const httpd_ws_link_t* const owner = owner_;
    if (owner == nullptr) return {};
    return owner->drop_stats();
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
    // A reply issued from inside this link's own handler frame is already on the httpd
    // task: it writes straight to the socket and never touches the pool or the control
    // queue (#1494 — @ref send_in_call carries the whole argument). Every other caller —
    // a producer task pushing a subscription value, a timer, the app on a thread of its
    // own — takes the queue, which is what the queue is for.
    if (t_serving_link == owner)
        owner->send_in_call(to, iov);
    else
        owner->queue_send(to, iov);
}

void httpd_ws_link_t::peer_resolution_t::send(std::span<const std::span<const std::byte>> head,
                                              const graph::value_t& value,
                                              const graph::delivery_lane_t& lane) {
    // The directed twin of the retained broadcast, with the identity test of the iovec
    // override above (#1013 / #954) — see there.
    httpd_ws_link_t* const owner = owner_;
    if (owner == nullptr) return;
    session_ref_t to;
    {
        const std::lock_guard lock(owner->peers_m_);
        session_t* const slot = slot_;
        if (slot == nullptr || !slot->open || slot->dead || slot->gen != gen_) {
            owner->tx_to_dead_peer_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        to = session_ref_t{slot, gen_};
    }
    if (t_serving_link == owner) {
        // In-call (#1494): the socket write happens now, from memory alive for the call, so
        // there is nothing to retain — the gathered in-call path, over head ++ links.
        std::array<std::span<const std::byte>, 8> iov;
        if (head.size() + value.link_count() > iov.size()) {
            owner->note_enqueue_drop(owner->live_fd(to), value.total_length());
            return;
        }
        std::size_t n = 0;
        for (const auto& part : head) iov[n++] = part;
        for (const view::view_t& l : value.links()) iov[n++] = l.bytes();
        owner->send_in_call(to, std::span<const std::span<const std::byte>>(iov.data(), n));
        return;
    }
    owner->queue_send_retained(to, head, value, lane);
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
    for (const session_t* const s : slots_)
        if (s->open && !s->dead && !s->auth_pending && !s->name.empty()) visit(s->name.view());
}

void httpd_ws_link_t::enumerate_peer_stats(const peer_stats_visitor_t& visit) const {
    const std::lock_guard lock(peers_m_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const session_t* const s = slots_[i];
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
        visit(peer_stats_t{s->name.view(), i, s->gen, s->st, s->endpoint_str,
                           std::string_view(s->subject, s->subject_len)});
    }
}

transport_t* httpd_ws_link_t::peer_link(std::string_view peer) {
    const std::lock_guard lock(peers_m_);
    // The exact compare IS the tenancy check: a name carrying a departed
    // session's generation, or none at all, equals no live slot's name and is refused.
    // `!dead` for the reason on enumerate_peers (#963): the endpoint this used to hand
    // out for a condemned slot was a guaranteed no-op, so a router lookup succeeded and
    // every frame through it was silently discarded. Refusing here turns that into an
    // honest "no such peer" the caller can act on.
    // `!auth_pending` (#1184): resolving an unauthenticated session would hand the routing
    // plane a working endpoint for a peer that has presented nothing, which is the whole
    // gate defeated by one lookup — a directed FWD reply does not consult the census.
    for (session_t* const s : slots_)
        if (s->open && !s->dead && !s->auth_pending && s->name.view() == peer)
            return acquire_resolution(s);
    return nullptr;
}

std::string_view httpd_ws_link_t::peer_name(peer_handle_t peer, std::span<char> scratch) const {
    // The name a slot is stamped with at the claim IS `p<slot>~<gen>` of its handle
    // (ADR-0073 §2), so the inverse is that formatting and needs neither
    // `peers_m_` nor the slot vector. The SAME formatter the claim stamps with (#1880), into
    // a local name and then the CALLER's scratch, so it allocates nothing on the per-frame
    // path.
    if (!peer.valid() || scratch.size() < 2) return {};
    slot_name_t name;
    name.len = format_slot_name(peer, name.text.data());
    if (name.len > scratch.size()) return {};
    std::copy_n(name.text.data(), name.len, scratch.data());
    return std::string_view(scratch.data(), name.len);
}

std::string_view httpd_ws_link_t::peer_subject(peer_handle_t peer, std::span<char> scratch) const {
    (void)scratch;
    // No lock: the claim, the ACCEPT and the reclaim that write these fields all run on the
    // httpd task, which is also the task delivering the frame this is asked about. The
    // generation compare is the tag (#1846): a handle of a departed session names a slot whose
    // generation has moved on, and gets no subject even if the slot's new session has one.
    if (!peer.valid() || peer.index >= slots_.size()) return {};
    const session_t* const s = slots_[peer.index];
    if (!s->open || s->auth_pending || s->gen != peer.generation) return {};
    return std::string_view(s->subject, s->subject_len);
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
        // Same shape as the slot claim's own growth: one small object per peer past the
        // high-water mark, never per frame, from `memory.state` (#1880). A refusal is the
        // honest "no such peer" below.
        got = mem::make_in<peer_resolution_t>(*state_src_);
        if (got != nullptr && !resolutions_.push_back(got)) {
            mem::drop_in(*state_src_, got);
            got = nullptr;
        }
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
            if (!s->open || s->dead || s->auth_pending || s->name.view() != peer) continue;
            // Mint the identity HERE, under the lock that resolved it (#954). The close runs
            // on the httpd task an arbitrary time later, by which point this slot may have
            // been reclaimed and re-earned the very same positional name — `p3` is a pure
            // function of the slot index, so the NAME cannot survive as the token. The
            // generation can.
            to = session_ref_t{s, s->gen};
            break;
        }
    }
    if (to.slot == nullptr) return false;  // no served session answers to that name
    // From `memory.state`, failable, one per call: this is an administrative action, not a
    // data-path one, so it does not draw on the frame path's storage. A revocation competing
    // with the fan-out for TX memory would be exactly backwards — it is most needed when the
    // frame path is under pressure.
    auto* const req = mem::make_in<close_req_t>(*g->src, g, to);
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
        mem::drop_in(*g->src, req);
        return false;  // refused: nothing was initiated, and the caller may retry
    }
    return true;
}

void httpd_ws_link_t::close_work(void* req_arg) {
    // Runs ON the httpd task (httpd_queue_work's whole contract) — the task that owns
    // accept, close and therefore the descriptor's lifetime, which is what makes the
    // `shutdown` inside close_session safe here and unsafe from the caller's task (#954's
    // recycled-fd hazard, and the precondition condemn() documents).
    auto* const raw = static_cast<close_req_t*>(req_arg);
    const mem::block_ptr_t<close_req_t> req(*raw->gate->src, raw);
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
