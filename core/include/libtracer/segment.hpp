/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * L1 segment: a refcounted span of real bytes owned by a tr::mem::mem_backend_t,
 * plus the intrusive segment_ptr_t handle that threads a segment's lifetime
 * through view fan-out. The refcount uses the canonical intrusive_ptr orderings
 * required by docs/reference/02-graph-model.md §required atomic operations
 * (increment = relaxed, decrement = acq_rel, inspect = acquire). On a core with
 * no atomic read-modify-write the count is a load and a store inside one section of
 * the build's guard (`config_t::guard_t`) instead (#1722).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <utility>

#include "libtracer/backend.hpp"
#include "libtracer/config.hpp"
#include "libtracer/guard.hpp"
#include "libtracer/guard_mutex.hpp"  // the host default graph::guard_t names its type

// Discovery, not a knob: glibc 2.32+ publishes __libc_single_threaded here (#1912).
#if __has_include(<sys/single_threaded.h>)
#include <sys/single_threaded.h>
#endif

/**
 * @file
 * @brief L1 (`tr::view`) refcounted `segment_t` and its owning `segment_ptr_t`.
 */

namespace tr::view {

namespace detail {

/**
 * @brief Whether this build counts segment references with one hardware RMW (`true`) or with a
 *        load and a store inside one section of `config_t::guard_t` (`false`, #1722).
 *
 * The same choice `tr::rmw_counter_t` makes for a vertex's write sequence: whether the target's
 * 32-bit atomic is always lock-free (`amoadd.w` on rv32imac, `ldrex`/`strex` on Cortex-M3 and
 * up, `lock xadd` on x86-64). A Cortex-M0 or rv32imc core takes the guarded binding; a
 * single-threaded build makes that guard free by binding `tr::no_guard_t`. A host test drives
 * the guarded binding by naming @ref basic_ref_count_t's `kNative` parameter itself.
 */
inline constexpr bool kNativeRefCount = std::atomic<std::uint_least32_t>::is_always_lock_free;

/**
 * @brief Whether the process has never started a second thread (#1912).
 *
 * The test `std::shared_ptr` makes before every count update (libstdc++'s
 * `__gnu_cxx::__is_single_threaded`): while it answers true no other thread can name a count,
 * so the native binding may update it with a plain load and store instead of a locked RMW.
 * Starting a thread clears the flag before the thread runs, and that start orders every
 * earlier plain update before the new thread's first access. Where the C library does not
 * publish the flag (an MCU's newlib, musl) this answers false and the count is always atomic.
 */
[[nodiscard]] inline bool process_single_threaded() noexcept {
#if __has_include(<sys/single_threaded.h>)
    return ::__libc_single_threaded != 0;
#else
    return false;
#endif
}

/**
 * @brief Intrusive refcount with the spec's orderings; @p kNative picks the binding.
 *
 * `dec_acq_rel` returns the value *before* the decrement, so a return of 1 means "this caller
 * dropped the last reference" — the canonical Boost intrusive_ptr release test.
 *
 * The guarded binding gives the same guarantees: every update is a load and a store inside
 * the guard covering this count, so no two updates interleave, and the guard's lock / unlock
 * order each update after the ones before it — the acquire / release pair the last dropper
 * needs to see every write made through the segment before it is reclaimed. Its loads and
 * stores stay single aligned-word instructions, with no library call.
 *
 * @tparam G       The build's critical-section guard (a `tr::guard`), taken only by the
 *                 guarded binding.
 * @tparam kNative Which binding. The bound @ref ref_count_t takes what the target supports; a
 *                 test names it to drive the guarded binding on a host that has atomic RMW.
 */
template <class G, bool kNative>
class basic_ref_count_t {
   public:
    /** @brief Whether the count is one hardware RMW (`true`) or a guarded load + store. */
    static constexpr bool is_native = kNative;

    /** @brief Start at @p initial references. */
    explicit basic_ref_count_t(std::uint_least32_t initial) noexcept : count_(initial) {}

    basic_ref_count_t(const basic_ref_count_t&) = delete;
    basic_ref_count_t& operator=(const basic_ref_count_t&) = delete;

    /** @brief Add one reference (relaxed: a new reference orders nothing). */
    void inc_relaxed() noexcept {
        if constexpr (kNative) {
            if (process_single_threaded())  // no other thread exists to race (#1912)
                count_.store(count_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            else
                count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            const guard_scope_t<G> section = open();
            count_.store(count_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    }

    /** @brief Drop one reference; returns the count BEFORE the drop. */
    [[nodiscard]] std::uint_least32_t dec_acq_rel() noexcept {
        if constexpr (kNative) {
            if (!process_single_threaded()) return count_.fetch_sub(1, std::memory_order_acq_rel);
            const std::uint_least32_t before = count_.load(std::memory_order_relaxed);
            count_.store(before - 1, std::memory_order_relaxed);
            return before;
        } else {
            const guard_scope_t<G> section = open();
            // Relaxed inside the section: its lock / unlock are the acquire / release.
            const std::uint_least32_t before = count_.load(std::memory_order_relaxed);
            count_.store(before - 1, std::memory_order_relaxed);
            return before;
        }
    }

    /** @brief The current count, acquire. Lock-free on both bindings. */
    [[nodiscard]] std::uint_least32_t load_acquire() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

   private:
    /** @brief Open the guard section covering this count (guarded binding only). */
    [[nodiscard]] guard_scope_t<G> open() const noexcept {
        static_assert(guard<G>,
                      "this target has no atomic read-modify-write for the segment refcount, "
                      "so it counts under config_t::guard_t, which must be a complete tr::guard "
                      "here: bind an interrupt-masked section, or tr::no_guard_t for a "
                      "single-threaded build, in libtracer/config_override.hpp (#1722)");
        return guard_scope_t<G>(this);
    }

    std::atomic<std::uint_least32_t> count_; /**< @brief The count; whole-word loads/stores. */
};

/** @brief The refcount as this build binds it. */
using ref_count_t = basic_ref_count_t<::tr::graph::guard_t, kNativeRefCount>;

}  // namespace detail

/**
 * @brief A refcounted span of real bytes: the L0↔L1 boundary object.
 *
 * Real bytes + the backend that reclaims them + an intrusive refcount. Never
 * copied or moved (the atomic refcount pins it in place); always handled
 * through @ref segment_ptr_t.
 *
 * @note `bytes` is writable at the type level, but whether writes are *legal*
 *       is the backend's contract — a const/ROM borrow must not be written
 *       through (see `%mem_borrowed.hpp`).
 */
struct segment_t {
    detail::ref_count_t refcount; /**< @brief Intrusive refcount (spec orderings). */
    mem::mem_backend_t* backend;  /**< @brief Reclaimer; non-const (cache hooks mutate it). */
    std::span<std::byte> bytes; /**< @brief The backing bytes this segment holds a reference to. */
    mem::mem_space_t space; /**< @brief Address space (HOST/DEVICE), inherited from @ref backend. */
    mem::backend_tag btag; /**< @brief Module-set tag, inherited from @ref backend (ADR-0047 §2). */
    /**
     * @brief Nonzero iff the first @ref tr::mem::kRxLoanBytes of @ref bytes are an INGRESS-LOAN
     * reserve (RFC-0028 §6.9, #1626), not payload.
     *
     * Set only by @ref alloc_rx, on a receive block a transport allocated with room for the
     * record the graph will need, so the terminus that stores a value shared out of this
     * frame builds that record IN the block instead of allocating one. A structural bit, never
     * an in-band marker: the reserve's bytes are never read to decide whether it exists, so a
     * peer cannot forge one. Rides the padding after @ref btag (`sizeof(segment_t)` is
     * unchanged on every target).
     */
    std::uint8_t rx_loan = 0;

    /** @brief Construct a segment over @p by, reclaimed by @p b, with @p initial refcount.
     *
     * The address space and module-set tag are taken from the backend
     * (`b->space()` / `b->tag()`); a `DEVICE` segment must not be
     * CPU-dereferenced (docs/adr/0024).
     */
    segment_t(mem::mem_backend_t* b, std::span<std::byte> by,
              std::uint_least32_t initial = 1) noexcept
        : refcount(initial),
          backend(b),
          bytes(by),
          space(b ? b->space() : mem::mem_space_t::HOST),
          btag(b ? b->tag() : mem::backend_tag::UNKNOWN) {}

    segment_t(const segment_t&) = delete;
    segment_t& operator=(const segment_t&) = delete;
};

/**
 * @brief Intrusive owning handle for a @ref segment_t.
 *
 * Copy = clone (refcount bump, relaxed); destruction = release (acq_rel); the
 * backend's `destroy` fires when the last handle drops. This is what makes a
 * borrowed (zero-copy) view safe to hold: the spans in a decoded TLV stay
 * valid as long as the view — and thus this handle — lives.
 */
class segment_ptr_t {
   public:
    segment_ptr_t() noexcept = default;

    /** @brief Adopt an existing reference (e.g. from `alloc`, refcount = 1) WITHOUT bumping. */
    [[nodiscard]] static segment_ptr_t adopt(segment_t* seg) noexcept {
        return segment_ptr_t(seg, false);
    }
    /** @brief Take a NEW shared reference to an already-live segment (bumps the count). */
    [[nodiscard]] static segment_ptr_t retain(segment_t* seg) noexcept {
        return segment_ptr_t(seg, true);
    }

    /** @brief Clone — a new shared reference to the same segment (relaxed increment). */
    segment_ptr_t(const segment_ptr_t& other) noexcept : seg_(other.seg_) {
        if (seg_) seg_->refcount.inc_relaxed();
    }
    /** @brief Transfer ownership of @p other's reference, leaving it empty. */
    segment_ptr_t(segment_ptr_t&& other) noexcept : seg_(other.seg_) { other.seg_ = nullptr; }
    /** @brief Copy-and-swap assignment — one operator covers copy- and move-assign. */
    segment_ptr_t& operator=(segment_ptr_t other) noexcept {
        std::swap(seg_, other.seg_);
        return *this;
    }
    ~segment_ptr_t() { reset(); }

    /** @brief Drop this reference (acq_rel); fires the backend's `destroy` at zero. */
    void reset() noexcept {
        if (seg_ && seg_->refcount.dec_acq_rel() == 1) {
            mem::destroy_dispatch(seg_);
        }
        seg_ = nullptr;
    }

    /** @brief The raw segment pointer (borrowed — no ownership transfer). */
    [[nodiscard]] segment_t* get() const noexcept { return seg_; }
    /** @brief Dereference to the owned segment. */
    [[nodiscard]] segment_t& operator*() const noexcept { return *seg_; }
    /** @brief Member access on the owned segment. */
    [[nodiscard]] segment_t* operator->() const noexcept { return seg_; }
    /** @brief True when this handle owns a segment. */
    [[nodiscard]] explicit operator bool() const noexcept { return seg_ != nullptr; }

    /** @brief Current refcount — debug / metrics only (acquire load), NOT a sync primitive. */
    [[nodiscard]] std::uint_least32_t use_count() const noexcept {
        return seg_ ? seg_->refcount.load_acquire() : 0;
    }

   private:
    segment_ptr_t(segment_t* seg, bool do_retain) noexcept : seg_(seg) {
        if (seg_ && do_retain) seg_->refcount.inc_relaxed();
    }
    segment_t* seg_ = nullptr;
};

/**
 * @brief The reserve's claim word: `0` = unclaimed, `1` = a value lives in the reserve.
 *
 * One claim per block, ever. A receive block carries one frame, and a frame stores at most one
 * value out of itself; a second attempt (a second subview of the same frame stored elsewhere)
 * finds the word taken and falls back to allocating its record, exactly as a block with no
 * reserve does.
 */
using rx_loan_word_t = std::atomic<std::uint32_t>;

namespace detail {

/**
 * @brief Set @p word from 0 to 1, once; the binding follows the refcount's (#1722).
 *
 * A template so that only the binding the build selects is instantiated: the guarded one needs
 * a complete guard, which a host build whose guard header is not included here never has.
 *
 * @param word   The claim word.
 * @param anchor The address whose guard the guarded binding takes (the segment).
 */
template <class G, bool kNative>
[[nodiscard]] bool claim_word(rx_loan_word_t* word, const void* anchor) noexcept {
    if constexpr (kNative) {
        (void)anchor;
        std::uint32_t expected = 0;
        return word->compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                             std::memory_order_relaxed);
    } else {
        static_assert(guard<G>, "config_t::guard_t must be a complete tr::guard here (#1722)");
        const guard_scope_t<G> section(anchor);
        if (word->load(std::memory_order_relaxed) != 0) return false;
        word->store(1, std::memory_order_relaxed);
        return true;
    }
}

}  // namespace detail

/**
 * @brief Claim @p seg's ingress-loan reserve for one value, once.
 *
 * @retval true  This caller owns the reserve: the value header may be placed at
 *               `bytes.data() + tr::mem::kRxLoanValueOffset`.
 * @retval false No reserve, or it is already claimed.
 */
[[nodiscard]] inline bool claim_rx_loan(segment_t* seg) noexcept {
    if (seg == nullptr || seg->rx_loan == 0) return false;
    auto* const word = reinterpret_cast<rx_loan_word_t*>(seg->bytes.data());
    return detail::claim_word<::tr::graph::guard_t, detail::kNativeRefCount>(word, seg);
}

}  // namespace tr::view

// The placement module: segment header, padding, the one-block-or-split rule and the receive-loan
// layout, plus the inline bodies of `mem_backend_t`'s one-block defaults. Included LAST because it
// needs the complete `segment_t`.
#include "libtracer/placement.hpp"
