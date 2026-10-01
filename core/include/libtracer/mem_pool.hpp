/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_pool — a fixed, caller-owned slab carved into equal slots. The bounded
 * "custom allocator" reference backend: alloc returns nullptr when the pool is
 * exhausted (the BACKPRESSURE signal, docs/reference/09 §pressure), making it
 * the deterministic choice for MCUs and bounded-memory targets (= mem_pool_static
 * in reference/09). The free list is threaded through the slab itself — there is
 * NO auxiliary heap allocation, so total memory use is exactly the caller's slab.
 *
 * Not internally synchronized: alloc/destroy on a shared pool are the
 * application's to serialize. Per ADR-0012 each backend declares its own
 * concurrency contract; this one's is "single-threaded reclamation."
 */
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>

#include "libtracer/backend.hpp"
#include "libtracer/config.hpp"
#include "libtracer/guard.hpp"
#include "libtracer/guard_mutex.hpp"  // the host default graph::guard_t names its type
#include "libtracer/segment.hpp"

/**
 * @file
 * @brief The bounded `mem_pool` L0 backend (`tr::mem`) over a caller-owned slab.
 */

namespace tr::mem {

/**
 * @brief A fixed-slot allocator over a caller-owned slab; `alloc`-or-`nullptr`.
 *
 * Carves the slab into equal slots with the free list threaded through the slab
 * (no auxiliary heap), so memory use is exactly the caller's slab and
 * exhaustion is a return value, not an OOM. The deterministic MCU choice.
 */
class pool_t final : public mem_backend_t {
   public:
    /**
     * @brief Carve @p slab (caller-owned; must outlive the pool) into slots.
     *
     * Each slot holds a `segment_t` control block plus @p slot_payload usable
     * bytes, payload aligned to @p align (a power of two). The slot count is
     * whatever fits after aligning the slab base.
     */
    pool_t(std::span<std::byte> slab, std::size_t slot_payload,
           std::size_t align = alignof(std::max_align_t)) noexcept;

    /**
     * @brief One slot as a raw block — the @ref block_source_t half (RFC-0028 §4.9): a request
     *        that fits a slot (header included) at no stricter alignment than the slab's.
     * @retval nullptr The pool is empty, or the request does not fit one slot.
     */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override;
    /** @brief Return a slot @ref try_alloc handed out (the size is implied by the stride). */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override;

    /**
     * @brief Hand out the next free slot as a `segment_t` of @p size bytes — the one-block
     *        layout, the header at the slot's head.
     * @retval nullptr `size` exceeds the slot payload, or the pool is exhausted.
     */
    view::segment_t* alloc(std::size_t size, alloc_hint_t hint = alloc_hint_t::NONE) override;
    /** @brief Return @p seg's slot to the free list (placement-destroying it). */
    void destroy(view::segment_t* seg) noexcept override;
    [[nodiscard]] std::size_t alignment() const noexcept override { return align_; }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override { return slot_payload_; }
    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::POOL; }

    // Module-set traits (ADR-0047 §2): compile-time backend contracts the seam
    // consumes in place of prose. `needs_cache_ops` is read by `mem::transfer`.
    //
    // `is_isr_safe` and `is_nonblocking` are DISTINCT properties (#928). This pool's
    // free-list ops are syscall-free — but `alloc`/`destroy` do an UNSYNCHRONIZED RMW on
    // `free_head_`/`free_count_`, so an ISR interleaving with task-context use corrupts the
    // list. ISR safety is a critical section, i.e. `synchronized_pool_t` with an ISR-safe
    // policy (`tr::esp::critical_pool_t`), never the bare pool.
    static constexpr bool needs_cache_ops =
        false; /**< @brief No DMA cache maintenance (plain RAM slab). */
    static constexpr bool is_isr_safe =
        false; /**< @brief Unsynchronized free-list RMW — NOT safe concurrent with an ISR. */
    static constexpr bool is_nonblocking =
        true; /**< @brief `alloc`/`destroy` are O(1) free-list ops — no heap, no syscall. */
    static constexpr bool owns_bytes =
        true; /**< @brief Bytes are backend-managed (freed only on `destroy`) — durably storable. */

    [[nodiscard]] std::size_t capacity() const noexcept {
        return slot_count_;
    } /**< @brief Total slots — the effective ceiling, i.e. whatever fitted in the caller's
         slab, never a constant. */
    /**
     * @brief FREE slots.
     *
     * @note The one shipped free-polarity accessor, kept for compatibility. New code reads
     *       @ref in_use — the unified vocabulary is used-polarity throughout
     *       (`core/STYLE.md` §Introspection), and `available` is the derived legacy name
     *       (`capacity() - in_use()`).
     */
    [[nodiscard]] std::size_t available() const noexcept { return free_count_; }
    /** @brief Slots handed out and not yet returned — occupancy in the used-polarity
     *         vocabulary (`core/STYLE.md` §Introspection). */
    [[nodiscard]] std::size_t in_use() const noexcept { return slot_count_ - free_count_; }

   private:
    static constexpr std::size_t kNil = ~std::size_t{0};
    [[nodiscard]] std::byte* slot_at(std::size_t i) const noexcept {
        return slab_.data() + i * stride_;
    }
    void store_next(std::size_t slot, std::size_t next) noexcept;
    [[nodiscard]] std::size_t load_next(std::size_t slot) const noexcept;

    std::span<std::byte> slab_;     // aligned, usable region of the caller's slab
    std::size_t slot_payload_ = 0;  // usable bytes per slot
    std::size_t align_ = 0;         // payload alignment reported via alignment()
    std::size_t header_ = 0;        // aligned sizeof(segment_t) prefixing each slot
    std::size_t stride_ = 0;        // header_ + aligned slot_payload_
    std::size_t slot_count_ = 0;
    std::size_t free_count_ = 0;
    std::size_t free_head_ = kNil;  // index of first free slot (intrusive free list)
};

/**
 * @brief A thread-safe `pool_t` whose SYNCHRONISATION IS A COMPILE-TIME POLICY
 *        (ADR-0060 §2), guarding the O(1) free-list with @p Sync.
 *
 * Any `mem_backend_t` injected at a shared seam MUST be thread-safe: a `segment`
 * self-routes its reclaim on whatever thread drops the last ref — typically a
 * *reader/subscriber* or transport *receive* thread, concurrent with a writer's `alloc`
 * (ADR-0060 §2; the same obligation holds for `graph_t`'s `value_backend`, the router's
 * `flat`, and `transport_vertex_t`'s `rx_backend`). A single thread-safe pool (never
 * per-stripe sharding, which removes no race and adds partition imbalance) is the answer.
 *
 * The policy is a `tr::guard` — the SAME critical-section trait the LKV slot binds
 * (RFC-0028 §5.5, slice 10), so a target states its concurrency model once:
 * @ref graph::guard_t is the default, which is `tr::mutex_guard_t` on a host (one RMW to
 * take; a contender naps rather than spins) and the interrupt-masked `tr::esp::critical_guard_t`
 * on an ESP-IDF chip (`tr::esp::critical_pool_t`). The target knows its concurrency model at
 * BUILD time, so the choice is a template argument, not a runtime knob: no branch, no vtable,
 * no per-alloc indirection on a ~120 ns operation. The many-core lock-free index+tag CAS
 * upgrade (the free list is already index-based) stays the recorded ADR-0060 §2 follow-up.
 *
 * This is **opt-in construction only** — no seam defaults to it. `heap_backend()` remains
 * the default everywhere; a target that wants its receive/value bytes inside its own slab
 * constructs one of these and injects it.
 *
 * Composition over `pool_t`: a freshly-`alloc`'d segment is re-pointed to `this` with a
 * `UNKNOWN` tag, so `destroy_dispatch` routes reclaim through the virtual (locked)
 * `destroy` here instead of the devirtualized POOL fast path (which would bypass the
 * lock). `pool_t::destroy` recovers the slot from the segment's slab offset, so the
 * re-point is invisible to the inner pool. The re-point touches only the just-allocated
 * segment, which no other thread can observe until the caller publishes it.
 */
template <::tr::guard Sync = graph::guard_t>
class synchronized_pool_t final : public mem_backend_t {
    // On a target that says spin-waiting is unsafe (`kSpinWaitSafe`), a guard that spin-waits
    // is not "slower" — it is a hang, and only the build knows which target this is. Checked
    // on INSTANTIATION, the pool twin of the slot's `may_spin` assertion in `%vertex.hpp`.
    static_assert(kSpinWaitSafe || !Sync::may_spin,
                  "synchronized_pool_t over a guard that declares may_spin = true spin-waits, "
                  "and this build set tr::mem::kSpinWaitSafe = false: a spinner that outranks "
                  "the lock holder never yields the CPU the holder needs, so the wait is "
                  "unbounded and the target hangs. Bind the target's interrupt-masked guard "
                  "instead -- on ESP-IDF that is tr::esp::critical_pool_t "
                  "(libtracer_esp/critical_pool.hpp).");

   public:
    /** @brief Carve @p slab into @p slot_payload-byte slots (see @ref pool_t), thread-safe. */
    synchronized_pool_t(std::span<std::byte> slab, std::size_t slot_payload,
                        std::size_t align = alignof(std::max_align_t)) noexcept
        : mem_backend_t(Sync::name), inner_(slab, slot_payload, align) {}

    /** @brief @ref pool_t::try_alloc inside @p Sync's critical section. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        sync_.lock();
        void* const p = inner_.try_alloc(bytes, align);
        sync_.unlock();
        return p;
    }
    /** @brief @ref pool_t::release inside @p Sync's critical section. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        sync_.lock();
        inner_.release(p, bytes, align);
        sync_.unlock();
    }

    /** @brief @ref pool_t::alloc inside @p Sync's critical section; reclaim re-routed here. */
    view::segment_t* alloc(std::size_t size, alloc_hint_t hint = alloc_hint_t::NONE) override {
        sync_.lock();
        view::segment_t* seg = inner_.alloc(size, hint);
        if (seg != nullptr) {
            seg->backend = this;               // reclaim routes back through this (locked)
            seg->btag = backend_tag::UNKNOWN;  // -> destroy_dispatch virtual fallback
        }
        sync_.unlock();
        return seg;
    }
    /** @brief @ref pool_t::destroy inside @p Sync's critical section. */
    void destroy(view::segment_t* seg) noexcept override {
        sync_.lock();
        inner_.destroy(seg);
        sync_.unlock();
    }
    [[nodiscard]] std::size_t alignment() const noexcept override { return inner_.alignment(); }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override {
        return inner_.max_segment_size();
    }
    /** @brief UNKNOWN so `destroy_dispatch` takes the virtual (locked) `destroy`, not the
     *         devirtualized POOL fast path that would bypass the critical section. */
    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::UNKNOWN; }

    // Module-set traits (ADR-0047 §2). ISR-safety is the POLICY's fact, forwarded here;
    // plain-RAM slab => no cache ops.
    static constexpr bool needs_cache_ops = false; /**< @brief Plain RAM slab. */
    static constexpr bool is_isr_safe =
        Sync::is_isr_safe; /**< @brief Whatever the sync policy guarantees. */
    static constexpr bool is_nonblocking =
        Sync::is_nonblocking; /**< @brief The pool's section is O(1); the WAIT is the policy's
                                 fact, so this forwards it rather than asserting it (#928). */
    static constexpr bool owns_bytes = true; /**< @brief Backend-managed, durably storable. */

    /** @brief Total slots (delegated to the inner @ref pool_t). */
    [[nodiscard]] std::size_t capacity() const noexcept { return inner_.capacity(); }

    /**
     * @brief Slots handed out and not yet returned (delegated to the inner @ref pool_t).
     *
     * The wrapper used to forward @ref capacity and STOP there, so wrapping a bounded
     * resource in its thread-safe form lost half its census: a host could read the ceiling
     * it had injected but not how much of it was gone — on precisely the pool that is
     * shared, i.e. the one whose occupancy is hardest to reason about (#1503 finding 4).
     *
     * Read without taking @p Sync, per the snapshot-coherence clause (`core/STYLE.md`
     * §Introspection): locking here would put a critical section on a ~120 ns free-list op
     * in order to serve a diagnostic, and the intended reading is the difference between
     * two samples, not an instant.
     */
    [[nodiscard]] std::size_t in_use() const noexcept { return inner_.in_use(); }

    /** @brief FREE slots (delegated). Free-polarity legacy spelling — new code reads
     *         @ref in_use; see @ref pool_t::available. */
    [[nodiscard]] std::size_t available() const noexcept { return inner_.available(); }

   private:
    pool_t inner_;
    Sync sync_{};
};

}  // namespace tr::mem
