/**
 * @file
 * @brief Implementation of @ref tr::net::esp_ws_client_link_t — the ESP-IDF
 *        `esp_transport_ws`-backed WebSocket *client* `transport_t`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer_esp/esp_ws_client_link.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include "esp_log.h"
#include "esp_thread.hpp"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"

namespace tr::net {

namespace {
/** @brief Log tag for this link. */
constexpr const char* kTag = "ws_client_link";

/**
 * @brief The task-watchdog period, seconds — the numerator every blocking bound on
 *        this link is derived from.
 *
 * Not a number of ours: `CONFIG_ESP_TASK_WDT_TIMEOUT_S` is the system's own normative
 * statement of how long a task may go unfed. IDF defines it through `sdkconfig.h`
 * (pulled in by FreeRTOS.h); the fallback is IDF's own Kconfig default for that symbol,
 * for a build with the task watchdog compiled out and for the host test build, so the
 * derivation has one provenance on every target. `httpd_ws_link.cpp` (#835) and
 * `twai_link.cpp` (#962) read the same symbol for their own bounds — the formulas
 * differ because the plane each one bounds differs, so this is a shared FACT, never a
 * shared constant.
 */
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
constexpr std::uint32_t kTaskWdtSeconds = CONFIG_ESP_TASK_WDT_TIMEOUT_S;
#else
constexpr std::uint32_t kTaskWdtSeconds = 5;
#endif

/** @brief That period in milliseconds. A zero (the symbol defined but the watchdog
 *  disabled) takes IDF's Kconfig default instead, so no bound below derives to 0 —
 *  which would mean "give up instantly", not "wait forever". */
constexpr int kWatchdogMs = static_cast<int>((kTaskWdtSeconds != 0 ? kTaskWdtSeconds : 5) * 1000U);

/**
 * @brief The teardown budget this link's two blocking legs are cut from.
 *
 * A destroying task's worst case is ONE dial (the recv thread is parked in
 * `esp_transport_connect`, and the destructor joins it) plus ONE full write bound (the
 * recv thread's read queues on `write_m_` behind a sender, or the destructor's own
 * handle teardown does). That sum must fit inside a single watchdog window, so the
 * window is split: half to the dial, a quarter to the write, a quarter left over for
 * whatever else the destroying task is holding. Nothing else in the loop is unbounded —
 * the backoff wakes on `stop_` and the poll turn is @ref kPollMs.
 */
constexpr int kDialTimeoutMs = kWatchdogMs / 2;

/**
 * @brief How many times IDF spends the write timeout inside ONE `esp_transport_write`.
 *
 * `_ws_write` (components/tcp_transport/transport_ws.c) passes the caller's timeout to
 * `esp_transport_poll_write`, then again to the parent's write for the WS header, then
 * again for the payload — so the caller's number is a per-leg bound, never a total. The
 * old 4000 ms literal was therefore a 12 s stall on a peer with a closed TCP window,
 * against a 5 s watchdog (#952).
 */
constexpr int kIdfWriteLegs = 3;

/** @brief What ONE `send()` may spend in total, all legs (ms) — a quarter of the
 *  teardown budget above. */
constexpr int kWriteBudgetMs = kWatchdogMs / 4;

/** @brief Write timeout handed to IDF (ms): the total budget divided by the legs IDF
 *  spends it on, so `kIdfWriteLegs * kWriteTimeoutMs == kWriteBudgetMs` is what a
 *  stalled peer can actually cost a sending task. Still several times the read timeout,
 *  so momentary TX congestion does not tear the connection down (a 100 ms write timeout
 *  thrashes) — it just stops being unbounded in practice. */
constexpr int kWriteTimeoutMs = kWriteBudgetMs / kIdfWriteLegs;

/**
 * @brief Read timeout after a positive poll (ms) — data is already buffered, so short.
 *
 * POLICY, not a derivation, so it is a Kconfig knob
 * (`CONFIG_LIBTRACER_WS_CLIENT_READ_TIMEOUT_MS`, #1160): it says how long the recv thread
 * is willing to wait for bytes the poll already promised, which is a property of the
 * network the node sits on and not of anything this component can compute. Per-IMAGE and
 * not per-link on purpose — it is the recv loop's cadence, shared by every client link in
 * the image, and there is no per-link RAM behind it to trade. The fallback is the
 * historical literal, so a build without the symbol (the host suite) compiles the value
 * this link always had.
 */
#ifdef CONFIG_LIBTRACER_WS_CLIENT_READ_TIMEOUT_MS
constexpr int kReadTimeoutMs = CONFIG_LIBTRACER_WS_CLIENT_READ_TIMEOUT_MS;
#else
constexpr int kReadTimeoutMs = 100;
#endif

/**
 * @brief Poll wait per recv-loop turn (ms) — bounds how fast a stop is observed.
 *
 * Kconfig for the same reason as @ref kReadTimeoutMs
 * (`CONFIG_LIBTRACER_WS_CLIENT_POLL_MS`, #1160), and it is the knob that trades teardown
 * latency against idle wakeups: the destructor's join cannot complete faster than one
 * turn of this, and a node that wakes its recv thread five times a second is paying for
 * that responsiveness in a power budget only the embedder can see.
 */
#ifdef CONFIG_LIBTRACER_WS_CLIENT_POLL_MS
constexpr int kPollMs = CONFIG_LIBTRACER_WS_CLIENT_POLL_MS;
#else
constexpr int kPollMs = 200;
#endif

/**
 * @brief Backoff before re-dialing after a failed/lost connection (ms) — an upper
 *        bound only: the wait is on a condition variable the destructor signals.
 *
 * Kconfig (`CONFIG_LIBTRACER_WS_CLIENT_RECONNECT_BACKOFF_MS`, #1160), and the most
 * deployment-shaped of the three: it is how hard a node re-dials a peer that is down, so
 * a fleet on a congested link wants it longer and a bench node chasing a reboot wants it
 * shorter. Nothing in this file can decide that.
 */
#ifdef CONFIG_LIBTRACER_WS_CLIENT_RECONNECT_BACKOFF_MS
constexpr int kReconnectBackoffMs = CONFIG_LIBTRACER_WS_CLIENT_RECONNECT_BACKOFF_MS;
#else
constexpr int kReconnectBackoffMs = 1500;
#endif

/**
 * @brief Poll slice for the destructor's sender drain (ms). A sleep, not a spin: a
 *        higher-priority destroying task that only yielded would starve on a unicore chip
 *        exactly the senders it is waiting for.
 *
 * DELIBERATELY FIXED (#1160). One tick is the smallest amount of "let the senders run"
 * the scheduler can express, so this is a floor imposed by the RTOS rather than a number
 * chosen here — raising it only makes teardown coarser and lowering it cannot work. There
 * is no trade for an embedder to make, so there is no knob.
 */
constexpr int kDrainSliceMs = 1;

/**
 * @brief TCP keepalive policy for the dialed connection: idle seconds before the first
 *        probe, seconds between probes, and probes before the stack declares the
 *        connection dead (#957).
 *
 * Not numbers of ours. They are the defaults ESP-IDF documents for `esp_http_server`'s
 * own keepalive (`esp_http_server.h`, `httpd_config_t`: `keep_alive_idle` "Default is 5
 * (second)", `keep_alive_interval` "Default is 5 (second)", `keep_alive_count` "Default
 * is 3 counts"), which is the server this link dials — so the same FACT is stated on
 * both ends of the same connection and a peer's death is declared at the same age from
 * either side. `httpd_ws_link_t::bound_socket` states it for accepted sockets; this
 * states it for dialed ones. A shared fact, never a shared constant.
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
 * @brief RAII half of the in-flight-sender tally the destructor drains (#952).
 *
 * Only the DECREMENT lives here. The raise cannot: it has to happen BEFORE the sender
 * queues on `write_m_`, because the window being closed is precisely "queued on a mutex
 * a returning destructor is about to destroy".
 *
 * The tally is therefore the exact boundary of what teardown covers: a sender whose
 * raise landed before the destructor's last drain load is waited out; one that has not
 * raised it yet is not — whether it has not called `send()` at all, or is between
 * `send()`'s entry checks and the `fetch_add` a few instructions below. That residual is
 * the caller's own lifetime problem, which no barrier inside the object can answer: the
 * raise would have to be atomic with the call, and a member cannot outlive the object it
 * is a member of.
 */
class sender_exit_t {
   public:
    /** @brief Adopt an ALREADY-raised @p tally; lower it on scope exit. */
    explicit sender_exit_t(std::atomic<std::uint32_t>& tally) noexcept : tally_(tally) {}
    /** @brief Lower the tally, releasing everything this sender did to the drain. */
    ~sender_exit_t() { tally_.fetch_sub(1, std::memory_order_release); }

    sender_exit_t(const sender_exit_t&) = delete;
    sender_exit_t& operator=(const sender_exit_t&) = delete;

   private:
    std::atomic<std::uint32_t>& tally_; /**< @brief esp_ws_client_link_t::senders_. */
};

}  // namespace

esp_ws_client_link_t::esp_ws_client_link_t(std::string host, std::uint16_t port,
                                           std::string ws_path, std::string handshake_headers,
                                           std::size_t rx_bytes, std::size_t tx_bytes,
                                           std::size_t recv_stack, bool defer_recv)
    : host_(std::move(host)),
      port_(port),
      ws_path_(std::move(ws_path)),
      handshake_headers_(std::move(handshake_headers)),
      rx_buf_(rx_bytes),
      tx_buf_(tx_bytes),
      armed_(!defer_recv) {
    // Every member the recv thread reads is initialized ABOVE this line, which is the
    // whole of #959: the thread spawned below dials at once, so a knob delivered after the
    // spawn is a data race, and for a handshake token it also leaves it undefined whether
    // the first dial carries one — and a dial WITHOUT one is what an admission hook refuses.
    // `handshake_headers_` was that knob; it is a ctor argument and `const` now, so the
    // ordering is a property of the type rather than a request in a doc comment. Nothing
    // may be added between here and the spawn that the recv thread also reads.
    //
    // The recv thread owns dialing + the read loop; the ctor never blocks on the
    // network. recv_stack==0 uses the pthread default; any other value is APPLIED —
    // this thread runs in-call delivery through the graph's on_write seam, so a node
    // that knows its delivery depth must be able to size it (#900).
    recv_thread_ = esp::spawn_thread(recv_stack, "ws_cli_rx", [this] { recv_loop(); });
}

void esp_ws_client_link_t::start_receiving() {
    // Idempotent, and safe on a link that never connected: `transport_vertex_t::
    // make_connection` arms every link unconditionally once the receiver is installed, so
    // a second call — or one on a link constructed without `defer_recv`, whose latch
    // started set — finds the exchange returning true and does nothing. The latch is
    // never cleared (one-shot): a re-dial only happens on a link that was already armed,
    // so reconnects need no re-gating (ADR-0081, #1102).
    if (armed_.exchange(true, std::memory_order_acq_rel)) return;
    {
        // Taking the latch mutex before notifying is what makes the wake safe against a
        // recv thread that has evaluated the predicate but not yet parked — the same
        // pattern the destructor uses for the backoff wake.
        const std::lock_guard<std::mutex> lk(backoff_m_);
    }
    backoff_cv_.notify_all();
}

esp_ws_client_link_t::~esp_ws_client_link_t() {
    // Disarm the link BEFORE waiting on anything. `stop_` is what every blocking wait
    // in the recv loop is predicated on, and it is also what a sender re-reads after it
    // gets `write_m_` — so from here on no NEW work reaches the handles. Clearing
    // `connected_` makes link_up() honest for the rest of teardown; the recv loop's own
    // stop_ re-check keeps it from reading that as "re-dial" (see recv_loop).
    stop_.store(true, std::memory_order_release);
    connected_.store(false, std::memory_order_release);
    {
        // Taking the backoff mutex before notifying is what makes the wakeup safe
        // against a recv thread that has evaluated the predicate but not yet parked:
        // either it is already waiting (and this notify reaches it), or it evaluates
        // the predicate after this store and never waits at all.
        const std::lock_guard<std::mutex> lk(backoff_m_);
    }
    backoff_cv_.notify_all();
    if (recv_thread_.joinable()) recv_thread_.join();
    {
        // The recv thread has stopped, but it was only ONE of the two handle users:
        // the header's contract is that send() may be called from any task, and the
        // pre-#952 destructor destroyed the handles with `write_m_` held nowhere. A
        // sender inside esp_transport_write holds this lock for the whole call, so
        // acquiring it means no sender is inside — and every sender that acquires it
        // afterwards finds `stop_` set (and `connected_` cleared, both stored above,
        // before this lock was taken) and leaves without reading either handle.
        // esp_transport_ws_init(parent) does NOT take ownership of the parent, so both
        // handles are destroyed here (ws first, then tcp) — mirrors IDF's own teardown.
        const std::lock_guard<std::mutex> lk(write_m_);
        if (ws_ != nullptr) {
            esp_transport_close(ws_);
            esp_transport_destroy(ws_);
            ws_ = nullptr;
        }
        if (tcp_ != nullptr) {
            esp_transport_destroy(tcp_);
            tcp_ = nullptr;
        }
    }
    // Drain the senders that ANNOUNCED themselves on the tally. Holding `write_m_` for
    // this would deadlock — that is the lock they are queued on — so the wait is here,
    // after it. Each one wakes, sees the disarmed link, and leaves; only then is it safe
    // for `write_m_` itself (and the buffers) to be destroyed under them. The wait is
    // bounded by one write budget: the sender that was INSIDE the transport is the only
    // one that can be slow, and its call is now bounded by kWriteBudgetMs.
    //
    // The tally, not `send()`'s first instruction, is the boundary — see sender_exit_t.
    // A caller that entered `send()` but whose raise lands after the last load below is
    // NOT waited out, and neither is one that has yet to call at all; both are the same
    // embedder-owned lifetime residual, not something this loop can widen its way into.
    while (senders_.load(std::memory_order_acquire) != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(kDrainSliceMs));
}

bool esp_ws_client_link_t::connect_once() {
    // Build a FRESH tcp+ws transport pair for every dial. Re-using a closed
    // esp_transport_ws handle would inherit its internal frame_state (bytes_remaining
    // etc.) — ws_connect() does not reset it, only ws_init() zero-allocates it — so a
    // reconnect mid-fragment would mis-parse the first frame of the new connection.
    // Rebuilding is the only way to guarantee a clean frame state.
    //
    // This rewrite runs with NO lock — deliberately: taking `write_m_` here would put the
    // whole dial bound in front of every sender, which is the stall #952 is about. What
    // makes it safe is that the handles are PUBLISHED by the `connected_` release store
    // at the end of this function, and nothing off this thread may read them without
    // first observing that store (see send()). `connected_` is false for the whole of
    // this rewrite, so a sender racing it turns back at the gate instead of reading.
    if (ws_ != nullptr) {
        esp_transport_close(ws_);
        esp_transport_destroy(ws_);
        ws_ = nullptr;
    }
    if (tcp_ != nullptr) {
        esp_transport_destroy(tcp_);
        tcp_ = nullptr;
    }
    tcp_ = esp_transport_tcp_init();
    if (tcp_ == nullptr) return false;
    ws_ = esp_transport_ws_init(tcp_);
    if (ws_ == nullptr) return false;
    esp_transport_ws_config_t cfg = {};
    cfg.ws_path = ws_path_.c_str();
    cfg.propagate_control_frames = false;  // esp_transport_ws answers PING/CLOSE itself
    // Optional handshake auth: extra header lines a peer's admission hook can gate on (a
    // b2b dial carries no browser cookie). esp_transport_ws appends them verbatim; empty
    // leaves the field null so the handshake is byte-for-byte the historical one.
    //
    // Read with no lock and no copy, which is sound because the member is `const` and was
    // set before this thread existed (#959). The pointer therefore cannot dangle, and the
    // FIRST dial carries the header. It used to be an ordinary member a setter assigned
    // after construction, unsynchronized with this read: which side won was undefined, so
    // whether dial one carried a token was undefined too. Every re-dial re-reads the same
    // bytes, so a reconnect re-authenticates.
    if (!handshake_headers_.empty()) cfg.headers = handshake_headers_.c_str();
    esp_transport_ws_set_config(ws_, &cfg);

    // esp_transport_connect performs the full RFC 6455 client handshake for ws_path_.
    // The bound is DERIVED (kDialTimeoutMs), not a literal: this call is the longest
    // thing the recv thread does, and the destructor joins it, so its size is the size
    // of a teardown that lands mid-dial. It is not interruptible — see the recv_loop
    // note on what that still costs.
    //
    // Timed, too: TCP connect + the opening exchange is an UPPER BOUND on ~2x RTT, and
    // it is the only latency fact this link can offer without an active probe (stats_t).
    const std::int64_t dial_t0 = esp_timer_get_time();
    const int rc = esp_transport_connect(ws_, host_.c_str(), port_, kDialTimeoutMs);
    if (rc != 0) {
        esp_transport_close(ws_);
        return false;
    }
    const std::int64_t dial_us = esp_timer_get_time() - dial_t0;
    // Disable Nagle on the freshly connected socket, symmetric with the server side
    // (httpd_ws_link_t::bound_socket): a board-to-board dial carries the same small,
    // latency-sensitive TLV frames whose replies the peer awaits, so delayed-ACK +
    // Nagle would add tens of ms per round-trip. esp_transport exposes the underlying
    // fd; best-effort, so a transport that hides it (rc < 0) just keeps Nagle for
    // this link rather than failing the dial.
    const int fd = esp_transport_get_socket(tcp_);
    if (fd >= 0) {
        const int nodelay = 1;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0)
            ESP_LOGW(kTag, "TCP_NODELAY not applied fd=%d", fd);
        // Bound the BLOCKING leg of the write, which the transport's own timeout does
        // not reach: after a positive poll_write, tcp_transport calls a plain write()
        // on this socket, and lwIP parks in it until the peer's window opens. Without
        // SO_SNDTIMEO the poll timeout is the only bound there is (#952). Expiring
        // here surfaces as a short write, which send() already treats as a torn
        // connection. Best-effort like NODELAY above, and the same shape the server
        // sibling applies to its accepted sockets (httpd_ws_link_t::bound_socket): a
        // stack that refuses the option keeps the poll bound for this link rather than
        // failing the dial.
        struct timeval snd = {};
        snd.tv_sec = static_cast<time_t>(kWriteTimeoutMs / 1000);
        snd.tv_usec = static_cast<suseconds_t>((kWriteTimeoutMs % 1000) * 1000);
        if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd)) != 0)
            ESP_LOGW(kTag, "SO_SNDTIMEO not applied fd=%d (%d ms)", fd, kWriteTimeoutMs);
        // Notice a peer that vanishes WITHOUT a FIN — a Wi-Fi drop, a power cut, a NAT
        // rebind (#957). Nothing else in this loop notices one: esp_transport_poll_read
        // keeps reporting "no data this turn" forever, so `connected_` stays true,
        // link_up() keeps answering true for a peer that no longer exists, and on an idle
        // link the only other bound is TCP's own retransmit timeout — minutes when there is
        // something to retransmit, never when there is not. Keepalive probes are the
        // answer that costs no protocol work: the stack fails the connection, which
        // surfaces on the very next poll or read as an error and takes the ordinary drop
        // path below. Applied as a GROUP and only behind the enable, because the three
        // tunables mean nothing without it — a stack that refuses SO_KEEPALIVE keeps
        // this link on the retransmit timeout rather than on half a policy. Best-effort
        // like the two options above.
        const int keepalive = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) != 0) {
            ESP_LOGW(kTag, "SO_KEEPALIVE not applied fd=%d (a silent peer death is undetected)",
                     fd);
        } else {
            const int idle = kKeepIdleSeconds;
            const int intvl = kKeepIntervalSeconds;
            const int cnt = kKeepProbes;
            // Each attempted independently: a stack that refuses one tunable still gets
            // the others, and `||` would stop at the first refusal.
            int refused = ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0;
            refused += ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) != 0;
            refused += ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) != 0;
            if (refused != 0)
                ESP_LOGW(kTag, "%d keepalive tunable(s) refused fd=%d (stack defaults apply)",
                         refused, fd);
        }
    }
    // The connection edge, and the only place the link can observe one: `drop()`
    // deliberately stays silent (see below), so this counter IS the reconnect signal.
    // The traffic counters are per-CONNECTION, so they reset here; `last_rx_us` goes
    // back to "never" rather than carrying the previous session's staleness forward.
    {
        const std::lock_guard<std::mutex> lk(st_m_);
        st_ = {};
        st_.connected_at_us = esp_timer_get_time();
        connect_ms_ = static_cast<std::uint32_t>(dial_us / 1000);
        ++reconnects_;
    }
    // The came-up fact latches here and is never cleared (#1059/#1203): `ok()` reports
    // "a handshake landed at least once", `connected_` (published release, below) reports
    // whether one is standing NOW. Relaxed — it is a hint, not a synchronisation point —
    // but stored ahead of the RELEASE publication below, so any observer that acquires
    // `connected_ == true` necessarily also sees the came-up fact.
    came_up_.store(true, std::memory_order_relaxed);
    connected_.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "connected ws://%s:%u%s", host_.c_str(), static_cast<unsigned>(port_),
             ws_path_.c_str());
    return true;
}

esp_ws_client_link_t::stats_t esp_ws_client_link_t::stats() const {
    stats_t out;
    {
        // st_m_, NOT write_m_. write_m_ is held across esp_transport_write for up to
        // kWriteTimeoutMs (4 s) on a stalled socket, so a snapshot taken under it
        // would drag any caller — the embedder's periodic publisher, which may hold
        // its own lock across this call — into that wait. This mutex is only ever
        // held for a counter bump or this copy, so the snapshot is bounded-brief.
        const std::lock_guard<std::mutex> lk(st_m_);
        out.c = st_;
        out.reconnects = reconnects_;
        out.connect_ms = connect_ms_;
    }
    // Filled from `dropped_rx_`, not kept in `st_`: the receive path already tallies every
    // inbound discard there (#953/#901), and a second counter bumped at the same sites
    // could only drift from it. `c.rx_drops` and `dropped_rx()` are therefore two
    // spellings of one number, never two numbers. Saturating, because the block is 32-bit
    // and the atomic is 64-bit — a link that really dropped 4 billion messages reports the
    // cap here and the exact figure through `dropped_rx()`.
    const std::uint64_t rx_dropped = dropped_rx_.load(std::memory_order_relaxed);
    out.c.rx_drops = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(rx_dropped, std::numeric_limits<std::uint32_t>::max()));
    // Read OUTSIDE the mutex: `connected_` is atomic and `drop()` clears it while
    // holding write_m_, so taking it inside would say nothing extra and reading it
    // here keeps the hold to the struct copy. This field is LIVENESS — the same answer
    // `link_up()` gives, not the came-up `ok()` (#1203).
    out.up = link_up();
    return out;
}

esp_ws_client_link_t::timing_t esp_ws_client_link_t::timing() noexcept {
    // Pure reporting of what this translation unit compiled: the two derived bounds and
    // the three Kconfig ones, in the units the header names. It takes no lock and touches
    // no member because none of the five is per-link (#1160).
    return {kDialTimeoutMs, kWriteTimeoutMs, kReadTimeoutMs, kPollMs, kReconnectBackoffMs};
}

void esp_ws_client_link_t::drop() {
    // Close under the syscall serializer so an in-flight send() cannot touch a
    // half-closed handle, and mark down so the recv loop re-dials (which rebuilds
    // the transport pair — see connect_once).
    //
    // The departure REPORT is not made here, and deliberately: this is called with
    // `write_m_` about to be taken, and `notify_down` re-enters the routing plane (the
    // notifier takes graph locks), which the transport contract forbids doing under an
    // internal lock. It is also not the only way down — `send()` clears `connected_` on
    // a failed or short write without ever calling this. The one place that observes
    // EVERY transition to down is the recv loop's own `!connected_` arm, so that is
    // where the report is made (see recv_loop, #957).
    const std::lock_guard<std::mutex> lk(write_m_);
    if (ws_ != nullptr) esp_transport_close(ws_);
    connected_.store(false, std::memory_order_release);
}

void esp_ws_client_link_t::send(std::span<const std::byte> frame) {
    // Counted under st_m_, never under write_m_ — write_m_ is held across the transport
    // write below for up to kWriteTimeoutMs, and a counter that rode it would make every
    // stats() snapshot inherit that wait. st_m_ is only ever taken for these bumps, so it
    // is uncontended and syscall-free. Where both are held the order is always
    // write_m_ -> st_m_ and never the reverse, which keeps it acyclic.
    const auto bump = [this](auto fn) {
        const std::lock_guard<std::mutex> lk(st_m_);
        fn();
    };
    // This early-out stays AHEAD of the sender tally and of write_m_ (#952 ordering): it
    // reads nothing the destructor can be racing. Counted without any lock held.
    if (frame.empty() || frame.size() > tx_buf_.size()) {  // drop oversize/empty
        // The oversize half is the `tx_bytes` CEILING, and this is the only place that can
        // name it. `transport_t::send` returns void, so the router cannot be told the frame
        // died; `st_.tx_drops` says one did, but the `!connected_` arm and the short-write
        // arm below bump that same counter, so a bump alone does not even say WHICH drop
        // this was, let alone which knob was too small. A per-frame ceiling nobody can see
        // is how a blob-carrying value or a composed reply vanishes with clean logs on both
        // ends (#959). Logged every time, exactly like the
        // rx-buffer arm in recv_loop: this firing means the link emits NOTHING at this
        // frame size, which is a misconfiguration to fix, not a rate to live with. The
        // empty half needs no log — there is nothing to put on the wire and no knob to
        // name — but it is the same drop and is counted the same way.
        if (!frame.empty())
            ESP_LOGW(kTag, "outbound frame %u B exceeds %u B tx buffer — dropped",
                     static_cast<unsigned>(frame.size()), static_cast<unsigned>(tx_buf_.size()));
        bump([this] { ++st_.tx_drops; });
        return;
    }
    // Announce this sender BEFORE queueing on write_m_ (#952). The queue on that mutex
    // is the hazard: it is held across the transport write, so a sender can be parked
    // on it while the destructor runs, and pre-#952 it woke up owning a destroyed
    // handle. The tally is what the destructor drains before it lets this object's own
    // members go.
    senders_.fetch_add(1, std::memory_order_relaxed);
    const sender_exit_t leaving(senders_);
    const std::lock_guard<std::mutex> lk(write_m_);
    // Re-checked, not re-read for tidiness: teardown may have run while this sender was
    // queued, and it disarms `stop_` BEFORE it takes this very lock to null the handles,
    // so a sender that wakes to a set `stop_` leaves without touching either handle.
    // NOT counted as a drop: the link is being destroyed, which is not a loss toward the
    // peer, and a teardown that bumped a counter would make every clean shutdown look
    // like a failure.
    if (stop_.load(std::memory_order_acquire)) return;
    // THE handle gate, and the reason nothing above it may read `ws_`. `write_m_` does
    // NOT order a handle read against a re-dial: connect_once() destroys and rewrites
    // ws_/tcp_ holding no lock at all. What orders them is this acquire pairing with the
    // release store connect_once makes only AFTER the rebuild — and `connected_` is
    // cleared only under this lock (drop(), a failed write) or by the destructor, whose
    // store the recv loop answers by breaking rather than re-dialing. So an observed
    // `true` here is a happens-before edge with the writes that built the handle this
    // call is about to use, and a handle check placed AHEAD of this line is read against
    // an unsynchronised rewrite — the same unsynchronised handle read #952 is about,
    // seen from the sender's side (TSan: send() vs connect_once, on a failing re-dial).
    //
    // This one IS a drop: the push vanishes toward a peer that is simply down, and it is
    // the loss that used to be completely invisible.
    if (!connected_.load(std::memory_order_acquire)) {  // best-effort, like UDP
        bump([this] { ++st_.tx_drops; });
        return;
    }
    // Copy into the reusable scratch: esp_transport_write masks IN-PLACE and unmasks
    // back (RFC 6455 client rule), but a delivered frame may be shared with the
    // concurrent server link reading the same bytes, so the caller's bytes must not be
    // transiently mutated — hence the copy onto our private scratch.
    std::memcpy(tx_buf_.data(), frame.data(), frame.size());
    const int n = esp_transport_write(ws_, reinterpret_cast<char*>(tx_buf_.data()),
                                      static_cast<int>(frame.size()), kWriteTimeoutMs);
    if (n < 0 || n < static_cast<int>(frame.size())) {
        // Error or short write (a partial WS frame would desync the peer) — tear the
        // connection down so the recv loop rebuilds it; the frame is best-effort-lost.
        bump([this] { ++st_.tx_drops; });
        connected_.store(false, std::memory_order_release);
        return;
    }
    bump([this, n = frame.size()] {
        ++st_.tx_frames;
        st_.tx_bytes += static_cast<std::uint32_t>(n);
    });
}

void esp_ws_client_link_t::recv_loop() {
    // `off` is the in-progress message accumulator; it PERSISTS across poll turns so a
    // fragmented message (spread over reads/polls, possibly interleaved with control
    // frames) reassembles correctly. Reset only on delivery, overflow, or a drop.
    std::size_t off = 0;
    // Unread bytes of the WS FRAME currently being consumed. esp_transport_read caps
    // every read at the space we offer it and, when a frame does not fit, keeps serving
    // the rest of that SAME frame on later reads (transport_ws.c: a new header is parsed
    // only once `bytes_remaining` hits 0) — while still reporting that frame's opcode and
    // fin flag on every one of them. So `fin` alone cannot say a message ended: without
    // this counter a partial read of a big frame is indistinguishable from a complete
    // small message, which is how the tail of a dropped message got delivered (#901).
    // The frame's total length comes from esp_transport_ws_get_read_payload_len.
    std::size_t frame_left = 0;
    // Set when the message being assembled has already overflowed rx_buf_: its remaining
    // bytes are read and thrown away until the fin that ends it, so the tail is never
    // mistaken for a message of its own. This is what makes the drop-don't-truncate
    // contract actually hold (#901).
    bool discarding = false;
    // Set while a MESSAGE is open — i.e. a BINARY/TEXT frame arrived without FIN and its
    // CONT frames are still expected. This cannot be inferred from `off`: a message whose
    // first fragment carries a zero-length payload (legal per RFC 6455 §5.4, and what a
    // peer emits when it starts a message before its first chunk is ready) leaves `off`
    // at 0, so an `off == 0` test reads its CONT frames as strays and drops the whole
    // message. Assembly is a property of the FIN flags seen, not of the byte count.
    bool assembling = false;
    // Whether a connection this loop has to REPORT the death of is currently standing.
    // Set only by a completed dial, cleared by the report — so the departure seam fires
    // exactly once per connection that was up, and never for a dial that never landed
    // (the first turn, and every failed re-dial, would otherwise announce the death of a
    // link that was never alive). It is a plain local because this thread is the only
    // writer of `connected_ == true`: a dial is the sole way up, and it happens here.
    bool was_up = false;
    // Whether the connection currently standing has ever had a MESSAGE from the peer
    // delivered out of it — the test that separates a refusal from a working link (#1128).
    //
    // INBOUND, deliberately, and not "did we connect" or "did we send". A post-101
    // admission refusal is a successful transport connect by construction: the refusal can
    // only be expressed after `101 Switching Protocols` is on the wire, so
    // `esp_transport_connect` returns 0 and `connect_once()` reports success. Nor can the
    // outbound side tell: a send into a socket the peer has already decided to close still
    // succeeds locally. A message ARRIVING is the one event that proves the peer admitted
    // this session at the application level, because a refusing peer emits nothing but the
    // CLOSE. So a connection that comes up and goes down having delivered nothing is
    // counted as a failed attempt, and pays the same backoff a failed dial does.
    //
    // The false positive it accepts: a genuinely idle-but-admitted link that the peer
    // closes without ever having sent anything also reads as unproductive, and re-dials
    // one backoff later instead of at once. That is the harmless direction — the cost is
    // one interval on a link that was silent anyway, against a spin that reboots the node.
    bool exchanged = false;
    // The `defer_recv` dial latch (ADR-0081's defer-the-dial arm, #1102): park BEFORE the
    // first dial, so an unarmed link has no connection and the peer has nothing to push
    // into the pre-sink window — "not delivered yet" spelled as "not connected yet", the
    // purest form of holding upstream of the library. Waited here ONCE and never
    // re-checked below, deliberately: every re-dial happens on a link that was already
    // armed, and the router keeps the receiver installed across a `link_down`, so a
    // per-connection re-latch would gate delivery against a sink that is standing
    // (triage Correction 1 on #1102). The wait shares the backoff CV, so the
    // destructor's stop wake covers a never-armed link too — teardown stays prompt.
    {
        std::unique_lock<std::mutex> lk(backoff_m_);
        backoff_cv_.wait(lk, [this] {
            return armed_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire);
        });
    }
    while (!stop_.load(std::memory_order_relaxed)) {
        if (!connected_.load(std::memory_order_acquire)) {
            off = 0;
            frame_left = 0;
            discarding = false;
            assembling = false;
            // Teardown clears `connected_` too, so re-check `stop_` here or a link
            // destroyed while connected would answer by starting a fresh dial — and
            // the destructor would join THAT (#952). This is also the general case: a
            // stop seen during the poll turn below costs no dial at all. It carries a
            // second load: a fresh dial REWRITES ws_/tcp_ with no lock held, so this
            // break is what keeps the destructor's `connected_` store from re-opening
            // the handle rewrite underneath a sender that observed the previous `true`.
            if (stop_.load(std::memory_order_acquire)) break;
            // Set by either arm below — a connection that produced nothing, or a dial that
            // did not land. Both are failed attempts and both spend the SAME interval;
            // there is one backoff site so they cannot drift apart.
            bool backoff_now = false;
            if (was_up) {
                was_up = false;
                // Captured before notify_down: the seam runs app code, which may send, and
                // nothing it does retroactively makes this connection productive.
                backoff_now = !exchanged;
                // The departure seam (RFC-0009 §D extended to peer departure), and the
                // reason it is reported at all (#957). This arm is downstream of all
                // three sites that clear `connected_` — drop() (peer CLOSE, a poll
                // error, a read error), send()'s failed/short-write arm, and the
                // destructor — so ONE report here covers every way this link goes down,
                // with no transport lock held, exactly as the contract asks.
                //
                // It fires on a blip too, and that is the choice, not an oversight. The
                // link cannot tell a blip from a peer that REBOOTED: the reconnect
                // rebuilds the transport pair and says nothing, so keeping the child's
                // edges and label bindings across it is only sound if the far side kept
                // its own — and a rebooted peer has forgotten every subscription and
                // every label it ever issued, leaving this node producing into a session
                // that no longer exists and resolving compact labels against a stranger's
                // label space. Re-establishing after a flap is cheap and correct;
                // resurrecting stale routing is neither. This is also what core's
                // portable `transport_ws_client` does at the end of its recv loop
                // (core/src/transport_ws.cpp), which this type claims to be a drop-in
                // for. Guarded by the `stop_` break above, on core's rule: a LOCAL
                // teardown is not a peer departure and reports nothing.
                notify_down();
            }
            exchanged = false;
            // An unproductive connection backs off INSTEAD of re-dialing this turn; the
            // dial happens on the next one, past the wait. Dialing first and sleeping
            // after would leave the spin intact for exactly one more cycle, which on the
            // failing peer is the cycle that matters.
            if (!backoff_now) {
                if (connect_once()) {
                    was_up = true;  // there is now a connection whose death is reportable
                } else {
                    backoff_now = true;
                }
            }
            if (backoff_now) {
                // Interruptible backoff. `sleep_for` was not: the destructor's join
                // inherited the whole 1.5 s on exactly the unreachable peer a re-dial
                // exists for. What remains uninterruptible is the dial itself —
                // esp_transport_connect takes no cancellation, and shutting its socket
                // down from another task is not portable across IDF versions — so a
                // teardown that lands mid-dial still costs up to kDialTimeoutMs, which
                // is why that constant is derived from the watchdog window.
                std::unique_lock<std::mutex> lk(backoff_m_);
                backoff_cv_.wait_for(lk, std::chrono::milliseconds(kReconnectBackoffMs),
                                     [this] { return stop_.load(std::memory_order_acquire); });
            }
            continue;
        }

        const int pr = esp_transport_poll_read(ws_, kPollMs);
        if (pr < 0) {
            drop();
            off = 0;
            frame_left = 0;
            discarding = false;
            assembling = false;
            continue;
        }
        if (pr == 0) continue;  // no data this turn; re-check stop_

        int n = 0;
        {
            const std::lock_guard<std::mutex> lk(write_m_);
            if (!connected_.load(std::memory_order_acquire)) {
                n = -1;
            } else {
                // Server→client frames are unmasked, so this reads the payload directly
                // into rx_buf_ at `off` — a zero-copy fill. While discarding, `off` is 0
                // and the whole buffer is offered as a sink for bytes we then throw away.
                n = esp_transport_read(ws_, reinterpret_cast<char*>(rx_buf_.data()) + off,
                                       static_cast<int>(rx_buf_.size() - off), kReadTimeoutMs);
            }
        }
        if (n < 0) {
            drop();
            off = 0;
            frame_left = 0;
            discarding = false;
            assembling = false;
            continue;
        }
        // A control frame (PING/PONG/CLOSE-ack) was consumed internally — keep `off`. IDF
        // reports a ZERO-PAYLOAD data frame the same way (transport_ws.c returns 0 for
        // both, and for a poll timeout), so a message ended by an empty final fragment is
        // invisible here. That is pre-existing and symmetric: such a message never
        // delivered either, because `off` was likewise left standing.
        if (n == 0) continue;

        const ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(ws_);
        const bool fin = esp_transport_ws_get_fin_flag(ws_);
        if (op == WS_TRANSPORT_OPCODES_CLOSE) {
            drop();
            off = 0;
            frame_left = 0;
            discarding = false;
            assembling = false;
            continue;
        }
        if (op != WS_TRANSPORT_OPCODES_BINARY && op != WS_TRANSPORT_OPCODES_TEXT &&
            op != WS_TRANSPORT_OPCODES_CONT) {
            continue;  // a leaked control/NONE opcode: not a data frame — keep `off`
        }

        // Frame bookkeeping. A read with nothing outstanding starts a new frame, so its
        // length is the one to latch; a length that does not exceed this read (or an
        // absent one) falls back to "this read was the whole frame" — the historical
        // assumption, kept as the conservative answer.
        const std::size_t consumed = static_cast<std::size_t>(n);
        if (frame_left == 0) {
            const int plen = esp_transport_ws_get_read_payload_len(ws_);
            frame_left = plen > n ? static_cast<std::size_t>(plen) : consumed;
        }
        frame_left -= consumed < frame_left ? consumed : frame_left;
        // The MESSAGE is over only when the final frame has been consumed WHOLE.
        const bool message_done = fin && frame_left == 0;

        if (discarding) {
            // These bytes belong to the message already dropped: consume, never deliver.
            // The discard ends with that message, not with this frame.
            if (message_done) discarding = false;
            continue;
        }
        if (op != WS_TRANSPORT_OPCODES_CONT) {
            assembling = !message_done;  // a data opcode opens a message unless it ends it
        }
        if (op == WS_TRANSPORT_OPCODES_CONT && !assembling) {
            // A CONTINUATION with no assembly open — the peer's stream is out of step and
            // this is a mid-payload tail, not a message. Drop it rather than start a
            // message from the middle; the server sibling drops exactly this
            // (httpd_ws_link_t::on_data_frame).
            ESP_LOGW(kTag, "stray CONT with no message open — dropped");
            dropped_rx_.fetch_add(1, std::memory_order_relaxed);
            if (!message_done) discarding = true;  // swallow the rest of that stray message
            continue;
        }

        off += consumed;
        if (message_done) {
            // A complete message that fit in the buffer: deliver borrowed, serviced
            // in-call by the router on this recv thread. `off == rx_buf_.size()` is an
            // EXACT FIT and delivers — it used to take the overflow branch (#901).
            //
            // Counted BEFORE the delivery, under st_m_ and NOT write_m_: write_m_ is held
            // across a transport write for up to kWriteTimeoutMs, so counting under it
            // would stall every inbound graph op behind one slow outbound frame. No lock
            // at all is held across the delivery itself — the router runs the app in-call
            // and the app may call back into send() on this very stack.
            if (off > 0) {
                {
                    const std::lock_guard<std::mutex> lk(st_m_);
                    ++st_.rx_frames;
                    st_.rx_bytes += static_cast<std::uint32_t>(off);
                    st_.last_rx_us = esp_timer_get_time();
                }
                // This connection has now proved the peer admitted it, so its eventual
                // death is a DROP to retry at once rather than a refusal to back off from
                // (#1128). Set before the delivery: the router runs the app in-call here
                // and the app may tear the link down from this very stack.
                exchanged = true;
                rx_.deliver_borrowed(std::span<const std::byte>(rx_buf_.data(), off));
            }
            off = 0;
            assembling = false;
        } else if (off == rx_buf_.size()) {
            // The buffer is full and the message is NOT over: it cannot fit. Drop it
            // rather than deliver a partial TLV (never silently truncate) — and discard
            // the rest of it, or the remainder would re-accumulate from 0 and be handed
            // up as a bogus standalone frame while the peer's stream desynchronised in
            // silence (#901). The drop is tallied by `dropped_rx_` just below — this link
            // keeps ONE inbound-drop truth, and `stats()` reads `c.rx_drops` from it.
            ESP_LOGW(kTag, "inbound message exceeds %u B rx buffer — dropped",
                     static_cast<unsigned>(rx_buf_.size()));
            dropped_rx_.fetch_add(1, std::memory_order_relaxed);
            off = 0;
            assembling = false;
            discarding = true;
        }
        // else (!message_done && off < size): keep accumulating the fragmented message.
    }
}

}  // namespace tr::net
