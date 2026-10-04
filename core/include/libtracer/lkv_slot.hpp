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
 * A slot type must provide, over the intrusive `value_t` of `%value.hpp` (RFC 0028 §5.1):
 *
 *   - `[[nodiscard]] bool store(value_t*)` — publish, sequentially consistent. The slot ADOPTS
 *     the one reference the caller passes and releases the displaced value's. The order
 *     matters: `vertex_t::store` relies on this sharing one total order with the `write_seq_`
 *     bump and the waiter count, which is what makes the waiterless publish (#555) unable to
 *     lose a wakeup. **`false` means nothing was published** and the previous value still
 *     stands — the reference stays the caller's, who must soft-fail (#477), never report the
 *     write as taken.
 *   - `void clear(std::memory_order)` — drop the published value. Cannot fail, and says so in
 *     the return type: a clear releases resources rather than acquiring any. Only
 *     `revert_to_placeholder` calls it, with `release`.
 *   - `[[nodiscard]] value_ref_t load() const` — read the published value as an OWNING handle
 *     (one `retain` the reader's `value_ref_t` releases).
 *   - `static constexpr bool may_spin` — whether any operation can SPIN-WAIT on another
 *     thread's progress. Declaring it is mandatory: `%vertex.hpp` refuses a policy that does not,
 *     and refuses one that says `true` on a target whose `config_t::kSpinWaitSafe` is `false`
 *     (#1618). The assertion is on the policy, so a future policy cannot forget it.
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
 * ## The two policies
 *
 *   - @ref tr::graph::single_writer_slot_t — the default, and the only one an RTOS target can
 *     bind: its one wait is `config_t::guard_t`, an interrupt-masked critical section
 *     there and an address-striped one-word lock on a host, whose contender sleeps rather than
 *     spins on a descheduled holder (#1618).
 *   - @ref tr::graph::hazard_slot_t — the lock-free opt-in for a host whose reads of one shared
 *     vertex contend across many cores.
 *
 * The refcount slot both replace, `std::atomic<std::shared_ptr<const rope_t>>`
 * (`sp_atomic_slot_t`), was deleted because it was the only policy that could spin: libstdc++
 * implements it with a pointer-lock bit that `load` and `store` spin on (RFC 0028 §4.5).
 *
 * ## What the hazard policy bought
 *
 * Measured end to end, `graph_t::read` on one shared LKV, the refcount slot against the hazard
 * slot, both built from the tree of the time (medians of six alternating runs, 24-core host —
 * ADR-0069 §6):
 *
 * | readers | refcount slot | `hazard_slot_t` | gain |
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
 * On one shared vertex the limit that remains is the value's refcount increment, the
 * promotion an owning read cannot skip (ADR-0069 §6, second erratum), which makes the next lever
 * for THAT shape an API question rather than a reclamation one.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <type_traits>

#include "libtracer/config.hpp"
#include "libtracer/guard.hpp"
#include "libtracer/guard_mutex.hpp"
#include "libtracer/reader_guard.hpp"  // the deprecated tr::graph guard aliases (#1703), one release
#include "libtracer/value.hpp"

namespace tr::graph {

/**
 * @brief The LKV slot contract as a concept (RFC-0028 §5.6): a nothrow adopting `store`, a
 *        nothrow owning `load`, and the mandatory `may_spin` declaration.
 *
 * `%vertex.hpp` asserts it on the bound `lkv_slot_t`, and separately refuses a policy that
 * says `may_spin` on a target whose `kSpinWaitSafe` is `false` (#1618).
 */
template <class S>
concept lkv_slot = requires(S s, value_t* v) {
    { s.store(v) } noexcept -> std::same_as<bool>;
    { s.load() } noexcept -> std::same_as<value_ref_t>;
    { S::may_spin } -> std::convertible_to<bool>;
};

/**
 * @brief Whether the slot policy @p S takes its sections on the guard @p G and offers the
 *        fused publish `store(v, in_section)` (#1715).
 *
 * The vertex fuses the write sequence's bump into the slot's section only when this holds and
 * the sequence's counter is the guarded binding; otherwise it bumps separately, as before.
 */
template <class S, class G>
concept publishes_under =
    requires { typename S::guard_type; } && std::same_as<typename S::guard_type, G>;

/**
 * @brief The slot for a single-writer build (RFC 0028 §5.5): one `value_t*`, exchanged and
 *        retained inside the guard @p G, which never spins.
 *
 * Why it exists (#1618). The refcount slot this replaced, `std::atomic<std::shared_ptr>`, is
 * spin-locked in libstdc++: `load` and `store` take a pointer-lock bit and a contender spins on
 * it with `sched_yield`. On a priority-preemptive single-core scheduler, `sched_yield` yields
 * only to equal or higher priority, so a high-priority reader that preempts a low-priority
 * writer inside that window spins until the task watchdog fires. Here the window is a guard
 * that cannot be spun on: an interrupt-masked critical section cannot be preempted at all, and
 * the host's @ref mutex_guard_t puts a contender to sleep once it has spun out.
 *
 * **What the guard covers, and what it does not.** `store` swaps the pointer inside the guard
 * and releases the displaced value AFTER leaving it, so a value's link destructors and its
 * block source never run with interrupts masked. `load` retains inside the guard, so the
 * refcount increment cannot interleave with the writer's release. Both sections are a handful
 * of instructions and call nothing that can block.
 *
 * **Writers are serialized too.** Nothing here relies on a single publisher: two writers
 * serialize on the guard like a writer and a reader do, so a build that publishes one vertex
 * from two threads is still memory-safe. The name comes from RFC 0028 §5.5, where the
 * single-writer build is the one that must bind it.
 *
 * **Why one publisher would not let the writer skip the guard, even now the slot is one
 * word.** It is tempting: one publisher, so nothing to exclude on the write side, publish with
 * a single atomic `exchange` and let readers `acquire`. The slot IS one word since RFC 0028
 * slice 3 (an intrusive `value_t*`), so the torn two-word swap the `shared_ptr` slot had is
 * gone — but the race that matters never needed two words. A reader inside its guard has loaded
 * the pointer and not yet retained it; a writer that exchanges outside the guard goes on to
 * `release` the displaced value, and if that was the last reference the block is freed under
 * the reader's `retain`. The reader's guard excludes the writer only if the writer's exchange
 * is inside a guard too: exclusion is pairwise, and the single-writer contract says nothing
 * about readers. So the writer keeps the guard on every target, which is why the per-build
 * `kSingleWriter` promise was removed (#1718): it had nothing left to unlock. (RFC 0028 §5.5's
 * sentence that a reader's `retain` inside its guard "cannot interleave with the writer's release"
 * once the slot is one word is the claim this paragraph corrects.) `lkv_slot_test`'s one-writer /
 * N-reader run is the test that bites when this is tried: with the writer's guard removed, a reader
 * reads a value after its free (ASan: heap-use-after-free).
 *
 * @tparam G A `tr::guard`; the slot takes `G::for_address(this)`. The bound slot uses
 *           `config_t::guard_t`; tests instantiate this template directly with a guard of
 *           their own.
 */
template <::tr::guard G>
class basic_single_writer_slot_t {
   public:
    /** @brief This policy's only wait is its guard, so it spins exactly when the guard does. */
    static constexpr bool may_spin = G::may_spin;
    /** @brief The guard the slot's sections take — what `publishes_under` reads (#1715). */
    using guard_type = G;

    basic_single_writer_slot_t() = default;
    basic_single_writer_slot_t(const basic_single_writer_slot_t&) = delete;
    basic_single_writer_slot_t& operator=(const basic_single_writer_slot_t&) = delete;

    /**
     * @brief Publish. The swap happens inside the guard; the displaced value is released
     *        after it, outside.
     *
     * No fence follows the guard. What `vertex_t::store` needs from the slot is that the value
     * is visible to anyone who observes the next `write_seq_` bump, and the bump is a `seq_cst`
     * read-modify-write sequenced after the guard's release, so it already carries the swap.
     * The waiterless-publish argument (#555) is about `write_seq_` and the waiter count only.
     *
     * @param v The value to publish; the slot adopts the caller's reference. Null clears.
     * @return Always `true`. A swap allocates nothing, so there is no failure to report.
     */
    [[nodiscard]] bool store(value_t* v, std::memory_order = std::memory_order_seq_cst) noexcept {
        {
            const ::tr::guard_scope_t<G> g{this};
            std::swap(v_, v);
        }
        value_t::release(v);  // `v` is now the displaced value, released here, unguarded
        return true;
    }

    /**
     * @brief Publish, and run @p in_section inside the SAME section as the swap (#1715).
     *
     * The fused publish of a core with no atomic read-modify-write: there the write sequence's
     * bump is itself a section of `G` (`tr::rmw_counter_t`'s guarded binding), so publishing
     * through @ref store and then bumping opened two sections back to back. Passing the bump
     * in here (`rmw_counter_t::bump_in_section`) opens one. The caller must name this slot's
     * address as the anchor of every OTHER bump of that counter, so that every bump still
     * serializes on one guard.
     *
     * **Publication, restated for the fused form.** @ref store's note relies on the bump being
     * a `seq_cst` RMW sequenced after the guard's release. Here the bump is a plain load and a
     * `seq_cst` store, made inside the guard, after the swap. A reader that observes the new
     * sequence value and then reads the slot still sees the swap: its `load()` takes the same
     * guard, and its section cannot come first, because the reader's read of the sequence is
     * sequenced before its section opens, while the store it read is sequenced after this
     * section opened. A load cannot read a store that happens after it. So the reader's section
     * follows this one, and the guard's release/acquire carries the swap to it. The writer's
     * Dekker pair is unchanged: the bump is still a `seq_cst` store, sequenced before the
     * `waiters` load the vertex makes after this returns.
     *
     * @param v          The value to publish; the slot adopts the caller's reference.
     * @param in_section Called once, inside the guard, after the swap. Must not throw, block
     *                   or allocate: it runs with interrupts masked on an RTOS chip.
     * @return Always `true`, as @ref store.
     */
    template <class F>
    [[nodiscard]] bool store(value_t* v, F&& in_section) noexcept {
        static_assert(std::is_nothrow_invocable_v<F&>,
                      "the fused section's callable runs inside the guard and must be noexcept");
        {
            const ::tr::guard_scope_t<G> g{this};
            std::swap(v_, v);
            in_section();
        }
        value_t::release(v);  // the displaced value, released after the section as in store()
        return true;
    }

    /** @brief Drop the published value. Releases a reference outside the guard; cannot fail. */
    void clear(std::memory_order = std::memory_order_seq_cst) noexcept {
        value_t* old = nullptr;
        {
            const ::tr::guard_scope_t<G> g{this};
            std::swap(v_, old);
        }
        value_t::release(old);
    }

    /**
     * @brief Read the published value: one guarded `retain`.
     *
     * The retain is the refcount increment that lets the handle outlive the guard, and it is the
     * only work inside it.
     */
    [[nodiscard]] value_ref_t load() const noexcept {
        const ::tr::guard_scope_t<G> g{this};
        return value_ref_t::share(v_);
    }

    /** @brief Release whatever is still published. No reader can be inside the guard by now. */
    ~basic_single_writer_slot_t() { value_t::release(v_); }

   private:
    value_t* v_ = nullptr; /**< @brief The published value; the slot owns one reference. */
};

/**
 * @brief @ref basic_single_writer_slot_t over this build's `config_t::guard_t` — the
 *        name an override fragment binds.
 *
 * A class rather than an alias so `%config.hpp` can forward-declare it: the fragment names the
 * slot before this header has been seen, and the guard it will use is a member of the very
 * traits type the fragment is defining.
 */
class single_writer_slot_t : public basic_single_writer_slot_t<config_t::guard_t> {};

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
 * @brief The indirection node the slot publishes — it owns ONE reference on the value
 *        (ADR-0069 §5, over RFC 0028's intrusive `value_t`).
 *
 * The slot holds `atomic<node_t*>` — lock-free — rather than the `value_t*` itself, so that
 * the retire list and the hazard protocol stay typed to a node the domain owns end to end: a
 * `clear` publishes `nullptr` and retires the node it displaces without allocating, and the
 * value's own reference is dropped exactly once, when a scan proves the node unpinned
 * (`recycle`). A read pins the node, re-validates, and `retain`s **out of the pinned node**,
 * which is what turns a pin into the owning `value_ref_t` `read_stored()` must return; that
 * retain cannot race the release because the release waits on the scan that saw no pin.
 *
 * `v` is immutable while the node is published; `next` is touched only once the node is off
 * the slot, so the two never race.
 */
struct node_t {
    const value_t* v = nullptr; /**< @brief The published value; this node holds one reference. */
    node_t* next = nullptr;     /**< @brief Retire / free-list link. */
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
 * The containment is a property of the **layout**, not just of the algorithm, and the layout
 * broke it twice. The claim table lived inside `%cell_t`, so an over-capacity thread's re-probe
 * — one per operation — took an in-capacity reader's announcement line exclusive (#899); and the
 * spin lock below shared its line with `registry_t::orphans`, which in-capacity threads load in
 * `%scan` and in `%retire_and_flush` (#1027). Each now has padded storage of its own — `%claims_t`,
 * `%overflow_lock_t` — asserted here by `alignas` and in `lkv_slot_test` by what an over-capacity
 * thread writes: no byte of the announcement table for the first, none of `orphans`' line for the
 * second.
 *
 * What is left is TRUE sharing, and inherent: a scan must read every announcement cell, this
 * index's included, because a pin announced there is exactly what stops a node being freed.
 * No layout hides that, and none should.
 */
inline constexpr std::size_t kOverflowIndex = kHazardReaderSlots;

/**
 * @brief The spin lock serializing users of `%kOverflowIndex` — on a line of its own (#1027).
 *
 * Padded for the reason the claim table is, against a different reader. A thread that could not
 * claim an index takes and drops this flag **once per operation** — a `test_and_set` at one end
 * of its `%ticket_t` and a `clear` at the other, so two unconditional read-modify-writes per
 * load, store and clear. Unpadded, it landed eight bytes past `registry_t::orphans`, on the same
 * `kDomainAlign` line, and `orphans` is LOADED by threads that are
 * inside the budget — `%scan` opens with one, and `%retire_and_flush` does too, so once per
 * `%hazard_slot_t` destruction. An over-capacity thread therefore took a line exclusive that
 * in-capacity threads read, which is the containment `%kOverflowIndex` promises leaking again.
 *
 * A wrapper type rather than an `alignas` on the member, so the alignment is `static_assert`-able
 * the way `%cell_t`'s and `%claims_t`'s already are. Parking the flag in the overflow index's own
 * `%cell_t` was the other option and is not better: `%scan` reads EVERY announcement cell, this
 * index's included, so that line is not private to the overflow population either.
 *
 * Cost is one padded line in a `constinit` registry that is already `(kHazardReaderSlots + 1)`
 * cells and lists wide, and none at all on a single-core profile, where @ref
 * tr::graph::kCacheLineBytes is 0 and `kDomainAlign` collapses to `alignof(std::size_t)`.
 */
struct alignas(kDomainAlign) overflow_lock_t {
    std::atomic_flag flag{}; /**< @brief Set while some thread is using `%kOverflowIndex`. */
};

static_assert(alignof(overflow_lock_t) == kDomainAlign,
              "the overflow lock's alignas was silently dropped — it must not share the line "
              "in-capacity threads read `orphans` on");

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
 * table and one padded lock, and **zero** for a target that does not bind this slot, since
 * nothing then references `registry()`.
 *
 * The member ORDER is load-bearing at the tail. `%claims_t` ahead of `orphans` is padded to a
 * whole number of lines and `%overflow_lock_t` behind it is line-aligned, so `orphans` — which
 * every in-capacity thread loads on the paths `%overflow_lock_t` names — ends up alone on a
 * line without needing a wrapper of its own.
 */
struct registry_t {
    std::array<cell_t, kHazardReaderSlots + 1> cells{};  /**< @brief Announcements. */
    std::array<lists_t, kHazardReaderSlots + 1> lists{}; /**< @brief Parked + recycled nodes. */
    claims_t claims{};                     /**< @brief Which indices are owned (#899). */
    std::atomic<node_t*> orphans{nullptr}; /**< @brief Left by exited threads; adopted by scans. */
    overflow_lock_t overflow_lock{};       /**< @brief Serializes `%kOverflowIndex` (#1027). */
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
/** @brief Free a node that no reader can reach, releasing the value reference it still holds
 *         (a retired node's; a free-list node's is already null). */
inline void destroy_node(node_t* n) noexcept {
    value_t::release(n->v);
    delete n;
}

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
        while (r.overflow_lock.flag.test_and_set(std::memory_order_acquire)) {
            r.overflow_lock.flag.wait(true, std::memory_order_relaxed);
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
    value_t::release(n->v);  // drop the value's reference NOW, not when the node is next used
    n->v = nullptr;
    if (l.freelist_n >= kRetireBatch) {
        destroy_node(n);
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
    r.overflow_lock.flag.clear(std::memory_order_release);
    r.overflow_lock.flag.notify_one();
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
 * reason a bounded target binds @ref single_writer_slot_t instead.
 *
 * ## What the orphan probe promises, and why it is not "at slot death" (#1037)
 *
 * The `orphans` term of the early return is a **check-then-act**, and deliberately stays one.
 * Nothing re-reads it on the taken branch, so a `~participant_t` pushing an exited thread's
 * list concurrently with the load is missed by *this* call; those nodes wait for the next
 * `%scan` on any thread, or for `%final_sweep_t`. The guarantee is therefore **released when
 * the slot dies OR at the next domain scan**, never the stronger "at slot death" this comment
 * used to claim.
 *
 * That weakening is a ruling, not an oversight — no ordering available here buys more:
 *
 * - **Re-probing after the ticket** narrows the window instead of closing it. `%scan` adopts by
 *   `exchange`-ing the orphan head, so a push that lands after that exchange is missed by the
 *   adopting scan too; the miss merely moves from "returned early" to "adopted a moment too
 *   soon", and the unconditional `%ticket_t` is paid on exactly the nothing-to-do path this
 *   early return exists to protect.
 * - **An orphan epoch** published before the push has the same shape one level down: a counter
 *   bumped before the push can still be read before it is bumped.
 *
 * The property the strong reading was wanted for — an injected resource never freed through
 * after its death — is obtained from the lifetime contract instead of from this probe:
 * ADR-0039 §Erratum 8 requires the resource to outlive every thread that wrote through it
 * **and** a domain quiescence point. Where that holds, "or at the next scan" is sufficient;
 * where it does not, no probe ordering here would have saved it.
 *
 * ## Who supplies the "next domain scan" (#897, #1376)
 *
 * Under `%tr::graph::reclaim_qsbr_t` that scan stops being the embedder's obligation and becomes
 * ROUTINE: every participant calls this function with a null node at its own quiescent point —
 * each outermost dispatch exit, and @ref tr::graph::graph_t::thread_quiescent for a thread that
 * displaces nodes without ever dispatching. So a writer thread drains ITS OWN list on ITS OWN
 * thread, which is exactly the half of ADR-0080 §"#897 maps onto the same seam" that policy is
 * asked to discharge: `%~hazard_slot_t` never has to reach across a still-live other thread's
 * private `%lists_t`, the thing the code before it tried and failed structurally to do.
 *
 * Note what that does NOT require, because it is the point: **nothing in this file changes, and
 * no `%store()`-path atomic is added.** The early return above is what makes the self-drain free
 * on a thread that parked nothing, so putting it at a quiescent point costs a publish nothing.
 * Under the two per-thread policies the guarantee is unchanged and stays "at slot death or at
 * the next domain scan", with the scan still owed by ADR-0039 §Erratum 8's lifetime rule.
 */
inline void retire_and_flush(node_t* n) {
    registry_t& r = registry();
    const std::size_t mine = self().index();
    const bool mine_empty = mine == kNoIndex || r.lists[mine].retired == nullptr;
    const bool orphans_empty = r.orphans.load(std::memory_order_relaxed) == nullptr;
    // A slot that held nothing, on a thread that parked nothing, with nothing left behind by a
    // thread that exited, has nothing to release — and a tree of never-written vertices should
    // not pay a domain scan each to learn that. Orphans are normally absent, so the extra
    // relaxed load costs nothing and buys the common case: a value written by a thread that has
    // since exited is usually released here, at slot death, rather than at process exit. A push
    // racing this load is missed and waits for the next scan — see the note above; that is the
    // guarantee, and the injected-resource property rests on ADR-0039's lifetime rule instead.
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
 *
 * @note **This is the global heap ON PURPOSE, and it is the one channel #873 does not close.**
 *       Phase 2 of that issue moved this allocation (and the matching `delete`s in `%recycle`,
 *       `%~participant_t` and `%~final_sweep_t`) onto the graph's injected
 *       @ref tr::mem::block_source_t, with a process default of @ref tr::mem::heap_source().
 *       It was implemented, measured against the gate the ruling staged it behind, and
 *       REVERTED: `bench/bench_hazard_node.cpp`, both arms pinned to one logical CPU over 12
 *       interleaved rounds, read **+22.7 %** on the allocating publish and — the disqualifying
 *       half — **+3.5 % on the FREE-LIST arm, which never touches the substrate at all**,
 *       against a two-binary A/A null of +0.45 % / −1.1 % and with disjoint ranges. End to end
 *       at this binding `bench_libtracer fan` lost 3.6-6.9 % of its deliveries/s.
 *       The cost is the indirect `try_alloc` against the direct `operator new` this line calls,
 *       plus the compiler budget that call re-partitions around `%scan`; moving the body out of
 *       line did not recover it. Do not re-migrate this site without re-running that bench under
 *       `docs/methodology.md` §"The A/B protocol" — the rationale, the full table and the shape
 *       a future attempt should start from instead are in
 *       `docs/reference/09-memory-substrate.md` §"The carve-out".
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
        destroy_node(n);
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
    mine[kOverflowIndex] = !r.overflow_lock.flag.test_and_set(std::memory_order_acquire);

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
    // left holding is unreachable — a reader only ever dereferences `v`.
    auto drop = [&pinned, np](node_t* n) {
        while (n != nullptr) {
            node_t* next = n->next;
            bool held = false;
            for (std::size_t i = 0; i < np && !held; ++i) held = pinned[i] == n;
            if (!held) destroy_node(n);
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
        r.overflow_lock.flag.clear(std::memory_order_release);
        r.overflow_lock.flag.notify_one();
    }
}

}  // namespace detail_hp

/**
 * @brief The host slot (ADR-0069 §1): a lock-free `atomic<node_t*>` reclaimed with hazard
 *        pointers, returning the same owning `value_ref_t` @ref single_writer_slot_t does.
 *
 * Why this exists: today's slot INVERTS under concurrent readers — measured through the real
 * path, `graph_t::read` on one shared LKV falls from 21.1 M/s at one reader to 1.7 M/s at
 * twenty-four, because both `load` and `store` take libstdc++'s `_Sp_locker` pointer-lock
 * bit. Hazard reclamation deletes that lock; what it cannot delete is the control-block
 * increment an owning read still owes. End to end that is worth **4.2×** at twenty-four
 * readers (7.4 M/s) — see the table in this file's header, and ADR-0069 §6 for why the
 * model bench's 20.8× did not survive contact with the whole read path.
 *
 * A host opt-in. The gain over the refcount slot it replaced is a concurrency gain, so a
 * single-core node buys nothing from it and still pays `(kHazardReaderSlots + 1) * 128` bytes
 * of registry, a deferred-reclamation lifetime rule (see `retire_and_flush`), and a publish
 * that can fail. Such a node binds @ref single_writer_slot_t.
 *
 * **Publish can fail under memory exhaustion**, which @ref single_writer_slot_t cannot: an empty
 * free list makes the first publish per participant allocate a 16-byte node. It is *reported*,
 * not silent — `store` returns `false` and `vertex_t::store` turns that into the same
 * `nullptr` → `BACKPRESSURE` soft-fail an LKV allocation failure already produces (#477), so
 * no write is ever reported as taken when it was not. Every later publish reuses the node its
 * own displacement recycled, so the window is a warm-up one — but it is still a real
 * difference in the policy's failure surface, and a third reason the MCU does not bind this
 * slot. Note also that the node comes from the **global heap**, not from a graph's injected
 * `block_source_t`: the slot policy is never handed one, and a bounded target that
 * needs every byte accounted for is another target that should bind @ref single_writer_slot_t.
 *
 * **It does not spin-wait.** The one loop in the domain that waits on another thread is the
 * overflow index's lock in `detail_hp::ticket_t`, and it waits with `atomic_flag::wait`, which
 * blocks (a futex, or libstdc++'s pooled condition variable) after a bounded spin. The read's
 * announce-and-revalidate loop retries only when a publish moved the slot, which is progress.
 */
class hazard_slot_t {
   public:
    /** @brief No operation spin-waits on another thread; see the class comment. */
    static constexpr bool may_spin = false;

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
     * A null value is not a publish — use @ref clear. On success the node adopts the caller's
     * reference; on `false` the reference is still the caller's.
     */
    [[nodiscard]] bool store(value_t* v,
                             std::memory_order order = std::memory_order_seq_cst) noexcept {
        if (v == nullptr) {
            clear(order);
            return true;
        }
        detail_hp::ticket_t t;
        detail_hp::lists_t& l = detail_hp::registry().lists[t.index()];
        detail_hp::node_t* fresh = detail_hp::acquire_node(l);
        if (fresh == nullptr) return false;  // nothing published; the caller soft-fails (#477)
        fresh->v = v;
        detail_hp::node_t* old = slot_.exchange(fresh, rmw_order(order));
        if (old != nullptr) detail_hp::retire(l, old);
        return true;
    }

    /**
     * @brief Drop the published value. Cannot fail — it publishes `nullptr`, which needs no
     *        node, so a clear allocates nothing even on a cold participant.
     */
    void clear(std::memory_order order = std::memory_order_seq_cst) noexcept {
        detail_hp::node_t* old = slot_.exchange(nullptr, rmw_order(order));
        if (old == nullptr) return;
        detail_hp::ticket_t t;
        detail_hp::retire(detail_hp::registry().lists[t.index()], old);
    }

    /**
     * @brief Read the published value.
     *
     * Announce, re-read, then `retain` the value out of the pinned node — the retain is the
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
    [[nodiscard]] value_ref_t load() const noexcept {
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
        value_ref_t out = value_ref_t::share(n->v);
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
