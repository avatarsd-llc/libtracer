/**
 * @file
 * @brief The LKV slot policy (ADR-0069 §1): how a vertex publishes and reads its
 *        last-known value.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `vertex_t` owns exactly one slot and touches it through three operations — publish,
 * clear, read. Naming that surface is what lets a target choose its reclamation strategy
 * at build time (ADR-0069) instead of every target paying for one compromise: a
 * single-core sensor node is write-dominated and wants the cheapest publish, while a
 * many-core host's cost is the read side, which today INVERTS under concurrent readers
 * (measured: 21.1 M `graph_t::read`/s at one reader falling to 1.7 at twenty-four —
 * `bench/bench_lkv_slot.cpp`, `lkvgraph_hot1-fan0-read`).
 *
 * ## The policy contract
 *
 * A slot type must provide, for `value_ptr_t = std::shared_ptr<const view::rope_t>`:
 *
 *   - `[[nodiscard]] bool store(value_ptr_t)` — publish, sequentially consistent. The order
 *     matters: `vertex_t::store` relies on this sharing one total order with the `write_seq_`
 *     bump and the waiter count, which is what makes the waiterless publish (#555) unable to
 *     lose a wakeup. **`false` means nothing was published** and the previous value still
 *     stands — the caller must soft-fail (#477), never report the write as taken.
 *   - `void clear(std::memory_order)` — drop the published value. Cannot fail, and says so in
 *     the return type: a clear releases resources rather than acquiring any. Only
 *     `revert_to_placeholder` calls it, with `release`.
 *   - `load() const` — read the published value.
 *
 * `store` returning `bool` is not ceremony. A slot that reclaims lazily has to allocate to
 * publish, and a policy surface that cannot say "I did not take this" forces the one thing
 * #477 exists to prevent: a dropped write reported as a successful one.
 *
 * ## The constraint any future slot must satisfy
 *
 * `load()` returns an **owning** handle, and that is not negotiable: `graph_t`'s composed
 * branch read (`read_subtree_folded`) stashes one LKV per node into a vector that outlives
 * the map lock and spans three passes, so **N values are held simultaneously**. A scheme
 * that can protect only one value per reader at a time — hazard pointers, as classically
 * stated — therefore cannot simply hand back a pinned pointer; it must promote the pin to
 * a counted reference before releasing it, and that promotion is a read-modify-write on
 * the one cache line every reader shares. Measured, that promotion is the difference
 * between a 1,806x and a 20.8x read win at twenty-four readers, which is why ADR-0069
 * carries the smaller number (see #642, and the erratum in #643).
 *
 * ## What the second policy actually bought
 *
 * Measured end to end, `graph_t::read` on one shared LKV, both slots built from this tree
 * (medians of six alternating runs, 24-core host — ADR-0069 §6):
 *
 * | readers | `sp_atomic_slot_t` | `hazard_slot_t` | gain |
 * | ---: | ---: | ---: | ---: |
 * | 1 | 21.1 M/s | 18.7 M/s | within run-to-run spread |
 * | 8 | 2.2 M/s | 5.2 M/s | 2.4x |
 * | 24 | 1.7 M/s | 7.4 M/s | **4.2x** |
 *
 * Four times, not the twenty the model bench projected: the slot is one term of a real
 * read, and once its lock bit is gone the rest of the path is what limits scaling. The
 * inversion is softened rather than removed — twenty-four readers still collectively run
 * 2.9x slower than one, against 12x before. Every other shape (distinct vertices, and the
 * write shapes) landed inside the 1.4-2.2x run-to-run spread, so the projected single-core
 * write penalty is not observable through `graph_t::write`.
 *
 * Two limits sit above this slot, and only the second is the slot's business (ADR-0069 §6,
 * second erratum — measured by ablation, after two wrong inferences):
 *
 *   - **Every `graph_t::read` takes `map_mutex_` shared** before it reaches the slot, to decide
 *     the leaf/branch fork (`graph.cpp:697` -> `:706`). That caps all reads at roughly 20 M/s
 *     per process whatever the topology; short-circuiting it takes the distinct-vertex read
 *     from 19.7 to 165.3 M/s at twenty-four readers. No slot policy can move it.
 *   - **On one shared vertex that lock is not what binds** — removing it changes nothing
 *     (1.74 -> 1.64 M/s). There the limit is the rope's control-block increment, the promotion
 *     an owning read cannot skip, which makes the next lever for THAT shape an API question
 *     rather than a reclamation one: the three `read_stored()` call sites that never keep the
 *     handle could take a scoped read instead.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>

#include "libtracer/config.hpp"
#include "libtracer/rope.hpp"

namespace tr::graph {

/**
 * @brief The slot libtracer ships today: `std::atomic<std::shared_ptr<const rope_t>>`.
 *
 * Reclamation is the shared_ptr refcount, so there is no scheme to implement and no
 * registry to size — the reason this is the checked-in default, and the reason a raw `-I`
 * consumer and the stock ESP-IDF component keep building exactly what they built before
 * the slot became a policy.
 *
 * **Lock-free BY CONTRACT, and spin-locked in practice.**
 * `std::atomic<std::shared_ptr<T>>::is_lock_free()` returns 0 on libstdc++, so both load
 * and store take its internal pointer-lock bit (`lock cmpxchg` to acquire, an `xchg` to
 * release). Measured, that is ~77 of the ~316 cycles of an in-process write and the
 * largest single term left on the path — 88% of `store`'s samples land on those three
 * instructions. Do not read "lock-free" here as "no serializing operation"; ADR-0064 §2
 * records why, and ADR-0069 records what replaces it on a host.
 */
class sp_atomic_slot_t {
   public:
    /** @brief The handle a publish takes and a read returns — owning, by the contract above. */
    using value_ptr_t = std::shared_ptr<const view::rope_t>;

    /**
     * @brief Publish. Sequentially consistent unless the caller says otherwise — the default
     *        is what orders the publish with `write_seq_` and the waiter count, which is what
     *        makes the waiterless publish (#555) unable to lose a wakeup.
     * @return Always `true`. This policy allocates nothing to publish — it takes a reference
     *         it was handed — so it has no failure to report. The signature exists because the
     *         *contract* has one (see the file header), not because this implementation does.
     */
    [[nodiscard]] bool store(value_ptr_t sp, std::memory_order order = std::memory_order_seq_cst) {
        v_.store(std::move(sp), order);
        return true;
    }

    /** @brief Drop the published value. Releases a reference; cannot fail. */
    void clear(std::memory_order order = std::memory_order_seq_cst) { v_.store({}, order); }

    /**
     * @brief Read the published value.
     *
     * A mid-read reader holds its own reference, so a concurrent publish or clear cannot
     * free the value under it — that is the whole of this policy's reclamation.
     */
    [[nodiscard]] value_ptr_t load() const { return v_.load(); }

   private:
    std::atomic<value_ptr_t> v_{};
};

/**
 * @brief The process-wide hazard-pointer domain behind @ref hazard_slot_t (ADR-0069 §2/§5).
 *
 * One domain serves every vertex: a per-vertex registry would cost
 * `kHazardReaderSlots * 64` bytes **per vertex**, which is not a slot policy, it is a leak
 * with a schedule. Every participant (reader or writer) claims one index for the life of
 * its thread; the index owns one announcement cell and one pair of node lists.
 */
namespace detail_hp {

/**
 * @brief The indirection node the slot publishes — it owns the rope's `shared_ptr`
 *        (ADR-0069 §5).
 *
 * The slot cannot hold the rope pointer directly: `std::atomic<std::shared_ptr<T>>` and
 * `std::atomic<std::weak_ptr<T>>` are both non-lock-free on libstdc++ (measured), and from
 * a bare `const rope_t*` there is no route back to a control block. So the slot holds
 * `atomic<node_t*>` — lock-free — and a read copies the `shared_ptr` **out of the pinned
 * node**, which is what turns a pin into the owning handle `read_stored()` must return.
 *
 * `sp` is immutable while the node is published; `next` is touched only once the node is
 * off the slot, so the two never race.
 */
struct node_t {
    std::shared_ptr<const view::rope_t> sp; /**< @brief The published value. */
    node_t* next = nullptr;                 /**< @brief Retire / free-list link. */
};

/**
 * @brief The domain's padding width: @ref tr::graph::kCacheLineBytes, floored at the widest
 *        payload's natural alignment so every value of the knob stays well-formed.
 *
 * A single-core target sets the knob to 0 and the two tables below collapse to their
 * payloads — there is no second core for a scan to false-share against. See the knob's own
 * documentation for why `alignas` may not simply be handed the number.
 */
inline constexpr std::size_t kDomainAlign =
    std::max({graph::kCacheLineBytes, alignof(std::atomic<node_t*>), alignof(std::size_t)});

/**
 * @brief One participant's announcement, cache-line isolated so a scan does not false-share.
 *
 * **Nothing else may live here.** The announcement is written by its owner on every `load()`,
 * so any other atomic sharing this line turns a write to that atomic into an invalidation of a
 * reader's hot line — which is the whole cost `kDomainAlign` was spent to avoid. The claim
 * state used to sit in this struct and did exactly that (#899); see `%claims_t`.
 */
struct alignas(kDomainAlign) cell_t {
    std::atomic<node_t*> pinned{nullptr}; /**< @brief The node this thread is reading, or null. */
};

static_assert(alignof(cell_t) == kDomainAlign,
              "the cell's alignas was silently dropped — see kDomainAlign's derivation");

/**
 * @brief The claim table's word — `size_t`, so the bitmap is never WIDER than the target's
 *        pointer and cannot be the reason a claim takes a lock.
 *
 * `uint64_t` would have been the obvious width and is the wrong one: on every 32-bit target
 * this library binds `%hazard_slot_t` on, a 64-bit atomic is not lock-free, so the table would
 * acquire a libatomic lock to claim an index. `size_t` removes the WIDTH as a cause.
 *
 * It does not, and cannot, make the table lock-free on a target that has no atomics at all:
 * esp32c3 is `rv32imc` — no A extension — so nothing of any width is always-lock-free there.
 * That is why the lock-free requirement is asserted where the slot BINDING is visible
 * (`%vertex.hpp`, beside `%lkv_slot_t`) rather than here: this header is included by every
 * consumer of a vertex, including targets that never bind `%hazard_slot_t` and for which the
 * requirement is vacuous. A namespace-scope assert here fires for all of them — it broke both
 * esp32c3 CI legs — because "the domain is emitted only in a build that binds `%hazard_slot_t`"
 * is a statement about CODEGEN, and a `static_assert` is not codegen.
 */
using claim_word_t = std::size_t;

/** @brief Claimable indices per `%claim_word_t`. */
inline constexpr std::size_t kClaimBitsPerWord =
    static_cast<std::size_t>(std::numeric_limits<claim_word_t>::digits);

/** @brief Words the claim bitmap needs to cover `kHazardReaderSlots` indices. */
inline constexpr std::size_t kClaimWords =
    (kHazardReaderSlots + kClaimBitsPerWord - 1) / kClaimBitsPerWord;

/**
 * @brief Which indices are owned — one bit each, on a line no announcement shares (#899).
 *
 * Separate storage, not tidiness. A thread that failed to claim re-probes this table on every
 * single operation, and a probe is a read-modify-write: with the flag inside `%cell_t`, each
 * probe took an in-capacity reader's announcement line exclusive, so one over-capacity thread
 * degraded every reader that was inside its budget — the opposite of what `%kOverflowIndex`
 * promises. Packed rather than padded on purpose: the bits are read as a **prefilter**, so the
 * fewer lines they span the cheaper a probe of a full table is (one load per 64 indices), and
 * claiming is a once-per-thread event that contention cannot matter to.
 */
struct alignas(kDomainAlign) claims_t {
    std::array<std::atomic<claim_word_t>, kClaimWords> words{}; /**< @brief One bit per index. */
};

static_assert(alignof(claims_t) == kDomainAlign,
              "the claim table's alignas was silently dropped — it must not share a cell's line");
static_assert(sizeof(claim_word_t) <= sizeof(void*),
              "the claim word must not be wider than the target's pointer — a wider one is not "
              "lock-free on a 32-bit target and would take a libatomic lock to claim an index; "
              "the lock-free requirement itself is asserted beside lkv_slot_t in vertex.hpp, "
              "where the binding that needs it is visible");

/** @brief One participant's private node lists — touched only by its owner, so never atomic. */
struct alignas(kDomainAlign) lists_t {
    node_t* retired = nullptr;  /**< @brief Displaced nodes awaiting a scan. */
    std::size_t retired_n = 0;  /**< @brief Length of `retired`. */
    std::size_t scan_at = 0;    /**< @brief Scan once `retired_n` reaches this. */
    node_t* freelist = nullptr; /**< @brief Scanned-clean nodes, reused by the next publish. */
    std::size_t freelist_n = 0; /**< @brief Length of `freelist`. */
};

/** @brief "This thread has not claimed an index." */
inline constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

/**
 * @brief The index reserved for threads that could not claim one of their own.
 *
 * ADR-0069 §3 fixed the exhaustion policy as "fall back to the refcounted read path", which
 * §5's design then made unimplementable: with the value reached **through** a node, a reader
 * that holds no pin cannot safely touch `node_t::sp` at all, so there is no unpinned read to
 * fall back to. What replaces it keeps the same guarantee — correctness never depends on the
 * bound being right — by a different mechanism: overflow threads share this one extra index
 * under a spin lock, so they serialize with each other and with nobody else.
 *
 * The containment is a property of the **layout**, not just of the algorithm, and it was broken
 * once: an over-capacity thread re-probes the claim table on every operation, and while that
 * table lived inside `%cell_t` each probe took an in-capacity reader's announcement line
 * exclusive (#899). The two are separate storage now, and the probe is a load — but read the
 * claim as scoped to what it says: the claim table and the announcement cells. An overflow
 * thread still shares `overflow_lock`'s line with `orphans`, which in-capacity threads read.
 */
inline constexpr std::size_t kOverflowIndex = kHazardReaderSlots;

/**
 * @brief How many nodes a participant parks before it scans.
 *
 * Derived from the one knob rather than picked (RFC-0006): the scan is O(participants), so
 * batching by the participant count is what amortizes it, and it is also the tightest batch
 * that can hope to free anything — at most `kHazardReaderSlots + 1` nodes can be pinned
 * domain-wide at any instant.
 */
inline constexpr std::size_t kRetireBatch = kHazardReaderSlots;

/** @brief Frees what the domain still owns at exit — never a live participant's (#898). */
struct final_sweep_t {
    ~final_sweep_t();
};

/**
 * @brief The domain's storage: `kHazardReaderSlots` claimable indices plus the overflow one.
 *
 * `constinit` and trivially destructible on purpose. It lands in `.bss` with no guard
 * variable and is never destroyed, so a `thread_local` participant unwinding at process
 * exit can always reach it — the ordering hazard that a `std::vector` or a `std::mutex`
 * in here would create. Cost: `(kHazardReaderSlots + 1) * 128` bytes plus one padded claim
 * table, and **zero** for a target that does not bind this slot, since nothing then
 * references `registry()`.
 */
struct registry_t {
    std::array<cell_t, kHazardReaderSlots + 1> cells{};  /**< @brief Announcements. */
    std::array<lists_t, kHazardReaderSlots + 1> lists{}; /**< @brief Parked + recycled nodes. */
    claims_t claims{};                     /**< @brief Which indices are owned (#899). */
    std::atomic<node_t*> orphans{nullptr}; /**< @brief Left by exited threads; adopted by scans. */
    std::atomic_flag overflow_lock{};      /**< @brief Serializes users of `kOverflowIndex`. */
};

/**
 * @brief Take index @p i, if nobody else holds it.
 * @return `true` iff **this call** took it; `false` means a live participant (or the exit
 *         sweep) owns it and this call changed nothing — the bit was already set.
 *
 * The one operation every owner of an index goes through, participants and the exit sweep
 * alike, which is what makes "held" an interlock rather than a sample (see `~final_sweep_t`).
 * `acq_rel` pairs with `%release_claim` so a claimant sees the previous owner's cleanup.
 */
[[nodiscard]] inline bool try_claim(registry_t& r, std::size_t i) {
    const claim_word_t bit = claim_word_t{1} << (i % kClaimBitsPerWord);
    return (r.claims.words[i / kClaimBitsPerWord].fetch_or(bit, std::memory_order_acq_rel) & bit) ==
           0;
}

/** @brief Give index @p i back, publishing everything its owner did with it. */
inline void release_claim(registry_t& r, std::size_t i) {
    const claim_word_t bit = claim_word_t{1} << (i % kClaimBitsPerWord);
    r.claims.words[i / kClaimBitsPerWord].fetch_and(static_cast<claim_word_t>(~bit),
                                                    std::memory_order_acq_rel);
}

/** @brief The one domain. Emitted only in a build that actually binds @ref hazard_slot_t. */
[[nodiscard]] inline registry_t& registry() {
    static constinit registry_t reg{};
    static final_sweep_t sweep;  // destroyed at exit, while `reg`'s storage is still valid
    (void)sweep;
    return reg;
}

/** @brief This thread's claim on a domain index, released when the thread ends. */
class participant_t {
   public:
    /** @brief Bind to the domain up front; see `self()` for why the timing matters. */
    explicit participant_t(registry_t& reg) : reg_(reg) {}
    participant_t(const participant_t&) = delete;
    participant_t& operator=(const participant_t&) = delete;
    ~participant_t();

    /** @brief The claimed index, or `kNoIndex` before `claim()` succeeds. */
    [[nodiscard]] std::size_t index() const { return idx_; }

    /**
     * @brief Claim an index once per thread.
     * @return The claimed index, or `kNoIndex` when every index is taken.
     *
     * A thread that fails calls this again on its **next** operation, so the failing path is
     * the one that has to be cheap: see the implementation for the prefilter that makes a
     * probe of a full table a shared read rather than a sweep of read-modify-writes (#899).
     */
    std::size_t claim();

    /**
     * @brief How many read-modify-writes this thread has issued against the claim table.
     *
     * The pathology #899 named is a **count** — an over-capacity thread re-probing every index
     * on every operation — and nothing else about the domain distinguishes one probe from
     * sixty-four, so this is what a guard can assert on. Plain and `thread_local`-resident,
     * never atomic: the RFC-0022 §6 instrument that shared one cache line cost more than the
     * effect it was measuring (`%pin_instrument.hpp`). It is not `#ifdef`-gated because these
     * are header-inline functions — a macro that only one translation unit defined would make
     * the counted and uncounted `claim()` two definitions of one function, and the guard would
     * silently measure whichever the linker kept. It costs one non-atomic increment on a path
     * that runs once per thread when it succeeds.
     */
    [[nodiscard]] std::size_t claim_probes() const { return probes_; }

   private:
    registry_t& reg_;
    std::size_t idx_ = kNoIndex;
    std::size_t probes_ = 0;
};

/**
 * @brief This thread's participant. Function-local so an unused slot policy emits no TLS.
 *
 * The `registry()` call in the initializer is **load-bearing, not decoration**: it forces the
 * domain (and the exit sweep registered with it) to be constructed BEFORE this thread_local,
 * so the sweep is destroyed after it. Constructed the other way round — which is what happens
 * if the participant reaches the domain lazily — the main thread's participant unwinds after
 * the sweep has already run and orphans its parked nodes into a registry nobody will read
 * again: a leak at exit, and one a leak checker only sees when a program happens to end with
 * something parked.
 */
[[nodiscard]] inline participant_t& self() {
    static thread_local participant_t p{registry()};
    return p;
}

void scan(registry_t& r, lists_t& l);

/**
 * @brief Resolves the calling thread to a domain index for the length of one operation.
 *
 * The common case is a `thread_local` read and a branch. Only a thread that found every
 * index taken pays anything more: it borrows `kOverflowIndex` under the domain spin lock
 * and gives it back here. Never nest two of these on one thread — the lock is not recursive.
 */
class ticket_t {
   public:
    ticket_t() : idx_(self().index()) {
        if (idx_ != kNoIndex) return;
        idx_ = self().claim();
        if (idx_ != kNoIndex) return;
        registry_t& r = registry();
        while (r.overflow_lock.test_and_set(std::memory_order_acquire)) {
            r.overflow_lock.wait(true, std::memory_order_relaxed);
        }
        overflow_ = true;
        idx_ = kOverflowIndex;
    }

    ticket_t(const ticket_t&) = delete;
    ticket_t& operator=(const ticket_t&) = delete;

    ~ticket_t();

    /** @brief The index this operation may use. */
    [[nodiscard]] std::size_t index() const { return idx_; }

   private:
    std::size_t idx_;
    bool overflow_ = false;
};

/** @brief Park a scanned-clean node for reuse, or free it once the free list is at its bound. */
inline void recycle(lists_t& l, node_t* n) {
    n->sp.reset();  // drop the rope reference NOW, not whenever the node is next published
    if (l.freelist_n >= kRetireBatch) {
        delete n;
        return;
    }
    n->next = l.freelist;
    l.freelist = n;
    ++l.freelist_n;
}

/**
 * @brief Free every parked node no participant is announcing.
 *
 * The `seq_cst` fence is the load-bearing half of the hazard protocol: it orders this
 * thread's publish (the `exchange` that displaced the node) before its reads of the
 * announcement cells, against the reader's `seq_cst` announce followed by its re-read of
 * the slot. One of the two must see the other, which is what makes "not announced" mean
 * "cannot become announced".
 */
inline void scan(registry_t& r, lists_t& l) {
    if (r.orphans.load(std::memory_order_relaxed) != nullptr) {
        node_t* o = r.orphans.exchange(nullptr, std::memory_order_acq_rel);
        while (o != nullptr) {
            node_t* next = o->next;
            o->next = l.retired;
            l.retired = o;
            ++l.retired_n;
            o = next;
        }
    }

    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::array<node_t*, kHazardReaderSlots + 1> pinned{};
    std::size_t np = 0;
    for (const cell_t& c : r.cells) {
        node_t* p = c.pinned.load(std::memory_order_seq_cst);
        if (p != nullptr) pinned[np++] = p;
    }

    node_t* keep = nullptr;
    std::size_t keep_n = 0;
    for (node_t* cur = l.retired; cur != nullptr;) {
        node_t* next = cur->next;
        bool held = false;
        for (std::size_t i = 0; i < np && !held; ++i) held = pinned[i] == cur;
        if (held) {
            cur->next = keep;
            keep = cur;
            ++keep_n;
        } else {
            recycle(l, cur);
        }
        cur = next;
    }
    l.retired = keep;
    l.retired_n = keep_n;
    l.scan_at = keep_n + kRetireBatch;
}

/**
 * @brief Give `kOverflowIndex` back, draining it first.
 *
 * The shared index is the one list with no owning thread: the threads that use it never
 * claimed anything, so no `participant_t` destructor ever comes back for what they parked.
 * Draining on release is what keeps that list from being a slow leak — and the overflow path
 * is serialized and degenerate anyway, so an O(participants) scan on top of it costs nothing
 * a correctly-sized `kHazardReaderSlots` would ever pay.
 */
inline ticket_t::~ticket_t() {
    if (!overflow_) return;
    registry_t& r = registry();
    if (r.lists[kOverflowIndex].retired != nullptr) scan(r, r.lists[kOverflowIndex]);
    r.overflow_lock.clear(std::memory_order_release);
    r.overflow_lock.notify_one();
}

/** @brief Park a displaced node on this participant's list, scanning once the batch fills. */
inline void retire(lists_t& l, node_t* n) {
    n->next = l.retired;
    l.retired = n;
    ++l.retired_n;
    if (l.retired_n >= l.scan_at) scan(registry(), l);
}

/**
 * @brief Park a node and scan at once, instead of waiting for the batch to fill.
 *
 * Deferred reclamation defers **destruction**, and libtracer has a contract that cares:
 * ADR-0039's injected `std::pmr::memory_resource` must outlive every value allocated from
 * it, and a value parked on a retire list is still allocated. Flushing when a slot dies is
 * what keeps "the graph released everything before its resource went away" true — without
 * it `graph_pmr_test` both fails its balance check and faults at exit, freeing a rope
 * through a resource that is already gone.
 *
 * The flush covers what this thread parked. A value written from one thread and whose slot
 * is destroyed on another stays parked on the writer's list until that thread's next batch
 * scan, its exit, or process exit — so an injected resource must outlive the threads that
 * wrote through it, not merely the graph. For the process-lifetime heap a host build
 * actually uses, that is vacuous; for a scoped arena it is a real constraint, and one more
 * reason `sp_atomic_slot_t` stays the default.
 */
inline void retire_and_flush(node_t* n) {
    registry_t& r = registry();
    const std::size_t mine = self().index();
    const bool mine_empty = mine == kNoIndex || r.lists[mine].retired == nullptr;
    const bool orphans_empty = r.orphans.load(std::memory_order_relaxed) == nullptr;
    // A slot that held nothing, on a thread that parked nothing, with nothing left behind by a
    // thread that exited, has nothing to release — and a tree of never-written vertices should
    // not pay a domain scan each to learn that. Orphans are normally absent, so the extra
    // relaxed load costs nothing and buys the case that matters: a value written by a thread
    // that has since exited is still released when its slot dies, not at process exit.
    if (n == nullptr && mine_empty && orphans_empty) return;
    ticket_t t;
    lists_t& l = r.lists[t.index()];
    if (n != nullptr) retire(l, n);
    if (l.retired != nullptr || r.orphans.load(std::memory_order_relaxed) != nullptr) scan(r, l);
}

/**
 * @brief A node for the next publish — recycled if this participant has one, else allocated.
 * @return A node with an empty `sp`, or `nullptr` when the allocation failed.
 *
 * Every publish displaces exactly one node, so after the first the free list keeps up and a
 * publish allocates nothing at all. That is what keeps ADR-0069 §5's "one allocation per
 * publish" off the steady-state write path; only a participant's first publish can allocate.
 */
[[nodiscard]] inline node_t* acquire_node(lists_t& l) {
    if (node_t* n = l.freelist) {
        l.freelist = n->next;
        --l.freelist_n;
        n->next = nullptr;
        return n;
    }
    return new (std::nothrow) node_t;
}

/**
 * @brief Take the first index the claim table shows free.
 *
 * **Prefiltered, because the failing case repeats.** A thread that finds the table full runs
 * this again on every operation for the rest of its life (`ticket_t`), so a probe per index
 * would be `kHazardReaderSlots` read-modify-writes per read or publish — each one taking a line
 * exclusive, which is what #899 measured as an over-capacity thread degrading readers that were
 * inside the budget. One relaxed load per word answers "are any of these 64 free?" as a shared
 * read that invalidates nothing, so a full table costs `kClaimWords` loads and no RMW at all.
 *
 * The load may be stale — a slot freed a moment ago can read as taken — and that is deliberate:
 * a stale read only defers the claim to this thread's next operation, whereas a permanent
 * "claiming failed" flag would strand a thread on the overflow index for the rest of its life
 * even once the table emptied.
 */
inline std::size_t participant_t::claim() {
    registry_t& r = reg_;
    for (std::size_t w = 0; w < kClaimWords; ++w) {
        claim_word_t taken = r.claims.words[w].load(std::memory_order_relaxed);
        const std::size_t base = w * kClaimBitsPerWord;
        const std::size_t n = std::min(kHazardReaderSlots - base, kClaimBitsPerWord);
        for (std::size_t b = 0; b < n; ++b) {
            if (((taken >> b) & 1) != 0) continue;  // owned as far as this thread can tell
            ++probes_;
            if (try_claim(r, base + b)) {
                const std::size_t i = base + b;
                r.lists[i].scan_at = kRetireBatch;
                idx_ = i;
                return i;
            }
            taken = r.claims.words[w].load(std::memory_order_relaxed);  // lost a race; re-read
        }
    }
    return kNoIndex;
}

inline participant_t::~participant_t() {
    if (idx_ == kNoIndex) return;
    registry_t& r = reg_;
    lists_t& l = r.lists[idx_];
    r.cells[idx_].pinned.store(nullptr, std::memory_order_release);

    // Free-list nodes are provably unreachable — a scan cleared them and nothing republished
    // them — so they can go now. Retired ones may still be announced, so the domain adopts
    // them and the next scan on any thread (or the final sweep) finishes the job.
    while (node_t* n = l.freelist) {
        l.freelist = n->next;
        delete n;
    }
    l.freelist_n = 0;
    if (node_t* head = l.retired) {
        node_t* tail = head;
        while (tail->next != nullptr) tail = tail->next;
        node_t* old = r.orphans.load(std::memory_order_relaxed);
        do {
            tail->next = old;
        } while (!r.orphans.compare_exchange_weak(old, head, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed));
        l.retired = nullptr;
        l.retired_n = 0;
    }
    l.scan_at = 0;

    release_claim(r, idx_);
    idx_ = kNoIndex;
}

/**
 * @brief Free what no live participant owns, and leave everything else exactly where it is.
 *
 * ## When this runs, and why that is not "after everything"
 *
 * The sweep is a function-local static of `registry()`, so the only ordering it gets for
 * free is against objects constructed *after* it: the main thread's `thread_local`
 * participant, which `self()` deliberately constructs later (see its comment) and which is
 * therefore destroyed **before** this. Nothing orders it against a static constructed
 * *earlier* — such an object is destroyed after the sweep and may legitimately join a worker
 * that was running during it — nor against any thread that has simply not exited yet. So
 * "every participant is gone by now" is false, and the previous unconditional
 * `delete`-everything-and-assign-`lists_t{}` was a data race and a use-after-free against a
 * live thread's `store()` / `load()` (#898).
 *
 * ## Interlock, not a sample
 *
 * The claim table and `overflow_lock` are already the two mechanisms that mean "a live
 * participant owns this index", so the sweep takes an index through the **same operation** a
 * participant uses — the `fetch_or` behind `%try_claim`, a `test_and_set` on the flag. Either
 * a thread holds the index and the sweep never touches its lists, or the sweep holds it and no
 * thread can claim it while they are being freed. Merely *reading* the table would narrow the
 * window rather than close it. Nothing here blocks: an index the sweep cannot get is skipped,
 * never waited for, so a worker still running cannot wedge process exit.
 *
 * A skipped index leaks whatever it is holding, and that is the correct answer — leak-checker
 * noise attributable to a thread that outlived the domain is a true report, whereas freeing a
 * list under its owner is a fault. Normal teardown is unaffected: every participant that has
 * run its destructor has already released its index, so a program whose threads have all been
 * joined still reclaims in full.
 */
inline final_sweep_t::~final_sweep_t() {
    registry_t& r = registry();

    std::array<bool, kHazardReaderSlots + 1> mine{};
    for (std::size_t i = 0; i < kHazardReaderSlots; ++i) mine[i] = try_claim(r, i);
    mine[kOverflowIndex] = !r.overflow_lock.test_and_set(std::memory_order_acquire);

    // Adopt the orphans, then read the announcements behind `scan`'s `seq_cst` fence and for
    // `scan`'s reason: a live reader can be mid-`load()`, announcing a node that has just been
    // displaced, and the fence is what makes "not announced here" mean "cannot become
    // announced". An announced node is deliberately left allocated — memory a live thread is
    // reading is memory this must not free, and at process exit leaking it costs nothing.
    node_t* orphans = r.orphans.exchange(nullptr, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::array<node_t*, kHazardReaderSlots + 1> pinned{};
    std::size_t np = 0;
    for (const cell_t& c : r.cells) {
        node_t* p = c.pinned.load(std::memory_order_seq_cst);
        if (p != nullptr) pinned[np++] = p;
    }

    // A kept node is unlinked from every list by the time this returns, so the `next` it is
    // left holding is unreachable — a reader only ever dereferences `sp`.
    auto drop = [&pinned, np](node_t* n) {
        while (n != nullptr) {
            node_t* next = n->next;
            bool held = false;
            for (std::size_t i = 0; i < np && !held; ++i) held = pinned[i] == n;
            if (!held) delete n;
            n = next;
        }
    };

    drop(orphans);
    for (std::size_t i = 0; i < r.lists.size(); ++i) {
        if (!mine[i]) continue;
        drop(r.lists[i].retired);
        drop(r.lists[i].freelist);
        r.lists[i] = lists_t{};
    }

    // Hand the indices back. The registry is `constinit` and never destroyed, so a thread born
    // after the sweep can still reach it and must still find a claimable, clean index.
    for (std::size_t i = 0; i < kHazardReaderSlots; ++i) {
        if (mine[i]) release_claim(r, i);
    }
    if (mine[kOverflowIndex]) {
        r.overflow_lock.clear(std::memory_order_release);
        r.overflow_lock.notify_one();
    }
}

}  // namespace detail_hp

/**
 * @brief The host slot (ADR-0069 §1): a lock-free `atomic<node_t*>` reclaimed with hazard
 *        pointers, returning the same owning `shared_ptr` @ref sp_atomic_slot_t does.
 *
 * Why this exists: today's slot INVERTS under concurrent readers — measured through the real
 * path, `graph_t::read` on one shared LKV falls from 21.1 M/s at one reader to 1.7 M/s at
 * twenty-four, because both `load` and `store` take libstdc++'s `_Sp_locker` pointer-lock
 * bit. Hazard reclamation deletes that lock; what it cannot delete is the control-block
 * increment an owning read still owes. End to end that is worth **4.2×** at twenty-four
 * readers (7.4 M/s) — see the table in this file's header, and ADR-0069 §6 for why the
 * model bench's 20.8× did not survive contact with the whole read path.
 *
 * Why the default is still `sp_atomic_slot_t`: the gain is entirely a concurrency gain —
 * at one thread the two slots are indistinguishable within run-to-run spread, so a
 * single-core node buys nothing and still pays `(kHazardReaderSlots + 1) * 128` bytes of
 * registry, a deferred-reclamation lifetime rule (see `retire_and_flush`), and
 * a publish that can fail. Bind this one from a host preset:
 * `-DLIBTRACER_LKV_SLOT=hazard_slot_t`.
 *
 * **Publish can fail under memory exhaustion**, which @ref sp_atomic_slot_t cannot: an empty
 * free list makes the first publish per participant allocate a 24-byte node. It is *reported*,
 * not silent — `store` returns `false` and `vertex_t::store` turns that into the same
 * `nullptr` → `BACKPRESSURE` soft-fail an LKV allocation failure already produces (#477), so
 * no write is ever reported as taken when it was not. Every later publish reuses the node its
 * own displacement recycled, so the window is a warm-up one — but it is still a real
 * difference in the policy's failure surface, and a third reason the MCU does not bind this
 * slot. Note also that the node comes from the **global heap**, not from a graph's injected
 * `std::pmr::memory_resource`: the slot policy is never handed one, and a bounded target that
 * needs every byte accounted for is another target that should keep the default.
 */
class hazard_slot_t {
   public:
    /** @brief The handle a publish takes and a read returns — owning, as the contract requires. */
    using value_ptr_t = std::shared_ptr<const view::rope_t>;

    hazard_slot_t() = default;
    hazard_slot_t(const hazard_slot_t&) = delete;
    hazard_slot_t& operator=(const hazard_slot_t&) = delete;

    /**
     * @brief Retire the published node rather than free it — a reader may still be pinning it —
     *        and flush, so the value cannot outlive the memory it was allocated from.
     *
     * A slot that was never written costs nothing here: no node, no flush, no domain access.
     */
    ~hazard_slot_t() {
        detail_hp::retire_and_flush(slot_.exchange(nullptr, std::memory_order_acq_rel));
    }

    /**
     * @brief Publish, sequentially consistent unless the caller says otherwise.
     * @return `false` if no node could be obtained for the value, in which case **nothing was
     *         published** and the previous value still stands. Only a participant's first
     *         publish can reach that: every later one reuses the node its own displacement
     *         recycled, so the free list makes the steady state allocation-free.
     *
     * An empty handle is not a publish — use @ref clear.
     */
    [[nodiscard]] bool store(value_ptr_t sp, std::memory_order order = std::memory_order_seq_cst) {
        if (!sp) {
            clear(order);
            return true;
        }
        detail_hp::ticket_t t;
        detail_hp::lists_t& l = detail_hp::registry().lists[t.index()];
        detail_hp::node_t* fresh = detail_hp::acquire_node(l);
        if (fresh == nullptr) return false;  // nothing published; the caller soft-fails (#477)
        fresh->sp = std::move(sp);
        detail_hp::node_t* old = slot_.exchange(fresh, rmw_order(order));
        if (old != nullptr) detail_hp::retire(l, old);
        return true;
    }

    /**
     * @brief Drop the published value. Cannot fail — it publishes `nullptr`, which needs no
     *        node, so a clear allocates nothing even on a cold participant.
     */
    void clear(std::memory_order order = std::memory_order_seq_cst) {
        detail_hp::node_t* old = slot_.exchange(nullptr, rmw_order(order));
        if (old == nullptr) return;
        detail_hp::ticket_t t;
        detail_hp::retire(detail_hp::registry().lists[t.index()], old);
    }

    /**
     * @brief Read the published value.
     *
     * Announce, re-read, then copy the `shared_ptr` out of the pinned node — the copy is the
     * promotion that lets the handle outlive the pin, and it is the one shared-cache-line RMW
     * this scheme cannot remove. A slot nobody has written costs a single acquire load and
     * never touches the domain at all.
     *
     * The announce and the **re-read** are both `seq_cst` so that both sit in one total order
     * with the publisher's `exchange` and the reclaimer's fence: if a scan did not observe this
     * announcement, then in that order the scan's read precedes it, the displacement precedes
     * the scan, and so this re-read must observe the displacement and retry. `acquire` on the
     * re-read is the usual spelling and is believed sound, but it leaves the argument resting
     * on coherence rather than on the total order — and it costs nothing to close, since a
     * `seq_cst` load is a plain `mov` on x86-64.
     *
     * Reusing a node is deliberately allowed to ABA: a reader can pin `n`, have it reclaimed
     * and republished, and revalidate against the same address. That is not a bug — `n` is live
     * and holds a value some writer published, which is all a read promises.
     */
    [[nodiscard]] value_ptr_t load() const {
        detail_hp::node_t* n = slot_.load(std::memory_order_acquire);
        if (n == nullptr) return {};
        detail_hp::ticket_t t;
        std::atomic<detail_hp::node_t*>& cell = detail_hp::registry().cells[t.index()].pinned;
        for (;;) {
            cell.store(n, std::memory_order_seq_cst);
            detail_hp::node_t* again = slot_.load(std::memory_order_seq_cst);
            if (again == n) break;
            n = again;
            if (n == nullptr) {
                cell.store(nullptr, std::memory_order_release);
                return {};
            }
        }
        value_ptr_t out = n->sp;
        cell.store(nullptr, std::memory_order_release);
        return out;
    }

   private:
    /**
     * @brief The read-modify-write order matching a requested store order.
     *
     * The exchange must acquire as well as release: whoever displaces a node goes on to read
     * and eventually free it, so it needs the publisher's writes to that node.
     */
    [[nodiscard]] static constexpr std::memory_order rmw_order(std::memory_order order) {
        return order == std::memory_order_seq_cst ? std::memory_order_seq_cst
                                                  : std::memory_order_acq_rel;
    }

    std::atomic<detail_hp::node_t*> slot_{nullptr};
};

}  // namespace tr::graph
