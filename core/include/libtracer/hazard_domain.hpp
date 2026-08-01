/**
 * @file
 * @brief The graph-owned, backend-injected hazard-pointer reclamation domain (ADR-0072).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One answer to the replaced-block question — *what happens to a heap block a
 * control-plane writer replaces while a lock-free reader still holds a raw reference
 * into it* — generalized from the announce/scan protocol `lkv_slot.hpp`'s `detail_hp`
 * namespace proved (ADR-0069 §5), with the two properties that blocked reusing it
 * directly repaired (ADR-0072 §2/§3):
 *
 *   - **Instance, not process-global.** A `hazard_domain_t` is an ordinary member
 *     (`graph_t` owns one), so it can take an injected resource and be torn down — the
 *     final sweep runs in its destructor, which is half of what #576 needed.
 *   - **Type-erased, backend-allocated retire records.** The retire API is
 *     `retire(void*, void (*)(void*, mem_backend_t&))`; announcement cells stay `void*`.
 *     Records draw from the injected `mem_backend_t`, so retirement pressure is
 *     bounded by a real resource the target sizes (no synthetic limits), and the MCU
 *     objection to `detail_hp` (global-heap allocation) is retired by construction.
 *
 * The first tenant is `graph_t`'s retired value seam (#576); the published edge array
 * (#635) rides the same plumbing next; the LKV slot's private static domain migrates
 * LAST, in its own measured slice — `detail_hp` is deliberately untouched here.
 *
 * ## Protocol summary
 *
 * A reader takes a `hazard_domain_t::guard_t` (claiming one cache-line-isolated
 * announcement cell for the length of the operation), announces the pointer it is about
 * to dereference via `protect` (announce, then **re-read** the source — the ADR-0069 §5
 * loop), and clears on guard destruction — after the user callback returns, so a slow
 * callback merely delays that one block's reclamation. A writer that displaced a block
 * hands it to `hazard_domain_t::retire`; parked records are freed by a scan (a cold,
 * threshold-batched control-plane event) once no cell announces them.
 *
 * **Exhaustion never leaks and never aborts** (ADR-0072 §3): a writer that cannot
 * allocate a retire record runs a scan (to shed parked blocks), then blocks until no
 * reader announces ITS block, and frees it inline — correct, slow, bounded. A reader
 * that finds every cell claimed serializes with other overflow readers on the one shared
 * overflow cell (the `detail_hp` policy): a misconfigured cell count costs throughput on
 * the overflow threads and nothing else.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>

#include "libtracer/backend.hpp"
#include "libtracer/config.hpp"
#include "libtracer/segment.hpp"

namespace tr::mem {

/**
 * @brief An instance-owned, type-erased hazard-pointer reclamation domain (ADR-0072).
 *
 * Owned by a composition root (`graph_t` today), borrowed by reference by its tenants.
 * Reader side: @ref guard_t. Writer side: @ref retire. Teardown: the destructor runs the
 * final sweep — every parked block's deleter fires, so nothing a tenant retired can
 * outlive the domain. The domain allocates ONLY retire records, and only from the
 * injected backend; announcement cells are embedded in the object itself.
 *
 * Thread-safety: `retire` and every guard operation may run concurrently from any
 * thread. Destruction requires quiescence — no guard live, no `retire` in flight — the
 * same precondition every C++ object's destructor has.
 */
class hazard_domain_t {
   public:
    /**
     * @brief A parked block's reclaimer: frees @p block, returning it to the resource it
     *        came from. The second argument is the domain's injected backend, for blocks
     *        that were allocated from it; a block from elsewhere (e.g. the global heap)
     *        ignores it. Must not throw and must not re-enter the domain.
     */
    using deleter_fn_t = void (*)(void* block, mem_backend_t& backend);

    /** @brief The claimable announcement cells; the config knob `detail_hp` sizes by too. */
    static constexpr std::size_t kCells = graph::kHazardReaderSlots;

    /** @brief The shared index readers fall back to when every claimable cell is taken. */
    static constexpr std::size_t kOverflowCell = kCells;

    /**
     * @brief How many records may park before a retire triggers a scan — the tightest
     *        batch that can hope to free anything, since at most `kCells + 1` blocks can
     *        be announced domain-wide at any instant (the `detail_hp` derivation).
     */
    static constexpr std::size_t kRetireBatch = kCells + 1;

    /**
     * @brief Construct a domain drawing its retire records from @p backend.
     *
     * @p backend must outlive the domain and must be thread-safe if tenants retire from
     * more than one thread (the same contract ADR-0060 §2 states for the graph's value
     * backend, which is what `graph_t` injects here).
     */
    explicit hazard_domain_t(mem_backend_t& backend) noexcept : backend_(backend) {}

    hazard_domain_t(const hazard_domain_t&) = delete;
    hazard_domain_t& operator=(const hazard_domain_t&) = delete;

    /**
     * @brief The final sweep (ADR-0072 §2): free every parked block through its deleter.
     *
     * Precondition: quiescence — no live guard, no concurrent `retire`. `graph_t`'s
     * destructor satisfies it the way every member destructor does: a caller destroying
     * the graph under a live reader was already undefined.
     */
    ~hazard_domain_t() {
        record_t* r = retired_.exchange(nullptr, std::memory_order_acq_rel);
        while (r != nullptr) {
            record_t* next = r->next;
            r->deleter(r->block, backend_);
            free_record(r);
            r = next;
        }
        retired_n_.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Hand a displaced @p block to the domain: parked until no reader announces
     *        it, then freed via @p deleter. Never leaks, never aborts (ADR-0072 §3).
     *
     * The common case allocates one record from the injected backend and returns; a
     * threshold-crossing retire additionally runs a scan (cold, O(cells + parked)). On
     * record exhaustion the calling thread scans, then **blocks** until no cell
     * announces @p block, and frees it inline — so a caller holding a lock a reader's
     * announced callback could re-enter must size the record backend for its churn.
     *
     * @param block   The displaced block; `nullptr` is a no-op.
     * @param deleter Frees @p block (see @ref deleter_fn_t). Must be non-null.
     */
    void retire(void* block, deleter_fn_t deleter) noexcept {
        if (block == nullptr) return;
        record_t* rec = alloc_record();
        if (rec == nullptr) {
            // Exhaustion policy (ADR-0072 §3): shed what a scan can, then free inline
            // once provably unannounced — correct, slow, bounded; never leak or abort.
            scan();
            wait_unannounced(block);
            deleter(block, backend_);
            return;
        }
        rec->block = block;
        rec->deleter = deleter;
        // Count BEFORE pushing: a concurrent scan may pop the list between the two, and
        // its decrement must never underflow the counter (the count may transiently
        // exceed the list length; it can never fall below it).
        const std::size_t n = retired_n_.fetch_add(1, std::memory_order_acq_rel) + 1;
        push_retired(rec);
        if (n >= kRetireBatch) scan();
    }

    /**
     * @brief Records currently parked (approximate under concurrency) — accounting for
     *        tests and a memory census, never a synchronization primitive.
     */
    [[nodiscard]] std::size_t retired_count() const noexcept {
        return retired_n_.load(std::memory_order_relaxed);
    }

    /**
     * @brief A reader's claim on one announcement cell for the length of one operation.
     *
     * Construction is FREE — the claim and the announcement are one CAS inside
     * @ref protect (claiming `nullptr` → the pointer being announced), so a guard whose
     * source turns out empty touches no shared state at all. Destruction clears the
     * announcement and releases the cell in one store — place the guard so it destructs
     * AFTER the user callback returns. When every cell is taken, `protect` serializes
     * with other overflow readers on the shared overflow cell under a spin lock; never
     * nest two guards of one domain on one thread (the lock is not recursive) — tenants
     * that announce once per operation cannot hit this.
     */
    class guard_t {
       public:
        /** @brief Bind to @p d. Claims nothing until @ref protect announces. */
        explicit guard_t(hazard_domain_t& d) noexcept : d_(d) {}

        guard_t(const guard_t&) = delete;
        guard_t& operator=(const guard_t&) = delete;

        /** @brief Clear the announcement and release the cell (and the overflow lock). */
        ~guard_t() {
            if (idx_ == kNoCell) return;
            d_.cells_[idx_].pinned.store(nullptr, std::memory_order_release);
            if (overflow_) {
                d_.overflow_lock_.clear(std::memory_order_release);
                d_.overflow_lock_.notify_one();
            }
        }

        /**
         * @brief Announce the pointer @p src currently publishes and return it pinned —
         *        the ADR-0069 §5 announce-then-re-read loop, so "not announced" means
         *        "cannot become announced" against a concurrent displace + scan.
         *
         * The cell claim IS the first announcement (one `seq_cst` CAS of `nullptr` → the
         * pointer), and the re-read is `seq_cst`, so both sit in one total order with
         * the writer's displacing exchange and the scanner's fence (the same argument
         * `hazard_slot_t::load` records). The returned pointer stays valid until this
         * guard clears — i.e. through the caller's user callback — even if a writer
         * displaces and retires it meanwhile.
         *
         * @return The pinned pointer, or `nullptr` when @p src publishes none — in which
         *         case, if nothing was yet claimed, the guard holds no cell and its
         *         destructor is free.
         */
        template <typename T>
        [[nodiscard]] T* protect(const std::atomic<T*>& src) noexcept {
            T* p = src.load(std::memory_order_acquire);
            if (p == nullptr && idx_ == kNoCell) return nullptr;  // nothing to pin, no claim
            if (idx_ == kNoCell) claim(p);
            std::atomic<void*>& cell = d_.cells_[idx_].pinned;
            for (;;) {
                T* again = src.load(std::memory_order_seq_cst);
                if (again == p) return p;
                // Source emptied mid-loop: return null but leave the STALE announcement
                // in place (it merely defers that one block until the guard clears).
                // Storing nullptr here would RELEASE the cell while this guard still
                // owns it — a later claimant's announcement would then be destroyed by
                // our destructor's clear, un-pinning a block a reader is inside.
                if (again == nullptr) return nullptr;
                p = again;
                cell.store(p, std::memory_order_seq_cst);  // re-announce, then re-read
            }
        }

       private:
        /** @brief "This guard holds no cell (yet)." */
        static constexpr std::size_t kNoCell = static_cast<std::size_t>(-1);

        /**
         * @brief Claim a cell with @p p as its first announcement: one CAS from a
         *        thread-local probe hint in the common case, the shared overflow cell
         *        under its spin lock when every claimable cell is taken (which costs
         *        throughput on the overflow threads and nothing else — ADR-0069 §3).
         */
        void claim(void* p) noexcept {
            std::size_t& hint = probe_hint();
            for (std::size_t k = 0; k < kCells; ++k) {
                const std::size_t i = (hint + k) % kCells;
                void* expected = nullptr;
                if (d_.cells_[i].pinned.compare_exchange_strong(
                        expected, p, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    idx_ = i;
                    hint = i;
                    return;
                }
            }
            while (d_.overflow_lock_.test_and_set(std::memory_order_acquire)) {
                d_.overflow_lock_.wait(true, std::memory_order_relaxed);
            }
            overflow_ = true;
            idx_ = kOverflowCell;
            d_.cells_[idx_].pinned.store(p, std::memory_order_seq_cst);
        }

        hazard_domain_t& d_;        /**< @brief The domain whose cell this guard holds. */
        std::size_t idx_ = kNoCell; /**< @brief The claimed cell index, or `kNoCell`. */
        bool overflow_ = false;     /**< @brief Whether `idx_` is the shared overflow cell. */
    };

   private:
    /**
     * @brief One type-erased retire record (ADR-0072 §3): the parked block, its
     *        reclaimer, the intrusive list link, and the segment the record itself
     *        lives in (so freeing the record returns it to the injected backend).
     */
    struct record_t {
        void* block = nullptr;           /**< @brief The displaced, parked block. */
        deleter_fn_t deleter = nullptr;  /**< @brief Frees `block` on reclaim. */
        record_t* next = nullptr;        /**< @brief Retired-list link. */
        view::segment_t* self = nullptr; /**< @brief The backend segment holding this record. */
    };

    /**
     * @brief The cell padding width: the cache-line knob floored at the payload's own
     *        alignment so every value of the knob stays well-formed (the `detail_hp`
     *        derivation). A single-core build sets the knob to 0 and the table collapses
     *        to its payloads.
     */
    static constexpr std::size_t kCellAlign = graph::kCacheLineBytes > alignof(std::atomic<void*>)
                                                  ? graph::kCacheLineBytes
                                                  : alignof(std::atomic<void*>);

    /** @brief One announcement cell, cache-line isolated so a scan does not false-share. */
    struct alignas(kCellAlign) cell_t {
        /** @brief The block a reader is dereferencing; `nullptr` = free. One word carries
         *         both the claim and the announcement (the guard claims by CAS-ing the
         *         announced pointer straight in). */
        std::atomic<void*> pinned{nullptr};
    };

    /** @brief This thread's claim-probe start — a reuse hint, never a correctness input. */
    [[nodiscard]] static std::size_t& probe_hint() noexcept {
        static thread_local std::size_t hint = 0;
        return hint;
    }

    /** @brief A record from the injected backend, or `nullptr` on exhaustion (nothrow). */
    [[nodiscard]] record_t* alloc_record() noexcept {
        view::segment_t* seg = backend_.alloc(sizeof(record_t));
        if (seg == nullptr) return nullptr;
        void* p = seg->bytes.data();
        if (seg->bytes.size() < sizeof(record_t) ||
            reinterpret_cast<std::uintptr_t>(p) % alignof(record_t) != 0) {
            // A backend whose blocks cannot hold a record is exhaustion, not an abort.
            view::segment_ptr_t::adopt(seg).reset();
            return nullptr;
        }
        record_t* rec = ::new (p) record_t{};
        rec->self = seg;
        return rec;
    }

    /** @brief Return a record's storage to the backend it came from. */
    static void free_record(record_t* rec) noexcept {
        view::segment_t* seg = rec->self;
        rec->~record_t();
        view::segment_ptr_t::adopt(seg).reset();  // refcount 1 → 0 → backend destroy
    }

    /** @brief Lock-free push onto the retired list (Treiber). */
    void push_retired(record_t* rec) noexcept {
        record_t* old = retired_.load(std::memory_order_relaxed);
        do {
            rec->next = old;
        } while (!retired_.compare_exchange_weak(old, rec, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed));
    }

    /**
     * @brief Free every parked block no cell announces; re-park the rest.
     *
     * The `seq_cst` fence is the load-bearing half of the hazard protocol (the
     * `detail_hp::scan` argument): it orders the retirer's displacement before its reads
     * of the cells, against a reader's `seq_cst` announce followed by its re-read of the
     * source — one of the two must see the other. Concurrent scans are safe: each owns
     * exactly the batch it popped.
     */
    void scan() noexcept {
        record_t* batch = retired_.exchange(nullptr, std::memory_order_acq_rel);
        if (batch == nullptr) return;

        std::atomic_thread_fence(std::memory_order_seq_cst);
        void* pinned[kCells + 1];
        std::size_t np = 0;
        for (const cell_t& c : cells_) {
            void* p = c.pinned.load(std::memory_order_seq_cst);
            if (p != nullptr) pinned[np++] = p;
        }

        std::size_t popped = 0;
        std::size_t kept = 0;
        for (record_t* cur = batch; cur != nullptr;) {
            record_t* next = cur->next;
            ++popped;
            bool held = false;
            for (std::size_t i = 0; i < np && !held; ++i) held = pinned[i] == cur->block;
            if (held) {
                push_retired(cur);  // still announced — park it for a later scan
                ++kept;
            } else {
                cur->deleter(cur->block, backend_);
                free_record(cur);
            }
            cur = next;
        }
        retired_n_.fetch_sub(popped - kept, std::memory_order_acq_rel);
    }

    /** @brief Block until no cell announces @p block — the inline-free half of exhaustion. */
    void wait_unannounced(const void* block) const noexcept {
        for (;;) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            bool held = false;
            for (const cell_t& c : cells_) {
                if (c.pinned.load(std::memory_order_seq_cst) == block) {
                    held = true;
                    break;
                }
            }
            if (!held) return;
            std::this_thread::yield();  // the announcer is mid-callback; let it finish
        }
    }

    mem_backend_t& backend_;           /**< @brief The injected record resource (ADR-0072 §3). */
    cell_t cells_[kCells + 1]{};       /**< @brief Announcements: claimable cells + overflow. */
    std::atomic_flag overflow_lock_{}; /**< @brief Serializes users of @ref kOverflowCell. */
    std::atomic<record_t*> retired_{nullptr}; /**< @brief Parked records awaiting a scan. */
    std::atomic<std::size_t> retired_n_{0};   /**< @brief Approximate length of `retired_`. */
};

}  // namespace tr::mem
