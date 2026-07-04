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

#include <cstdint>
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
 *       through (see mem_borrowed.hpp).
 */
struct segment_t {
    detail::ref_count_t refcount; /**< @brief Intrusive refcount (spec orderings). */
    mem::mem_backend_t* backend;  /**< @brief Reclaimer; non-const (cache hooks mutate it). */
    std::span<std::byte> bytes; /**< @brief The backing bytes this segment holds a reference to. */
    mem::mem_space_t space; /**< @brief Address space (HOST/DEVICE), inherited from @ref backend. */
    mem::backend_tag btag; /**< @brief Module-set tag, inherited from @ref backend (ADR-0047 §2). */

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

}  // namespace tr::view
