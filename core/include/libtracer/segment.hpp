/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * L1 segment: a refcounted span of real bytes owned by a tr::mem::mem_backend_t,
 * plus the intrusive segment_ptr_t handle that threads a segment's lifetime
 * through view fan-out. The refcount uses the canonical intrusive_ptr orderings
 * required by docs/reference/02-graph-model.md §required atomic operations
 * (increment = relaxed, decrement = acq_rel, inspect = acquire). Define
 * LIBTRACER_NO_ATOMIC for single-threaded / Cortex-M0 builds (no cross-thread
 * segment sharing).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <utility>

#include "libtracer/backend.hpp"

#ifndef LIBTRACER_NO_ATOMIC
#include <atomic>
#endif

/**
 * @file
 * @brief L1 (`tr::view`) refcounted `segment_t` and its owning `segment_ptr_t`.
 */

namespace tr::view {

namespace detail {

// Intrusive refcount with the spec's orderings. dec_acq_rel returns the value
// *before* the decrement, so a return of 1 means "this caller dropped the last
// reference" — the canonical Boost intrusive_ptr release test.
class ref_count_t {
   public:
    explicit ref_count_t(std::uint_least32_t initial) noexcept : count_(initial) {}

    ref_count_t(const ref_count_t&) = delete;
    ref_count_t& operator=(const ref_count_t&) = delete;

#ifdef LIBTRACER_NO_ATOMIC
    void inc_relaxed() noexcept { ++count_; }
    [[nodiscard]] std::uint_least32_t dec_acq_rel() noexcept { return count_--; }
    [[nodiscard]] std::uint_least32_t load_acquire() const noexcept { return count_; }

   private:
    std::uint_least32_t count_;
#else
    void inc_relaxed() noexcept { count_.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] std::uint_least32_t dec_acq_rel() noexcept {
        return count_.fetch_sub(1, std::memory_order_acq_rel);
    }
    [[nodiscard]] std::uint_least32_t load_acquire() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

   private:
    std::atomic<std::uint_least32_t> count_;
#endif
};

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
     * @brief Nonzero iff the first @ref kRxLoanBytes of @ref bytes are an INGRESS-LOAN reserve
     *        (RFC-0028 §6.9, #1626), not payload.
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
 * @brief Bytes a loaned receive block reserves in front of its frame (RFC-0028 §6.9, #1626):
 *        the claim word, then room for a one-link `tr::graph::value_t` header.
 *
 * Sized for the value's header and its one link — 16 + 24 B on a 64-bit host, 12 + 12 B on
 * rv32 — after an aligned claim word. `value.hpp` asserts the fit, so a change to either side
 * fails the build rather than overrunning a frame.
 */
inline constexpr std::size_t kRxLoanBytes = sizeof(void*) >= 8 ? 48 : 32;

/** @brief Offset of the value header inside the reserve: one pointer-aligned word past its
 *         start, which is where the claim word sits. */
inline constexpr std::size_t kRxLoanValueOffset = sizeof(void*);

/**
 * @brief The reserve's claim word: `0` = unclaimed, `1` = a value lives in the reserve.
 *
 * One claim per block, ever. A receive block carries one frame, and a frame stores at most one
 * value out of itself; a second attempt (a second subview of the same frame stored elsewhere)
 * finds the word taken and falls back to allocating its record, exactly as a block with no
 * reserve does.
 */
#ifdef LIBTRACER_NO_ATOMIC
using rx_loan_word_t = std::uint32_t;
#else
using rx_loan_word_t = std::atomic<std::uint32_t>;
#endif

/**
 * @brief Claim @p seg's ingress-loan reserve for one value, once.
 *
 * @retval true  This caller owns the reserve: the value header may be placed at
 *               `bytes.data() + kRxLoanValueOffset`.
 * @retval false No reserve, or it is already claimed.
 */
[[nodiscard]] inline bool claim_rx_loan(segment_t* seg) noexcept {
    if (seg == nullptr || seg->rx_loan == 0) return false;
    auto* const word = reinterpret_cast<rx_loan_word_t*>(seg->bytes.data());
#ifdef LIBTRACER_NO_ATOMIC
    if (*word != 0) return false;
    *word = 1;
    return true;
#else
    std::uint32_t expected = 0;
    return word->compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                         std::memory_order_relaxed);
#endif
}

}  // namespace tr::view

namespace tr::view {

/**
 * @brief The alignment a one-block segment is drawn at for a backend that guarantees
 *        @p align: the stricter of @p align and the header's own (RFC-0028 §4.9).
 */
[[nodiscard]] constexpr std::size_t segment_block_align(std::size_t align) noexcept {
    return align < alignof(segment_t) ? alignof(segment_t) : align;
}

/**
 * @brief Bytes the @ref segment_t header occupies at the head of a one-block segment, padded
 *        so the payload after it starts at `segment_block_align()`.
 */
[[nodiscard]] constexpr std::size_t segment_header_bytes(std::size_t align) noexcept {
    const std::size_t a = segment_block_align(align);
    return (sizeof(segment_t) + a - 1) / a * a;
}

/** @brief The whole block a one-block segment of @p size payload bytes draws at @p align. */
[[nodiscard]] constexpr std::size_t segment_block_bytes(std::size_t size,
                                                        std::size_t align) noexcept {
    return segment_header_bytes(align) + size;
}

/**
 * @brief Place a @ref segment_t over the one block @p block, reclaimed by @p backend: the
 *        header at the head, @p size payload bytes after it (a null, empty span for 0).
 */
[[nodiscard]] inline segment_t* place_segment(mem::mem_backend_t* backend, void* block,
                                              std::size_t size, std::size_t align) noexcept {
    auto* const base = static_cast<std::byte*>(block);
    std::byte* const payload = size != 0 ? base + segment_header_bytes(align) : nullptr;
    return new (base) segment_t(backend, std::span<std::byte>(payload, size));
}

}  // namespace tr::view

namespace tr::mem {

/** @brief The one-block layout (see the declaration in `%backend.hpp`). */
inline view::segment_t* mem_backend_t::alloc_in_block(std::size_t size,
                                                      std::size_t align) noexcept {
    void* const block =
        try_alloc(view::segment_block_bytes(size, align), view::segment_block_align(align));
    return block != nullptr ? view::place_segment(this, block, size, align) : nullptr;
}

/** @brief The mirror of @ref mem_backend_t::alloc_in_block. */
inline void mem_backend_t::destroy_in_block(view::segment_t* seg, std::size_t align) noexcept {
    const std::size_t size = seg->bytes.size();
    seg->~segment_t();
    release(seg, view::segment_block_bytes(size, align), view::segment_block_align(align));
}

/** @brief The default segment: one block through @ref mem_backend_t::try_alloc. */
inline view::segment_t* mem_backend_t::alloc(std::size_t size, alloc_hint_t /*hint*/) {
    return alloc_in_block(size, alignment());
}

/** @brief The default reclaim: one sized release of the block @ref mem_backend_t::alloc drew. */
inline void mem_backend_t::destroy(view::segment_t* seg) noexcept {
    destroy_in_block(seg, alignment());
}

}  // namespace tr::mem
