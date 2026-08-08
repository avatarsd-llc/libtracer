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
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "esp_log.h"
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
 */
constexpr int kDrainTurns = 200;

/** @brief One drain turn, milliseconds — see @ref kDrainTurns. */
constexpr int kDrainSliceMs = 5;

/** @brief The calling task's identity, as the opaque token `server_task_` latches. */
[[nodiscard]] void* current_task() noexcept {
    return static_cast<void*>(xTaskGetCurrentTaskHandle());
}

/**
 * @brief httpd task stack, in bytes.
 *
 * The inbound graph request is serviced IN-CALL on this task — decode, resolve,
 * reply, and (the deep path) the whole /unit batch-apply transaction. The device
 * node measured that transaction overflowing an 8 KB stack and needing ~12 KB on
 * the raw ws recv thread (F2b, 2026-07-09); the 4 KB `esp_http_server` default is
 * far too small. Keep parity with that measured figure; httpd's own per-request
 * framing adds a little on top, so HIL should confirm this task's high-water mark
 * under a batch apply and bump it if a stack-protection reboot appears.
 */
constexpr std::size_t kHttpdTaskStack = 12288;

/**
 * @brief Upper bound on a single inbound message (one frame, or a reassembly).
 *
 * A borrowed-delivery transport heap-allocates the frame's bytes per receive, so
 * an unbounded length is a heap-exhaustion lever. Graph control-plane TLVs are far
 * smaller; a frame or reassembly past this is treated as abuse and the peer is
 * dropped (or the message discarded).
 */
constexpr std::size_t kMaxFrameBytes = 32768;

/** @brief Sockets reserved beyond the peer cap: httpd's internal working sockets
 *         plus headroom so the (cap+1)th peer is still ACCEPTED and can be refused
 *         cleanly in the handshake handler rather than held in the SYN backlog. */
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

/** @brief Peer cap assumed when the caller passes `max_peers == 0` (unbounded): the
 *         shared lwIP socket pool is small, so an unbounded cap still needs a finite
 *         socket budget — and the same finite number is the send bound's divisor. */
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
 * One full fan-out round — one bounded send to EVERY peer, all of them stalled — must
 * fit inside one watchdog window, because that round is exactly the failure #835
 * observed: the sends serialize on the single httpd task and the task is starved for
 * their sum. So the window is divided by the number of peers that can be in it. Both
 * inputs are facts already in hand (@ref kTaskWdtSeconds and the caller's peer cap), so
 * there is no knob and no millisecond literal to justify.
 */
[[nodiscard]] constexpr std::uint32_t derive_send_timeout_ms(std::size_t peer_cap) noexcept {
    const std::size_t peers = peer_cap != 0 ? peer_cap : kDefaultPeerCap;
    return static_cast<std::uint32_t>(kTaskWdtSeconds * 1000U / peers);
}

/** @brief Clamp @p want to the server's own per-socket send bound, seconds — the value
 *         REST sockets keep. A WS socket may be bounded tighter, never looser. */
[[nodiscard]] constexpr std::uint32_t clamp_send_timeout_ms(std::uint32_t want,
                                                            int send_wait_timeout_s) noexcept {
    const std::uint32_t ceiling = static_cast<std::uint32_t>(send_wait_timeout_s) * 1000U;
    return want < ceiling ? want : ceiling;
}

/**
 * @brief Reusable RX scratch capacity, bytes — a frame at or under this reads into
 *        a once-allocated buffer instead of taking a per-frame heap allocation.
 *
 * The httpd task is the only RX thread and delivery is synchronous (borrowed,
 * serviced in-call), so ONE scratch per link suffices and needs no lock. Graph
 * control TLVs — writes, subscribes, value pushes — sit well under this; a larger
 * frame (up to kMaxFrameBytes) falls back to the exact-size nothrow heap path,
 * trading one allocation for not pinning 32 KB of RAM permanently.
 */
constexpr std::size_t kRxScratchBytes = 2048;

/** @brief TX work slots pre-allocated per link, claimed lock-free by sending tasks
 *         (see @ref httpd_ws_link_t::tx_slot_t). Sized past the steady-state
 *         in-flight depth (a reply + a couple of subscription pushes); a burst past
 *         it falls back to the heap work item, never blocks. */
constexpr std::size_t kTxPoolSlots = 4;

/**
 * @brief Inline payload capacity of one TX work slot, bytes.
 *
 * Sized past the common outbound frames (value pushes, directed replies, census)
 * so a steady-state send gathers straight into the slot with NO allocation; a
 * larger frame (e.g. a composed-root snapshot reply) keeps the pooled shell but
 * takes a nothrow heap payload. Roughly one Ethernet MTU.
 */
constexpr std::size_t kTxInlineBytes = 1600;

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
 * @brief `<ip>:<port>` of the far side of @p fd — the peer name (bus tag / census),
 *        byte-compatible with transport_ws_server's naming. Falls back to `fd<n>`.
 *
 * The address family is read from what `getpeername` actually filled, never assumed. With
 * `CONFIG_LWIP_IPV6` on — the default on this target — `esp_http_server` binds its
 * listener `PF_INET6` (httpd_main.c), so every accepted WS socket is an AF_INET6 one and
 * a `sockaddr_in6` is what comes back. Decoding that as a `sockaddr_in` reads the port
 * correctly (same offset) and then reads `sin6_flowinfo` as the address — which is zero,
 * so every peer on the node was named `0.0.0.0:<port>`. That is what the on-silicon run
 * saw in the strike log, and it made the strike unattributable to a peer at exactly the
 * moment the attribution mattered. A v4-mapped v6 address (`::ffff:a.b.c.d`) is unwrapped
 * so a dual-stack node keeps naming its IPv4 peers the way the census always has.
 */
[[nodiscard]] std::string peer_name(int fd) {
    sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        char ip[INET6_ADDRSTRLEN] = {};
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
            return std::string(ip) + ':' + std::to_string(ntohs(a6.sin6_port));
        }
        if (addr.ss_family == AF_INET) {
            const auto& a4 = reinterpret_cast<const sockaddr_in&>(addr);
            ::inet_ntop(AF_INET, &a4.sin_addr, ip, sizeof(ip));
            return std::string(ip) + ':' + std::to_string(ntohs(a4.sin_port));
        }
    }
    return std::string("fd") + std::to_string(fd);
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
 * Slots are recycled in place across connections (never freed before the link), so
 * the @ref peer_endpoint_t `peer_link` hands out stays pointer-valid for the link's
 * life. Threading: `fd`/`open`/`name` are read cross-thread (peer_link /
 * enumerate_peers / a send's fd snapshot) and written by the httpd task
 * (accept/close) — all under `peers_m_`. `asm_buf` is touched only on the httpd
 * task (RX reassembly). `endpoint` and `gate` are set once at creation and never
 * change.
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
    bool open = false;         /**< @brief True between handshake and close. */
    std::string name;          /**< @brief `<ip>:<port>` of the peer. */
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
    peer_endpoint_t endpoint; /**< @brief The directed facade `peer_link` returns. */
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
 * @note That is the guarantee, and it is narrower than "this endpoint is peer X". The
 *       reference is only as good as the moment of minting: a caller that resolves a peer,
 *       is preempted, and sends afterwards mints from the slot's CURRENT generation, so the
 *       check is self-satisfying for whoever holds the slot then. Closing that needs an
 *       identity captured at RESOLVE time, which the shared per-slot endpoint cannot carry.
 *       Tracked as #1013; do not read this type as making the directed path safe.
 */
struct httpd_ws_link_t::session_ref_t {
    session_t* slot = nullptr; /**< @brief The peer slot; compared, never dereferenced blind. */
    std::uint32_t gen = 0;     /**< @brief @ref session_t::gen at the moment of minting. */
};

/**
 * @brief One queued outbound frame: the payload is gather-copied ONCE, nothrow, so
 *        it outlives the send() caller's spans until the httpd task drains the work
 *        item.
 *
 * Two storage shapes behind one struct (see @ref queue_send for the selection):
 * pooled — the work item IS a @ref tx_slot_t member and `payload` points at the
 * slot's inline buffer (or at `owned` when the frame outgrows it); heap — the
 * shell itself is `new (std::nothrow)` and `owned` holds the payload. Never a
 * `std::vector` for the copy — the vector's THROWING allocator inside a braced
 * initializer defeated the `new (std::nothrow)` guard on the shell: under
 * `-fno-exceptions` a reply-sized copy hitting heap exhaustion aborted the node
 * (the browser-session crash).
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
    tx_slot_t* slot = nullptr;    /**< @brief Owning pool slot; nullptr => heap shell. */
    std::unique_ptr<std::byte[]> owned; /**< @brief Heap payload (fallback shapes only). */
};

/**
 * @brief The lifecycle of one TX pool slot — and the whole of why a stranded work item
 *        cannot corrupt anything (#944).
 *
 * A single `busy` flag could not express this. `httpd_queue_work` on the default
 * non-blocking path is a bare `sendto` to the loopback control socket, so an enqueue past
 * the receiver's UDP mbox is DROPPED inside lwIP while `sendto` — and therefore
 * `httpd_queue_work` — still returns ESP_OK (httpd_main.c; the same fact the close path
 * was routed around with `shutdown`, see @ref httpd_ws_link_t::condemn). The work item
 * then never runs, and `busy` was cleared ONLY by the work item: the slot was pinned for
 * the rest of the boot, and four such drops killed the pool outright.
 *
 * Reclaiming a pinned slot is only safe if a work item that arrives AFTER the reclaim is
 * harmless, and the state machine is what makes it so — WITHOUT any identity token. A
 * pooled work item lives INSIDE its slot (@ref httpd_ws_link_t::tx_slot_t::work), so it
 * cannot carry a generation the way @ref httpd_ws_link_t::session_ref_t does for its
 * destination: a re-claim overwrites the very field the check would read. So the item
 * carries no identity at all and is treated as a bare TOKEN meaning "go send whatever is
 * armed in slot i". A token that arrives late either finds nothing armed (it returns,
 * touching nothing) or finds a LATER frame armed and sends that one — correctly, because
 * a payload and the destination it was gathered for are armed together and travel
 * together. Every armed payload is therefore sent by exactly one token, and no token can
 * ever observe a half-written slot.
 *
 * The two exclusive states are the load-bearing ones: CLAIMED and RUNNING both mean "one
 * task owns this slot outright", so neither a claim, nor a reap, nor a token can touch a
 * slot another is inside. ARMED is the only state either a token or the reaper may take.
 *
 * MACHINE-WORD underlying type, deliberately, and the reason is codegen rather than range.
 * The chip targets are rv32imac, whose reservations are word-granular (`lr.w`/`sc.w`), so a
 * BYTE-sized compare-exchange is still lock-free but has to read-modify-write the
 * containing word behind a shift/mask: 23 instructions against 9 for the same CAS at 32
 * bits (measured, riscv32-esp-elf-g++ 15.1, -Os, rv32imac). This protocol adds a second CAS
 * per send — the token's — on a hot publish path, so at a byte it would have cost more than
 * the `std::atomic<bool>` it replaces, and at a word two CASes still cost less than that one
 * did. The three extra bytes are free: the slot is already 8-byte aligned for @ref armed_at.
 *
 * @note This says NOTHING about #1013, and must not be read as if it did. A payload and
 *       the @ref httpd_ws_link_t::session_ref_t it was gathered for are written into the
 *       slot together and armed by the same release, so a token always sends a frame to
 *       the destination THAT frame was resolved for — a reclaim never crosses the two. The
 *       [resolve -> mint] window on the directed path is upstream of every state here (it
 *       is already closed, or already lost, before a slot is ever claimed) and is neither
 *       widened nor narrowed by any of this.
 */
enum class tx_state_t : std::uint32_t {
    FREE,    /**< @brief Unclaimed — the one state @ref claim_tx_slot may take. */
    CLAIMED, /**< @brief Owned outright by one task: filling it, or reaping it. */
    ARMED,   /**< @brief Payload complete, a token enqueued for it. Runnable, reapable. */
    RUNNING  /**< @brief A token is inside the send. Never claimable, never reapable. */
};

/**
 * @brief One pre-allocated TX work slot: claimed lock-free (a CAS on @ref state) by
 *        any sending task in @ref claim_tx_slot, released by the httpd task once
 *        its send drains (@ref release_tx_work) — so a steady-state send allocates
 *        nothing. The pool (kTxPoolSlots of these) is allocated once per link.
 */
struct httpd_ws_link_t::tx_slot_t {
    std::atomic<tx_state_t> state{tx_state_t::FREE}; /**< @brief See @ref tx_state_t. */
    /**
     * @brief When @ref arm published this slot's payload — the reaper's only input.
     *
     * Written while the slot is CLAIMED (owned outright, so no atomic is needed) and
     * read only after a CAS has taken it back to CLAIMED, so it is never read by a task
     * that does not own the slot and an ABA between the read and the decision is
     * impossible by construction.
     */
    std::chrono::steady_clock::time_point armed_at{};
    tx_work_t work;                       /**< @brief The slot's embedded work item. */
    std::byte inline_buf[kTxInlineBytes]; /**< @brief Inline payload storage. */

    /** @brief Publish the filled payload and open the slot to tokens (CLAIMED -> ARMED). */
    void arm() noexcept {
        armed_at = std::chrono::steady_clock::now();
        state.store(tx_state_t::ARMED, std::memory_order_release);
    }
    /**
     * @brief Reopen a slot the sweep judged young, WITHOUT restamping @ref armed_at.
     *
     * The sweep has to take a slot to CLAIMED to read its clock safely, so it must put
     * every young slot back. Putting it back through @ref arm would restamp `armed_at`,
     * which measures the slot's age from the LAST SWEEP rather than from its arm — and
     * since the sweep's only trigger is an exhausted claim, ordinary traffic sweeps far
     * more often than the window is long. A permanently stranded slot would then never
     * reach the window and never be reclaimed, with `tx_strands()` reading 0 the whole
     * time: the instrument added to make the strand visible would report healthy while
     * the pool stayed dead. The age must belong to the arm being judged, so the state
     * store is the only thing that may be repeated.
     */
    void rearm() noexcept { state.store(tx_state_t::ARMED, std::memory_order_release); }
    /**
     * @brief Take an armed payload back for the owning task (ARMED -> CLAIMED).
     *
     * @retval false  A token got there first and is inside the send — the slot is its
     *                property now, and the caller must NOT recycle it. Rare but real:
     *                a token stranded by an earlier claim of this same slot can fire in
     *                the window between @ref arm and a refused enqueue, and releasing
     *                the slot under it would hand a live send's buffer to a new claimant.
     */
    [[nodiscard]] bool disarm() noexcept {
        tx_state_t expected = tx_state_t::ARMED;
        return state.compare_exchange_strong(expected, tx_state_t::CLAIMED,
                                             std::memory_order_acquire);
    }
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

httpd_ws_link_t::httpd_ws_link_t(std::uint16_t bind_port, std::size_t max_peers, bool peer_named,
                                 std::uint32_t send_timeout_ms)
    : port_(bind_port), max_peers_(max_peers), peer_named_(peer_named) {
    if (!open_gate()) return;  // ok() stays false; nothing was registered
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = bind_port;
    // A SECOND httpd instance must not share the first's control UDP port — the SPA
    // httpd (web_server.c on :80) keeps the default, so offset ours by one or
    // httpd_start fails to bind the control socket.
    cfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    cfg.stack_size = kHttpdTaskStack;
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

    if (httpd_start(&handle_, &cfg) != ESP_OK) {
        handle_ = nullptr;  // ok() stays false
        return;
    }
    uri_ = "/";            // owns_httpd_ stays true; the dtor stops the server, but keep uri_
                           // coherent with the adopting path (both register the same handler).
    httpd_uri_t uri = {};  // zero-init, then set fields by name (robust to optional
    uri.uri = "/";         // ws_pre/post_handshake_cb members behind extra Kconfig)
    uri.method = HTTP_GET;
    uri.handler = &httpd_ws_link_t::ws_handler;
    // The GATE, never `this` — esp_http_server latches this pointer into every upgraded
    // session and keeps dispatching through it until that session is deleted, which can
    // be long after this link is gone. See gate_t.
    uri.user_ctx = gate_;
    uri.is_websocket = true;
    uri.handle_ws_control_frames = false;  // httpd answers PING/PONG and tracks CLOSE
    if (httpd_register_uri_handler(handle_, &uri) != ESP_OK) {
        httpd_stop(handle_);
        handle_ = nullptr;
        return;
    }
    alloc_buffers();
}

httpd_ws_link_t::httpd_ws_link_t(httpd_handle_t external, const char* uri_pattern,
                                 std::size_t max_peers, bool peer_named,
                                 std::uint32_t send_timeout_ms)
    : max_peers_(max_peers), peer_named_(peer_named) {
    if (!open_gate()) return;  // ok() stays false; nothing was registered
    // The adopted server's httpd_config_t belongs to the caller and esp_http_server
    // exposes no reader for it, so the clamp uses IDF's default send_wait_timeout — the
    // value that server has unless its owner tightened it, in which case the socket
    // option we set at admission is the tighter of the two anyway.
    const httpd_config_t defaults = HTTPD_DEFAULT_CONFIG();
    send_timeout_ms_ = clamp_send_timeout_ms(
        send_timeout_ms != 0 ? send_timeout_ms : derive_send_timeout_ms(max_peers),
        defaults.send_wait_timeout);
    // Adopt an already-running server (the firmware's :80 SPA httpd): register the WS URI
    // as one more handler on it rather than standing up a second esp_http_server. We do
    // NOT own the server, so port_ is 0 (no bind of ours) and the dtor must never
    // httpd_stop it — only unregister the URI. No cfg / ctrl_port / httpd_start here: with
    // one server the control-UDP-port clash the owning ctor guards against cannot arise.
    handle_ = external;
    owns_httpd_ = false;
    port_ = 0;
    uri_ = uri_pattern;

    httpd_uri_t uri = {};    // zero-init, then set fields by name (robust to optional
    uri.uri = uri_.c_str();  // ws_pre/post_handshake_cb members behind extra Kconfig)
    uri.method = HTTP_GET;
    uri.handler = &httpd_ws_link_t::ws_handler;
    uri.user_ctx = gate_;  // the GATE, never `this` — see gate_t
    uri.is_websocket = true;
    uri.handle_ws_control_frames = false;  // httpd answers PING/PONG and tracks CLOSE
    if (httpd_register_uri_handler(external, &uri) != ESP_OK) {
        handle_ = nullptr;  // ok() stays false; do NOT httpd_stop — we do not own the server
        return;
    }
    alloc_buffers();
}

bool httpd_ws_link_t::open_gate() {
    // Nothrow, and load-bearing: the gate is what makes the registered handler safe to
    // dispatch after this link dies, so a link that could not allocate one must not
    // register a handler at all. Both constructors bail to ok() == false on failure.
    gate_ = new (std::nothrow) gate_t;
    if (gate_ == nullptr) return false;
    gate_->link = this;
    return true;
}

void httpd_ws_link_t::close_gate() {
    if (gate_ == nullptr) return;
    // A destructor reached from INSIDE the URI handler — an app teardown driven by the
    // very frame being serviced (#814) — IS the in-flight frame, so waiting for `depth`
    // to reach zero would wait on a stack frame below this one. Running on the server's
    // task is the proof of that, and equally the proof that no OTHER frame can be in
    // flight: esp_http_server dispatches every request from that one task.
    const bool on_server_task = server_task_.load(std::memory_order_relaxed) == current_task();
    std::unique_lock lock(gate_->m);
    // From here no dispatch can enter the link — ws_handler refuses (httpd closes that
    // socket) and on_session_closed is inert. Both were reachable a moment ago through
    // pointers the adopted server latched and no API of ours can revoke.
    gate_->link = nullptr;
    // Unbounded BY DESIGN, unlike the two drains: there is no leak-instead-of-free
    // fallback for the link itself, so a handler frame still reading peers_m_ /
    // rx_scratch_ / slots_ has to be joined, not out-waited. It is bounded in practice by
    // the app callback the delivery runs, and the one way it could wait on itself is the
    // case skipped above. Since #960 a departure notification in flight is joined here
    // too: it dereferences the link and hands the routing plane a name, and it runs with
    // `m` released precisely so this wait — not the mutex — is what holds it.
    if (!on_server_task) gate_->cv.wait(lock, [this] { return gate_->depth == 0; });
}

void httpd_ws_link_t::alloc_buffers() {
    // Both are once-per-link, nothrow, and OPTIONAL: if either allocation fails the
    // link still works — every frame just takes the per-frame heap fallback path.
    rx_scratch_.reset(new (std::nothrow) std::byte[kRxScratchBytes]);
    tx_pool_.reset(new (std::nothrow) tx_slot_t[kTxPoolSlots]);
    // Bind each slot to its embedded work item ONCE, here, and never again. A claim must
    // not write this field: a token that arrives late reads `work->slot` before it has
    // proved anything (that is how it finds the state word to prove it WITH), so a
    // concurrent claimer storing the same value into it would be a plain data race for no
    // gain. The back-pointer is a property of the slot, not of the claim.
    if (tx_pool_ != nullptr)
        for (std::size_t i = 0; i < kTxPoolSlots; ++i) tx_pool_[i].work.slot = &tx_pool_[i];
}

httpd_ws_link_t::tx_slot_t* httpd_ws_link_t::claim_tx_slot() {
    // Teardown gate: once the dtor is draining, new sends must stop claiming slots or
    // the drain never converges (unregistering the URI stops RX only — subscription
    // pushers keep sending until the router detaches the transport). The heap fallback
    // they get instead never touches the pool, so it is safe to run past the dtor.
    if (tx_pool_ == nullptr || stopping_.load(std::memory_order_relaxed)) return nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < kTxPoolSlots; ++i) {
            tx_state_t expected = tx_state_t::FREE;
            if (tx_pool_[i].state.compare_exchange_strong(expected, tx_state_t::CLAIMED,
                                                          std::memory_order_acquire))
                return &tx_pool_[i];
        }
        // Exhausted. THIS is the reclaim trigger, and deliberately the only one: a strand
        // costs nothing until the pool runs out, and reclaiming on demand needs no timer
        // task, no periodic wakeup, and not one instruction in the steady state. A second
        // pass then re-scans whatever the sweep freed; if it frees nothing the caller
        // heap-falls-back exactly as before, so a genuinely busy pool is unaffected.
        if (pass == 0) sweep_tx_slots();
    }
    return nullptr;  // every slot in flight this instant — caller heap-falls-back
}

/**
 * @brief Reclaim TX pool slots whose work item the control socket silently binned (#944).
 *
 * @note Unrelated to @ref httpd_ws_link_t::reclaim_slot despite the shared verb: that one
 *       recycles a departed PEER's session slot and names the peer the routing plane's
 *       eviction notifier is owed for. This one recycles a TX work slot, holds no mutex,
 *       and notifies nothing.
 */
void httpd_ws_link_t::sweep_tx_slots() {
    // The window: every OTHER slot in the pool ahead of you, each stalled to this link's
    // full per-socket send bound. Both factors are facts already in hand — the pool size
    // caps how many items of ours can precede one in the control queue, and
    // send_timeout_ms_ (itself derived, see derive_send_timeout_ms) caps how long the
    // httpd task can spend on any one of them — so there is no millisecond literal and no
    // knob. At the default peer cap the product is exactly one kTaskWdtSeconds window,
    // which is the system's own statement of how long a task may go unserviced.
    //
    // A link with no send bound of its own has nothing to derive a drain latency FROM, so
    // it declares nothing stranded rather than inventing a number.
    if (send_timeout_ms_ == 0) return;
    const auto window = std::chrono::milliseconds(kTxPoolSlots * send_timeout_ms_);
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kTxPoolSlots; ++i) {
        tx_slot_t& slot = tx_pool_[i];
        // Take the slot OUT of ARMED before looking at its clock, never the other way
        // round: after this CAS wins the slot is ours outright, so the timestamp cannot
        // belong to a different arm than the one being judged. Reading the clock first and
        // reaping second is the ABA that would drop a freshly armed frame. A slot that is
        // FREE, being filled, or inside a send fails the CAS and is not the sweep's
        // business — RUNNING especially: that token is reading this payload right now.
        if (!slot.disarm()) continue;
        if (now - slot.armed_at < window) {
            slot.rearm();  // young: put it back exactly as its claimer left it, clock and all
            continue;
        }
        // Past every drain latency this link can itself produce, and still armed: the
        // token was enqueued with ESP_OK and silently binned by lwIP. Recycle the slot.
        // A token that turns up afterwards is harmless by construction (see tx_state_t) —
        // it finds this slot FREE, or armed with a LATER frame it will send correctly.
        const std::size_t lost = slot.work.len;  // read while the slot is still OURS
        slot.work.owned.reset();
        slot.work.payload = nullptr;
        slot.state.store(tx_state_t::FREE, std::memory_order_release);
        const std::uint32_t total = tx_strands_.fetch_add(1, std::memory_order_relaxed) + 1;
        ESP_LOGW(kTag, "tx slot reclaimed: work item never ran (len=%u total=%u)", (unsigned)lost,
                 (unsigned)total);
    }
}

void httpd_ws_link_t::release_tx_work(tx_work_t* work) {
    if (work->slot != nullptr) {
        work->owned.reset();  // drop an overflow heap payload before the slot recycles
        work->slot->state.store(tx_state_t::FREE, std::memory_order_release);
    } else {
        delete work;
    }
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
    // so frames keep arriving at the handler regardless (see gate_t). Once close_gate
    // returns, no dispatch can reach a single member of this link — which is also what
    // makes the session snapshot below COMPLETE, since no handler can still be about to
    // claim a slot behind it.
    close_gate();
    if (handle_ != nullptr) {
        if (owns_httpd_) {
            httpd_stop(handle_);
        } else {
            // Courtesy only, now that the gate is shut: it stops new handshakes from
            // latching the route, so the population of stale sessions cannot grow.
            httpd_unregister_uri_handler(handle_, uri_.c_str(), HTTP_GET);
            detach_sessions();
        }
    }
    // Adopted mode: the caller's server keeps running after our URI is unregistered,
    // so a queued tx_work may still execute on its task while it references a pool
    // slot — wait (bounded) for the in-flight slots to drain before tx_pool_ dies.
    // Owning mode needs no wait: httpd_stop has halted the task, so nothing can touch
    // the pool afterwards (an undrained work item is simply never run).
    if (!owns_httpd_ && tx_pool_ != nullptr) {
        // Deliberately NOT swept: the sweep recycles a stranded slot for REUSE, which is
        // sound only while the pool stays allocated. Here the pool is about to be freed,
        // and a token arriving afterwards would read freed memory rather than a recycled
        // slot — so the expiry below keeps the leak-instead-of-free answer it always had.
        bool busy = true;
        for (int turn = 0; turn < kDrainTurns && busy; ++turn) {
            busy = false;
            for (std::size_t i = 0; i < kTxPoolSlots; ++i)
                if (tx_pool_[i].state.load(std::memory_order_acquire) != tx_state_t::FREE)
                    busy = true;
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
            // bounded, teardown-only loss (kTxPoolSlots inline slots) that the
            // drained path never pays. tx_work touches only the work item and the
            // caller's still-running server handle, never this link, so the leaked
            // pool is the one allocation that must outlive us.
            ESP_LOGW(kTag, "tx pool leaked at teardown: a queued send outlived the drain bound");
            (void)tx_pool_.release();
        }
    }
    // The gate outlives the link exactly when the server does. Owning mode: httpd_stop
    // has deleted every session, so nothing holds the pointer any more and it is freed.
    // Adopted mode: the set of sessions that latched it is unknowable (a peer that
    // upgraded and never sent a frame is invisible here) and no API deletes them on
    // demand, so it is LEAKED — one small block, teardown-only, and the price of a
    // handler that stays safe to dispatch forever. Nothing was registered when the
    // constructor failed, so that case frees it too.
    if (owns_httpd_ || handle_ == nullptr) {
        delete gate_;
    } else {
        ESP_LOGD(kTag, "handler gate leaked at teardown: the adopted server still routes to it");
    }
    gate_ = nullptr;
    handle_ = nullptr;
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
    if (gate_ != nullptr) {
        int serving_fd = -1;
        {
            // `depth` counts departure notifications too since #960, so it alone no longer
            // implies a request scope — but `serving_fd` is set and cleared by @ref
            // ws_handler under this same lock and is -1 for every other holder, so the
            // pair still names exactly the request-scoped session and nothing else.
            const std::lock_guard lock(gate_->m);
            if (gate_->depth != 0) serving_fd = gate_->serving_fd;
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
    req->handle = handle_;
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
    } else if (httpd_queue_work(handle_, &httpd_ws_link_t::detach_work, raw) != ESP_OK) {
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
    // Clearing `open`/`fd` also keeps it out of any later snapshot, and the endpoint
    // facade's own null-owner case covers a directed send that outlives the link.
    slot->open = false;
    slot->fd = -1;
    slot->asm_buf.clear();
    slot->endpoint.owner_ = nullptr;
    slot->endpoint.slot_ = nullptr;
}

// ---------------------------------------------------------------------------
// RX — runs on the esp_http_server task.
// ---------------------------------------------------------------------------

esp_err_t httpd_ws_link_t::ws_handler(httpd_req_t* req) {
    // req->user_ctx is the GATE (see gate_t), because esp_http_server keeps dispatching
    // through this pointer for as long as the SESSION lives — unregistering the URI does
    // not revoke it, and the link may be long gone. Resolving the link through the gate
    // is the admission test AND the barrier registration.
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
    // The opening HTTP GET Upgrade arrives as method GET (httpd has already sent the
    // 101); every subsequent data frame re-enters here with method != GET.
    const esp_err_t err =
        req->method == HTTP_GET ? self->on_handshake(req) : self->on_data_frame(req);
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

esp_err_t httpd_ws_link_t::on_handshake(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return ESP_FAIL;
    // Admission hook (optional): let the host refuse an unauthenticated peer before any
    // slot is touched. Consulted FIRST so a refusal costs nothing, and returns ESP_FAIL
    // (httpd closes the socket) — the same clean refusal path as the max_peers cap. A
    // null hook admits every peer, preserving the historical open-graph behavior.
    if (admission_fn_ != nullptr && !admission_fn_(admission_ctx_, req)) {
        ESP_LOGW(kTag, "peer refused by admission hook (fd=%d)", fd);
        return ESP_FAIL;
    }
    // No teardown test here: reaching this point already means the handler gate admitted
    // this frame, and the destructor's barrier will not take its session snapshot until
    // this frame has left. So a session armed here — however late — is still IN that
    // snapshot, and refusing the frame would only lose a message the link can still serve.
    session_t* slot = nullptr;
    {
        const std::lock_guard lock(peers_m_);
        // Admission cap: refuse the new peer cleanly (ESP_FAIL => httpd closes the
        // socket). A clean refusal at the cap, not an evicted live peer.
        if (max_peers_ != 0) {
            std::size_t open_n = 0;
            for (const auto& s : slots_)
                if (s->open) ++open_n;
            if (open_n >= max_peers_) {
                ESP_LOGW(kTag, "peer refused: at max_peers=%u", (unsigned)max_peers_);
                return ESP_FAIL;
            }
        }
        // Reuse a departed slot, else grow (push_back keeps existing slots' addresses
        // stable — endpoints handed out by peer_link stay valid).
        for (const auto& s : slots_)
            if (s->fd < 0) {
                slot = s.get();
                break;
            }
        if (slot == nullptr) {
            auto s = std::make_unique<session_t>();
            slot = s.get();
            slot->gate = gate_;
            slot->endpoint.owner_ = this;
            slot->endpoint.slot_ = slot;
            slots_.push_back(std::move(s));
        }
        slot->name = peer_name(fd);
        slot->asm_buf.clear();
        slot->fd = fd;
        // A NEW session begins here, in a slot the previous one may still be addressed by:
        // bump the generation before the slot goes live, so every reference minted for the
        // departed peer fails its check instead of resolving onto this one (#954).
        ++slot->gen;
        slot->open = true;
    }
    // Reclaim the slot when httpd tears this session down (the free_ctx_fn fires on the
    // httpd task at close — the departure signal).
    httpd_sess_set_ctx(req->handle, fd, slot, &httpd_ws_link_t::on_session_closed);
    bound_socket(fd);  // WS admission is the ONLY place the send bound is applied (#835)
    return ESP_OK;
}

esp_err_t httpd_ws_link_t::on_data_frame(httpd_req_t* req) {
    httpd_ws_frame_t frame = {};
    // Pass 1 (max_len 0): read the header only — fills frame.len / frame.type. The
    // payload is NOT consumed off the socket here; pass 2 below does that.
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;                    // socket error => httpd closes the session
    if (frame.len > kMaxFrameBytes) return ESP_FAIL;  // abusive frame => drop the peer

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
        if (rx_scratch_ != nullptr && frame.len <= kRxScratchBytes) {
            payload = rx_scratch_.get();
        } else {
            heap_payload.reset(new (std::nothrow) std::byte[frame.len]);
            if (heap_payload == nullptr) {
                ESP_LOGW(kTag, "rx alloc failed (len=%u) - closing session", (unsigned)frame.len);
                return ESP_FAIL;
            }
            payload = heap_payload.get();
        }
        frame.payload = reinterpret_cast<std::uint8_t*>(payload);
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) return err;
    }
    const std::span<const std::byte> body(payload, frame.len);
    // Only data frames carry a TLV (control frames are httpd's — handle_ws_control_frames
    // is off); a stray TEXT/PONG is now drained and ignored.
    if (frame.type != HTTPD_WS_TYPE_BINARY && frame.type != HTTPD_WS_TYPE_CONTINUE) return ESP_OK;

    const int fd = httpd_req_to_sockfd(req);
    // Resolve the slot (peer name for the bus tag + the reassembly buffer). Copy the
    // name out under the lock — the deliver below is synchronous, so a local string
    // outlives the whole in-call servicing.
    // esp_http_server responds the WS handshake INTERNALLY and (IDF v6) does NOT call the
    // URI handler for the opening GET — so on_handshake never fires. Claim the peer LAZILY
    // here, on its first data frame; an existing slot for `fd` is reused (idempotent, so the
    // on_handshake path still works on any IDF that does call the GET handler).
    session_t* slot = nullptr;
    std::string peer;
    bool newly_claimed = false;
    {
        const std::lock_guard lock(peers_m_);
        for (const auto& s : slots_)
            if (s->open && s->fd == fd) {
                slot = s.get();
                break;
            }
        if (slot == nullptr) {
            // No teardown test (see on_handshake): the gate admitted this frame, so the
            // barrier has not snapshotted yet and a session armed here is still caught.
            // Admission cap: refuse cleanly (ESP_FAIL => httpd closes the socket).
            if (max_peers_ != 0) {
                std::size_t open_n = 0;
                for (const auto& s : slots_)
                    if (s->open) ++open_n;
                if (open_n >= max_peers_) {
                    ESP_LOGW(kTag, "peer refused: at max_peers=%u", (unsigned)max_peers_);
                    return ESP_FAIL;
                }
            }
            for (const auto& s : slots_)
                if (s->fd < 0) {
                    slot = s.get();
                    break;
                }  // reuse a departed slot
            if (slot == nullptr) {
                auto s = std::make_unique<session_t>();
                slot = s.get();
                slot->gate = gate_;
                slot->endpoint.owner_ = this;
                slot->endpoint.slot_ = slot;
                slots_.push_back(std::move(s));
            }
            slot->name = peer_name(fd);
            slot->asm_buf.clear();
            slot->fd = fd;
            ++slot->gen;  // the other claim edge — see on_handshake (#954)
            slot->open = true;
            newly_claimed = true;
        }
        peer = slot->name;
    }
    // Reclaim the slot on close — armed once, when first claimed (the free_ctx fires on the
    // httpd task at close). Outside peers_m_ so no httpd lock nests under ours.
    if (newly_claimed) {
        httpd_sess_set_ctx(req->handle, fd, slot, &httpd_ws_link_t::on_session_closed);
        // The other admission point, and the one that actually fires on IDF v6 (the
        // opening GET never reaches the handler there) — so the bound is applied here
        // too, on the same first-claim edge, and exactly once per session.
        bound_socket(fd);
    }

    // Reassembly — asm_buf is httpd-task-only, so no lock. The SPA sends one whole TLV
    // per unfragmented BINARY frame (the fast path); a fragmented message chains here.
    if (frame.type == HTTPD_WS_TYPE_BINARY && frame.final && slot->asm_buf.empty()) {
        deliver(peer, body);  // unfragmented: deliver borrowed, no extra copy
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_CONTINUE && slot->asm_buf.empty())
        return ESP_OK;  // stray CONTINUE with no assembly open — drop
    if (frame.type == HTTPD_WS_TYPE_BINARY)
        slot->asm_buf.clear();  // a BINARY mid-assembly discards the stale partial
    if (slot->asm_buf.size() + body.size() > kMaxFrameBytes) {
        slot->asm_buf.clear();
        return ESP_OK;  // reassembly would exceed the cap — drop the message
    }
    if (!slot->asm_buf.append(body)) {
        ESP_LOGW(kTag, "reassembly alloc failed - message dropped");
        return ESP_OK;  // nothrow growth failed: drop the message, keep the peer
    }
    if (frame.final) {
        // Take the message OUT of the slot before delivering it. Delivery runs the app
        // in-call and the app may destroy this link (#814) — after which `slot` and every
        // other member is freed, so the old clear()-after-deliver was a use-after-free on
        // the fragmented path. Nothing owned by the link is touched past this point.
        const asm_buf_t message = slot->asm_buf.take();
        deliver(peer, message.bytes());
    }
    return ESP_OK;
}

void httpd_ws_link_t::on_session_closed(void* ctx) {
    auto* const slot = static_cast<session_t*>(ctx);
    if (slot == nullptr || slot->gate == nullptr) return;
    gate_t* const gate = slot->gate;
    httpd_ws_link_t* owner = nullptr;
    std::string departed;
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
        departed = owner->reclaim_slot(slot);
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
    owner->notify_departed(departed);
    {
        // `owner` may be DESTROYED by now, exactly as at the tail of @ref ws_handler: the
        // notifier can drive an app teardown, and the barrier above is what let it start.
        // Only `gate`, which deliberately outlives the link, may be touched from here on.
        const std::lock_guard lock(gate->m);
        --gate->depth;
    }
    gate->cv.notify_all();
}

std::string httpd_ws_link_t::reclaim_slot(session_t* slot) {
    std::string departed;
    bool was_open;
    {
        const std::lock_guard lock(peers_m_);
        was_open = slot->open;
        departed = std::move(slot->name);
        slot->open = false;
        slot->fd = -1;
        slot->name.clear();
        slot->asm_buf.clear();
        slot->tx_drops = 0;
        // Slots are RECYCLED in place, so both TX verdicts about the departed peer must
        // be cleared with it. The dead mark especially: lwIP hands a descriptor NUMBER
        // straight back, so a mark left standing would refuse every frame to whichever
        // unrelated peer next landed on that number. This runs from `free_ctx` on the
        // httpd task — before that task can accept anything onto it — so the clear
        // strictly precedes any reuse.
        slot->dead = false;
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
    return departed;
}

void httpd_ws_link_t::notify_departed(std::string_view peer) {
    // Peer-named mode evicts just the departed peer's edges; flat mode has one peer's
    // departure BE the whole link down — the same fork transport_ws_server::teardown_slot
    // takes, and for the same reason (which sink the router installed).
    if (peer_named_)
        notify_peer_down(peer);
    else
        notify_down();
}

void httpd_ws_link_t::deliver(std::string_view peer, std::span<const std::byte> frame) {
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
    if (handle_ == nullptr) return;
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
    if (fd < 0) return;
    // Gather-copy the payload ONCE: httpd_queue_work is asynchronous, so the caller's
    // spans are gone by the time the httpd task runs tx_work. Fast path: claim a
    // pre-allocated pool slot (lock-free CAS) and gather straight into its inline
    // buffer — no allocation at all. Fallbacks, in order: a pooled shell with a
    // nothrow heap payload (frame > kTxInlineBytes), then a fully heap work item
    // (pool exhausted/absent). Nothrow END TO END on every arm — never a std::vector
    // for the copy: its THROWING allocator once defeated the `new (std::nothrow)`
    // guard on the shell, aborting the node under -fno-exceptions on a reply-sized
    // copy. An allocation failure is exactly the drop contract below: note_tx_result
    // counts it and the streak closes the session.
    std::size_t total = 0;
    for (const auto& part : iov) total += part.size();
    tx_work_t* work = nullptr;
    std::byte* dst = nullptr;
    if (tx_slot_t* const slot = claim_tx_slot(); slot != nullptr) {
        work = &slot->work;
        work->handle = handle_;
        work->gate = gate_;
        work->to = to;
        work->len = total;
        if (total <= kTxInlineBytes) {
            dst = slot->inline_buf;
        } else {
            work->owned.reset(new (std::nothrow) std::byte[total]);
            dst = work->owned.get();
            if (dst == nullptr) {  // overflow-payload OOM: recycle the slot, drop below
                release_tx_work(work);
                work = nullptr;
            }
        }
        if (work != nullptr) work->payload = dst;
    } else {
        std::unique_ptr<std::byte[]> buf(new (std::nothrow) std::byte[total]);
        if (buf != nullptr) {
            dst = buf.get();
            // If the shell allocation fails the initializer never runs, so `buf` is not
            // moved-from and frees itself on return — no leak either way.
            work = new (std::nothrow)
                tx_work_t{handle_, gate_, to, dst, total, nullptr, std::move(buf)};
        }
    }
    bool queued = false;
    if (work != nullptr) {  // work == nullptr => OOM: drop this frame (backpressure)
        std::byte* p = dst;
        for (const auto& part : iov) {
            if (!part.empty()) std::memcpy(p, part.data(), part.size());
            p += part.size();
        }
        // Publish the filled payload BEFORE the enqueue: the token may run the instant
        // httpd_queue_work posts it, and a token that finds the slot still CLAIMED would
        // conclude it has nothing to send. Everything the token reads was written above,
        // so the release here is what makes it visible (see tx_state_t).
        if (work->slot != nullptr) work->slot->arm();
        queued = httpd_queue_work(handle_, &httpd_ws_link_t::tx_work, work) == ESP_OK;
        if (!queued) {
            // A REFUSED enqueue — visible, and the one this path always handled. Take the
            // payload back before recycling: disarm failing means a token stranded by an
            // earlier claim of this slot fired in the window just above and is inside the
            // send, so the frame IS going out and the slot belongs to that token.
            if (work->slot == nullptr || work->slot->disarm())
                release_tx_work(work);  // could not enqueue — recycle/free, no leak
            else
                queued = true;
        }
    }
    // A frame that never reached the queue is charged to the LINK, not to this peer: a
    // refused enqueue is evidence about the shared control queue, and under #835's shape
    // the peer that saturated it is precisely the one NOT sending here. The session
    // streak is fed from tx_work, where the result names its destination.
    if (!queued) note_enqueue_drop(fd, total);
}

void httpd_ws_link_t::queue_send(const session_ref_t& to, std::span<const std::byte> frame) {
    // One-span sugar over the gather form — the single copy/backpressure locus.
    const std::span<const std::byte> one[] = {frame};
    queue_send(to, std::span<const std::span<const std::byte>>(one));
}

std::size_t httpd_ws_link_t::tx_slots_busy() const noexcept {
    if (tx_pool_ == nullptr) return 0;
    std::size_t busy = 0;
    for (std::size_t i = 0; i < kTxPoolSlots; ++i)
        if (tx_pool_[i].state.load(std::memory_order_relaxed) != tx_state_t::FREE) ++busy;
    return busy;
}

std::size_t httpd_ws_link_t::tx_slot_capacity() noexcept { return kTxPoolSlots; }

void httpd_ws_link_t::note_enqueue_drop(int fd, std::size_t bytes) {
    const std::uint32_t total = enqueue_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
    ESP_LOGW(kTag, "tx enqueue drop (queue full / OOM) fd=%d len=%u total=%u", fd, (unsigned)bytes,
             (unsigned)total);
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
    // safe against that lifetime because both callers run ON the httpd task (a tx_work
    // item and a send override invoked from one), the single task that accepts and closes
    // sockets, so the fd cannot be recycled underneath this call.
    if (::shutdown(fd, SHUT_RDWR) != 0) ESP_LOGW(kTag, "shutdown failed fd=%d (%d)", fd, errno);
    // Best-effort belt: if the control socket does have room, this reaps the session a
    // select cycle sooner. Its return is deliberately not trusted — see above — so a
    // failure is logged at debug and nothing depends on it.
    if (httpd_sess_trigger_close(handle_, fd) != ESP_OK)
        ESP_LOGD(kTag, "trigger_close not queued fd=%d (the shutdown carries the close)", fd);
}

void httpd_ws_link_t::note_tx_result(const session_ref_t& to, bool sent, std::size_t bytes) {
    bool close_now = false;
    std::string peer;
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
            slot->tx_drops = 0;  // a failure streak is CONSECUTIVE — any success resets it
            return;
        }
        if (slot->tx_drops < kMaxConsecutiveTxDrops) ++slot->tx_drops;
        close_now = slot->tx_drops >= kMaxConsecutiveTxDrops;
        peer = slot->name;
        fd = slot->fd;
        // Condemn UNDER the same lock that reached the verdict. Any later moment is a
        // window in which the fan-out enqueues more frames for a peer already known to be
        // broken, and each of those costs the httpd task a whole send bound.
        if (close_now) slot->dead = true;
    }
    if (!close_now) return;  // the drop itself is already logged by tx_work
    // The peer name comes from the slot, filled at admission. Never `getpeername` here:
    // the socket is by definition the one that is not working, and naming a peer at the
    // moment it is being struck must not depend on that socket answering.
    ESP_LOGW(kTag, "%u consecutive failed sends peer=%s fd=%d len=%u - closing session",
             (unsigned)kMaxConsecutiveTxDrops, peer.c_str(), fd, (unsigned)bytes);
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
    if (httpd_sess_set_send_override(handle_, fd, &httpd_ws_link_t::send_guarded) != ESP_OK)
        ESP_LOGW(kTag, "send override not installed fd=%d (short writes unguarded)", fd);
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
    const int ret = static_cast<int>(::send(fd, buf, len, flags));
    if (ret < 0) {
        const int err = errno;
        return err == EAGAIN || err == EWOULDBLOCK || err == EINTR ? HTTPD_SOCK_ERR_TIMEOUT
                                                                   : HTTPD_SOCK_ERR_FAIL;
    }
    if (static_cast<std::size_t>(ret) >= len) return ret;
    // A SHORT write: the bound expired with SOME of this buffer already on the wire.
    // esp_http_server checks only `ret < 0`, so returning the partial count here would
    // report a half-written WebSocket frame as a delivered one and leave the peer parsing
    // the remainder of the frame as the next frame's header — silent stream corruption,
    // for as long as the socket lives.
    //
    // Resolve the link the same way every other latched callback does: through the
    // session's slot and its gate, both of which deliberately outlive the link.
    auto* const slot = static_cast<session_t*>(httpd_sess_get_ctx(handle, fd));
    if (slot != nullptr && slot->gate != nullptr) {
        const std::lock_guard lock(slot->gate->m);
        if (httpd_ws_link_t* const owner = slot->gate->link; owner != nullptr)
            owner->note_send_desync(slot, static_cast<std::size_t>(ret), len);
    }
    return HTTPD_SOCK_ERR_FAIL;  // and report the failure IDF would otherwise have missed
}

void httpd_ws_link_t::note_send_desync(session_t* slot, std::size_t written, std::size_t len) {
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
    ESP_LOGE(kTag, "short write peer=%s fd=%d (%u of %u bytes) - closing: stream desynchronised",
             peer.c_str(), fd, (unsigned)written, (unsigned)len);
    // NOT the streak: a different fault class. The streak means "this peer keeps missing
    // whole frames"; this means "the byte stream is no longer parseable", and one
    // occurrence is already proof. Dropping the frame and keeping the socket — the #481
    // response — is only sound when ZERO bytes of it reached the wire.
    //
    // This close is ONE-SHOT by construction (the mark above makes a second call return
    // early), so it may not be an operation that can silently fail to happen — which the
    // control queue's is. @ref condemn is the one that cannot.
    condemn(fd);
}

void httpd_ws_link_t::tx_work(void* arg) {
    auto* const work = static_cast<tx_work_t*>(arg);
    // Take ownership of the slot's armed payload, or do nothing at all. A pooled item is a
    // TOKEN, not a frame: it says "send whatever slot i has armed", because it lives inside
    // that slot and cannot carry an identity a re-claim would not overwrite (see
    // tx_state_t). Winning this CAS is what makes every field below safe to read — it pairs
    // with the release in tx_slot_t::arm — and losing it is the whole reason a reclaimed
    // slot is harmless: the token simply has no work, and returns without touching one
    // byte of a slot that is now somebody else's. `work->slot` is stable for the link's
    // life (bound once in alloc_buffers; the abandon path leaks the pool rather than
    // freeing it, exactly so this stays true).
    if (work->slot != nullptr) {
        tx_state_t expected = tx_state_t::ARMED;
        if (!work->slot->state.compare_exchange_strong(expected, tx_state_t::RUNNING,
                                                       std::memory_order_acquire))
            return;
    }
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
    if (work->gate != nullptr) {
        const std::lock_guard lock(work->gate->m);
        if (httpd_ws_link_t* const owner = work->gate->link; owner != nullptr)
            fd = owner->live_fd(work->to);
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
        err = httpd_ws_send_frame_async(work->handle, fd, &f);
        if (err != ESP_OK) {
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
            ESP_LOGW(kTag, "ws send failed (%s) fd=%d len=%u - frame dropped", esp_err_to_name(err),
                     fd, (unsigned)work->len);
        }
    }
    // Copy out everything the accounting needs BEFORE the slot goes back to the pool:
    // once released, another task may claim it and overwrite the work item.
    gate_t* const gate = work->gate;
    const session_ref_t to = work->to;
    const std::size_t len = work->len;
    release_tx_work(work);  // recycle the pool slot, or free the heap-fallback shell
    // A skipped send is not evidence about anyone: no result. Every skip qualifies — the
    // peer departed, a different session now holds its slot, or it was condemned and the
    // verdict is already in.
    if (!live || gate == nullptr) return;
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
    // mutation of `slots_` is the APPEND at the two admission sites (the push_back in
    // on_handshake and the one in on_data_frame), and the two sites that remove entries —
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
                if (s->open && !s->dead) targets[n++] = session_ref_t{s.get(), s->gen};
            }
            more = next < slots_.size();
        }
        for (std::size_t i = 0; i < n; ++i) queue_send(targets[i], iov);
    }
}

void httpd_ws_link_t::peer_endpoint_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void httpd_ws_link_t::peer_endpoint_t::send(std::span<const std::span<const std::byte>> iov) {
    // The directed reply path (fwd_router hands the reply rope's iovec here): one
    // nothrow gather into the tx work item, no intermediate flatten temporary.
    if (owner_ == nullptr || slot_ == nullptr) return;
    // The endpoint is bound to a SLOT for the link's life, and slots outlive the sessions
    // they carry — so "which peer is this endpoint for" is only answered together with the
    // generation. A directed FWD reply resolved against a stale one would be delivered to
    // whichever tab reconnected onto the slot, on a link whose whole point is that a reply
    // reaches only the tab that asked (#954).
    session_ref_t to;
    {
        const std::lock_guard lock(owner_->peers_m_);
        if (!slot_->open || slot_->dead) return;  // departed or condemned => no-op
        to = session_ref_t{slot_, slot_->gen};
    }
    owner_->queue_send(to, iov);
}

// ---------------------------------------------------------------------------
// bus_link_t facet — peer enumeration / resolution (cross-thread reads).
// ---------------------------------------------------------------------------

void httpd_ws_link_t::enumerate_peers(const peer_visitor_t& visit) const {
    const std::lock_guard lock(peers_m_);
    for (const auto& s : slots_)
        if (s->open && !s->name.empty()) visit(s->name);
}

transport_t* httpd_ws_link_t::peer_link(std::string_view peer) {
    const std::lock_guard lock(peers_m_);
    for (const auto& s : slots_)
        if (s->open && s->name == peer) return &s->endpoint;
    return nullptr;
}

}  // namespace tr::net
