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
 * A single-link view_t is the hot path and allocates nothing; only a multi-link
 * rope_t allocates (one vector for the chain). A bounded/embedded small-buffer
 * rope is a follow-on optimization that does not change this API.
 */
#pragma once

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
    rope_t(view_t v) { links_.push_back(std::move(v)); }

    /** @brief Append a link (chaining — no copy). */
    void append(view_t v) { links_.push_back(std::move(v)); }
    /** @brief Chain @p other's links onto this rope (no copy). */
    rope_t& concat(const rope_t& other) {
        links_.insert(links_.end(), other.links_.begin(), other.links_.end());
        return *this;
    }

    /** @brief Number of links in the chain. */
    [[nodiscard]] std::size_t link_count() const noexcept { return links_.size(); }
    /** @brief The links, in order. */
    [[nodiscard]] const std::vector<view_t>& links() const noexcept { return links_; }

    /** @brief Total logical length across all links. */
    [[nodiscard]] std::size_t total_length() const noexcept {
        std::size_t n = 0;
        for (const auto& l : links_) n += l.length;
        return n;
    }

    /** @brief True when every link is CPU-addressable (HOST).
     *
     * A `false` rope is **heterogeneous** — it has a DEVICE link (e.g. a GPU
     * payload, docs/adr/0024) that the CPU must not dereference, so host-side
     * operations (`flatten`, CRC) cannot touch it.
     */
    [[nodiscard]] bool all_host() const noexcept {
        for (const auto& l : links_) {
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
        for (const auto& l : links_) {
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
        for (const auto& l : links_) fn(l.bytes());
    }

    /**
     * @brief Scatter-gather egress: spans into the original segments (no copy).
     *
     * Hand the result to `writev`/`sendmsg`-style I/O for true zero-copy transmit.
     */
    [[nodiscard]] std::vector<std::span<const std::byte>> to_iovec() const {
        std::vector<std::span<const std::byte>> iov;
        iov.reserve(links_.size());
        for (const auto& l : links_) iov.push_back(l.bytes());
        return iov;
    }

    /**
     * @brief Materialize the rope into one contiguous segment from @p backend (one copy).
     *
     * The single bridge-boundary copy — taken only when a flat-buffer consumer
     * demands it. The flattened view can then be cast with `view_as_tlv`
     * (frame.hpp, `tr::wire`).
     * @retval {} An empty view if the backend cannot allocate, **or if the rope
     *            is not @ref all_host** (a DEVICE link cannot be CPU-memcpy'd —
     *            docs/adr/0024; lower such a payload via its device transport).
     */
    [[nodiscard]] view_t flatten(mem::mem_backend_t& backend = mem::heap_backend()) const;

   private:
    std::vector<view_t> links_;
};

/** @brief Concatenate two ropes (chaining — no copy). */
[[nodiscard]] inline rope_t operator+(rope_t lhs, const rope_t& rhs) {
    lhs.concat(rhs);
    return lhs;
}

}  // namespace tr::view
