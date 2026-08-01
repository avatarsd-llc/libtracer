/**
 * @file
 * @brief The graph-owned, non-blocking hazard-pointer reclamation domain (ADR-0072 + its
 *        erratum 1: intrusive records, per-thread announcement, asymmetric barriers).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One answer to the replaced-block question — *what happens to a heap block a
 * control-plane writer replaces while a lock-free reader still holds a raw reference into
 * it* — generalized from the announce/scan protocol `lkv_slot.hpp`'s `detail_hp` namespace
 * proved (ADR-0069 §5). `detail_hp` itself is deliberately untouched: its migration is a
 * later, separately measured slice (ADR-0072 §2).
 *
 * ## The three properties that make it usable on a bounded, latency-first node
 *
 *   1. **`retire` allocates nothing and never waits.** The retire record is INTRUSIVE: the
 *      tenant embeds a `retire_link_t` in the block it will one day retire, so parking a
 *      block is a Treiber push of storage that already exists. There is no allocation, so
 *      there is no exhaustion path, so there is no "block until the readers drain" fallback
 *      — the shape that deadlocked against `graph_t::map_mutex_` (a retirer holds the map
 *      lock; the announcing readers are outside it *precisely so their user callbacks may
 *      re-enter the graph*, so waiting for one is waiting for a thread that is waiting for
 *      you). This is ADR-0072 §3's erratum: the bound is not "records draw from an injected
 *      resource" but the strictly stronger "records ARE the blocks" — a node cannot retire
 *      more blocks than it allocated.
 *   2. **The reader announces into a PER-THREAD word, not a per-operation cell.** A cell
 *      claimed per operation costs a `seq_cst` CAS on shared storage every single read; a
 *      thread claims its participant once (and releases it at thread exit) and thereafter
 *      pays a store to a line it owns. Announcement storage is process-global rather than
 *      per-domain, which is safe *because announcements are compared by ADDRESS*: two live
 *      blocks in two domains cannot share one, so a foreign announcement can never match a
 *      parked block. ADR-0072 §2's three reasons to own the domain per instance — an
 *      injected resource, a teardown point, isolated bounds — are all about the RETIRED
 *      LIST, which stays per-instance.
 *   3. **The announcement store is LIGHT where the platform can flush a remote store
 *      buffer.** Hazard protection fundamentally needs StoreLoad ordering between "I
 *      announce p" and "is p still published?" — on x86 that is an `mfence`-class
 *      instruction, and it was the whole of the measured regression this design replaces.
 *      When the OS can serialize every other CPU on demand (Linux `membarrier`
 *      `PRIVATE_EXPEDITED`), the reader drops to a plain store and the *reclaimer* pays the
 *      barrier — cold, once per scan. Where it cannot (or under ThreadSanitizer, whose
 *      model cannot see an IPI), the reader falls back to `seq_cst` and the protocol is
 *      exactly the classical one. See `hazard_light_announce_available`.
 *
 * ## Protocol summary
 *
 * A reader takes a `hazard_domain_t::guard_t`, announces the pointer it is about to
 * dereference via `protect` (announce, then **re-read** the source — the ADR-0069 §5 loop),
 * and clears on guard destruction — after the user callback returns, so a slow callback
 * merely delays that one block's reclamation. A writer that displaced a block hands it to
 * `hazard_domain_t::retire`; parked blocks are freed by a scan (a cold, threshold-batched
 * control-plane event) once no participant announces them.
 *
 * **Nothing on either side blocks.** A reader that can get no announcement word — more
 * concurrent reader threads than `tr::graph::kHazardReaderSlots`, or a user callback that
 * re-entered the graph deeper than a participant has words — increments the domain's
 * *stall* counter instead, which makes scans free nothing for the life of that guard. It is
 * a counter, not a lock: it nests to any depth, it cannot self-deadlock, and it costs
 * reclamation throughput on the overflowing thread and nothing else.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include "libtracer/backend.hpp"
#include "libtracer/config.hpp"

namespace tr::mem {

/**
 * @brief A parked block's reclaimer: frees @p block, returning it to the resource it came
 *        from. The second argument is the domain's injected backend, for blocks that were
 *        allocated from it; a block from elsewhere (e.g. the global heap) ignores it. Must
 *        not throw and must not re-enter the domain.
 */
using reclaim_fn_t = void (*)(void* block, mem_backend_t& backend);

/**
 * @brief The intrusive retire record a tenant embeds — **as its first member** — in the
 *        block it retires.
 *
 * **ONE word**, and the size is load-bearing rather than incidental. Carrying the record
 * inside the block is what makes `retire` allocation-free and therefore non-blocking (see
 * the file header, property 1) — but the block is a per-vertex allocation on a node with a
 * RAM budget, and the three-word form this replaced pushed the 96-byte seam block into the
 * next allocator size class, costing 16 B on every handler-bearing vertex (measured, as
 * the perf gate's `mem:reg_escape` row). One word costs nothing at all, because:
 *
 *   - the **block address is the link's own address**, which is why the link must be the
 *     FIRST member of the block (assert `offsetof(block, link) == 0` at the tenant), and
 *   - the **reclaimer belongs to the domain**, fixed at construction, rather than to each
 *     record — one domain per kind of block. Domains are a few words each; the expensive
 *     part, the announcement table, is shared process-wide, so the second one is nearly
 *     free.
 *
 * The domain writes `next` only while the block is parked — i.e. after the writer detached
 * it from its publication slot — and a lock-free reader inside the block touches only the
 * tenant's own members, so the two never touch the same subobject.
 *
 * A block may be parked once at a time; retiring a block whose link is still parked is a
 * tenant bug (it would splice the retired list into itself). Since retiring requires having
 * just unpublished the block, this is structurally hard to do.
 */
struct retire_link_t {
    retire_link_t* next = nullptr; /**< @brief Retired-list link; the block IS `this`. */
};

/**
 * @brief Whether this process can force every other CPU to a serializing point on demand
 *        (Linux `membarrier` `PRIVATE_EXPEDITED`), so readers may announce with a PLAIN
 *        store and the reclaimer carries the ordering.
 *
 * Detected and registered once, on first call. `false` ⇒ the reader announces `seq_cst`
 * (the classical hazard-pointer protocol) — always correct, just slower. Forced `false`
 * under ThreadSanitizer: TSan's happens-before model cannot represent a barrier delivered
 * by an inter-processor interrupt, so the light protocol would be reported as a race even
 * though the hardware honours it.
 */
[[nodiscard]] bool hazard_light_announce_available() noexcept;

/**
 * @brief The reclaimer-side barrier: after it returns, every announcement any other thread
 *        issued before it is visible here. `membarrier` where available (see
 *        @ref hazard_light_announce_available), a `seq_cst` fence otherwise.
 */
void hazard_heavy_barrier() noexcept;

/**
 * @brief The process-global announcement table the reader protocol writes into.
 *
 * Global rather than per-domain because announcements are matched by ADDRESS: a block
 * parked in one domain can never equal a live block announced against another, so a shared
 * table costs a scan a few more words to read and buys every reader a per-thread claim
 * (see the file header, property 2). Nothing here is ever allocated or freed — it is a
 * fixed `.bss` table sized by @ref tr::graph::kHazardReaderSlots, the same knob the
 * per-domain cells used, so the static-RAM census does not move (it drops, for a process
 * with more than one graph).
 */
namespace detail_hz {

/** @brief How many threads may hold announcements at once — one participant each. */
inline constexpr std::size_t kParticipants = graph::kHazardReaderSlots;

/**
 * @brief Announcement words per participant, i.e. how deep a thread may nest guards
 *        before it falls back to the stall counter.
 *
 * Two, because the library's own reachable nesting is exactly two deep: a seam read
 * announces, and its user callback may re-enter the graph and announce a second seam. A
 * third level is legal and costs a stall, not a deadlock — which is why this is a tuning
 * number and not a correctness one (the no-synthetic-limits rule: exceeding it degrades,
 * it never fails).
 */
inline constexpr std::size_t kAnnouncePerThread = 2;

/** @brief Padding width: the cache-line knob, floored at the payload's own alignment so a
 *         single-core build (knob 0) stays well-formed. */
inline constexpr std::size_t kAnnAlign = graph::kCacheLineBytes > alignof(std::atomic<void*>)
                                             ? graph::kCacheLineBytes
                                             : alignof(std::atomic<void*>);

/** @brief One thread's announcement storage, cache-line isolated so a scan cannot false-
 *         share with a reader mid-announce. */
struct alignas(kAnnAlign) participant_t {
    std::atomic<bool> claimed{false};             /**< @brief Owned by some live thread. */
    std::atomic<void*> ann[kAnnouncePerThread]{}; /**< @brief Announced blocks; null = free. */
};

/** @brief The table itself, and how far into it any thread has ever claimed. */
inline participant_t g_participants[kParticipants]{};
/** @brief One past the highest participant index any thread has ever claimed — a scan
 *         reads only that far, so a node with two threads never walks the whole table. */
inline std::atomic<std::size_t> g_claimed_high_water{0};

/** @brief This thread's participant, how many of its words are in use, and whether it may
 *         announce with a plain store. All three are constant-initialized PODs on purpose:
 *         a `thread_local` with dynamic initialization compiles to a guarded (sometimes
 *         out-of-line) access, and these are read on the seam hot path. They live in
 *         thread-local storage rather than on the domain because the DOMAIN's line is
 *         written by every retire — a reader loading a flag from it would take a coherence
 *         miss on every control-plane event, which is the shape of cost this whole design
 *         exists to remove. */
inline thread_local participant_t* t_participant = nullptr;
/** @brief How many of this thread's announcement words are in use (guards nest LIFO). */
inline thread_local std::size_t t_depth = 0;
/** @brief Whether this thread may announce with a plain store — see @ref t_participant. */
inline thread_local bool t_light = false;

/** @brief Releases this thread's participant back to the table at thread exit — the only
 *         thing here with a destructor, and it is touched ONLY on the cold claim path so
 *         the hot path never pays for its initialization guard. Without it a process that
 *         churns threads would consume the table and degrade every later reader to the
 *         stall path. */
struct releaser_t {
    bool armed = false; /**< @brief Written by the claim path purely to force TLS init. */
    ~releaser_t() {
        if (t_participant != nullptr) {
            t_participant->claimed.store(false, std::memory_order_release);
            t_participant = nullptr;
        }
    }
};
/** @brief The thread-exit hook itself — see @ref releaser_t. */
inline thread_local releaser_t t_releaser{};

/** @brief Claim this thread's participant (once per thread), or `nullptr` when the table
 *         is full — in which case the caller stalls reclamation instead of blocking. */
[[nodiscard]] inline participant_t* my_participant() noexcept {
    if (t_participant != nullptr) return t_participant;
    t_releaser.armed = true;  // force the thread-exit release to be registered
    t_light = hazard_light_announce_available();
    for (std::size_t i = 0; i < kParticipants; ++i) {
        bool expected = false;
        if (g_participants[i].claimed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            t_participant = &g_participants[i];
            std::size_t hw = g_claimed_high_water.load(std::memory_order_relaxed);
            while (hw < i + 1 && !g_claimed_high_water.compare_exchange_weak(
                                     hw, i + 1, std::memory_order_acq_rel,
                                     std::memory_order_relaxed)) { /* retry with the new hw */
            }
            return t_participant;
        }
    }
    return nullptr;
}

}  // namespace detail_hz

/**
 * @brief An instance-owned, type-erased hazard-pointer reclamation domain (ADR-0072).
 *
 * Owned by a composition root (`graph_t` today), borrowed by reference by its tenants.
 * Reader side: @ref guard_t. Writer side: @ref retire. Teardown: the destructor runs the
 * final sweep — every parked block's reclaimer fires, so nothing a tenant retired can
 * outlive the domain.
 *
 * Thread-safety: `retire` and every guard operation may run concurrently from any thread,
 * and NEITHER EVER BLOCKS — no lock, no spin on another thread's progress. Destruction
 * requires quiescence — no guard live, no `retire` in flight — the same precondition every
 * C++ object's destructor has.
 */
class hazard_domain_t {
   public:
    /** @brief @ref reclaim_fn_t, under the name the tenants' call sites use. */
    using deleter_fn_t = reclaim_fn_t;

    /**
     * @brief How many blocks may park before a retire triggers a scan.
     *
     * A threshold, not a bound: it is the smallest batch that can hope to free anything,
     * since at most `kParticipants` blocks can be announced domain-wide at any instant.
     */
    static constexpr std::size_t kRetireBatch = detail_hz::kParticipants + 1;

    /**
     * @brief Construct a domain whose reclaimers return blocks to @p backend.
     *
     * @p backend must outlive the domain and must be thread-safe if tenants retire from
     * more than one thread (the same contract ADR-0060 §2 states for the graph's value
     * backend, which is what `graph_t` injects here). The domain itself never allocates.
     *
     * @p reclaim frees a block of THIS domain's one block kind. The type erasure ADR-0072
     * §3 wanted lives here, on a construction parameter, instead of on every retire record
     * — which is what lets @ref retire_link_t be a single word. A composition root with two
     * kinds of reclaimable block owns two domains.
     */
    explicit hazard_domain_t(mem_backend_t& backend, reclaim_fn_t reclaim) noexcept
        : backend_(backend), reclaim_(reclaim) {}

    hazard_domain_t(const hazard_domain_t&) = delete;
    hazard_domain_t& operator=(const hazard_domain_t&) = delete;

    /**
     * @brief The final sweep (ADR-0072 §2): free every parked block through its reclaimer.
     *
     * Precondition: quiescence — no live guard, no concurrent `retire`. `graph_t`'s
     * destructor satisfies it the way every member destructor does: a caller destroying the
     * graph under a live reader was already undefined.
     */
    ~hazard_domain_t() {
        retire_link_t* r = retired_.exchange(nullptr, std::memory_order_acq_rel);
        while (r != nullptr) {
            retire_link_t* next = r->next;
            reclaim_(r, backend_);
            r = next;
        }
        retired_n_.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Hand a displaced @p block to the domain: parked until no reader announces it,
     *        then freed via @p reclaim.
     *
     * **Never allocates, never blocks, never leaks, never aborts.** @p link is storage the
     * tenant owns (embedded in @p block, in the common case), which is what removes the
     * exhaustion path the ADR originally specified a blocking fallback for — see the file
     * header, property 1. The common case is one Treiber push; a threshold-crossing retire
     * additionally runs a scan (cold, O(participants + parked)), which frees the parked
     * blocks nobody announces and re-parks the rest. It never waits for a reader.
     *
     * Safe to call while holding a lock a reader's user callback might take: the whole
     * point of the design.
     *
     * @param link The displaced block's own first member; must not already be parked.
     */
    void retire(retire_link_t& link) noexcept {
        // Count BEFORE pushing: a concurrent scan may pop the list between the two, and its
        // decrement must never underflow the counter (the count may transiently exceed the
        // list length; it can never fall below it).
        const std::size_t n = retired_n_.fetch_add(1, std::memory_order_acq_rel) + 1;
        push_retired(&link);
        if (n >= kRetireBatch) scan();
    }

    /**
     * @brief Blocks currently parked (approximate under concurrency) — accounting for
     *        tests and a memory census, never a synchronization primitive.
     */
    [[nodiscard]] std::size_t retired_count() const noexcept {
        return retired_n_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Run a scan now (a cold control-plane courtesy — `retire` batches its own).
     */
    void collect() noexcept { scan(); }

    /**
     * @brief A reader's protection over one block for the length of one operation.
     *
     * Construction is FREE: the guard takes an announcement word only when @ref protect
     * finds something to announce, so a guard whose source turns out empty touches no
     * shared state at all. Destruction clears the announcement in one store — place the
     * guard so it destructs AFTER the user callback returns.
     *
     * Guards NEST to any depth. Past @ref detail_hz::kAnnouncePerThread levels on one
     * thread (or when every participant is claimed) a guard falls back to the domain's
     * stall counter, which pauses reclamation rather than waiting for anything — so an
     * inner guard can never deadlock against an outer one, and a user callback that
     * re-enters the graph is always safe.
     */
    class guard_t {
       public:
        /** @brief Bind to @p d. Announces nothing until @ref protect is called. */
        explicit guard_t(hazard_domain_t& d) noexcept : d_(d) {}

        guard_t(const guard_t&) = delete;
        guard_t& operator=(const guard_t&) = delete;

        /** @brief Clear the announcement (or release the stall), in one store. */
        ~guard_t() {
            if (ann_ != nullptr) {
                // Release, so the reader's reads of the block cannot sink past the clear —
                // that is exactly the window in which a scan would free it.
                ann_->store(nullptr, std::memory_order_release);
                --detail_hz::t_depth;
            } else if (stalled_) {
                d_.stall_.fetch_sub(1, std::memory_order_release);
            }
        }

        /**
         * @brief Announce the pointer @p src currently publishes and return it protected —
         *        the announce-then-re-read loop, so "not announced" means "cannot become
         *        announced" against a concurrent displace + scan.
         *
         * The returned pointer stays valid until this guard clears — i.e. through the
         * caller's user callback — even if a writer displaces and retires it meanwhile.
         *
         * @return The protected pointer, or `nullptr` when @p src publishes none.
         */
        template <typename T>
        [[nodiscard]] T* protect(const std::atomic<T*>& src) noexcept {
            T* p = src.load(std::memory_order_acquire);
            if (p == nullptr && ann_ == nullptr && !stalled_) return nullptr;
            if (ann_ == nullptr && !stalled_) take_slot();
            if (stalled_) {
                // Reclamation is paused domain-wide for this guard's life, so whatever the
                // source publishes cannot be freed under us. The seq_cst load is the other
                // half of scan()'s stall check: it puts this read after the stall increment
                // in the single total order, so a scan that saw stall == 0 provably ran
                // before this load and therefore before the displacement it might race.
                return src.load(std::memory_order_seq_cst);
            }
            for (;;) {
                T* again = announce_and_reread(p, src);
                if (again == p) return p;
                if (again == nullptr) {
                    ann_->store(nullptr, std::memory_order_release);
                    return nullptr;
                }
                p = again;
            }
        }

       private:
        /** @brief Take this thread's next announcement word, or fall back to the stall
         *         counter when there is none (table full, or nested too deep). */
        void take_slot() noexcept {
            detail_hz::participant_t* self = detail_hz::my_participant();
            if (self != nullptr && detail_hz::t_depth < detail_hz::kAnnouncePerThread) {
                ann_ = &self->ann[detail_hz::t_depth++];
                return;
            }
            d_.stall_.fetch_add(1, std::memory_order_seq_cst);
            stalled_ = true;
        }

        /**
         * @brief Publish @p p in this guard's word, then re-read @p src, with the ordering
         *        the domain's barrier mode requires.
         *
         * LIGHT mode: a plain store plus a compiler barrier. The StoreLoad the protocol
         * needs is supplied from the other side — the reclaimer's @ref hazard_heavy_barrier
         * serializes every CPU between its displacement and its read of the announcements,
         * so a reader either announced before that point (the reclaimer sees it) or issues
         * its re-read after it (and sees the displacement). HEAVY mode: the classical
         * `seq_cst` store, which is the fence itself.
         */
        template <typename T>
        [[nodiscard]] T* announce_and_reread(T* p, const std::atomic<T*>& src) noexcept {
            if (detail_hz::t_light) {
                ann_->store(p, std::memory_order_relaxed);
                std::atomic_signal_fence(std::memory_order_seq_cst);
                return src.load(std::memory_order_acquire);
            }
            ann_->store(p, std::memory_order_seq_cst);
            return src.load(std::memory_order_seq_cst);
        }

        hazard_domain_t& d_;                /**< @brief The domain being read. */
        std::atomic<void*>* ann_ = nullptr; /**< @brief This guard's announcement word. */
        bool stalled_ = false;              /**< @brief Holding the domain's stall instead. */
    };

   private:
    /** @brief Lock-free push onto the retired list (Treiber). */
    void push_retired(retire_link_t* rec) noexcept {
        retire_link_t* old = retired_.load(std::memory_order_relaxed);
        do {
            rec->next = old;
        } while (!retired_.compare_exchange_weak(old, rec, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed));
    }

    /**
     * @brief Free every parked block no participant announces; re-park the rest.
     *
     * @ref hazard_heavy_barrier is the load-bearing half of the protocol: it orders the
     * retirer's displacement before its reads of the announcements, against a reader's
     * announcement followed by its re-read of the source — one of the two must see the
     * other. Concurrent scans are safe: each owns exactly the batch it popped. Nothing here
     * waits for a reader — an announced block is simply re-parked for a later scan.
     */
    void scan() noexcept {
        retire_link_t* batch = retired_.exchange(nullptr, std::memory_order_acq_rel);
        if (batch == nullptr) return;

        hazard_heavy_barrier();

        // A stalled reader announces no address, so nothing can be proven unannounced:
        // re-park the whole batch. Bounded work, no waiting; it self-clears when the
        // stalling guard does.
        const bool stalled = stall_.load(std::memory_order_seq_cst) != 0;
        const std::size_t hw = detail_hz::g_claimed_high_water.load(std::memory_order_acquire);

        std::size_t popped = 0;
        std::size_t kept = 0;
        for (retire_link_t* cur = batch; cur != nullptr;) {
            retire_link_t* next = cur->next;  // the reclaimer frees the link with the block
            ++popped;
            if (stalled || announced(cur, hw)) {
                push_retired(cur);  // still announced — park it for a later scan
                ++kept;
            } else {
                reclaim_(cur, backend_);
            }
            cur = next;
        }
        retired_n_.fetch_sub(popped - kept, std::memory_order_acq_rel);
    }

    /**
     * @brief Is @p block announced by any participant right now?
     *
     * Read directly out of the table rather than into a snapshot buffer: the snapshot
     * would be a `kParticipants * kAnnouncePerThread` pointer array on the stack of a
     * function a bounded target calls under its map lock, and the loop it saves is cold.
     * Re-reading per block is sound for the same reason one read is: past
     * @ref hazard_heavy_barrier no reader can NEWLY announce an already-displaced block —
     * it would re-read the source and find it gone — so "unannounced now" is permanent
     * for that block.
     */
    [[nodiscard]] static bool announced(const void* block, std::size_t hw) noexcept {
        for (std::size_t i = 0; i < hw; ++i) {
            for (const std::atomic<void*>& a : detail_hz::g_participants[i].ann) {
                if (a.load(std::memory_order_seq_cst) == block) return true;
            }
        }
        return false;
    }

    mem_backend_t& backend_;     /**< @brief Passed to the reclaimer (ADR-0072 §3). */
    const reclaim_fn_t reclaim_; /**< @brief Frees this domain's one kind of block. */
    std::atomic<retire_link_t*> retired_{nullptr}; /**< @brief Parked blocks awaiting a scan. */
    std::atomic<std::size_t> retired_n_{0};        /**< @brief Approximate length of `retired_`. */
    /** @brief Guards that could get no announcement word; while non-zero a scan frees
     *         nothing. A counter, never a lock — it nests and it cannot deadlock. */
    std::atomic<std::size_t> stall_{0};
};

}  // namespace tr::mem
