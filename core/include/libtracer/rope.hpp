/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * L1 rope: an ordered chain of views forming one logical byte sequence that may
 * span multiple segments without copying (docs/reference/08 §ropes). A rope is
 * how one TLV can be assembled from, say, a static header segment + a live DMA
 * payload segment. Assembly is chaining views — never a memcpy. Walk it or
 * scatter-gather it at egress with zero copies; flatten it to one contiguous
 * segment only when a flat-buffer consumer demands it (the single
 * bridge-boundary copy). See docs/adr/0016 §1.
 *
 * A rope_t keeps its first two links in inline small-buffer storage, so the hot
 * path — a single-link value (or a two-link head+payload) — allocates nothing
 * for the chain; only a third link spills the chain to the heap. This is the
 * ADR-0053 §6 trivial-case cost guard: a rope-valued vertex slot costs exactly
 * what a view-valued slot cost, so the contention/latency benches do not move.
 */
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief L1 (`tr::view`) `rope_t`: the transport-agnostic scatter-gather chain.
 */

namespace tr::view {

/**
 * @brief An ordered chain of @ref view_t links — one logical byte sequence
 *        spread across segments, assembled by chaining and never by copying.
 *
 * The rope is the transport-agnostic scatter-gather representation: each
 * transport lowers it to its native DMA (`iovec`/`sendmsg`, CAN descriptors,
 * RDMA verbs) via @ref to_iovec. The single contiguous copy is @ref flatten,
 * taken only at a substrate boundary that cannot scatter-gather.
 */
class rope_t {
   public:
    rope_t() = default;
    /** @brief Implicitly adopt a single view as a one-link rope. */
    rope_t(view_t v) { append(std::move(v)); }

    /** @brief Append a link (chaining — no copy). */
    void append(view_t v) {
        if (!heap_.empty()) {
            heap_.push_back(std::move(v));  // already spilled — stays on the heap chain
            return;
        }
        if (inline_n_ < kInline) {
            inline_[inline_n_++] = std::move(v);  // fits in small-buffer storage — no alloc
            return;
        }
        // The kInline+1-th link spills the whole chain to the heap (the only allocation).
        heap_.reserve(kInline + 1);
        for (std::size_t i = 0; i < inline_n_; ++i) heap_.push_back(std::move(inline_[i]));
        heap_.push_back(std::move(v));
        inline_n_ = 0;
        for (view_t& s : inline_) s = view_t{};  // drop the moved-from links' refcounts eagerly
    }
    /** @brief Chain @p other's links onto this rope (no copy). */
    rope_t& concat(const rope_t& other) {
        for (const view_t& l : other.links()) append(l);
        return *this;
    }

    /**
     * @brief Nothrow-reserve room for @p links more @ref append / @ref concat links —
     *        the soft-fail growth the composed-reply builder needs.
     *
     * The chain's spill to `heap_` is a `std::vector` growth that throws `std::bad_alloc`
     * on OOM, which under `-fno-exceptions` is an `abort()` — a node reboot when a large
     * (e.g. composed-root) reply is assembled on a fragmented heap. A caller that knows
     * its final link count reserves it here up front: on success the next @p links
     * @ref append calls are guaranteed **non-reallocating hence nothrow** (this migrates
     * the inline links into `heap_` so no `append` re-enters the inline→heap spill
     * `reserve`; an empty rope stays inline, but the reserved capacity makes even that
     * one spill `reserve` a no-op). On failure the rope is unchanged and the caller drops
     * the reply (BACKPRESSURE) instead of aborting.
     * @retval false Reservation failed (OOM / impossible count) — the rope is untouched.
     */
    [[nodiscard]] bool try_reserve(std::size_t links) noexcept {
        const std::size_t have = link_count();
        if (links > heap_.max_size() - have) return false;  // impossible count
        if (!tr::detail::try_reserve(heap_, have + links)) return false;
        // Force heap_ mode: migrate the inline links so subsequent append()s take the
        // push_back fast path and never re-enter the inline→heap spill reserve.
        for (std::size_t i = 0; i < inline_n_; ++i) heap_.push_back(std::move(inline_[i]));
        if (inline_n_ > 0) {
            inline_n_ = 0;
            for (view_t& s : inline_) s = view_t{};
        }
        return true;
    }

    /** @brief Number of links in the chain. */
    [[nodiscard]] std::size_t link_count() const noexcept {
        return heap_.empty() ? inline_n_ : heap_.size();
    }
    /** @brief The links, in order (inline small-buffer storage or the spilled chain). */
    [[nodiscard]] std::span<const view_t> links() const noexcept {
        if (heap_.empty()) return std::span<const view_t>(inline_.data(), inline_n_);
        return std::span<const view_t>(heap_);
    }

    /**
     * @brief The single contiguous link — the consumer's explicit "this value is
     *        one segment" (ADR-0053 §6), zero copy.
     * @note Precondition: `link_count() == 1` (debug-asserted). A consumer that
     *       cannot promise contiguity calls @ref materialize instead.
     */
    [[nodiscard]] const view_t& only() const noexcept {
        assert(link_count() == 1);
        return links()[0];
    }

    /**
     * @brief The rope as one contiguous @ref view_t — zero copy when single-link,
     *        one @ref flatten copy otherwise.
     *
     * The visible choice a contiguous-bytes consumer makes (ADR-0053 §6): a
     * single-link rope is returned as its link (a refcount bump, no byte copy); a
     * multi-link rope pays the single flatten copy from @p backend. Distinct from
     * @ref flatten, which always copies — this keeps the trivial case free.
     */
    [[nodiscard]] view_t materialize(mem::mem_backend_t& backend = mem::heap_backend()) const {
        if (link_count() == 1) return links()[0];
        return flatten(backend);
    }

    /** @brief Total logical length across all links. */
    [[nodiscard]] std::size_t total_length() const noexcept {
        std::size_t n = 0;
        for (const view_t& l : links()) n += l.length;
        return n;
    }

    /** @brief True when every link is CPU-addressable (HOST).
     *
     * A `false` rope is **heterogeneous** — it has a DEVICE link (e.g. a GPU
     * payload, docs/adr/0024) that the CPU must not dereference, so host-side
     * operations (`flatten`, CRC) cannot touch it.
     */
    [[nodiscard]] bool all_host() const noexcept {
        for (const view_t& l : links()) {
            if (l.is_device()) return false;
        }
        return true;
    }

    /**
     * @brief The `[off, off + len)` sub-range as its own rope (chaining — no copy).
     *
     * Trims the covering links with @ref view_t::subview, so the result shares
     * (refcounts) exactly the segments its window touches and keeps only those
     * alive — the region primitive of the lazy decode tier (ADR-0053 §1): a
     * child TLV, a routed path suffix, or a payload handed onward is a subrope
     * of the inbound frame, never a copy of it.
     * @note Precondition: `off + len <= total_length()` (debug-asserted via the
     *       subview window invariant; a shorter tail yields a shorter rope).
     */
    [[nodiscard]] rope_t subrope(std::size_t off, std::size_t len) const {
        rope_t out;
        std::size_t skip = off;
        std::size_t remaining = len;
        for (const view_t& l : links()) {
            if (remaining == 0) break;
            if (skip >= l.length) {
                skip -= l.length;
                continue;
            }
            const std::size_t take = std::min(remaining, l.length - skip);
            if (take > 0) out.append(l.subview(skip, take));
            remaining -= take;
            skip = 0;
        }
        return out;
    }

    /** @brief Visit each link's contiguous bytes in order (parsers, serializers, CRC). */
    template <class Fn>
    void walk(Fn&& fn) const {
        for (const view_t& l : links()) fn(l.bytes());
    }

    /**
     * @brief Scatter-gather egress: spans into the original segments (no copy).
     *
     * Hand the result to `writev`/`sendmsg`-style I/O for true zero-copy transmit.
     */
    [[nodiscard]] std::vector<std::span<const std::byte>> to_iovec() const {
        std::vector<std::span<const std::byte>> iov;
        iov.reserve(link_count());
        for (const view_t& l : links()) iov.push_back(l.bytes());
        return iov;
    }

    /**
     * @brief Nothrow @ref to_iovec — fill @p out with one span per link (no copy),
     *        soft-failing instead of aborting when the span table cannot be grown.
     *
     * The `reserve` in @ref to_iovec throws on OOM (an `abort()` under
     * `-fno-exceptions`); the terminus reply egress builds this table per send, so on
     * a fragmented heap that aborted the node. This nothrow-reserves @p out to
     * @ref link_count first and drops the reply on failure. @p out is cleared on entry.
     * @retval false The span table could not be reserved — @p out is left empty.
     */
    [[nodiscard]] bool try_to_iovec(std::vector<std::span<const std::byte>>& out) const noexcept {
        out.clear();
        if (!tr::detail::try_reserve(out, link_count())) return false;
        for (const view_t& l : links()) out.push_back(l.bytes());  // reserved — no reallocation
        return true;
    }

    /**
     * @brief Materialize the rope into one contiguous segment from @p backend (one copy).
     *
     * The single bridge-boundary copy — taken only when a flat-buffer consumer
     * demands it. The flattened view can then be cast with `decode(view_t)`
     * (frame.hpp, `tr::wire`).
     * @retval {} An empty view if the backend cannot allocate, **or if the rope
     *            is not @ref all_host** (a DEVICE link cannot be CPU-memcpy'd —
     *            docs/adr/0024; lower such a payload via its device transport).
     */
    [[nodiscard]] view_t flatten(mem::mem_backend_t& backend = mem::heap_backend()) const;

   private:
    // Inline small-buffer storage for the first two links (the ADR-0053 §6 cost
    // guard). `heap_` is empty iff the chain is inline; the kInline+1-th append
    // spills the whole chain there and `inline_n_` goes to 0 (see @ref append).
    static constexpr std::size_t kInline = 2;
    std::array<view_t, kInline> inline_{};
    std::size_t inline_n_ = 0;
    std::vector<view_t> heap_;
};

/** @brief Concatenate two ropes (chaining — no copy). */
[[nodiscard]] inline rope_t operator+(rope_t lhs, const rope_t& rhs) {
    lhs.concat(rhs);
    return lhs;
}

}  // namespace tr::view
