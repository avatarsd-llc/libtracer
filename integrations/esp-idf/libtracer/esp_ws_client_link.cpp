/**
 * @file
 * @brief Implementation of @ref tr::net::esp_ws_client_link_t — the ESP-IDF WebSocket
 *        *client* `transport_t`: raw bytes from `esp_transport_tcp`, framed by core (#2484).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer_esp/esp_ws_client_link.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/random.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "freertos/FreeRTOS.h"
#include "libtracer/config.hpp"
#include "libtracer/frame_ingress.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source_backend.hpp"
#include "libtracer/mem_string.hpp"
#include "libtracer/park.hpp"
#include "libtracer/sync.hpp"
#include "libtracer/wait.hpp"
#include "libtracer/ws.hpp"
#include "libtracer/ws_assembler.hpp"
#include "libtracer/ws_framing.hpp"

namespace tr::net {

namespace {
/** @brief Log tag for this link. */
constexpr const char* kTag = "ws_client_link";
#ifdef CONFIG_LIBTRACER_WS_CLIENT_DIAL_STATS
/** @brief Minimum spacing of the failed-dial WARN (#1606), microseconds: one line a minute
 *         at most, whatever the backoff. Only with the dial-stats option on (#1662). */
constexpr std::int64_t kDialWarnIntervalUs = 60LL * 1000 * 1000;
#endif

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
 * @brief What ONE dial may spend inside `esp_transport_connect` (ms) — half the watchdog
 *        window.
 *
 * It is no longer a teardown term (#1058). It used to be the dominant one: the recv
 * thread parked in `esp_transport_connect` and the destructor joined it, so a teardown
 * landing mid-dial paid the whole of this. The destructor now CONDEMNS an in-flight dial
 * and detaches the recv thread, and since #2787 no sender waits inside a write, so a
 * destroying task's worst case is @ref kPollMs: how fast the recv thread observes `stop_`.
 *
 * What this bound still sizes is the DIAL: how long a recv thread may sit on an
 * unreachable peer before it backs off and re-dials, and hence how long a CONDEMNED
 * dial's orphaned recv thread outlives the link that started it. Half the window keeps
 * that orphan comfortably shorter than the reconnect cadence and leaves the other half
 * for the write bound (a quarter) and whatever else the destroying task holds. Nothing
 * else in the loop holds a teardown up: the backoff (untimed since #2337) wakes on `stop_`,
 * and the poll turn is @ref kPollMs.
 */
constexpr int kDialTimeoutMs = kWatchdogMs / 2;

/**
 * @brief How many times the opening handshake's write can spend the write timeout.
 *
 * The handshake request is the one `esp_transport_write` this link still makes, on the dial's
 * own thread before the connection is adopted: `tcp_write`
 * (components/tcp_transport/transport_ssl.c) polls for writability with the caller's
 * timeout, then `send()`s, so the caller's number is a per-leg bound, never a total. A frame
 * never waits at all (#2787): it is one `send()` with `MSG_DONTWAIT`, see write_locked.
 */
constexpr int kIdfWriteLegs = 2;

/** @brief What the handshake's write may spend in total, all legs (ms) — a quarter of the
 *  watchdog window. */
constexpr int kWriteBudgetMs = kWatchdogMs / 4;

/** @brief Write timeout handed to IDF for the handshake (ms): the total budget divided by the
 *  legs IDF spends it on, so `kIdfWriteLegs * kWriteTimeoutMs == kWriteBudgetMs`. */
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
 * that responsiveness in a power budget only the embedder can see. Since #1058 it is the
 * DOMINANT teardown term rather than one of two — the dial no longer contributes at all,
 * and since #2787 neither does a write.
 */
#ifdef CONFIG_LIBTRACER_WS_CLIENT_POLL_MS
constexpr int kPollMs = CONFIG_LIBTRACER_WS_CLIENT_POLL_MS;
#else
constexpr int kPollMs = 200;
#endif

/**
 * @brief Backoff before re-dialing after a failed/lost connection (ms), measured on the
 *        application's `now` (@ref esp_ws_client_link_t::sweep, #2337): the link reads no
 *        clock and the wait has no timeout of its own.
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
 * @brief The tally's top bit: a destructor is waiting for the count under it to reach zero
 *        (#2338).
 *
 * The drain blocks on the tally itself (`std::atomic::wait`), so it needs no clock and no
 * slice, and a sender pays for the wake only when it is the one that matters: the last one
 * out while this bit is set. Every other exit is the one `fetch_sub` it always was. The
 * count under the bit is the number of senders, which never comes near 2^31.
 */
constexpr std::uint32_t kDrainWaiting = std::uint32_t{1} << 31;

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
    /**
     * @brief Lower the tally, releasing everything this sender did to the drain, and wake
     *        the drain if this was its last sender.
     *
     * The wake is this sender's last act, and the woken destructor may already be freeing
     * the object. `tr::park_wake` keys the wake on the tally's ADDRESS, in a table of its
     * own, and loads and stores nothing at it: the same address-only wake a mutex unlock
     * ends with, which is what lets any `std::mutex` be destroyed by the thread that just
     * locked it after another's unlock. It is not `std::atomic::notify_all`, whose
     * libstdc++ waiter table costs 2 KB of static RAM on esp32c6 (#2507).
     */
    ~sender_exit_t() {
        if (tally_.fetch_sub(1, std::memory_order_release) == (kDrainWaiting | 1))
            park_wake(tally_);
    }

    sender_exit_t(const sender_exit_t&) = delete;
    sender_exit_t& operator=(const sender_exit_t&) = delete;

   private:
    std::atomic<std::uint32_t>& tally_; /**< @brief esp_ws_client_link_t::senders_. */
};

/**
 * @brief A fresh RFC 6455 masking key for one client frame (#2265), from the source IDF's
 *        `_ws_write` drew it from. On the chip `getrandom` is the hardware RNG and does not
 *        fail; a failure elsewhere gives a zero key, which still frames correctly.
 */
std::uint32_t mask_key() noexcept {
    std::uint32_t key = 0;
    return getrandom(&key, sizeof(key), 0) == static_cast<ssize_t>(sizeof(key)) ? key : 0;
}

/**
 * @brief Close and destroy one TCP transport, and null the handle — the ONE release every
 *        owner of a connection uses (#1606).
 *
 * Closing a transport whose dial failed is harmless; closing one whose handshake landed
 * takes the connection down on the peer's side too. A null handle is skipped.
 */
void release_tcp(esp_transport_handle_t& tcp) noexcept {
    if (tcp == nullptr) return;
    esp_transport_close(tcp);
    esp_transport_destroy(tcp);
    tcp = nullptr;
}

/**
 * @brief The most one opening-handshake response may take, bytes, before the dial gives up
 *        on it (RFC 6455 §4.1): a `101` from core's server or `esp_http_server` is about
 *        130 B, and 1024 B is what `esp_transport_ws` defaulted to, so a server or proxy
 *        with a longer answer still connects (#2515). What a server pipelined behind its
 *        header and still fits is kept and framed.
 *
 * Drawn per dial from `memory.state` and returned once the connection's first bytes are
 * framed, so no connection holds it (#1606). The request is built in the same block, which
 * is drawn larger when a long `ws_path` or header block needs it.
 */
constexpr std::size_t kHandshakeBytes = 1024;

}  // namespace

/**
 * @brief The state one dial owns — everything an in-flight `esp_transport_connect` needs,
 *        held where a destroyed link cannot take it away (#1058).
 *
 * The reason it is not simply the link's own members: the destructor no longer joins a
 * dial. `esp_transport_connect(handle, host, port, timeout_ms)` is IDF's whole surface —
 * no cancellation, no second entry point — and `esp_transport_get_socket` only names a
 * descriptor AFTER the connect returns, so there is nothing to shut down from outside
 * while the dial is parked. The dial therefore keeps running after the link is gone, and
 * every byte it is still reading has to belong to this slot rather than to the corpse:
 * the pre-#1058 call passed `host_.c_str()` and `cfg.headers = handshake_headers_.c_str()`
 * — pointers INTO members — to a call that blocks for up to `kDialTimeoutMs`.
 *
 * Since #1880 the slot is the link's ONLY copy of those three strings, drawn with the slot
 * itself from `memory.state`, and it records that store so whichever party frees it last —
 * the destructor, or a condemned dial — returns it there.
 */
struct esp_ws_client_link_t::dial_t {
    /** @brief Guards the two flags below and the handle hand-off. A LEAF: never taken with
     *         `write_m_`, `st_m_` or `backoff_m_` held, and never held across the connect. */
    tr::mutex_t m;
    /** @brief True between the publish and the resolve — i.e. exactly while a thread is
     *         inside `esp_transport_connect` on this link's behalf. */
    bool in_flight = false;
    /** @brief The link that started this dial is gone. Set by the destructor under `m`
     *         when it finds `in_flight`; the dial reads it at the resolve and releases the
     *         transport pair itself. */
    bool condemned = false;
    mem::block_source_t* const src; /**< @brief The store this slot and its strings came from. */
    mem::string_t host;             /**< @brief COPY of the link's host — see the note above. */
    mem::string_t ws_path;          /**< @brief COPY of the requested WS URI. */
    mem::string_t headers;          /**< @brief COPY of the handshake header lines, "" = none. */
    const std::uint16_t port;       /**< @brief COPY of the peer's TCP port. */
    /** @brief All three strings were copied; false means @ref src refused one. */
    const bool copied;
    esp_transport_handle_t tcp = nullptr; /**< @brief The connection being built, until the
                                           *         resolve hands it to the link or the
                                           *         orphan releases it. */
    /** @brief The handshake's request, then its response; the link frames what the server
     *         pipelined behind the response's header, then returns it (#2484). */
    fixed_array_t<std::byte> hs;
    std::size_t pipelined_from = 0; /**< @brief Where those pipelined bytes start in @ref hs. */
    std::size_t pipelined_to = 0;   /**< @brief Where they end. */

    /** @brief Copy the dial's inputs out of the caller's config, once, at construction. */
    dial_t(mem::block_source_t& from, std::string_view h, std::uint16_t p, std::string_view path,
           std::string_view hdrs) noexcept
        : src(&from),
          host(from),
          ws_path(from),
          headers(from),
          port(p),
          copied(host.assign(h) && ws_path.assign(path) && headers.assign(hdrs)) {}

    /**
     * @brief The RFC 6455 opening handshake on the connected @ref tcp: send the Upgrade
     *        request, read the response's header, check its `101` and Sec-WebSocket-Accept.
     *
     * Touches nothing but this slot, so a dial condemned mid-handshake finishes it safely
     * (#1058). The request carries @ref headers verbatim after the standard lines (#959).
     * Each read is bounded by the dial timeout and the response by @ref hs, so a peer that
     * never answers costs this dial a bounded wait, as `esp_transport_ws`'s own connect did.
     *
     * @return Whether the server accepted; on true, @ref pipelined_from and
     *         @ref pipelined_to name the frame bytes it sent behind its header.
     */
    [[nodiscard]] bool upgrade() noexcept {
        std::array<std::uint32_t, 4> nonce{};
        for (std::uint32_t& w : nonce) w = mask_key();
        std::array<char, ws::base64_len(sizeof(nonce))> key_text{};
        const std::string_view key(key_text.data(),
                                   ws::base64_into(std::as_bytes(std::span(nonce)), key_text));
        std::array<char, 8> port_text{};
        const std::string_view port_view(
            port_text.data(),
            static_cast<std::size_t>(
                std::to_chars(port_text.data(), port_text.data() + port_text.size(), port).ptr -
                port_text.data()));
        const std::string_view parts[] = {
            "GET ",
            ws_path.view(),
            " HTTP/1.1\r\nHost: ",
            host.view(),
            ":",
            port_view,
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ",
            key,
            "\r\nSec-WebSocket-Version: 13\r\n",
            headers.view(),
            "\r\n",
        };
        std::size_t len = 0;
        for (const std::string_view part : parts) len += part.size();
        if (!hs.assign(*src, std::max(len, kHandshakeBytes))) return false;
        char* const text = reinterpret_cast<char*>(hs.data());
        std::size_t at = 0;
        for (const std::string_view part : parts) {
            std::memcpy(text + at, part.data(), part.size());
            at += part.size();
        }
        if (esp_transport_write(tcp, text, static_cast<int>(len), kWriteTimeoutMs) !=
            static_cast<int>(len))
            return false;
        // The response, read into the same block. Whatever arrives past the header's CRLFCRLF
        // is already the frame stream, so it is kept for the link to frame.
        for (std::size_t have = 0; have < hs.size();) {
            const int n = esp_transport_read(tcp, text + have, static_cast<int>(hs.size() - have),
                                             kDialTimeoutMs);
            if (n <= 0) return false;
            have += static_cast<std::size_t>(n);
            const std::string_view got(text, have);
            const std::size_t end = got.find("\r\n\r\n");
            if (end == std::string_view::npos) continue;
            pipelined_from = end + 4;
            pipelined_to = have;
            return ws::upgrade_accepted(got.substr(0, pipelined_from), key);
        }
        return false;  // no header within the budget: not a handshake
    }
};

/**
 * @brief The link's end of core's RFC 6455 receive rules (@ref ws_framing_t, #2484): the one
 *        connection's frame and fragment state, the link's cap and counters, masked control
 *        replies through the one write path, and delivery to the receiver.
 *
 * Lives on the recv thread's stack for the life of the thread; only that thread touches it.
 */
struct esp_ws_client_link_t::framing_end_t {
    esp_ws_client_link_t& link; /**< @brief The link (its counters, receiver and writer). */
    mem::mem_backend_t& rx;     /**< @brief Where every body is drawn, and where a fragmented
                                 *          message is flattened for a span-only sink. */
    bool& exchanged;            /**< @brief The recv loop's "this connection delivered" fact. */
    ws_rx_frame_t frame;        /**< @brief The header of the frame being received. */
    ws_assembler_t assembler;   /**< @brief Fragment reassembly. */

    /** @brief The receive cap: `rx_bytes`, one frame's payload and a fragmented message's
     *         charged cost. */
    [[nodiscard]] std::size_t cap() const { return link.rx_bytes_; }
    /** @brief Count a frame that broke the rules; the connection is closed (#872, #1060). */
    void malformed() const { link.drops_.count_malformed_rx(); }
    /** @brief Count a message shed: a refused body or chain (receiver pays). */
    void dropped() const { link.drops_.count_dropped_rx(); }
    /** @brief Answer a PING with a PONG echoing its payload. */
    void pong(std::span<const std::byte> payload) const { reply(ws::opcode_t::PONG, payload); }
    /** @brief Answer a CLOSE with a CLOSE echoing its payload (RFC 6455 §5.5.1); the
     *         connection is then dropped. */
    void closing(std::span<const std::byte> payload) const { reply(ws::opcode_t::CLOSE, payload); }

    /**
     * @brief A masked control reply, sent on the control lane like any frame from no vertex
     *        (#2787): a write may have left part of a frame on the wire, so a reply written
     *        around the egress would land inside it. The header rules held @p payload to
     *        125 bytes (§5.5).
     */
    void reply(ws::opcode_t op, std::span<const std::byte> payload) const {
        const std::span<const std::byte> one[1] = {payload};
        link.send_on(one, graph::delivery_lane_t{}, static_cast<std::uint8_t>(op));
    }

    /**
     * @brief Hand one whole message up: owning to a link given `memory.rx` (#1661), borrowed
     *        otherwise, flattened once when it came in fragments. An empty message carries no
     *        TLV and is not delivered.
     *
     * Counted and marked BEFORE the delivery, which runs the app in-call on this thread: the
     * app may send from it, and no lock is held across it.
     */
    void hand_up(view::rope_t message) const {
        const std::size_t len = message.total_length();
        if (len == 0) return;
#ifdef CONFIG_LIBTRACER_LINK_TRAFFIC_STATS
        {
            const std::lock_guard<tr::mutex_t> lk(link.st_m_);
            ++link.traffic_.rx_frames;
            link.traffic_.rx_bytes += static_cast<std::uint32_t>(len);
            link.traffic_.last_rx_us = esp_timer_get_time();
        }
#endif
        // This connection has now proved the peer admitted it, so its death is a DROP to
        // retry at once rather than a refusal to back off from (#1128).
        exchanged = true;
        if (link.rx_backend_ != nullptr) {
            // A message no sink took (a span-only sink whose materialize the rx source
            // refused, #2504) is a dropped_rx, as on the borrowed arm below.
            link.drops_.count_dropped_rx(static_cast<std::size_t>(
                !link.rx_.deliver_rope(std::move(message), *link.rx_backend_)));
            return;
        }
        const std::expected<view::view_t, view::flatten_err_t> flat = message.try_materialize(rx);
        if (!flat) {
            link.drops_.count_dropped_rx();
            return;
        }
        link.rx_.deliver_borrowed(flat->bytes());
    }
};

esp_ws_client_link_t::esp_ws_client_link_t(std::string_view host, std::uint16_t port,
                                           const esp_ws_client_config_t& config)
    // Drawn ONCE, here, and reused by every dial — not per dial: the three strings never
    // change for the link's life, so one slot serves them all and the file's "NO per-frame
    // allocation" posture is untouched.
    : dial_(mem::make_in<dial_t>(config.memory.state_or_default(), config.memory.state_or_default(),
                                 host, port, config.ws_path, config.handshake_headers)),
      rx_backend_(config.memory.rx),
      // The send side's store (#1661): each send's frame, the egress rings and the base
      // class's gather temporary draw from the application's `memory.io`, and a null `io`
      // resolves to the net sub-pool (the static arena on this target) as on every other
      // link (#2265). The host client (`ws_client_transport_t`) resolves it the same way.
      tx_src_(&config.memory.io_or_default()),
      rx_bytes_(config.rx_bytes),
      tx_bytes_(config.tx_bytes),
      recv_stack_(config.recv_stack),
      held_(*tx_src_, tx_bytes_),
      egress_(*tx_src_) {
    // The dial slot is drawn at setup, so a store that cannot hold it is a sizing bug, never
    // a runtime condition (ADR-0083): the constructor stops here, before the recv thread
    // that would read it exists. Nothing connection-sized is drawn here (#1606): the
    // handshake block is drawn by each dial and returned once the connection's first bytes
    // are framed.
    mem::block_source_t& state = config.memory.state_or_default();
    if (dial_ == nullptr || !dial_->copied)
        mem::exhausted_at_init(state, "esp_ws_client_link_t dial slot");
    if (config.memory.io != nullptr) set_egress_source(*config.memory.io);
    egress_.open(peer_);  // one peer for the link's life: a re-dial is the same peer
    // Every member the recv thread reads is initialized ABOVE this line, which is the
    // whole of #959: the thread start_receiving() spawns dials at once, so a knob delivered
    // after the spawn is a data race, and for a handshake token it also leaves it undefined whether
    // the first dial carries one — and a dial WITHOUT one is what an admission hook refuses.
    // `handshake_headers_` was that knob; it is a ctor argument and `const` now, so the
    // ordering is a property of the type rather than a request in a doc comment. Nothing
    // may be added after construction that the recv thread also reads. The thread is
    // spawned at the owner's arm, never here (#2249).
}

bool esp_ws_client_link_t::start_receiving() {
    // Idempotent, and safe on a link that never connected: `transport_vertex_t::
    // make_connection` arms every link unconditionally once the receiver is installed, so
    // a second call finds the exchange returning true and does nothing. The latch is never
    // cleared (one-shot): a re-dial only happens on a link that was already armed, so
    // reconnects need no re-gating (ADR-0081, #1102). A link already shut down stays down.
    //
    // The latch IS the spawn (#1606): a deferred link used to spawn its thread in the
    // ctor and park it on a condition variable until this call, holding a whole recv stack
    // for a link that could not dial. Now nothing exists to park. The recv thread owns
    // dialing + the read loop; the caller never blocks on the network. recv_stack==0 uses
    // the pthread default; any other value is APPLIED — this thread runs in-call delivery
    // through the graph's on_write seam, so a node that knows its delivery depth must be
    // able to size it (#900).
    if (stop_.load(std::memory_order_acquire) || armed_.exchange(true, std::memory_order_acq_rel))
        return true;
    // The stack comes from the process thread-stack source (`mem::stack_source()`, its own
    // Kconfig-sized region on an arena build), never a source that dies with this link:
    // shut_down may detach the thread mid-dial, and a detached static task's block is reaped
    // later (static_thread_t::detach, #2288). A refused draw is reported to the owner, which
    // rolls the link back, rather than leaving a link that never dials.
    return recv_thread_.start(&esp_ws_client_link_t::recv_entry, this, recv_stack_, "ws_cli_rx",
                              mem::stack_source());
}

void esp_ws_client_link_t::recv_entry(void* self) {
    static_cast<esp_ws_client_link_t*>(self)->recv_loop();
}

void esp_ws_client_link_t::shut_down() {
    // Disarm the link BEFORE waiting on anything. `stop_` is what every blocking wait
    // in the recv loop is predicated on, and it is also what a sender re-reads after it
    // gets `write_m_` — so from here on no NEW work reaches the handles. Clearing
    // `connected_` makes link_up() honest for the rest of teardown; the recv loop's own
    // stop_ re-check keeps it from reading that as "re-dial" (see recv_loop). The exchange
    // makes a second disarm — the destructor after an owner's shut_down — a no-op.
    if (!stop_.exchange(true, std::memory_order_acq_rel)) {
        connected_.store(false, std::memory_order_release);
        {
            // Taking the backoff mutex before notifying is what makes the wakeup safe
            // against a recv thread that has evaluated the predicate but not yet parked:
            // either it is already waiting (and this notify reaches it), or it evaluates
            // the predicate after this store and never waits at all.
            const std::lock_guard<tr::mutex_t> lk(backoff_m_);
        }
        backoff_cv_.notify_all();
        // CONDEMN an in-flight dial rather than wait for it (#1058). `esp_transport_connect`
        // takes no cancellation, so the dial cannot be stopped — but the destroying task does
        // not have to pay for it. Taking `dial_->m` is the LINEARIZATION POINT of the whole
        // scheme, and it has no window on either side: `stop_` was stored above, BEFORE this
        // lock, and connect_once() publishes `in_flight` under this same lock only after
        // re-reading `stop_` under it. So either the recv thread gets here first and this sees
        // an in-flight dial to condemn, or this gets here first and that dial never starts.
        //
        // A condemned dial owns its transport pair (the members are null for the whole of a
        // dial) and releases it when the call resolves, so detaching leaks nothing: what it
        // costs is one recv-thread stack — the one already allocated, no new thread — held for
        // the remainder of kDialTimeoutMs. That is the price of not charging this task for a
        // blocking call it cannot interrupt, and it is stated in the header's threading note.
        {
            const std::lock_guard<tr::mutex_t> lk(dial_->m);
            if (dial_->in_flight) {
                dial_->condemned = true;
                dial_orphaned_ = true;
            }
        }
    }
    // Deferred on a thread that runs link callbacks (the recv thread among them): it only
    // shuts down. The destructor joins the recv thread and releases the connection.
    if (on_callback_thread()) return;
    if (dial_orphaned_ && recv_thread_.joinable()) {
        ESP_LOGI(kTag, "torn down mid-dial: the dial is orphaned, not waited out");
        recv_thread_.detach();
    } else if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    {
        // The recv thread has stopped (or been condemned and detached, in which case it
        // touches nothing on this object ever again), but it was only ONE of the two
        // handle users:
        // the header's contract is that send() may be called from any task, and the
        // pre-#952 destructor destroyed the handles with `write_m_` held nowhere. A
        // sender inside esp_transport_write holds this lock for the whole call, so
        // acquiring it means no sender is inside — and every sender that acquires it
        // afterwards finds `stop_` set (and `connected_` cleared, both stored above,
        // before this lock was taken) and leaves without reading either handle.
        // A connection standing at the stop is released here: the recv loop breaks on
        // `stop_` before its departure arm, which releases every other one.
        // `connected_` is cleared AGAIN, here, beside the release: a dial that had already
        // resolved when the top-of-function store ran was not condemned, so it may have
        // stored `true` since, and the joined thread then broke on `stop_` without a
        // departure arm. The last store wins, so link_up() is false once this returns.
        const std::lock_guard<tr::mutex_t> lk(write_m_);
        release_tcp(tcp_);
        connected_.store(false, std::memory_order_release);
    }
}

esp_ws_client_link_t::~esp_ws_client_link_t() {
    // Shut down now, free here (#1606): an owner that already called shut_down() at the
    // link's removal finds the disarm done, and the destructor — at the graph's collect() —
    // joins the recv thread if that shut_down was deferred, waits out the senders and
    // returns the memory. Destructors always join, off every callback thread.
    assert(!on_callback_thread() && "a link's destructor runs off every callback thread");
    shut_down();
    // The slot has exactly one owner left (#1880): a condemned dial returns it itself as it
    // resolves, so only a slot with no dial in flight is this destructor's to return.
    if (!dial_orphaned_) mem::drop_in(*dial_->src, dial_);
    // Drain the senders that ANNOUNCED themselves on the tally. Holding `write_m_` for
    // this would deadlock — that is the lock they are queued on — so the wait is here,
    // after it. Each one wakes, sees the disarmed link, and leaves; only then is it safe
    // for `write_m_` itself (and the buffers) to be destroyed under them. The wait is
    // short: a sender inside the transport is in one non-blocking `send()` (#2787).
    //
    // The tally, not `send()`'s first instruction, is the boundary — see sender_exit_t.
    // A caller that entered `send()` but whose raise lands after the last load below is
    // NOT waited out, and neither is one that has yet to call at all; both are the same
    // embedder-owned lifetime residual, not something this loop can widen its way into.
    //
    // The wait is on the tally (#2338): the bit says a drain is waiting, the last sender out
    // under it wakes the drain, and `park_wait` returns at once if the count moved after the read
    // it was handed, so no wake is lost between the read and the block.
    std::uint32_t n = senders_.fetch_or(kDrainWaiting, std::memory_order_acquire);
    while ((n & ~kDrainWaiting) != 0) {
        park_wait(senders_, n, std::memory_order_acquire);
        n = senders_.load(std::memory_order_acquire);
    }
    // Every sender has left, so no writer holds the role: the close only returns what is
    // left in the rings to `tx_src_` before that store can go.
    egress_.close(peer_);
}

esp_ws_client_link_t::dial_outcome_t esp_ws_client_link_t::connect_once() {
    // Build a FRESH TCP transport for every dial, and do the opening handshake on it here,
    // in the slot (#2484): the link reads raw bytes and core frames them, so no transport
    // holds frame state across a reconnect. No connection is standing here: a failed dial
    // releases its own below, and a lost connection's is released by the recv loop's
    // departure arm (#1606).
    //
    // PUBLISH the dial (#1058). From the lock below until the resolve at the bottom of
    // this block, this thread touches NOT ONE member of the link — everything the dial
    // needs is in the slot, whose lifetime is not this object's (#1880). That is
    // what lets the destructor detach instead of joining: the corpse is never read.
    //
    // The lock is the linearization point against the destructor, which stores `stop_`
    // BEFORE it takes this mutex. Either this side wins and publishes an in-flight dial
    // for the destructor to condemn, or the destructor wins and this load sees the stop
    // and never dials at all. There is no third interleaving and hence no window.
#ifdef CONFIG_LIBTRACER_WS_CLIENT_DIAL_STATS
    // #1606 ask 1: the attempt is counted whatever it ends as.
    dial_attempts_.fetch_add(1, std::memory_order_relaxed);
#endif
    dial_t* const slot = dial_;
    {
        const std::lock_guard<tr::mutex_t> lk(slot->m);
        if (stop_.load(std::memory_order_acquire)) return dial_outcome_t::FAILED;
        slot->in_flight = true;
    }

    int rc = -1;
    std::int64_t dial_us = 0;
    slot->tcp = esp_transport_tcp_init();
    if (slot->tcp != nullptr) {
        // TCP connect, then the RFC 6455 opening handshake for ws_path (dial_t::upgrade), with
        // the optional handshake auth lines a peer's admission hook can gate on (a b2b dial
        // carries no browser cookie). Both read only the SLOT, never a member — the #1058
        // half of #959: a destructor that lands mid-dial condemns this call and detaches
        // rather than joining it, so a pointer into a member would dangle. Every re-dial
        // re-reads the same bytes, so a reconnect still re-authenticates.
        //
        // The connect bound is DERIVED (kDialTimeoutMs), not a literal. Timed, too: TCP
        // connect + the opening exchange is an UPPER BOUND on ~2x RTT, and it is the only
        // latency fact this link can offer without an active probe (stats_t).
        const std::int64_t dial_t0 = esp_timer_get_time();
        rc = esp_transport_connect(slot->tcp, slot->host.c_str(), static_cast<int>(slot->port),
                                   kDialTimeoutMs);
        if (rc == 0 && !slot->upgrade()) rc = -1;
        dial_us = esp_timer_get_time() - dial_t0;
    }

    // RESOLVE. `condemned` is read under the same mutex the teardown set it under, so a
    // condemning shut_down and this resolve cannot both miss each other: once `in_flight`
    // is cleared here, a teardown joins this thread instead, and nothing else touches the
    // slot's pair — so the pair is this thread's alone below, lock or no lock.
    bool orphaned = false;
    {
        const std::lock_guard<tr::mutex_t> lk(slot->m);
        slot->in_flight = false;
        orphaned = slot->condemned;
    }
    const bool up = !orphaned && rc == 0;
    // Invariant 1 of the #1058 ruling, structural rather than remembered: the connection this
    // dial built is released by EXACTLY ONE owner. A dial that did not come up releases it
    // right here, on the failure path (#1606 ask 2) — an orphan, a failed connect and a
    // refused handshake alike, closing first so a landed TCP connect is taken down on the
    // peer's side too. A dial that came up hands it to the link, whose departure arm releases
    // it. The handshake block goes back with a failed dial; an orphan's goes with its slot.
    if (!up) {
        release_tcp(slot->tcp);
        if (!orphaned) slot->hs.reset();
    }
    if (orphaned) {
        // The link is gone, so this dial is the slot's last owner and returns it — with its
        // mutex released, never from inside it. No log (the strings go with the slot) and no
        // counter bump: this thread has no object left to talk about. The caller must
        // return at once.
        mem::drop_in(*slot->src, slot);
        return dial_outcome_t::ORPHANED;
    }
    if (!up) {
#ifdef CONFIG_LIBTRACER_WS_CLIENT_DIAL_STATS
        const std::uint32_t failures = dial_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
        // #1606 ask 1: the failure arm is no longer silent. Rate-limited, because a peer
        // that stays unreachable fails once per backoff forever; the counter carries the
        // exact tally, the log only has to say it is happening.
        const std::int64_t now = esp_timer_get_time();
        if (failures == 1 || now - last_dial_warn_us_ >= kDialWarnIntervalUs) {
            last_dial_warn_us_ = now;
            ESP_LOGW(kTag, "dial ws://%s:%u%s failed (%u failures so far)", dial_->host.c_str(),
                     static_cast<unsigned>(dial_->port), dial_->ws_path.c_str(),
                     static_cast<unsigned>(failures));
        }
#endif
        // With the option off (the default, #1662) a failed dial says nothing: counting and
        // logging are the application's choice, and it did not make one.
        return dial_outcome_t::FAILED;
    }
    // Adopted with no lock, deliberately: taking `write_m_` here would put this in front of
    // every sender, which is the stall #952 is about. The handle is
    // PUBLISHED by the `connected_` release store at the end of this function, and nothing
    // off this thread may read them without first observing that store (see send()).
    tcp_ = slot->tcp;
    slot->tcp = nullptr;
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
    // The per-connection counters reset here; `last_rx_us` goes back to "never" rather
    // than carrying the previous session's staleness forward.
#ifdef CONFIG_LIBTRACER_LINK_TRAFFIC_STATS
    {
        const std::lock_guard<tr::mutex_t> lk(st_m_);
        traffic_ = {};
    }
#endif
    tx_drops_.store(0, std::memory_order_relaxed);
    held_.restart_peaks();
    connected_at_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
    connect_ms_.store(static_cast<std::uint32_t>(dial_us / 1000), std::memory_order_relaxed);
    reconnects_.fetch_add(1, std::memory_order_relaxed);
    // The came-up fact latches here and is never cleared (#1059/#1203): `ok()` reports
    // "a handshake landed at least once", `connected_` (published release, below) reports
    // whether one is standing NOW. Relaxed — it is a hint, not a synchronisation point —
    // but stored ahead of the RELEASE publication below, so any observer that acquires
    // `connected_ == true` necessarily also sees the came-up fact.
    came_up_.store(true, std::memory_order_relaxed);
    connected_.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "connected ws://%s:%u%s", dial_->host.c_str(),
             static_cast<unsigned>(dial_->port), dial_->ws_path.c_str());
    return dial_outcome_t::UP;
}

esp_ws_client_link_t::stats_t esp_ws_client_link_t::stats() const {
    stats_t out;
#ifdef CONFIG_LIBTRACER_LINK_TRAFFIC_STATS
    {
        // st_m_, NOT write_m_: write_m_ serializes the socket calls, and a snapshot
        // taken under it would queue any caller — the embedder's periodic publisher,
        // which may hold its own lock across this call — behind them. This mutex is only ever
        // held for a counter bump or this copy, so the snapshot is bounded-brief.
        const std::lock_guard<tr::mutex_t> lk(st_m_);
        out.c.rx_frames = traffic_.rx_frames;
        out.c.rx_bytes = traffic_.rx_bytes;
        out.c.tx_frames = traffic_.tx_frames;
        out.c.tx_bytes = traffic_.tx_bytes;
        out.c.last_rx_us = traffic_.last_rx_us;
    }
#endif
    // Relaxed loads, one by one: none of these is used to check another, and each is
    // written only on a failure, at a (re)connect or, for the queue peaks, when a frame has
    // to wait (#2464), never per frame written (#1663).
    out.c.tx_drops = tx_drops_.load(std::memory_order_relaxed);
    out.c.connected_at_us = connected_at_us_.load(std::memory_order_relaxed);
    out.c.tx_queue_peak = held_.peak_frames();
    out.tx_queue_peak_bytes = held_.peak_bytes();
    out.reconnects = reconnects_.load(std::memory_order_relaxed);
#ifdef CONFIG_LIBTRACER_WS_CLIENT_DIAL_STATS
    out.dial_attempts = dial_attempts_.load(std::memory_order_relaxed);
    out.dial_failures = dial_failures_.load(std::memory_order_relaxed);
#endif
    out.connect_ms = connect_ms_.load(std::memory_order_relaxed);
    // Filled from the tally's `dropped_rx`, not kept in a second counter: the receive path already
    // tallies every inbound discard there (#953/#901), and a second counter bumped at the same
    // sites could only drift from it. `c.rx_drops` and `drop_stats().dropped_rx` are therefore two
    // spellings of one number, never two numbers. Saturating, because the block is 32-bit and the
    // tally is word-wide — on a 64-bit host a link that really dropped 4 billion messages reports
    // the cap here and the exact figure through `drop_stats().dropped_rx`.
    const std::uint64_t rx_dropped = drops_.snapshot().dropped_rx;
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

std::uint64_t esp_ws_client_link_t::sweep(std::uint64_t now, std::uint64_t /*deadline*/) {
    constexpr std::uint64_t backoff = kReconnectBackoffMs;
    std::uint64_t next = 0;
    bool due = false;
    {
        const std::lock_guard<tr::mutex_t> lk(backoff_m_);
        clocked_ = true;  // from here on the app's `now`, not a send, ends the backoff
        // The first call that finds the recv thread backing off stamps the end; a call at or
        // past the stamp ends it. A stamp is never `now`, so one call cannot do both.
        if (backing_off_ && retry_at_ == 0) retry_at_ = now + backoff;
        due = backing_off_ && now >= retry_at_;
        if (due) {
            backing_off_ = false;
            retry_at_ = 0;
        }
        // Down and not parked (a dial in flight, or about to start): nothing is stamped yet,
        // so ask to be called one backoff from now, when a failed dial will be waiting.
        const bool down = armed_.load(std::memory_order_acquire) &&
                          !stop_.load(std::memory_order_acquire) &&
                          !connected_.load(std::memory_order_acquire);
        next = backing_off_ ? retry_at_ : (down ? now + backoff : 0);
    }
    if (due) backoff_cv_.notify_all();
    return next;
}

void esp_ws_client_link_t::drop() {
    // Close under the syscall serializer so an in-flight send() cannot touch a
    // half-closed handle, and mark down so the recv loop re-dials (which rebuilds
    // the transport pair — see connect_once).
    //
    // The departure REPORT is not made here, and deliberately: this is called with
    // `write_m_` about to be taken, and `notify_down` re-enters the routing plane (the
    // notifier takes graph locks), which the transport contract forbids doing under an
    // internal lock. It is also not the only way down — shut_down() clears `connected_`
    // without ever calling this. The one place that observes
    // EVERY transition to down is the recv loop's own `!connected_` arm, so that is
    // where the report is made (see recv_loop, #957).
    const std::lock_guard<tr::mutex_t> lk(write_m_);
    if (tcp_ != nullptr) esp_transport_close(tcp_);
    connected_.store(false, std::memory_order_release);
}

/**
 * @brief The egress's write step (#2787): one frame from @p off, through write_locked. One
 *        type for a sender's turn and the recv loop's, so both share one drain.
 */
struct esp_ws_client_link_t::resume_t {
    esp_ws_client_link_t& link; /**< @brief The link written to. */
    /** @brief Write @p f from byte @p off. */
    tr::net::egress_step_t operator()(const tr::net::egress_frame_t& f, std::size_t off) const {
        return link.write_locked(f, off);
    }
};

void esp_ws_client_link_t::send(std::span<const std::byte> frame) {
    // One span through the gathered path — the one implementation (#1620 (a)).
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void esp_ws_client_link_t::send(std::span<const std::span<const std::byte>> iov) {
    send_on(iov, graph::delivery_lane_t{});  // no vertex: the control lane
}

void esp_ws_client_link_t::send(std::span<const std::span<const std::byte>> head,
                                const graph::value_t& value, const graph::delivery_lane_t& lane) {
    // The base lowering's stack table (#1620 (c)), kept here only to carry the lane through.
    std::array<std::span<const std::byte>, 8> inline_iov;
    iov_table_t<std::span<const std::byte>> table(inline_iov, egress_source());
    const std::size_t n = head.size() + value.link_count();
    std::span<const std::byte>* const iov = table.acquire(n);
    if (iov == nullptr) {
        tx_drops_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::copy(head.begin(), head.end(), iov);
    std::transform(value.links().begin(), value.links().end(), iov + head.size(),
                   [](const view::view_t& l) { return l.bytes(); });
    send_on(std::span<const std::span<const std::byte>>(iov, n), lane);
}

void esp_ws_client_link_t::send_on(std::span<const std::span<const std::byte>> iov,
                                   const graph::delivery_lane_t& lane, std::uint8_t op) {
    std::size_t total = 0;
    for (const std::span<const std::byte>& part : iov) total += part.size();
    // A drop is a relaxed atomic bump, never a lock (#1663 took the drop half off st_m_).
    // This early-out stays AHEAD of the sender tally and of write_m_ (#952 ordering): it
    // reads nothing the destructor can be racing. Counted without any lock held. An empty
    // payload is a frame like any other (a PONG to an empty PING is one).
    if (total > tx_bytes_) {  // drop oversize
        // This is the `tx_bytes` CEILING, and this is the only place that can
        // name it. `transport_t::send` returns void, so the router cannot be told the frame
        // died; `tx_drops_` says one did, but a send to a peer that is down and a frame a
        // full ring sheds bump that same counter, so a bump alone does not even say WHICH drop
        // this was, let alone which knob was too small. A per-frame ceiling nobody can see
        // is how a blob-carrying value or a composed reply vanishes with clean logs on both
        // ends (#959). Logged every time, exactly like the
        // rx-buffer arm in recv_loop: this firing means the link emits NOTHING at this
        // frame size, which is a misconfiguration to fix, not a rate to live with.
        ESP_LOGW(kTag, "outbound frame %u B exceeds %u B tx buffer — dropped",
                 static_cast<unsigned>(total), static_cast<unsigned>(tx_bytes_));
        tx_drops_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Announce this sender BEFORE it can reach write_m_ (#952). A sender that becomes the
    // writer holds write_m_ across the transport write, so it can still be inside the
    // transport while the destructor runs, and pre-#952 a sender woke up owning a destroyed
    // handle. The tally is what the destructor drains before it lets this object's own
    // members go — and a writer drains the queue inside this same tally, so the records it
    // writes for other publishers are covered too.
    senders_.fetch_add(1, std::memory_order_relaxed);
    const sender_exit_t leaving(senders_);
    // The shared egress module (ADR-0087 §2, #1619). Only ONE sender writes at a time; one
    // that arrives while a write is in flight leaves its frame in its lane's ring and returns
    // at once rather than queueing on write_m_ for the rest of that write — up to a whole
    // write budget on a peer whose window has closed. Delivery is in-call, so that sender is
    // whatever unrelated task published; it must not pay for this peer's socket. A full ring
    // sheds by its lane's arm, counted.
    //
    // The frame is built ONCE, drawn from `memory.io`: the header, then the payload gathered
    // and masked in the same pass (RFC 6455 client rule, #2265). A delivered frame may be
    // shared with the concurrent server link reading the same bytes, so the caller's bytes
    // are only ever read; the masked copy is the ONE pass a frame costs. The writer that
    // takes a waiting frame writes it as it stands, and the block goes back when its last
    // reference does, so no buffer outlives the frame. A refused draw drops the frame,
    // counted, and the writer still drains what others left waiting.
    // Only a frame that WAITS is charged to the `tx_bytes` budget (#2277): the one the direct
    // arm builds is the write in flight. A waiting frame the budget refuses is shed, counted.
    bool direct = false;
    tr::net::egress_once_t once([&] {
        return tr::net::egress_frame_t::make(
            direct ? *tx_src_ : static_cast<mem::block_source_t&>(held_),
            ws::detail::client_frame_bytes(total), [&](std::span<std::byte> out) {
                // The header, then the payload gathered and masked in one pass (#2265).
                return ws::detail::put_client_frame(out, static_cast<ws::opcode_t>(op), iov,
                                                    mask_key(), /*fin=*/true);
            });
    });
    // A write never waits (#2787): what the socket will not take now stays on the wire for the
    // next turn (the next send, or the recv loop's), and the connection is the read side's to
    // drop. The egress's strike verdict has nothing to close: liveness is the application's
    // frozen-peer check, on its own clock.
    resume_t resume{*this};
    const tr::net::egress_result_t r = egress_.send(
        peer_, lane, once,
        [&] {
            direct = true;
            const tr::net::egress_frame_t* const f = once();
            return f != nullptr ? resume(*f, 0) : tr::net::egress_step_t{0, false, true};
        },
        resume, [] {});
    tx_drops_.fetch_add(r.shed, std::memory_order_relaxed);
    // A send on a link that is down is the event that ends a backoff no app clock drives
    // (#2337). Inside the sender tally, so the destructor cannot have destroyed the mutex.
    if (connected_.load(std::memory_order_acquire)) return;
    bool woke = false;
    {
        const std::lock_guard<tr::mutex_t> lk(backoff_m_);
        woke = backing_off_ && !clocked_;
        backing_off_ = backing_off_ && !woke;
    }
    if (woke) backoff_cv_.notify_all();
}

tr::net::egress_step_t esp_ws_client_link_t::write_locked(const tr::net::egress_frame_t& f,
                                                          std::size_t off) {
    const std::span<const std::byte> frame = f.head();
    const std::lock_guard<tr::mutex_t> lk(write_m_);
    // Re-checked, not re-read for tidiness: teardown may have run while this record waited,
    // and it disarms `stop_` BEFORE it takes this very lock to null the handles, so a writer
    // that sees a set `stop_` leaves without touching either handle. NOT counted as a drop:
    // the link is being destroyed, which is not a loss toward the peer, and a teardown that
    // bumped a counter would make every clean shutdown look like a failure.
    if (stop_.load(std::memory_order_acquire)) return {frame.size() - off, false, false};
    // THE handle gate, and the reason nothing above it may read `tcp_`. `write_m_` does
    // NOT order a handle read against a re-dial: connect_once() adopts a fresh tcp_
    // holding no lock at all. What orders them is this
    // acquire pairing with the release store connect_once makes only AFTER the adoption —
    // and `connected_` is cleared only under this lock (drop(), the
    // shut_down() release) or at shut_down()'s top, whose store the recv loop answers by
    // breaking rather than re-dialing. The handle is destroyed only under
    // this lock with `connected_` false: the departure arm, or shut_down() (#1606). So an observed
    // `true` here is a happens-before edge with the writes that built the handle this
    // call is about to use, and a handle check placed AHEAD of this line is read against
    // an unsynchronised rewrite — the same unsynchronised handle read #952 is about,
    // seen from the sender's side (TSan: send() vs connect_once, on a failing re-dial).
    //
    // This one IS a drop, counted by the egress: the push vanishes toward a peer that is
    // simply down, and it is the loss that used to be completely invisible. So is the rest of
    // a frame begun on a connection that has since died (`off` past 0 and `cut_` cleared by
    // the departure arm): written to the new stream, it would desync it (#2787).
    if (!connected_.load(std::memory_order_acquire) || (off != 0 && !cut_)) return {0, false, true};
    // ONE non-blocking `send()` on the TCP parent's socket (#2787), the frame's header and
    // masked payload contiguous (#2265). A full peer window takes some bytes or none, and the
    // rest stays on the wire for the egress's next turn, so no producer waits on this peer and
    // a slow reader costs counted drops behind it, never the connection. A failed call is
    // held the same way, not judged here: a socket that is really dead fails its next poll
    // or read on the recv thread too, which drops the connection (drop()), and keepalive
    // finds one that died silently. So the link goes down on the read side's transport
    // error, the application's frozen-peer check, or teardown, never on a write.
    const std::span<const std::byte> rest = frame.subspan(off);
    const ssize_t n =
        ::send(esp_transport_get_socket(tcp_), rest.data(), rest.size(), MSG_DONTWAIT);
    const std::size_t moved = n < 0 ? 0 : static_cast<std::size_t>(n);
    cut_ = moved < rest.size();
#ifdef CONFIG_LIBTRACER_LINK_TRAFFIC_STATS
    // The success path counts only when the application asked for traffic counters
    // (#1663), once per frame, on its last byte. Nesting is write_m_ -> st_m_, never the
    // reverse.
    if (!cut_) {
        const std::lock_guard<tr::mutex_t> st_lk(st_m_);
        ++traffic_.tx_frames;
        // Payload bytes, as every kind counts them. A client frame's header is 6, 8 or 14
        // bytes, and its second byte's length code (126, 127, or the length itself) says which
        // (#2265).
        const unsigned code = static_cast<unsigned>(frame[1]) & 0x7FU;
        traffic_.tx_bytes += static_cast<std::uint32_t>(frame.size() - (code < 126    ? 6U
                                                                        : code == 126 ? 8U
                                                                                      : 14U));
    }
#endif
    return {moved, cut_, false};  // held: the rest waits for the egress's next turn
}

void esp_ws_client_link_t::recv_loop() {
    const callback_thread_scope_t delivering;  // a teardown made here waits for nothing
    // The connection's framing (#2484): core's RFC 6455 receive rules over the raw TCP bytes,
    // on this thread's stack. Every body is received into a segment of its exact length drawn
    // from the link's own source — `memory.rx` when the application named one (#1661),
    // `memory.state` otherwise — so no buffer is held between frames, and a fragmented
    // message is the assembler's chain of those segments (receiver pays).
    mem::source_backend_t state_rx(*dial_->src);
    mem::mem_backend_t& rx = rx_backend_ != nullptr ? *rx_backend_ : state_rx;
    // Whether the connection currently standing has delivered a message (#1128); see below.
    bool exchanged = false;
    framing_end_t end{*this, rx, exchanged, {}, ws_assembler_t{rx}};
    const ws_framing_t<framing_end_t> framing{end};
    // No read-ahead scratch: each read is one `esp_transport_read` into the header slot or
    // straight into the body's segment (frame_ingress_t::read_with).
    frame_ingress_t<ws::kMaxClientFrameHeader, 0> ingress;
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
    // (`exchanged`, declared above, is set by the delivery.)
    // No dial latch is waited here: an unarmed link has no recv thread at all
    // (#1606), and this thread only exists once start_receiving() armed the link.
    while (!stop_.load(std::memory_order_relaxed)) {
        // What waits for the socket goes on every turn too, not only on the next send (#2787):
        // a burst's tail leaves once the peer reads again even when nothing more is
        // published. No new wait, this is the loop's own poll cadence. On the turn that finds
        // the connection down it drains what waited, so nothing of the old connection is
        // written to the next one. Those losses are counted like a send to a link that is
        // down, so `stats()` read during a backoff says what was lost.
        {
            resume_t resume{*this};
            tx_drops_.fetch_add(egress_.flush(peer_, resume, [] {}).shed,
                                std::memory_order_relaxed);
        }
        if (!connected_.load(std::memory_order_acquire)) {
            // A new connection is a new stream: nothing of the old one's frame or message
            // survives into it.
            ingress.reset();
            end.assembler.reset();
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
                // Release the dead connection NOW, before the backoff and the re-dial (#1606),
                // so a link whose peer went away is back to holding only what a
                // never-connected one holds. Under `write_m_`, because a writer reads the
                // handle only under it, past the `connected_` gate — and `connected_` is
                // already false. Before notify_down: no transport lock may be held across it.
                // `cut_` goes with it (#2787): a frame the dead socket cut short is then
                // refused by write_locked, so its rest never lands on the next connection.
                {
                    const std::lock_guard<tr::mutex_t> lk(write_m_);
                    release_tcp(tcp_);
                    cut_ = false;
                }
                // The departure seam (RFC-0009 §D extended to peer departure), and the
                // reason it is reported at all (#957). This arm is downstream of all
                // sites that clear `connected_` — drop() (peer CLOSE, a poll
                // error, a read error) and the destructor — so ONE report here covers every way
                // this link goes down, with no transport lock held, exactly as the contract asks.
                //
                // It fires on a blip too, and that is the choice, not an oversight. The
                // link cannot tell a blip from a peer that REBOOTED: the reconnect
                // rebuilds the transport pair and says nothing, so keeping the child's
                // edges across it is only sound if the far side kept its own — and a
                // rebooted peer has forgotten every subscription, leaving this node
                // producing into a session that no longer exists. Re-establishing after a flap is
                // cheap and correct; resurrecting stale routing is neither. This is also what
                // core's portable `ws_client_transport_t` does at the end of its recv loop
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
                const dial_outcome_t out = connect_once();
                // ORPHANED means this link was destroyed while that dial was in flight:
                // the dial released its own transport pair and THIS OBJECT IS GONE. Return
                // without touching one more member — not even `stop_` — which is the whole
                // of what the destructor's detach relies on (#1058). The lambda that
                // called us holds `this` as a pointer it never dereferences again, so
                // unwinding out of here ends the thread cleanly.
                if (out == dial_outcome_t::ORPHANED) return;
                if (out == dial_outcome_t::UP) {
                    was_up = true;  // there is now a connection whose death is reportable
                    // Bytes the server pipelined behind its 101 are the start of the frame
                    // stream, already off the socket: a peer that pushes its state on connect
                    // and then goes quiet must still have that message delivered. Then the
                    // handshake block goes back to the store.
                    const bool framed =
                        ingress.feed(
                            std::span<const std::byte>(dial_->hs.data() + dial_->pipelined_from,
                                                       dial_->pipelined_to - dial_->pipelined_from),
                            rx, framing) >= 0;
                    dial_->hs.reset();
                    if (!framed) drop();
                } else {
                    backoff_now = true;
                }
            }
            if (backoff_now) {
                // The backoff has no timeout and reads no clock (#2337): it ends when the
                // app's `now` passes it (sweep), on a send while no app clock drives the link,
                // or at teardown. With none of them it stays parked, which is the "never
                // spins" half. The dial itself is STILL uninterruptible —
                // esp_transport_connect takes no cancellation — but since #1058 a teardown
                // does not pay for it: it condemns the in-flight dial and detaches this
                // thread, and the orphaned dial pays its own kDialTimeoutMs out here.
                std::unique_lock<tr::mutex_t> lk(backoff_m_);
                backing_off_ = true;
                backoff_cv_.wait(
                    lk, [this] { return !backing_off_ || stop_.load(std::memory_order_acquire); });
                backing_off_ = false;
            }
            continue;
        }

        const int pr = esp_transport_poll_read(tcp_, kPollMs);
        if (pr < 0) {
            drop();
            continue;
        }
        if (pr == 0) continue;  // no data this turn; re-check stop_

        // One read of the raw stream, straight into the header slot or the body's segment;
        // core frames it (ws_framing_t) and delivers each completed message in-call. False
        // for a peer that closed, a read error, a CLOSE, or an RFC 6455 breach (§7.1.7),
        // which is what stops the server we dialled from naming our memory budget (#872).
        //
        // NO lock (#2265). The handle is this thread's: it adopts it at a dial and releases
        // it in the departure arm, and shut_down() releases it only after joining it. lwIP
        // sockets are full-duplex (`LWIP_NETCONN_FULLDUPLEX`), so a send never waits out a
        // read and a read never waits out a send. Control frames are answered through the
        // write path (framing_end_t::reply).
        const bool alive = ingress.read_with(
            [this](std::span<std::byte> into) {
                return esp_transport_read(tcp_, reinterpret_cast<char*>(into.data()),
                                          static_cast<int>(into.size()), kReadTimeoutMs);
            },
            rx, framing);
        if (!alive) drop();
    }
}

}  // namespace tr::net
