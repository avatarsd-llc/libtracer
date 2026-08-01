/**
 * @file
 * @brief The graph-owned, non-blocking hazard-pointer reclamation domain (ADR-0072 + its
 *        erratum 1: intrusive records, per-thread announcement, asymmetric barriers, and
 *        erratum 2: reclamation is a caller-scheduled event, not a side effect of retiring).
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
 * ## The four properties that make it usable on a bounded, latency-first node
 *
 *   1. **`retire` allocates nothing, never waits, and NEVER FREES ANYTHING.** The retire
 *      record is INTRUSIVE: the tenant embeds a `retire_link_t` in the block it will one
 *      day retire, so parking a block is a Treiber push of storage that already exists.
 *      There is no allocation, so there is no exhaustion path, so there is no "block until
 *      the readers drain" fallback — the shape that deadlocked against `graph_t::map_mutex_`
 *      (a retirer holds the map lock; the announcing readers are outside it *precisely so
 *      their user callbacks may re-enter the graph*, so waiting for one is waiting for a
 *      thread that is waiting for you). And it frees nothing, which is the other half of the
 *      same lesson: freeing a block runs the TENANT's destructor, which for a value seam is
 *      a `std::function`'s captured state — arbitrary user code, permitted by RFC-0010 §A.3
 *      to re-enter the graph. A retirer holding the map lock cannot be allowed to run it
 *      either. Reclamation is therefore a separate, caller-scheduled event: `collect()`,
 *      which the tenant calls where it holds nothing. See `retire` and `collect` below.
 *   2. **The reader announces into a PER-THREAD word, not a per-operation cell.** A cell
 *      claimed per operation costs a `seq_cst` CAS on shared storage every single read; a
 *      thread claims its participant once (and releases it at thread exit) and thereafter
 *      pays a store to a line it owns. Announcement storage is process-global rather than
 *      per-domain, which is safe *because announcements are compared by ADDRESS*: two live
 *      blocks in two domains cannot share one, so a foreign announcement can never match a
 *      parked block. ADR-0072 §2's three reasons to own the domain per instance — an
 *      injected resource, a teardown point, isolated bounds — are all about the RETIRED
 *      LIST, which stays per-instance.
 *   3. **Participants and nesting depth are UNBOUNDED, and cost zero `.bss` by default.**
 *      Neither "how many threads read" nor "how deep a user callback re-enters" is a number
 *      this library gets to pick (the no-synthetic-limits rule), and a fixed table sized for
 *      the worst case would be unconditional static RAM on a node that runs one reader —
 *      4 KB of a ~16 KB budget on the MCU half of the dual target. So participants are
 *      claimed per thread on first use and released at thread exit, and a thread that nests
 *      past its participant's inline words extends it with a grow-only chain. The
 *      @ref tr::graph::kReclaimAnnounceSlots knob reserves the first N participants in
 *      `.bss` for a target that must not touch the allocator on a reader path; it defaults
 *      to 0 and it is a *determinism* knob, never a capacity one.
 *   4. **The announcement store is LIGHT where the platform can flush a remote store
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
 * `hazard_domain_t::retire`, which parks it. The tenant then calls `collect()` **outside
 * every lock it owns**; that is where blocks nobody announces are freed.
 *
 * **Nothing on either side blocks, and no stalled reader can freeze the domain.** A guard
 * that can get no announcement word AT ALL — which now requires the process to be unable to
 * allocate a participant, since neither thread count nor nesting depth is bounded any more —
 * increments the domain's *stall* counter, which makes scans free nothing for the life of
 * that guard. It is a counter, not a lock: it nests to any depth and it cannot self-deadlock.
 * A stall is domain-wide while it lasts (it is the absence of information about what one
 * reader holds, and the domain cannot know which blocks that covers), so a scan under one
 * costs O(1) rather than O(parked): it declines to scan and raises its own threshold, which
 * is what keeps `retire` amortized O(1) whatever a reader is doing.
 */
#pragma once

#include <atomic>
#include <cstddef>
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
 *     part, the announcement storage, is shared process-wide, so the second one is nearly
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
 * @brief The process-global announcement registry the reader protocol writes into.
 *
 * Global rather than per-domain because announcements are matched by ADDRESS: a block
 * parked in one domain can never equal a live block announced against another, so a shared
 * registry costs a scan a few more words to read and buys every reader a per-thread claim
 * (see the file header, property 2).
 *
 * **Its static footprint is @ref tr::graph::kReclaimAnnounceSlots participants — zero by
 * default.** That is a correction of this header's own first cut, which sized a fixed table
 * off `kHazardReaderSlots` and so emitted 4 KB of unconditional `.bss` into every build,
 * including the MCU one where `main` emits no hazard registry at all. Participants past the
 * reserved N are allocated on a thread's first protected read and reused by later threads,
 * so the census moves with the threads a node actually runs.
 */
namespace detail_hz {

/**
 * @brief Announcement words per chain link, i.e. how deep a thread may nest guards before
 *        it extends its participant.
 *
 * Two, because the library's own reachable nesting is exactly two deep: a seam read
 * announces, and its user callback may re-enter the graph and announce a second seam. A
 * third level costs one grow-only chain link on that thread, once — never a stall, never a
 * limit (the no-synthetic-limits rule).
 */
inline constexpr std::size_t kAnnouncePerThread = 2;

/** @brief Participants reserved in `.bss` — see @ref tr::graph::kReclaimAnnounceSlots. */
inline constexpr std::size_t kStaticParticipants = graph::kReclaimAnnounceSlots;

/** @brief Padding width: the cache-line knob, floored at the payload's own alignment so a
 *         single-core build (knob 0) stays well-formed. */
inline constexpr std::size_t kAnnAlign = graph::kCacheLineBytes > alignof(std::atomic<void*>)
                                             ? graph::kCacheLineBytes
                                             : alignof(std::atomic<void*>);

/**
 * @brief One link of a thread's announcement chain: @ref kAnnouncePerThread words plus the
 *        pointer to the next link.
 *
 * Links are **grow-only**: the owning thread appends one when it nests deeper than the
 * chain currently reaches, and nothing ever unlinks or frees one. That is what makes it
 * safe for a scan to walk the chain concurrently — the alternative, popping a link when the
 * nesting unwinds, would hand a scanner a pointer into storage the reader is about to
 * reuse. A link outlives the thread that added it and is reused by whichever thread claims
 * the participant next.
 */
struct ann_link_t {
    std::atomic<void*> ann[kAnnouncePerThread]{}; /**< @brief Announced blocks; null = free. */
    std::atomic<ann_link_t*> deeper{nullptr};     /**< @brief Next link; grow-only. */
};

/**
 * @brief One thread's announcement storage — cache-line isolated so a scan cannot false-
 *        share with a reader mid-announce.
 *
 * Claimed by a thread on its first protected read and released at its exit; a released
 * participant is reused, never freed, so a process that churns threads reaches a high-water
 * mark rather than growing without bound.
 */
struct alignas(kAnnAlign) participant_t {
    std::atomic<bool> claimed{false};          /**< @brief Owned by some live thread. */
    ann_link_t words{};                        /**< @brief This thread's first announce link. */
    std::atomic<participant_t*> next{nullptr}; /**< @brief Registry link (allocated nodes). */
};

/**
 * @brief The reserved participants, if any. Specialized at 0 so that the default build
 *        emits no table at all rather than a one-element stub.
 */
template <std::size_t N>
struct static_participants_t {
    participant_t p[N]; /**< @brief The reserved participants themselves. */
    /** @brief The i-th reserved participant. */
    [[nodiscard]] participant_t* at(std::size_t i) noexcept { return &p[i]; }
};
/** @brief The default: nothing reserved, so nothing emitted. */
template <>
struct static_participants_t<0> {
    /** @brief Never called — @ref kStaticParticipants is 0, so no index exists. */
    [[nodiscard]] participant_t* at(std::size_t) noexcept { return nullptr; }
};

/** @brief The reserved participants (empty by default — see @ref static_participants_t). */
inline static_participants_t<kStaticParticipants> g_static{};
/** @brief Head of the allocated-participant registry: everything past the reserved ones.
 *         Push-only and never freed, so a scan may walk it without protecting it. */
inline std::atomic<participant_t*> g_allocated{nullptr};

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
/** @brief Set while this thread is inside a scan, so a reclaimed block's own destructor —
 *         arbitrary user code — cannot recurse back into one. The skipped scan is not lost:
 *         the outer scan already popped its batch, and the next @ref hazard_domain_t::collect
 *         picks up whatever the destructor retired. */
inline thread_local bool t_in_scan = false;

/** @brief Releases this thread's participant back to the registry at thread exit — the only
 *         thing here with a destructor, and it is touched ONLY on the cold claim path so
 *         the hot path never pays for its initialization guard. Without it a process that
 *         churns threads would allocate a participant per thread forever. */
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

/**
 * @brief FAULT INJECTION, cold path only: make announcement storage unobtainable, as an
 *        allocation failure would.
 *
 * The stall counter is the domain's last-resort degradation and, since participants and
 * nesting depth became unbounded, an allocation failure is the only thing that reaches it —
 * which would leave the repo's scariest branch unexercised, and the mutation-sweep rule
 * says a guard is vacuous until it is. Read only by @ref claim_participant and
 * @ref announce_word, both of which run once per thread (or once per new nesting level),
 * never on a protected read. Nothing but a test ever writes it.
 */
inline std::atomic<bool> g_deny_announce_storage{false};

/**
 * @brief Claim a participant for this thread: a free reserved one, else a free allocated
 *        one, else a newly allocated one. `nullptr` only when the allocation itself fails.
 */
[[nodiscard]] inline participant_t* claim_participant() noexcept {
    if (g_deny_announce_storage.load(std::memory_order_relaxed)) return nullptr;
    for (std::size_t i = 0; i < kStaticParticipants; ++i) {
        participant_t* p = g_static.at(i);
        bool expected = false;
        if (p->claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
            return p;
    }
    for (participant_t* p = g_allocated.load(std::memory_order_acquire); p != nullptr;
         p = p->next.load(std::memory_order_acquire)) {
        bool expected = false;
        if (p->claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
            return p;
    }
    // Cold: once per thread, and only for a thread past the reserved count. The node is
    // never freed — it goes on the registry a scan walks lock-free, and the next thread
    // reuses it — so its lifetime is the process's and LeakSanitizer sees it as reachable.
    participant_t* fresh = new (std::nothrow) participant_t{};
    if (fresh == nullptr) return nullptr;
    fresh->claimed.store(true, std::memory_order_relaxed);
    participant_t* head = g_allocated.load(std::memory_order_relaxed);
    do {
        fresh->next.store(head, std::memory_order_relaxed);
    } while (!g_allocated.compare_exchange_weak(head, fresh, std::memory_order_release,
                                                std::memory_order_relaxed));
    return fresh;
}

/** @brief This thread's participant, claiming one on first use. `nullptr` only when the
 *         process could not allocate one — the single remaining road to a stall. */
[[nodiscard]] inline participant_t* my_participant() noexcept {
    if (t_participant != nullptr) return t_participant;
    t_releaser.armed = true;  // force the thread-exit release to be registered
    t_light = hazard_light_announce_available();
    t_participant = claim_participant();
    return t_participant;
}

/**
 * @brief This thread's announcement word at nesting depth @p depth, extending its chain if
 *        it does not reach that far. `nullptr` only when extending needs an allocation the
 *        process cannot serve.
 *
 * Only the owning thread walks or extends the chain, so the append needs no CAS — just the
 * release store that publishes the new link to a concurrently walking scan.
 */
[[nodiscard]] inline std::atomic<void*>* announce_word(participant_t& self,
                                                       std::size_t depth) noexcept {
    ann_link_t* link = &self.words;
    while (depth >= kAnnouncePerThread) {
        ann_link_t* deeper = link->deeper.load(std::memory_order_acquire);
        if (deeper == nullptr) {
            if (g_deny_announce_storage.load(std::memory_order_relaxed)) return nullptr;
            deeper = new (std::nothrow) ann_link_t{};
            if (deeper == nullptr) return nullptr;
            link->deeper.store(deeper, std::memory_order_release);
        }
        link = deeper;
        depth -= kAnnouncePerThread;
    }
    return &link->ann[depth];
}

/** @brief Is @p block announced anywhere in @p self's chain? */
[[nodiscard]] inline bool announced_by(participant_t& self, const void* block) noexcept {
    for (ann_link_t* link = &self.words; link != nullptr;
         link = link->deeper.load(std::memory_order_acquire))
        for (std::atomic<void*>& a : link->ann)
            if (a.load(std::memory_order_acquire) == block) return true;
    return false;
}

/**
 * @brief Is @p block announced by any participant right now?
 *
 * Read directly out of the registry rather than into a snapshot buffer: the snapshot would
 * be a stack array sized by a number that is no longer a compile-time constant, and the
 * loop it saves is cold. Re-reading per block is sound for the same reason one read is:
 * past @ref hazard_heavy_barrier no reader can NEWLY announce an already-displaced block —
 * it would re-read the source and find it gone — so "unannounced now" is permanent for that
 * block. Acquire rather than `seq_cst` on the loads: the StoreLoad the protocol needs is
 * the barrier the caller already issued, not a per-load fence.
 */
[[nodiscard]] inline bool announced(const void* block) noexcept {
    for (std::size_t i = 0; i < kStaticParticipants; ++i)
        if (announced_by(*g_static.at(i), block)) return true;
    for (participant_t* p = g_allocated.load(std::memory_order_acquire); p != nullptr;
         p = p->next.load(std::memory_order_acquire))
        if (announced_by(*p, block)) return true;
    return false;
}

}  // namespace detail_hz

/**
 * @brief An instance-owned, type-erased hazard-pointer reclamation domain (ADR-0072).
 *
 * Owned by a composition root (`graph_t` today), borrowed by reference by its tenants.
 * Reader side: @ref guard_t. Writer side: @ref retire, then @ref collect. Teardown: the
 * destructor runs the final sweep — every parked block's reclaimer fires, so nothing a
 * tenant retired can outlive the domain.
 *
 * Thread-safety: `retire` and every guard operation may run concurrently from any thread,
 * and NEITHER EVER BLOCKS — no lock, no spin on another thread's progress, no allocation.
 * `collect` is also concurrency-safe, but it runs the tenant's reclaimer and therefore the
 * tenant's destructors: see its own contract. Destruction requires quiescence — no guard
 * live, no `retire` in flight — the same precondition every C++ object's destructor has.
 */
class hazard_domain_t {
   public:
    /** @brief @ref reclaim_fn_t, under the name the tenants' call sites use. */
    using deleter_fn_t = reclaim_fn_t;

    /**
     * @brief The smallest parked batch @ref reclaim_due reports as worth collecting.
     *
     * A batching threshold, never a bound: a scan costs one process-wide barrier (an IPI
     * broadcast where the light protocol is available), so collecting one block at a time
     * would spend that barrier per retire. The live threshold RISES from here — see
     * @ref reclaim_due — so that a batch a scan could not free never has to be re-walked by
     * the very next retire.
     */
    static constexpr std::size_t kRetireBatch = 16;

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
     * graph under a live reader was already undefined. Like @ref collect, this runs the
     * tenant's destructors, so a composition root places the domain where they are still
     * legal to run — for `graph_t`, early in its member teardown, with no lock held.
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
     * @brief Park a displaced @p block until no reader announces it. **Frees nothing.**
     *
     * Never allocates, never blocks, never leaks, never aborts, and never runs a line of
     * tenant code: it is one Treiber push of storage the tenant already owns. That is what
     * makes it safe under any lock, including one a reader's user callback takes — which is
     * the whole point, because the retirer that displaced the block is holding exactly such
     * a lock.
     *
     * Freeing is deliberately NOT here (ADR-0072 erratum 2). The reclaimer runs the block's
     * destructor, and for the value seam that destroys a `std::function`'s captured state —
     * arbitrary user code, which RFC-0010 §A.3 permits to call back into the graph. Running
     * it from `retire` would run it under the caller's lock. So the tenant calls
     * @ref collect once it holds nothing; @ref reclaim_due says when that is worth doing.
     *
     * @param link The displaced block's own first member; must not already be parked.
     */
    void retire(retire_link_t& link) noexcept {
        // Count BEFORE pushing: a concurrent scan may pop the list between the two, and its
        // decrement must never underflow the counter (the count may transiently exceed the
        // list length; it can never fall below it).
        retired_n_.fetch_add(1, std::memory_order_acq_rel);
        push_retired(&link, &link);
    }

    /**
     * @brief Has enough parked to be worth a @ref collect?
     *
     * The threshold starts at @ref kRetireBatch and is RAISED by any scan that could not
     * free what it walked — to twice what stayed parked. Two things follow, and both are
     * requirements rather than tuning:
     *
     *   - **`retire` stays amortized O(1)** however many blocks are unfreeable. A fixed
     *     threshold makes every retire past it run a full scan — O(parked), each with a
     *     process-wide barrier — for as long as one reader stays inside a callback. That
     *     was measured at 0.062 µs/retire unstalled against 105 µs/retire at 15 000 parked,
     *     still climbing.
     *   - **The parked set stays bounded**, at roughly twice the blocks readers actually
     *     hold plus a batch — proportional to concurrency, not to time, which is the
     *     property #576 exists to restore.
     */
    [[nodiscard]] bool reclaim_due() const noexcept {
        return retired_n_.load(std::memory_order_relaxed) >=
               threshold_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Blocks currently parked (approximate under concurrency) — accounting for
     *        tests and a memory census, never a synchronization primitive.
     */
    [[nodiscard]] std::size_t retired_count() const noexcept {
        return retired_n_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Free every parked block no participant announces — the reclamation event.
     *
     * **Call this holding nothing a tenant's destructor could need.** It runs the domain's
     * reclaimer, which runs the retired block's destructor, which is tenant (and, through a
     * `std::function`'s captures, embedder) code that may take any lock and re-enter any
     * API. Every other operation on this domain is safe under a lock; this one is not, and
     * the difference is the whole of ADR-0072 erratum 2.
     *
     * Cold, non-blocking, and safe to call concurrently with itself (each scan owns exactly
     * the batch it popped) and with `retire`. Re-entering it from a reclaimed block's own
     * destructor is a no-op, so a destructor that retires and collects cannot recurse.
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
     * Guards NEST to any depth, at the cost of one grow-only chain link per two levels on
     * the thread that goes there. Only an allocation failure leaves a guard without a word,
     * and it then falls back to the domain's stall counter, which pauses reclamation rather
     * than waiting for anything — so an inner guard can never deadlock against an outer one,
     * and a user callback that re-enters the graph is always safe.
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
                d_.stall_.fetch_sub(1, std::memory_order_seq_cst);
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
                // in the single total order, so a scan that popped its batch before seeing
                // stall == 0 provably ran before this load — and therefore before any
                // displacement this load could still miss.
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
        /** @brief Take this thread's next announcement word, extending its chain if the
         *         nesting has gone deeper than the chain reaches. Only an allocation
         *         failure lands on the stall counter. */
        void take_slot() noexcept {
            detail_hz::participant_t* self = detail_hz::my_participant();
            if (self != nullptr) {
                if (std::atomic<void*>* word = detail_hz::announce_word(*self, detail_hz::t_depth);
                    word != nullptr) {
                    ann_ = word;
                    ++detail_hz::t_depth;
                    return;
                }
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
    /** @brief Lock-free push of the sublist @p head .. @p tail onto the retired list
     *         (Treiber). One node is the degenerate case, `head == tail`. */
    void push_retired(retire_link_t* head, retire_link_t* tail) noexcept {
        retire_link_t* old = retired_.load(std::memory_order_relaxed);
        do {
            tail->next = old;
        } while (!retired_.compare_exchange_weak(old, head, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed));
    }

    /** @brief Sets @ref detail_hz::t_in_scan for the length of one scan. */
    struct scan_reentry_t {
        scan_reentry_t() noexcept { detail_hz::t_in_scan = true; }
        ~scan_reentry_t() { detail_hz::t_in_scan = false; }
    };

    /**
     * @brief Raise the collect threshold to twice what is parked, so the next @ref retire
     *        past it is geometrically later rather than immediately.
     */
    void raise_threshold(std::size_t parked) noexcept {
        const std::size_t next = parked > kRetireBatch / 2 ? parked * 2 : kRetireBatch;
        threshold_.store(next, std::memory_order_relaxed);
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
        // A reclaimed block's destructor may retire and collect again; let the outer scan
        // own the work rather than recursing through user code.
        if (detail_hz::t_in_scan) return;
        const scan_reentry_t reentry;

        // Cheap early-out on a stalled domain: declining to scan is always safe, and NOT
        // popping the list is what keeps this O(1) instead of O(parked) (#576 round 2 —
        // a persistent stall made every retire re-walk and re-park the whole set).
        if (stall_.load(std::memory_order_relaxed) != 0) {
            raise_threshold(retired_n_.load(std::memory_order_relaxed));
            return;
        }

        retire_link_t* batch = retired_.exchange(nullptr, std::memory_order_acq_rel);
        if (batch == nullptr) return;

        hazard_heavy_barrier();

        // The correctness check, and it must come AFTER the pop: a stalled reader announces
        // no address, so nothing in this batch can be proven unannounced. Seeing zero here
        // means every stalling guard that could hold one of these blocks had already
        // cleared — a guard whose increment follows this load re-reads its source after it,
        // past the displacement of everything in the batch, so it cannot hold one.
        if (stall_.load(std::memory_order_seq_cst) != 0) {
            retire_link_t* tail = batch;
            while (tail->next != nullptr) tail = tail->next;
            push_retired(batch, tail);
            raise_threshold(retired_n_.load(std::memory_order_relaxed));
            return;
        }

        retire_link_t* keep = nullptr;
        retire_link_t* keep_tail = nullptr;
        std::size_t freed = 0;
        for (retire_link_t* cur = batch; cur != nullptr;) {
            retire_link_t* next = cur->next;  // the reclaimer frees the link with the block
            if (detail_hz::announced(cur)) {
                if (keep_tail == nullptr) keep_tail = cur;
                cur->next = keep;
                keep = cur;
            } else {
                reclaim_(cur, backend_);  // tenant code — see collect()'s contract
                ++freed;
            }
            cur = next;
        }
        if (keep != nullptr) push_retired(keep, keep_tail);
        if (freed != 0) retired_n_.fetch_sub(freed, std::memory_order_acq_rel);
        raise_threshold(retired_n_.load(std::memory_order_relaxed));
    }

    mem_backend_t& backend_;     /**< @brief Passed to the reclaimer (ADR-0072 §3). */
    const reclaim_fn_t reclaim_; /**< @brief Frees this domain's one kind of block. */
    std::atomic<retire_link_t*> retired_{nullptr}; /**< @brief Parked blocks awaiting a scan. */
    std::atomic<std::size_t> retired_n_{0};        /**< @brief Approximate length of `retired_`. */
    /** @brief Parked blocks at which @ref reclaim_due fires — raised by a scan that could
     *         not free what it walked, so retire never becomes O(parked). */
    std::atomic<std::size_t> threshold_{kRetireBatch};
    /** @brief Guards that could get no announcement word at all — i.e. that met an
     *         allocation failure, the one road left. While non-zero a scan frees nothing
     *         and costs O(1). A counter, never a lock — it nests and it cannot deadlock. */
    std::atomic<std::size_t> stall_{0};
};

}  // namespace tr::mem
