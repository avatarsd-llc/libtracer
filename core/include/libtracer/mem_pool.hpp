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
     * @brief Hand out the next free slot as a `segment_t` of @p size bytes.
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
    static constexpr bool needs_cache_ops =
        false; /**< @brief No DMA cache maintenance (plain RAM slab). */
    static constexpr bool is_isr_safe =
        true; /**< @brief `alloc`/`destroy` are O(1) free-list ops — no heap, no syscall. */
    static constexpr bool owns_bytes =
        true; /**< @brief Bytes are backend-managed (freed only on `destroy`) — durably storable. */

    [[nodiscard]] std::size_t capacity() const noexcept {
        return slot_count_;
    } /**< @brief Total slots. */
    [[nodiscard]] std::size_t available() const noexcept {
        return free_count_;
    } /**< @brief Free slots. */

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
 * @brief The compile-time synchronisation seam of `synchronized_pool_t` (ADR-0047 §2
 *        module-set trait, ADR-0068 compile-time doctrine).
 *
 * A policy owns ONE critical-section mechanism: `lock()` / `unlock()` around the pool's
 * O(1) free-list ops, plus the two facts the seam publishes upward — whether the section
 * is ISR-safe and what the resulting backend is called. The target knows its concurrency
 * model at BUILD time (a single-core priority-preemptive MCU never becomes a multi-core
 * host), so the choice is a template argument, not a runtime knob: no branch, no vtable,
 * no per-alloc indirection on a ~120 ns operation.
 */
template <class P>
concept pool_sync_policy = requires(P& p) {
    { p.lock() } noexcept;
    { p.unlock() } noexcept;
    { P::is_isr_safe } -> std::convertible_to<bool>;
    { P::name } -> std::convertible_to<const char*>;
};

/**
 * @brief The MULTI-CORE HOST policy: an `std::atomic_flag` spinlock.
 *
 * Negligible contention on an O(1) section, and it avoids the ~2 µs OS-mutex round-trip
 * that would dominate the ~120 ns free-list op. NOT ISR-safe, and wrong for a single-core
 * priority-preemptive target, where a lower-priority holder cannot run while a
 * higher-priority task spins (ADR-0060 §2) — such a target supplies the interrupt-disable
 * critical-section policy instead (`tr::esp::portmux_sync_t` for ESP-IDF).
 */
struct spin_sync_t {
    static constexpr bool is_isr_safe = false;           /**< @brief Spin => not ISR-safe. */
    static constexpr const char* name = "mem_sync_pool"; /**< @brief Backend name. */
    /** @brief Acquire the flag, spinning (the guarded section is O(1)). */
    void lock() noexcept {
        while (lk_.test_and_set(std::memory_order_acquire)) { /* spin: O(1) section */
        }
    }
    /** @brief Release the flag. */
    void unlock() noexcept { lk_.clear(std::memory_order_release); }

   private:
    std::atomic_flag lk_{};
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
 * The mechanism is the target's to pick, because only the target knows its concurrency
 * model: @ref spin_sync_t on a multi-core host, an interrupt-disable critical section on a
 * single-core priority-preemptive MCU (`tr::esp::portmux_sync_t`, shipped by the ESP-IDF
 * component — it needs FreeRTOS headers, so it lives outside `core/`). The many-core
 * lock-free index+tag CAS upgrade (the free list is already index-based) stays the
 * recorded ADR-0060 §2 follow-up.
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
template <pool_sync_policy Sync>
class synchronized_pool_t final : public mem_backend_t {
    // The one policy this library ships that spin-waits. On a target that says spin-waiting is
    // unsafe (@ref kSpinWaitSafe), binding it here is not "slower" — it is a hang, and only the
    // build knows which target this is. Checked on INSTANTIATION, so the `sync_pool_t` alias
    // below still names the type freely; declaring or constructing one is what trips the guard.
    static_assert(kSpinWaitSafe || !std::is_same_v<Sync, spin_sync_t>,
                  "synchronized_pool_t<spin_sync_t> (a.k.a. tr::mem::sync_pool_t) spin-waits, "
                  "and this build set tr::mem::kSpinWaitSafe = false: a spinner that outranks "
                  "the lock holder never yields the CPU the holder needs, so the wait is "
                  "unbounded and the target hangs. Bind the target's interrupt-disable policy "
                  "instead -- on ESP-IDF that is tr::esp::critical_pool_t "
                  "(libtracer_esp/critical_pool.hpp).");

   public:
    /** @brief Carve @p slab into @p slot_payload-byte slots (see @ref pool_t), thread-safe. */
    synchronized_pool_t(std::span<std::byte> slab, std::size_t slot_payload,
                        std::size_t align = alignof(std::max_align_t)) noexcept
        : mem_backend_t(Sync::name), inner_(slab, slot_payload, align) {}

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
        Sync::is_isr_safe;                   /**< @brief Whatever the sync policy guarantees. */
    static constexpr bool owns_bytes = true; /**< @brief Backend-managed, durably storable. */

    /** @brief Total slots (delegated to the inner @ref pool_t). */
    [[nodiscard]] std::size_t capacity() const noexcept { return inner_.capacity(); }

   private:
    pool_t inner_;
    Sync sync_{};
};

/**
 * @brief The multi-core-host spelling of `synchronized_pool_t` — a spinlock-guarded pool.
 *
 * The name predates the policy seam and is kept as the host default (ADR-0060 §2's
 * spinlock variant); a single-core MCU wants the critical-section policy instead — and
 * because the short name is the discoverable one, a build that sets @ref kSpinWaitSafe to
 * false rejects this instantiation outright rather than shipping a hang.
 */
using sync_pool_t = synchronized_pool_t<spin_sync_t>;

}  // namespace tr::mem
