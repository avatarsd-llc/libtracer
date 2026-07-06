/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The rope-source cursor + rope validator (ADR-0048 §1): the link-walking twin of
 * grammar::span_cursor. It satisfies the same structural cursor concept the one
 * grammar core (grammar.hpp) reads through, so the identical header/trailer rules
 * validate a frame delivered as a scatter-gather rope (CAN reassembly, WS
 * fragments) WITHOUT first flattening it — a header or trailer that straddles a
 * link boundary is stitched a byte at a time (they are small and bounded), and a
 * payload the CRC must cover is fed link-by-link (grammar's incremental CRC).
 *
 * This is a SEPARATE translation unit from grammar.hpp so a span-only target (an
 * MCU that never links a rope-delivering transport) never instantiates the rope
 * cursor (ADR-0048 §1, the ADR-0016/0047 inside-a-module templating rule).
 *
 * SINK NOTE: this validates STRUCTURE + CRC over a rope; it does not yet
 * materialize a rope frame into a tlv_t / arena node, because both sink node
 * types hold a borrowed contiguous std::span that cannot name a straddling
 * payload (ADR-0041 §2). Producing sink nodes from a rope is the ratification-
 * gated follow-on (the rope-aware-decode sink-type proposal).
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "libtracer/error.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief The rope byte-source cursor + `wire::validate_rope` (ADR-0048 §1).
 */

namespace tr::wire::grammar {

/**
 * @brief The rope byte-source cursor — the link-walking twin of `span_cursor`.
 *
 * Reads the grammar's bytes across an ordered chain of @ref view::view_t links so
 * the same `parse_header` rules serve a scatter-gather frame. A window
 * `[base, end)` into the rope; @ref region narrows it in O(1) (offsets only, never
 * copying links) to descend into a structured node's children region — the rope
 * analogue of the span cursor's `subspan`.
 *
 * @warning Reads dereference link bytes on the CPU, so every link must be HOST
 *          (@ref view::rope_t::all_host). `validate_rope` enforces this.
 */
class rope_cursor {
   public:
    /** @brief A cursor over the whole of rope @p r. */
    explicit rope_cursor(const view::rope_t& r) noexcept
        : links_(r.links()), end_(r.total_length()) {}

    /** @brief Number of bytes available from this cursor's origin. */
    [[nodiscard]] std::size_t size() const noexcept { return end_ - base_; }

    /**
     * @brief A sub-cursor over the `[off, off + len)` window of this cursor.
     *
     * O(1) — adjusts the absolute base/end only, sharing the same link chain.
     * Used to descend into a node's children region exactly as `decode_into`
     * `subspan`s the payload.
     */
    [[nodiscard]] rope_cursor region(std::size_t off, std::size_t len) const noexcept {
        rope_cursor c = *this;
        c.base_ += off;
        c.end_ = c.base_ + len;
        return c;
    }

    /** @brief The unsigned byte at offset @p off (walks to its link). */
    [[nodiscard]] std::uint8_t byte_at(std::size_t off) const noexcept {
        const auto [li, intra] = locate(base_ + off);
        return std::to_integer<std::uint8_t>(links_[li].bytes()[intra]);
    }

    /** @brief Load @p n little-endian bytes at @p off as a u64 (stitched across links). */
    [[nodiscard]] std::uint64_t load_le(std::size_t off, std::size_t n) const noexcept {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            v |= static_cast<std::uint64_t>(byte_at(off + i)) << (8u * i);
        }
        return v;
    }

    /**
     * @brief Visit the @p n bytes at @p off as contiguous per-link sub-spans, in order.
     *
     * The CRC-feed seam: a range wholly inside one link yields a single span; a
     * straddling range yields one span per link it crosses, so the grammar's
     * incremental CRC crosses a link boundary with no concatenation buffer.
     */
    template <class Fn>
    void for_each_span(std::size_t off, std::size_t n, Fn&& fn) const {
        std::size_t remaining = n;
        auto [li, intra] = locate(base_ + off);
        while (remaining > 0 && li < links_.size()) {
            const std::span<const std::byte> lb = links_[li].bytes();
            const std::size_t take = std::min(remaining, lb.size() - intra);
            if (take > 0) {
                fn(lb.subspan(intra, take));
                remaining -= take;
            }
            ++li;
            intra = 0;
        }
    }

   private:
    // Absolute offset -> (link index, intra-link offset). A linear scan: the reads
    // that use it (header/trailer stitching) are small and bounded; bulk payload
    // goes through for_each_span, which scans once then walks forward.
    [[nodiscard]] std::pair<std::size_t, std::size_t> locate(std::size_t a) const noexcept {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < links_.size(); ++i) {
            const std::size_t len = links_[i].length;
            if (a < acc + len) return {i, a - acc};
            acc += len;
        }
        return {links_.empty() ? 0 : links_.size() - 1, 0};  // at/after end (never deref'd)
    }

    std::span<const view::view_t> links_;
    std::size_t base_ = 0;
    std::size_t end_ = 0;
};

}  // namespace tr::wire::grammar

namespace tr::wire {

/**
 * @brief Validate one whole TLV frame delivered as a scatter-gather rope (ADR-0048 §1).
 *
 * Applies the exact `grammar::parse_header` grammar — bounds, `type == 0x00`
 * reject, reserved-bit reject, `LL` width, trailer sizing, the two-region CRC, the
 * `kMaxDepth` cap, and trailing-bytes reject — over the rope's links WITHOUT
 * flattening, so CAN/WS ingress can reject a malformed reassembled frame before
 * paying the single flatten copy. Iterative (no recursion), mirroring
 * `decode_into`'s walk.
 *
 * @param r The reassembled frame. Every link MUST be HOST (@ref
 *          view::rope_t::all_host) — a device link cannot be CPU-read to verify a
 *          CRC and is rejected with `FRAME_INVALID`.
 * @return `{}` when @p r is exactly one valid frame; otherwise the `err_t` the
 *         grammar rejects with — identical to `decode(flatten(r))`'s error for the
 *         same bytes.
 */
[[nodiscard]] std::expected<void, err_t> validate_rope(const view::rope_t& r);

}  // namespace tr::wire
