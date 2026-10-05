/**
 * @file
 * @brief The RFC-0014 link-liveness engine for an owned dial connection: dormant, dial on
 *        demand, self-heal, close.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0014 §4 S5 — the link-liveness ENGINE for an owned, factory-constructed DIAL
 * connection (#492). The connection *vertex* is explicit and persistent (RFC-0014 §3);
 * the *link* it names is this state machine: dormant until demanded, dialed on demand,
 * self-healing with backoff while a standing binding holds it, and closed back to
 * dormant when the last binding releases. `tr::net::self_heal_link_t` IS the
 * `transport_t` the router registers for such a connection — a stable routing identity
 * whose inner socket comes and goes underneath it — so the registry never rebinds and
 * subscriber edges survive a transient loss (the soft half of RFC-0014 §Teardown;
 * eviction happens only on the hard `NAME`-remove path).
 */
#pragma once

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include "libtracer/config.hpp"
#include "libtracer/thread_id.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_factory.hpp"

namespace tr::net {

/** @brief The engine's own retry interval when the SPEC's `backoff` is 0/absent
 *         (RFC-0014 §4: an engine default, overridden by config — never a synthetic
 *         limit; the retry itself runs forever while a standing binding holds). */
inline constexpr std::uint32_t kDefaultBackoffMs = 1000;

/** @brief The engine's own dial-attempt deadline when the SPEC's `connect_timeout` is
 *         0/absent — how long an auto-woken op waits for ONE connect attempt before it
 *         answers link-down (RFC-0014 §4). */
inline constexpr std::uint32_t kDefaultConnectTimeoutMs = 5000;

/**
 * @brief The RFC-0014 §4 S5 liveness engine over one owned DIAL connection — a
 *        `transport_t` whose inner socket is constructed, healed, and closed by the
 *        engine itself (#492).
 *
 * `transport_vertex_t` mints one of these instead of running the kind's factory when the
 * kind was registered with `transport_kind_traits_t::self_heal_dial` and the connection's
 * role is `DIAL`. The engine owns the factory (a copy), the parsed universal settings, and
 * the SPEC's raw config bytes, so it can re-run construction on every dial — creation
 * itself constructs NO socket (the vertex is minted `DORMANT`, RFC-0014 §4's
 * refcount-0 resting state).
 *
 * **The state machine** (DIAL subset of `link_state_t`, published to the connection
 * vertex through the installed liveness publisher — the engine is the sole writer of
 * these transitions):
 *
 * - `DORMANT` → `DIALING`: any op auto-wakes the link (@ref send blocks for ONE connect
 *   attempt, bounded by `connect_timeout`, then serves or drops — the §4 stall-on-dial),
 *   and @ref acquire kicks the same wake without blocking.
 * - `DIALING` → `UP` on a successful construct; → `RECONNECTING` (a standing binding
 *   holds) or back to `DORMANT` (none does — a lone one-shot's failed dial triggers NO
 *   background retry, §4's transient-hold rule) on a failed one.
 * - `UP` → `RECONNECTING` on socket loss with refcount > 0: the self-heal retry loop —
 *   an attempt per `backoff` interval, FOREVER (no give-up bound and no terminal state,
 *   the §4 no-synthetic-limits ruling). Ops on a `RECONNECTING` link fail fast
 *   (dropped and counted), never block on a dead peer.
 * - `UP` → `DORMANT` on socket loss with refcount 0, and on the LAST @ref release
 *   (§4: refcount → 0 → close socket, go dormant, stop retrying).
 *
 * **The refcount** counts STANDING bindings (@ref acquire / @ref release — the seam the
 * routing plane's subscription/await integration drives; S6 wires the callers). A
 * one-shot op's transient hold is implicit in @ref send itself: it wakes a dormant link
 * and rides the attempt, and its release is invisible at this seam (send is
 * fire-and-forget), so an op-woken socket with no standing binding stays up until loss
 * rather than being torn down per-op. That keep-up is the MAY of §4.1 (Amendment 1,
 * 2026-08-21): the amendment leaves the close-on-transient-release question to the
 * implementation, and this engine exercises the keep-up arm, because a dial per one-shot
 * op is exactly the hidden-handshake latency the RFC's own §Alternatives rejects. The
 * three §4.1 MUSTs are what this engine is held to, and all three hold here: no
 * background retry at refcount 0, re-dormant with no retry on loss (or on a failed
 * wake-dial) at refcount 0, and close-plus-re-dormant on the last STANDING release.
 *
 * **Threading.** One worker thread per engine, started lazily on the first transition
 * and joined by @ref stop / the destructor; it is the only thread that dials and the only
 * publisher of liveness, so transitions publish in order. Dead sockets are reaped off the
 * notifier thread (a socket's down-notifier fires ON its own receive thread, which its
 * destructor joins — reaping in place would self-deadlock). The engine takes no lock of
 * `transport_vertex_t` or the router; the publisher writes the graph vertex directly, so
 * the owner may hold its control mutex while joining this engine (`stop()`), and the
 * declared lock order is never entered backwards.
 *
 * @note Engine-managed kinds are POINT-TO-POINT: @ref bus is nullptr by construction
 *       (there is no socket to ask at creation, and a bus kind must not be registered
 *       `self_heal_dial` — its peer facet would be invisible to the router's bus wiring).
 */
class self_heal_link_t final : public transport_t {
   public:
    /** @brief The liveness sink the engine publishes every transition through —
     *         installed once by the owner (a write of the 1-byte `link_state_t` VALUE
     *         to the connection vertex), before the link is wired into the router. */
    using liveness_publish_fn_t = inline_fn_t<void(link_state_t)>;

    /**
     * @brief Bind the engine over @p factory with the connection's creation-time config.
     *
     * @param factory        The kind's transport factory (copied; re-run on every dial).
     * @param settings       The parsed universal settings. `backoff_ms` /
     *                       `connect_timeout_ms` of 0 are resolved to the engine defaults
     *                       HERE, so the factory and the engine see the same effective
     *                       values (RFC-0014 §4: config overrides the engine's defaults).
     * @param raw_config     The SPEC's `config` SETTINGS TLV as contiguous bytes (empty = the
     *                       SPEC carried none): the kind-private keys the factory re-parses
     *                       on each dial. BORROWED: the owner keeps them, and the text views
     *                       in @p settings, alive until this engine is destroyed —
     *                       `transport_vertex_t` holds them as the connection's config copy
     *                       and drops that copy only after the engine (#1780).
     * @param inner_delivers_ropes The kind's delivery capability
     *                       (`transport_kind_traits_t::delivers_ropes`): the engine must
     *                       answer @ref delivers_ropes BEFORE any socket exists, because
     *                       `fwd_router_t::add_child` installs the matching receiver on
     *                       the ENGINE exactly once, at registration.
     * @param src            The store each dial's socket, and the engine's record of it, is
     *                       drawn from — the factory's third argument (receiver pays).
     */
    self_heal_link_t(transport_factory_t factory, conn_settings_t settings,
                     std::span<const std::byte> raw_config, bool inner_delivers_ropes,
                     mem::block_source_t& src);

    /** @brief Stops the engine (see @ref stop) and destroys any remaining socket. */
    ~self_heal_link_t() override;

    self_heal_link_t(const self_heal_link_t&) = delete;
    self_heal_link_t& operator=(const self_heal_link_t&) = delete;

    /**
     * @brief Install the liveness publisher — call after the connection vertex exists and
     *        BEFORE the engine is wired into the router (no transition can fire earlier).
     *
     * The engine's worker invokes it with no engine lock held, so the sink may take graph
     * locks freely; it must tolerate a write to an already-retired vertex (teardown
     * stops the worker first, but the sink is the safety net).
     */
    void set_liveness_publisher(liveness_publish_fn_t fn);

    /**
     * @brief A STANDING binding takes its hold (RFC-0014 §4: a routed subscription or
     *        `await` that needs the peer reachable).
     *
     * Non-blocking: a dormant link is kicked toward `UP` (the worker dials); the caller
     * that must WAIT for `UP` uses `await` on the connection vertex (S6's verb). While
     * refcount > 0 the engine self-heals on loss, forever.
     */
    void acquire();

    /**
     * @brief The standing binding releases its hold.
     *
     * The LAST release closes an `UP` socket and re-dormants the link, and stops an
     * in-flight self-heal at its next gate (§4: refcount → 0 → close socket, go dormant,
     * stop retrying). Unbalanced releases are ignored.
     */
    void release();

    /**
     * @brief Stop the engine: join the worker, tear down every socket. Idempotent.
     *
     * `transport_vertex_t::remove_connection` calls this BEFORE retiring the connection
     * vertex, so no liveness write can land on a retired vertex; after it returns the
     * engine publishes nothing and @ref send drops everything. A dial attempt in flight
     * is waited for (the factory's own connect deadline bounds the wait).
     */
    void stop();

    /** @brief The engine's current liveness state (the owner/test introspection door). */
    [[nodiscard]] link_state_t state() const;

    /**
     * @brief Emit one frame — the §4 op door. `UP` sends on the inner socket; `DORMANT`
     *        auto-wakes (blocks for ONE attempt, bounded by `connect_timeout`) then sends
     *        or drops; `RECONNECTING` fails fast. Every drop counts in @ref drop_stats.
     */
    void send(std::span<const std::byte> frame) override;

    /** @brief Scatter-gather twin of @ref send(std::span<const std::byte>) — same gate. */
    void send(std::span<const std::span<const std::byte>> iov) override;

    /** @brief The kind's delivery capability, answered for the router at registration
     *         time (see the constructor's `inner_delivers_ropes`). */
    [[nodiscard]] bool delivers_ropes() const override { return inner_ropes_; }

    /** @brief Runtime liveness (#1059): true iff the engine is `UP`. */
    [[nodiscard]] bool link_up() const noexcept override;

    /** @brief The CURRENT socket's counters plus the engine's own fail-fast drops
     *         (`dropped_tx`). A healed link's previous socket takes its counts with it. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override;

   private:
    /** @brief One constructed socket plus the identity its down-notifier reports with:
     *         the ctx handed to `transport_t::set_down_notifier` must outlive every
     *         notification, so it lives WITH the socket it names.
     *
     *  Shared by count, not by `std::shared_ptr` (#1780): the record is one block from the
     *  engine's store, the count is in the block, and the corpse list threads through it,
     *  so parking a dead socket can never fail for want of memory. */
    struct sock_t {
        transport_ptr_t link;               /**< @brief The factory-constructed socket. */
        self_heal_link_t* self = nullptr;   /**< @brief Back-pointer for the notifier. */
        std::uint64_t gen = 0;              /**< @brief Dial generation — a stale corpse's
                                                        late down must not kill a healed
                                                        successor. */
        std::atomic<std::uint32_t> refs{1}; /**< @brief Holders: `inner_`, the corpse list,
                                                        and each in-flight send. */
        sock_t* next_corpse = nullptr;      /**< @brief The corpse list's link. */
    };

    /**
     * @brief One counted hold on a `sock_t` — what `inner_` and an in-flight send keep.
     *        The last hold to go destroys the socket (joining its receive thread) and
     *        returns its block, on whichever thread drops it, as the `std::shared_ptr` it
     *        replaces did.
     */
    class sock_ref_t {
       public:
        /** @brief An empty hold. */
        sock_ref_t() noexcept = default;
        /** @brief Adopt one count already taken on @p s (a fresh record starts at one). */
        explicit sock_ref_t(sock_t* s) noexcept : s_(s) {}
        /** @brief Take one more count on @p o's socket. */
        sock_ref_t(const sock_ref_t& o) noexcept : s_(o.s_) {
            if (s_ != nullptr) s_->refs.fetch_add(1, std::memory_order_relaxed);
        }
        /** @brief Take over @p o's count; @p o is left empty. */
        sock_ref_t(sock_ref_t&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }
        /** @brief Drop this hold, then take @p o's (copy-and-swap). */
        sock_ref_t& operator=(sock_ref_t o) noexcept {
            sock_t* const t = s_;
            s_ = o.s_;
            o.s_ = t;
            return *this;
        }
        /** @brief Drops the hold. */
        ~sock_ref_t() { reset(); }
        /** @brief Drop the hold; the last one destroys the socket and frees its record. */
        void reset() noexcept;
        /** @brief Give the count up without dropping it (the corpse list adopts it). */
        [[nodiscard]] sock_t* detach() noexcept {
            sock_t* const s = s_;
            s_ = nullptr;
            return s;
        }
        /** @brief The socket record, or null. */
        [[nodiscard]] sock_t* get() const noexcept { return s_; }
        /** @brief Member access. Precondition: not empty. */
        [[nodiscard]] sock_t* operator->() const noexcept { return s_; }
        /** @brief True when a socket is held. */
        [[nodiscard]] explicit operator bool() const noexcept { return s_ != nullptr; }

       private:
        sock_t* s_ = nullptr; /**< @brief The held record, or null. */
    };

    /** @brief The worker body — sole dialer and sole liveness publisher. */
    void worker_main();

    /** @brief One dial: run the factory (UNLOCKED — it may block for its connect
     *         deadline), wire the socket's sinks, adopt it as `inner_`. Returns true on
     *         `UP`. @p l is held on entry and exit. */
    [[nodiscard]] bool attempt_locked(std::unique_lock<std::mutex>& l);

    /** @brief Install the engine's trampolines on a fresh socket and start it receiving
     *         (frames land in this engine's `rx_`, i.e. whatever the router installed). */
    void wire_socket(sock_t& sock);

    /** @brief The inner socket's down event, on the socket's own thread — flags the loss,
     *         parks the corpse, wakes the worker. Never destroys, never publishes. */
    void on_socket_down(sock_t& sock);

    /** @brief Destroy parked corpses with the lock RELEASED (each destruction joins a
     *         receive thread that may itself be blocked reporting down against `m_`). */
    void reap_locked(std::unique_lock<std::mutex>& l);

    /** @brief Publish @p s through the installed sink with the lock RELEASED (the write
     *         fans out to subscribers, which may re-enter this engine's `send`). */
    void publish_unlocked(std::unique_lock<std::mutex>& l, link_state_t s);

    /** @brief Spawn the worker if it is not running and the engine is not stopping;
     *         caller holds `m_`. */
    void ensure_worker_locked();

    /** @brief `pthread_create` trampoline into `%worker_main` (the `void*` is `this`). */
    static void* worker_entry(void* self);

    /** @brief The op-side gate: the §4 transient hold. Returns the socket to send on, or
     *         nullptr = drop (fail-fast, or the one bounded auto-wake attempt failed). */
    [[nodiscard]] sock_ref_t ready_socket();

    /** @brief Park @p dead on the corpse list (it adopts the hold); caller holds `m_`. */
    void park_locked(sock_ref_t dead) noexcept;

    /** @brief Drop the list's hold on every corpse from @p head; call with `m_` RELEASED. */
    static void drop_corpses(sock_t* head) noexcept;

    /** @brief Serializes every engine transition. Ordering: `m_` is taken AFTER the
     *         owner's control mutex (acquire/release under `ctl_m_`) and never before it;
     *         nothing under `m_` calls back into `transport_vertex_t` or the router. */
    mutable std::mutex m_;
    std::condition_variable cv_; /**< @brief Worker wake + op-waiter rendezvous. */

    const transport_factory_t factory_;           /**< @brief Re-run per dial. */
    const conn_settings_t settings_;              /**< @brief Universal keys, defaults resolved. */
    const std::span<const std::byte> raw_config_; /**< @brief The SPEC's config TLV bytes
                                                              (borrowed, see the ctor). */
    const bool inner_ropes_;                      /**< @brief The kind's delivery capability. */
    mem::block_source_t& src_;      /**< @brief Where each socket and its record come from. */
    liveness_publish_fn_t publish_; /**< @brief Set once, before router wiring. */

    link_state_t state_ = link_state_t::DORMANT; /**< @brief The machine (DIAL subset). */
    std::uint32_t refs_ = 0;                     /**< @brief Standing bindings (§4). */
    std::uint64_t attempt_seq_ = 0; /**< @brief Concluded dial attempts — the op-waiter's
                                                "my attempt finished" edge. */
    std::uint64_t live_gen_ = 0;    /**< @brief Generation of `inner_` (0 = none). */
    bool wake_requested_ = false;   /**< @brief A dormant link was demanded. */
    bool publish_pending_ = false;  /**< @brief A non-worker thread changed `state_`;
                                                the worker owes the publish. */
    bool stop_ = false;             /**< @brief Terminal; set by @ref stop. */

    sock_ref_t inner_;          /**< @brief The live socket (UP only). */
    sock_t* corpses_ = nullptr; /**< @brief Dead sockets awaiting reap — an intrusive list
                                            through `sock_t::next_corpse`, one hold each. */

    // A raw pthread rather than a `std::thread`, for the two reasons `posix_endpoint_t` and
    // `socketcan_link_t` already are (#1470): `std::thread`'s constructor THROWS on spawn
    // failure, which is a `std::abort()` under `-fno-exceptions`, while `pthread_create`
    // returns an error code the engine can absorb; and `pthread_attr_setstacksize` is the
    // only portable way to size the stack, which an RTOS target must do — IDF's pthread
    // otherwise takes `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` (12,288 B stock) from the
    // heap per link. See `tr::net::kSelfHealWorkerStackBytes`.
    pthread_t worker_{};              /**< @brief Valid only while `%worker_started_` is set. */
    bool worker_started_ = false;     /**< @brief Lazy; joined by @ref stop. */
    detail::thread_id_t worker_id_{}; /**< @brief Re-entrancy guard: the worker's own
                                                  publish fan-out must not block on the
                                                  worker (see `ready_socket`). */
    /**
     * @brief Dial-generation mint. 64-bit ON PURPOSE (#1697): a generation is an identity a
     *        stale socket is compared against, so it must never alias, and the one bump per
     *        DIAL is cold — the rv32 libatomic call it costs is paid once per reconnect.
     */
    std::atomic<std::uint64_t> gen_ctr_{0};
    /** @brief Fail-fast drops. Word-wide, as every drop counter is (core/STYLE.md
     *         §Introspection clause 5, #1697): a 64-bit atomic is a libatomic call on rv32. */
    std::atomic<std::size_t> engine_dropped_tx_{0};
};

}  // namespace tr::net
