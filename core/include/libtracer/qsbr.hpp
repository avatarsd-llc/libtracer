/**
 * @file
 * @brief The QSBR domain (#1376): the cross-thread grace period behind
 *        @ref tr::graph::reclaim_qsbr_t, ADR-0080's third reclamation policy.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Quiescent-state-based reclamation in the URCU sense, applied to two things: the `{fn, ctx}`
 * leg of a retired subscription under `reclaim_qsbr`, and, in every build, what the transport
 * plane parks on the graph (a removed link, a retired seam). The read-side announcement it
 * needs is ALREADY COMPUTED by the seam PR #1377 shipped: `graph.cpp`'s dispatch bracket keeps
 * a per-thread depth whose transition to 0 is the proof that this thread holds no
 * `edge_view_t` snapshot. That is `reclaim_local`'s entire grace-point argument, and this
 * domain reuses it verbatim, adding one announcement at the OUTERMOST bracket and nothing at
 * all per edge.
 *
 * The shape is modelled on `%edge_pin.hpp`, not on `%lkv_slot.hpp`: a bounded `constinit`
 * registry in `.bss`, a claimable per-thread cell, and no exit sweep to register.
 *
 * **Two users, one domain.** `%graph.cpp`'s dispatch bracket reaches it only through an
 * `if constexpr` branch a non-QSBR build discards, so under the other two policies `fan_out`
 * emits nothing from here. The transport plane uses it in EVERY build: `fwd_router_t` brackets
 * each inbound frame and each origination with `%enter` / `%leave`, and the graph frees a
 * parked link or value seam only once every bracket open when it was parked has closed
 * (`%advance`, `%all_quiescent_past`). Both share one per-thread nesting depth, so a
 * dispatch nested in a frame neither re-announces nor goes quiescent early.
 *
 * @section qsbr_why_global Why the retired list is GLOBAL and the participant table is not
 *
 * ADR-0080 §"#897 maps onto the same seam" says each thread self-drains *its own* retired
 * list. That is the right shape for #897's displaced LKV nodes, which are pushed by a WRITER
 * thread that goes on writing — and it is discharged verbatim by calling
 * `%detail_hp::retire_and_flush(nullptr)` at the quiescent point, with no change to
 * `%lkv_slot.hpp` at all.
 *
 * It is the WRONG shape for a retired subscription pair, and the difference is the case this
 * policy exists for. Thread B unsubscribes while thread A is mid-dispatch; B must defer,
 * because A's live snapshot still names the pair. If B parked that pair in B's OWN
 * thread-local list, nothing would ever drain it: B is not dispatching, so B has no quiescent
 * point coming, and "unsubscribe once, then never touch the graph again" is the DOMINANT
 * teardown shape. The hook would never run. So the retired pairs live in one bounded
 * `.bss`-resident table that ANY participant drains at its own quiescent point — and in the
 * scenario above it is A, the thread whose quiescence completed the grace period, that runs
 * the hook.
 *
 * That has a consequence the API must state rather than hide: under this policy the release
 * hook may run **on a thread other than the caller of `unsubscribe()`**. Both shipped
 * policies promise the caller's own thread; a grace period that spans threads structurally
 * cannot, because the only alternatives are to block the caller (ADR-0080 §Decision 4 rejects
 * waiting outright) or to leak. See @ref tr::graph::reclaim_qsbr_t.
 *
 * @section qsbr_protocol The protocol
 *
 * Classic QSBR. One global epoch (`%control_t::epoch`), one per-thread state word
 * (`%cell_t::state`):
 *
 * - **online** (dispatch depth 0 → 1): `state = epoch | kOnlineBit`. One relaxed load of a
 *   read-mostly shared line plus one store to this thread's own cache-line-isolated cell. No
 *   atomic RMW, and once per fan-out rather than once per edge.
 * - **offline** (depth 1 → 0): `state = 0`. `0` means "holds nothing", which is what makes an
 *   idle thread parked in `epoll` non-blocking — the classic QSBR liveness hole, closed
 *   structurally instead of by a `thread_offline()` verb the embedder must remember.
 * - **retire**: bump the epoch (one RMW, control plane only) and scan. A pair retired at
 *   epoch `E` is free once every participant is either offline or online at an epoch `> E`.
 *
 * @section qsbr_wrap The 31-bit epoch, and why a wrap only delays a free
 *
 * The epoch and the online flag share one 32-bit word, so the announce and the exit are plain
 * 32-bit stores on every target, rv32 included: a 64-bit atomic there is a `libatomic` call
 * that masks interrupts, three of them per bracket. The epoch therefore wraps, after 2^31
 * advances, and "after" is decided by serial-number arithmetic (`%epoch_after`): `a` is after
 * `b` when `(a - b) mod 2^31` is in `[1, 2^30)`. That reads correctly as long as the epochs
 * compared are less than 2^30 apart.
 *
 * The only epochs that can drift that far apart are an open bracket's and the counter's: the
 * counter advances once per retirement (`graph_t::retire`, `park_release`, an unsubscribe under
 * `reclaim_qsbr`), never per frame, so the bound is a bracket held open across 2^30 removals.
 * The writer keeps it from being reached: `%advance` does not bump the epoch while any open
 * bracket is already `%kStallLag` (2^29) behind it, so the counter stops at most 2^29 plus
 * the concurrent writers ahead of the oldest open bracket. A stalled counter only DELAYS frees
 * — a pair retired at the stalled epoch waits for every bracket, the old one included — and
 * the first advance after that bracket closes moves on. So a bracket held open however long
 * never makes a pair look free while it is open. A thread counted in `overflow_online` has no
 * epoch at all, and already blocks every free while it is online.
 *
 * The scan is `O(kQsbrParticipants)` and lives on the RECLAIM path only. That is the precise
 * difference from the shape #635 rejected, which put a hazard scan on the READ path.
 *
 * @section qsbr_ordering Why the announce store is `seq_cst`
 *
 * The hazard is Dekker-shaped and it is the only ordering argument this file makes. The
 * reader announces itself and then reads the edge array; the retiring thread mutates the edge
 * array (`clear_edge`, already done before it gets here) and then reads the announcements.
 * Neither side may miss the other. Store-buffering permits exactly that outcome unless BOTH
 * sides carry a full barrier, so both do: the reader's announcing store is `seq_cst`, and the
 * retiring thread's epoch bump is a `seq_cst` read-modify-write standing between its mutation
 * and its scan. `%all_quiescent_past` opens with an explicit `seq_cst` fence for the OTHER
 * caller — a drainer at a quiescent point, which did not just bump the epoch — and that fence
 * sits below the early-out, so the dispatch path never reaches it.
 *
 * Going OFFLINE needs only `release`: it must publish that this thread's reads are done, and
 * a release store is exactly "nothing above me moves below me".
 */
#ifndef LIBTRACER_QSBR_HPP
#define LIBTRACER_QSBR_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "libtracer/config.hpp"

namespace tr::graph::detail_qsbr {

/**
 * @brief The domain's padding width: @ref tr::graph::kCacheLineBytes, floored at the
 *        announcement's natural alignment so every value of the knob stays well-formed.
 *
 * Identical in derivation to `%edge_pin.hpp`'s `kCellAlign`, and for the identical reason: a
 * single-core target sets the knob to 0 and the cell collapses onto its payload, because there
 * is no second core for a scan to false-share against.
 */
inline constexpr std::size_t kCellAlign = kCacheLineBytes > alignof(std::atomic<std::uint32_t>)
                                              ? kCacheLineBytes
                                              : alignof(std::atomic<std::uint32_t>);

/**
 * @brief The flag distinguishing "online at epoch 0" from "offline".
 *
 * The state word is `epoch | kOnlineBit` while dispatching and exactly `0` while quiescent, so
 * a scan tests one word per participant. The bit is the TOP one, leaving 31 bits of epoch,
 * which wraps; see @ref qsbr_wrap.
 */
inline constexpr std::uint32_t kOnlineBit = std::uint32_t{1} << 31;

/** @brief The epoch's 31 bits within the state word and the counter. */
inline constexpr std::uint32_t kEpochMask = kOnlineBit - 1;

/** @brief Half the epoch space: two epochs this far apart or more have no order. */
inline constexpr std::uint32_t kEpochWindow = std::uint32_t{1} << 30;

/** @brief How far the counter may run ahead of an open bracket before @ref advance stalls. */
inline constexpr std::uint32_t kStallLag = std::uint32_t{1} << 29;

/**
 * @brief Is epoch @p a strictly after epoch @p b, modulo 2^31 (see @ref qsbr_wrap)?
 *
 * Two epochs @ref kEpochWindow or more apart compare as NOT after in both directions, so a
 * reader the counter has lapped is never read as past a retirement: the free waits.
 */
[[nodiscard]] constexpr bool epoch_after(std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t d = (a - b) & kEpochMask;
    return d != 0 && d < kEpochWindow;
}

static_assert(epoch_after(1, 0) && !epoch_after(0, 0) && !epoch_after(0, 1),
              "epoch_after is a strict order on nearby epochs");
static_assert(epoch_after(0, kEpochMask) && !epoch_after(kEpochMask, 0),
              "across the wrap, 0 follows 2^31 - 1");
static_assert(epoch_after(kEpochWindow - 1, 0) && !epoch_after(kEpochWindow, 0) &&
                  !epoch_after(0, kEpochWindow),
              "epochs half the space apart have no order, so neither reads as past the other");
static_assert(epoch_after(kOnlineBit | 5, 4) == epoch_after(5, 4),
              "the online bit and the counter's 32nd bit are outside the compare");
static_assert(kStallLag < kEpochWindow, "the stall must engage before the compare window ends");

/** @brief "This thread has not claimed a participant index." */
inline constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

/**
 * @brief One participant's quiescent-state announcement, cache-line isolated.
 *
 * The isolation is the whole many-core argument: the announcing store lands on a line no other
 * participant reads except during a control-plane scan, so a dispatching thread never bounces
 * a line against a sibling that is also dispatching.
 */
struct alignas(kCellAlign) cell_t {
    /** @brief `epoch | kOnlineBit` (31 bits of epoch), or 0 when quiescent — a plain 32-bit
     *         store per bracket, never an RMW, on every target. */
    std::atomic<std::uint32_t> state{0};
    std::atomic<bool> claimed{false}; /**< @brief Whether a live thread owns this index. */
};

static_assert(alignof(cell_t) == kCellAlign,
              "the cell's alignas was silently dropped — see kCellAlign's derivation");

/** @brief The lifecycle of one `%retired_slot_t`, as a lock-free four-state handoff. */
enum class slot_state_t : std::uint32_t {
    EMPTY = 0,    /**< @brief Free for a retiring thread to claim. */
    FILLING = 1,  /**< @brief Claimed; its payload is not yet published. */
    READY = 2,    /**< @brief Payload published; awaiting its grace period. */
    CLAIMING = 3, /**< @brief One drainer has won this slot and will run its hook. */
};

/**
 * @brief One deferred release obligation and the epoch it was retired at.
 *
 * `ctx` / `release` are written only under @ref slot_state_t::FILLING and read only under
 * @ref slot_state_t::CLAIMING — both states are owned exclusively by one thread — so they are
 * plain members rather than atomics, published and consumed by @ref state's release/acquire
 * transitions.
 */
struct retired_slot_t {
    std::atomic<slot_state_t> state{slot_state_t::EMPTY}; /**< @brief The handoff. */
    void* ctx = nullptr;                                  /**< @brief The subscriber's context. */
    void (*release)(void*) = nullptr;                     /**< @brief Its release hook. */
    std::uint32_t epoch = 0;                              /**< @brief The epoch it retired at. */
};

/**
 * @brief The read-mostly control words, isolated from the participant cells.
 *
 * Both members are LOADED on the dispatch path (the epoch when going online, the live count
 * when going offline) and STORED only on the control plane, so they want to sit together on
 * one line that stays Shared in every core's cache — and want to be nowhere near the retired
 * table, which a retiring thread writes.
 */
struct alignas(kCellAlign) control_t {
    /**
     * @brief The global epoch; bumped once per retirement.
     *
     * Starts at **0**, and that is a footprint decision, not an arbitrary one. A non-zero
     * initializer anywhere in @ref registry_t moves the WHOLE object out of `.bss` and into
     * `.data` — measured on rv32 at 2,560 B of flash-backed initialized RAM instead of 2,560 B
     * of zero RAM, i.e. the cost paid twice. Nothing needs it to start at 1: "offline" is the
     * state word being exactly 0, and an online participant always sets @ref kOnlineBit, so
     * online-at-epoch-0 is already distinct from offline.
     *
     * 32-bit and wrapping; only its low 31 bits are compared, by @ref epoch_after. #1697 made
     * it 64-bit because an ORDER compare would read a wrapped epoch as old; the serial compare
     * and @ref advance's stall make a wrap delay a free instead (@ref qsbr_wrap). The advance is
     * a control-plane RMW (once per retirement), not a per-dispatch one.
     */
    std::atomic<std::uint32_t> epoch{0};
    /** @brief How many @ref retired_slot_t entries are occupied — the drain path's early-out. */
    std::atomic<std::uint32_t> live{0};
    /** @brief Dispatching threads that could not claim a cell; see @ref participant_t::index. */
    std::atomic<std::uint32_t> overflow_online{0};
};

/**
 * @brief The domain's storage.
 *
 * `constinit` and trivially destructible, exactly as `%edge_pin.hpp`'s registry is and for the
 * same reason: it lands in `.bss` with no guard variable and is never destroyed, so a
 * `thread_local` participant unwinding at process exit can always reach it.
 */
struct registry_t {
    std::array<cell_t, kQsbrParticipants> cells{}; /**< @brief The announcements. */
    control_t ctl{};                               /**< @brief The read-mostly words. */
    std::array<retired_slot_t, kDeferredReleaseSlots> retired{}; /**< @brief Deferred releases. */
    /** @brief Pairs dropped for want of a slot. Word-wide, as every drop counter is
     *         (core/STYLE.md §Introspection clause 5, #1697). */
    std::atomic<std::size_t> drops{0};
};

/** @brief The one domain. */
[[nodiscard]] inline registry_t& registry() {
    static constinit registry_t reg{};
    return reg;
}

/** @brief This thread's claim on a participant index, released when the thread ends. */
class participant_t {
   public:
    participant_t() = default;
    participant_t(const participant_t&) = delete;
    participant_t& operator=(const participant_t&) = delete;

    /** @brief Go quiescent, then give the index back so a later thread can claim it. */
    ~participant_t() {
        if (idx_ == kNoIndex) return;
        cell_t& c = registry().cells[idx_];
        c.state.store(0, std::memory_order_seq_cst);
        c.claimed.store(false, std::memory_order_release);
    }

    /**
     * @brief This thread's index, claimed on first dispatch.
     *
     * @return The claimed index, or @ref kNoIndex when every index is taken. Unlike
     *         `%edge_pin.hpp`'s equivalent, @ref kNoIndex here is NOT a performance fallback:
     *         a thread that cannot announce itself is a thread a scan cannot see, which would
     *         be a use-after-free rather than a slow path. The caller therefore counts itself
     *         into @ref control_t::overflow_online instead, and any non-zero reading of that
     *         blocks every reclamation — safe, bounded, and visible as a stall rather than a
     *         crash. Raise @ref tr::graph::default_config_t::kQsbrParticipants if it happens.
     */
    [[nodiscard]] std::size_t index() noexcept {
        // Fast path only, so the CAS sweep below never lands in an inlined `fan_out`: after the
        // first dispatch this is one predicted branch on a thread-local field.
        if (idx_ != kNoIndex || tried_) return idx_;
        return claim();
    }

   private:
    /** @brief The once-per-thread table walk, deliberately out of line — see @ref index. */
    [[gnu::noinline]] std::size_t claim() noexcept {
        registry_t& r = registry();
        for (std::size_t i = 0; i < kQsbrParticipants; ++i) {
            bool expected = false;
            if (r.cells[i].claimed.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                idx_ = i;
                return idx_;
            }
        }
        // Do not re-walk a table already known full on every dispatch: the overflow counter is
        // already correct, and an O(N) CAS sweep per fan-out would turn "undersized knob" into
        // a far larger cost than the stall it is trying to avoid.
        tried_ = true;
        return kNoIndex;
    }

    std::size_t idx_ = kNoIndex;
    bool tried_ = false;

   public:
    /** @brief How many @ref enter calls this thread has not yet left; 0 ⇒ quiescent. */
    unsigned depth = 0;
};

/** @brief This thread's participant. Function-local so nothing is emitted for a TU that never
 *         dispatches under this policy. */
[[nodiscard]] inline participant_t& self() noexcept {
    static thread_local participant_t p;
    return p;
}

/** @brief Announce that this thread is entering a dispatch — the read-side half. */
inline void go_online() noexcept {
    registry_t& r = registry();
    const std::size_t idx = self().index();
    if (idx == kNoIndex) {
        r.ctl.overflow_online.fetch_add(1, std::memory_order_seq_cst);
        return;
    }
    // Relaxed on the epoch: reading a STALE (smaller) epoch is always the conservative
    // direction — it can only make a scan decide this thread is not yet past a retirement it
    // is in fact past, which defers a free rather than permitting one. The store is seq_cst
    // for the Dekker argument in this file's header.
    const std::uint32_t e = r.ctl.epoch.load(std::memory_order_relaxed);
    r.cells[idx].state.store((e & kEpochMask) | kOnlineBit, std::memory_order_seq_cst);
}

/**
 * @brief Announce that this thread now holds no snapshot — the quiescent state itself.
 *
 * Re-resolves `%self()` rather than taking the index @ref go_online found. That costs one more
 * guarded `thread_local` access per outermost fan-out and buys something worth more: the
 * dispatch bracket in `%graph.cpp` needs no per-thread field of its own, so its thread-local
 * keeps byte-for-byte the layout the default build already had.
 */
inline void go_offline() noexcept {
    registry_t& r = registry();
    const std::size_t idx = self().index();
    if (idx == kNoIndex) {
        r.ctl.overflow_online.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }
    r.cells[idx].state.store(0, std::memory_order_release);
}

/**
 * @brief Enter a read-side section on this thread. Only the outermost one announces, so a
 *        nested section never re-announces at a newer epoch than the one it is covered by.
 */
inline void enter() noexcept {
    if (self().depth++ == 0) go_online();
}

/**
 * @brief Leave one read-side section.
 * @return True when it was the outermost: this thread is now quiescent.
 */
[[nodiscard]] inline bool leave() noexcept {
    if (--self().depth != 0) return false;
    go_offline();
    return true;
}

/**
 * @brief Close the epoch after a writer's unpublish: what the writer parks now is free once
 *        @ref all_quiescent_past answers true for the value returned.
 *
 * `seq_cst`, and load-bearing for the same reason @ref retire's bump is: this RMW (or, when
 * stalled, the fence) is the full barrier between the caller's unpublish and any later scan.
 *
 * Does not bump while an open bracket is @ref kStallLag or more behind the counter, and
 * answers the current epoch instead: what is parked then waits for that bracket too
 * (@ref qsbr_wrap). The check is one relaxed load per participant, on the control plane.
 */
[[nodiscard]] inline std::uint32_t advance() noexcept {
    registry_t& r = registry();
    const std::uint32_t now = r.ctl.epoch.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kQsbrParticipants; ++i) {
        const std::uint32_t s = r.cells[i].state.load(std::memory_order_relaxed);
        // A cell AHEAD of `now` (a concurrent bump landed after the load) reads as a lag past
        // the window and is not a stall; a cell really behind is never more than kStallLag
        // plus the concurrent writers behind, far inside it.
        const std::uint32_t lag = (now - s) & kEpochMask;
        if (s != 0 && lag >= kStallLag && lag < kEpochWindow) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return now;
        }
    }
    return r.ctl.epoch.fetch_add(1, std::memory_order_seq_cst);
}

/**
 * @brief Has every participant passed a quiescent state since epoch @p e?
 *
 * True when each is either offline or online at an epoch strictly greater than @p e. The
 * opening fence is what makes this sound for a caller that did NOT just bump the epoch (a
 * drainer at a quiescent point); it is below every early-out on the dispatch path.
 */
[[nodiscard]] [[gnu::noinline]] inline bool all_quiescent_past(std::uint32_t e) noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    registry_t& r = registry();
    if (r.ctl.overflow_online.load(std::memory_order_seq_cst) != 0) return false;
    for (std::size_t i = 0; i < kQsbrParticipants; ++i) {
        const std::uint32_t s = r.cells[i].state.load(std::memory_order_seq_cst);
        if (s != 0 && !epoch_after(s, e)) return false;
    }
    return true;
}

/**
 * @brief Run every retired pair whose grace period has elapsed.
 *
 * Called at a quiescent point (and once more by a retiring thread that had to defer). The
 * early-out is one relaxed load of a read-mostly line that is 0 on every node that is not
 * mid-teardown, which is what keeps this off the dispatch path's budget.
 *
 * A slot is EMPTIED before its hook runs, for the reason `%graph.cpp`'s park drain gives: a
 * hook is arbitrary user code that may publish, and a nested publish re-enters here.
 */
[[gnu::noinline]] inline void drain_ripe() noexcept {
    registry_t& r = registry();
    for (std::size_t i = 0; i < kDeferredReleaseSlots; ++i) {
        retired_slot_t& s = r.retired[i];
        if (s.state.load(std::memory_order_acquire) != slot_state_t::READY) continue;
        if (!all_quiescent_past(s.epoch)) continue;
        slot_state_t expected = slot_state_t::READY;
        // Two drainers may reach the same ripe slot; the CAS is what makes the hook run
        // exactly once, and the loser simply moves on.
        if (!s.state.compare_exchange_strong(expected, slot_state_t::CLAIMING,
                                             std::memory_order_acq_rel, std::memory_order_relaxed))
            continue;
        void* const ctx = s.ctx;
        void (*const fn)(void*) = s.release;
        s.ctx = nullptr;
        s.release = nullptr;
        s.epoch = 0;
        r.ctl.live.fetch_sub(1, std::memory_order_relaxed);
        s.state.store(slot_state_t::EMPTY, std::memory_order_release);
        fn(ctx);
    }
}

/**
 * @brief The quiescent-point entry point: reclaim anything ripe, cheaply deciding there is not.
 *
 * THE EARLY-OUT IS THE WHOLE POINT and it is why @ref drain_ripe is `noinline`. This is called
 * from every outermost dispatch exit, so all a publish may pay is one relaxed load of a
 * read-mostly line and one branch nobody takes; the O(slots × participants) scan behind it must
 * never be inlined into `fan_out`, which is a property `objdump` is asked to confirm rather than
 * a hope. (Measured: letting it inline added 162 instructions to `fan_out`.)
 */
inline void drain() noexcept {
    if (registry().ctl.live.load(std::memory_order_relaxed) == 0) return;
    drain_ripe();
}

/**
 * @brief Retire one `{ctx, release}` pair at this policy's grace point.
 *
 * Releases INLINE when no participant can still be holding a snapshot that names it — which
 * is every unsubscribe on a quiet node, and is what keeps property (a) ("quiescent on return")
 * true under this policy exactly as it is under the other two. Otherwise the pair is deferred
 * into the shared table and freed by whichever participant next completes the grace period.
 *
 * @return False when the pair was DROPPED for want of a slot; the caller counts it. Dropping
 *         is the same deliberate leak the per-thread park makes, for the same reason: running
 *         the hook here would free a context a live fan-out still names.
 */
inline bool retire(void* ctx, void (*release)(void*)) noexcept {
    registry_t& r = registry();
    // seq_cst, and load-bearing: this RMW is the full barrier standing between the caller's
    // `clear_edge` and the scan below, which is the writer half of the Dekker pairing.
    const std::uint32_t e = advance();
    if (all_quiescent_past(e)) {
        release(ctx);
        return true;
    }
    for (std::size_t i = 0; i < kDeferredReleaseSlots; ++i) {
        retired_slot_t& s = r.retired[i];
        slot_state_t expected = slot_state_t::EMPTY;
        if (!s.state.compare_exchange_strong(expected, slot_state_t::FILLING,
                                             std::memory_order_acq_rel, std::memory_order_relaxed))
            continue;
        s.ctx = ctx;
        s.release = release;
        s.epoch = e;
        r.ctl.live.fetch_add(1, std::memory_order_relaxed);
        s.state.store(slot_state_t::READY, std::memory_order_release);
        // One retry, on THIS thread, now that the pair is visible to every drainer: the
        // participants that were online a moment ago may have quiesced during the push, and a
        // node whose only other thread has since gone idle would otherwise wait for a dispatch
        // that never comes. After this, a surviving entry means a participant genuinely has
        // not quiesced yet.
        drain();
        return true;
    }
    r.drops.fetch_add(1, std::memory_order_relaxed);
    return false;
}

/** @brief How many pairs this domain has dropped for want of a slot. */
[[nodiscard]] inline std::uint64_t drops() noexcept {
    return registry().drops.load(std::memory_order_relaxed);
}

}  // namespace tr::graph::detail_qsbr

#endif  // LIBTRACER_QSBR_HPP
