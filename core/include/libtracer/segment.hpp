// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// L0 segment: a refcounted span of real bytes owned by a MemBackend, plus the
// intrusive SegmentPtr handle that threads a segment's lifetime through view
// fan-out. The refcount uses the canonical intrusive_ptr orderings required by
// docs/reference/02-graph-model.md §required atomic operations (increment =
// relaxed, decrement = acq_rel, inspect = acquire). Define LIBTRACER_NO_ATOMIC
// for single-threaded / Cortex-M0 builds (no cross-thread segment sharing).
#pragma once

#include <cstdint>
#include <span>
#include <utility>

#include "libtracer/backend.hpp"

#ifndef LIBTRACER_NO_ATOMIC
#include <atomic>
#endif

namespace tracer {

namespace detail {

// Intrusive refcount with the spec's orderings. dec_acq_rel returns the value
// *before* the decrement, so a return of 1 means "this caller dropped the last
// reference" — the canonical Boost intrusive_ptr release test.
class RefCount {
   public:
    explicit RefCount(std::uint_least32_t initial) noexcept : count_(initial) {}

    RefCount(const RefCount&) = delete;
    RefCount& operator=(const RefCount&) = delete;

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

// A segment: real bytes + the backend that reclaims them + an intrusive
// refcount. Never copied or moved (the atomic refcount pins it in place); always
// handled through SegmentPtr. `bytes` is writable at the type level, but whether
// writes are *legal* is the backend's contract — a const/ROM borrow must not be
// written through (see mem_borrowed.hpp).
struct Segment {
    detail::RefCount refcount;
    MemBackend* backend;  // non-const: alloc/destroy/cache-hooks mutate backend state
    std::span<std::byte> bytes;

    Segment(MemBackend* b, std::span<std::byte> by, std::uint_least32_t initial = 1) noexcept
        : refcount(initial), backend(b), bytes(by) {}

    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;
};

// Intrusive owning handle. Copy = clone (refcount bump, relaxed); destruction =
// release (acq_rel); the backend's destroy fires when the last handle drops.
// This is what makes M1's borrowed (zero-copy) Tlv safe to hold: the spans in a
// decoded Tlv stay valid as long as the View — and thus this handle — lives.
class SegmentPtr {
   public:
    SegmentPtr() noexcept = default;

    // Adopt an existing reference (e.g. from MemBackend::alloc, which yields a
    // segment with refcount = 1) WITHOUT bumping the count.
    [[nodiscard]] static SegmentPtr adopt(Segment* seg) noexcept { return SegmentPtr(seg, false); }
    // Take a new shared reference to an already-live segment (bumps the count).
    [[nodiscard]] static SegmentPtr retain(Segment* seg) noexcept { return SegmentPtr(seg, true); }

    SegmentPtr(const SegmentPtr& other) noexcept : seg_(other.seg_) {
        if (seg_) seg_->refcount.inc_relaxed();
    }
    SegmentPtr(SegmentPtr&& other) noexcept : seg_(other.seg_) { other.seg_ = nullptr; }
    // Copy-and-swap: one operator covers both copy- and move-assignment.
    SegmentPtr& operator=(SegmentPtr other) noexcept {
        std::swap(seg_, other.seg_);
        return *this;
    }
    ~SegmentPtr() { reset(); }

    void reset() noexcept {
        if (seg_ && seg_->refcount.dec_acq_rel() == 1) {
            seg_->backend->destroy(seg_);
        }
        seg_ = nullptr;
    }

    [[nodiscard]] Segment* get() const noexcept { return seg_; }
    [[nodiscard]] Segment& operator*() const noexcept { return *seg_; }
    [[nodiscard]] Segment* operator->() const noexcept { return seg_; }
    [[nodiscard]] explicit operator bool() const noexcept { return seg_ != nullptr; }

    // Debug / metrics only (acquire load); NOT a synchronization primitive.
    [[nodiscard]] std::uint_least32_t use_count() const noexcept {
        return seg_ ? seg_->refcount.load_acquire() : 0;
    }

   private:
    SegmentPtr(Segment* seg, bool do_retain) noexcept : seg_(seg) {
        if (seg_ && do_retain) seg_->refcount.inc_relaxed();
    }
    Segment* seg_ = nullptr;
};

}  // namespace tracer
