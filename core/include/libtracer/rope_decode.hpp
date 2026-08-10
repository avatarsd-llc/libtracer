/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The rope-source cursor + rope validator (ADR-0048 §1): the link-walking twin of
 * grammar::span_cursor. It satisfies the same structural cursor concept the one
 * grammar core (`%grammar.hpp`) reads through, so the identical header/trailer rules
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
#include <cassert>
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
 * the same `parse_header` rules serve a scatter-gather frame. A window of @ref size
 * bytes ANCHORED at a `(link index, intra-link offset)` origin; @ref region narrows
 * it to descend into a structured node's children region — the rope analogue of the
 * span cursor's `subspan`.
 *
 * ## Why the origin is a link anchor and not an absolute offset (#917)
 *
 * The cursor used to hold an absolute `[base, end)` and resolve every read with a
 * `locate()` that scanned the chain **from link 0**. `region` was then O(1) but every
 * read through the narrowed cursor re-paid the walk to its origin, so `grammar::walk`
 * — which regions once per child — cost Θ(children × links), and `load_le` paid one
 * full scan PER BYTE (a timestamp's 8-byte tail load: eight chain scans).
 *
 * Anchoring the origin instead moves that walk to @ref region, where it advances
 * FORWARD from the parent's own anchor: the whole child sweep costs O(links) once,
 * not O(links) per child, and a read at a small offset (the header bytes every
 * `parse_header` starts with) resolves in about one step. Nothing else changes —
 * the cursor stays trivially copyable, as the walk stack's relocation memcpy needs.
 *
 * @warning Reads dereference link bytes on the CPU, so every link must be HOST
 *          (@ref view::rope_t::all_host). `validate_rope` enforces this.
 */
class rope_cursor {
   public:
    /** @brief An empty cursor (zero bytes) — the walk-stack inline-slot default. */
    rope_cursor() noexcept = default;

    /** @brief A cursor over the whole of rope @p r. */
    explicit rope_cursor(const view::rope_t& r) noexcept
        : links_(r.links()), size_(r.total_length()) {}

    /**
     * @brief A cursor over @p len bytes of @p links starting at link @p li, byte
     *        @p intra — the RESUMABLE form.
     *
     * Lets a caller that already walked to a position (the lazy child iterator,
     * `%tlv_view.hpp`) re-enter the chain there instead of from link 0, which is the
     * same Θ(children × links) the anchored origin removes inside `grammar::walk`.
     * @note Precondition: the anchor names a byte the chain holds (or its exact end,
     *       for an empty window) and @p len bytes follow it.
     */
    [[nodiscard]] static rope_cursor at(std::span<const view::view_t> links, std::size_t li,
                                        std::size_t intra, std::size_t len) noexcept {
        rope_cursor c;
        c.links_ = links;
        c.li_ = li;
        c.intra_ = intra;
        c.size_ = len;
        return c;
    }

    /** @brief Number of bytes available from this cursor's origin. */
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /** @brief This cursor's origin as a `(link index, intra-link offset)` anchor. */
    [[nodiscard]] std::pair<std::size_t, std::size_t> anchor() const noexcept {
        return {li_, intra_};
    }

    /**
     * @brief A sub-cursor over the `[off, off + len)` window of this cursor.
     *
     * O(links crossed by @p off) — walks the anchor forward from THIS cursor's origin
     * (never from link 0) and re-windows, sharing the same link chain and copying no
     * link. Used to descend into a node's children region exactly as `decode_into`
     * `subspan`s the payload.
     * @note Precondition: `off + len <= size()` (debug-asserted) — the same
     *       containment contract `span_cursor::region` gets for free from
     *       `std::span::subspan`, and that @ref view::view_t::subview asserts.
     */
    [[nodiscard]] rope_cursor region(std::size_t off, std::size_t len) const noexcept {
        // Precondition, enforced in debug builds (zero release cost; fuzz + sanitizer CI
        // catches a violation): the sub-window must lie within this cursor's window. An
        // unclamped region would otherwise let a cursor claim bytes that do not exist.
        assert(off + len <= size());
        rope_cursor c = *this;
        c.size_ = len;
        // Fast path — the sub-window opens inside the SAME link. This is what a header
        // region is on any rope whose links are larger than a TLV header, so it must not
        // pay a loop: the old absolute-offset cursor made `region` two adds, and charging
        // even a short walk to every descent cost the 2-link hop ~3% before this branch
        // existed. Not merely an optimisation of the loop below — it is the case the loop
        // is amortised over.
        if (c.li_ < c.links_.size() && off < c.links_[c.li_].length - c.intra_) {
            c.intra_ += off;
            return c;
        }
        // Advancing the anchor is the only walk the whole descent pays. `off` may land
        // exactly on the chain's end (an empty region at the tail), which leaves
        // `li_ == links_.size()` — a legal empty cursor, not a violation.
        c.intra_ += off;
        while (c.li_ < c.links_.size() && c.intra_ >= c.links_[c.li_].length) {
            c.intra_ -= c.links_[c.li_].length;
            ++c.li_;
        }
        return c;
    }

    /**
     * @brief The unsigned byte at offset @p off (walks to its link).
     * @note Precondition: `off < size()` (debug-asserted). The grammar callers
     *       bounds-check before every read; this makes that contract visible and
     *       a violation loud instead of a silent wrong byte.
     */
    [[nodiscard]] std::uint8_t byte_at(std::size_t off) const noexcept {
        // Precondition, enforced in debug builds (zero release cost; fuzz + sanitizer CI
        // catches a violation): the byte must lie inside this cursor's window.
        assert(off < size());
        const auto [li, intra] = seek(off);
        return std::to_integer<std::uint8_t>(links_[li].bytes()[intra]);
    }

    /**
     * @brief Load @p n little-endian bytes at @p off as a u64 (stitched across links).
     *
     * ONE `seek` then a forward walk — not @p n seeks. The trailer loads
     * (`parse_header`'s length field, a CRC value, an 8-byte timestamp) are the
     * cursor's densest reads, and paying the chain walk per byte made a straddling
     * timestamp cost eight of them.
     */
    [[nodiscard]] std::uint64_t load_le(std::size_t off, std::size_t n) const noexcept {
        // Same window containment @ref byte_at asserted once per byte.
        assert(n == 0 || off + n <= size());
        if (n == 0) return 0;
        std::uint64_t v = 0;
        unsigned shift = 0;
        std::size_t remaining = n;
        auto [li, intra] = seek(off);
        while (remaining > 0 && li < links_.size()) {
            const std::span<const std::byte> lb = links_[li].bytes();
            const std::size_t take = std::min(remaining, lb.size() - intra);
            for (std::size_t k = 0; k < take; ++k) {
                v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(lb[intra + k]))
                     << shift;
                shift += 8u;
            }
            remaining -= take;
            ++li;
            intra = 0;
        }
        return v;
    }

    /**
     * @brief Visit the @p n bytes at @p off as contiguous per-link sub-spans, in order.
     *
     * The CRC-feed seam: a range wholly inside one link yields a single span; a
     * straddling range yields one span per link it crosses, so the grammar's
     * incremental CRC crosses a link boundary with no concatenation buffer.
     * @note Precondition: `off + n <= size()` (debug-asserted) — the same window
     *       containment @ref region and @ref byte_at carry, and that the sibling
     *       `span_cursor::for_each_span` gets for free from `std::span::subspan`.
     *       The walk's own guards are chain-end and `seek`'s past-chain assert,
     *       neither of which sees a feed that overshoots a NARROWED window while
     *       staying inside the chain — without this the caller would be handed
     *       real-but-wrong bytes and told it succeeded.
     * @warning Unlike @ref byte_at there is NO release backstop: `byte_at`'s
     *          out-of-window read degrades to an out-of-range subscript (`seek`
     *          returns the one-past-the-end index) that a sanitizer reports, but
     *          an overshooting feed reads bytes the chain genuinely holds, so a
     *          release (NDEBUG) build walks past the window silently and no
     *          sanitizer can see it. In release this contract is the CALLER's to
     *          keep; the assert closes the defect class for debug and CI only.
     */
    template <class Fn>
    void for_each_span(std::size_t off, std::size_t n, Fn&& fn) const {
        // Precondition, enforced in debug builds (zero release cost): the whole feed must
        // lie inside this cursor's window. Unlike byte_at, a violation is NOT sanitizer-
        // visible in a release build — the overshot bytes really are in the chain, just
        // outside the window — so this assert is the only guard there is.
        assert(off + n <= size());
        // An empty feed names no byte, so it must not `seek` one: the grammar's CRC
        // feed calls this with n == 0 for an absent payload or trailer, at an offset
        // that is legitimately the end of the window.
        if (n == 0) return;
        std::size_t remaining = n;
        auto [li, intra] = seek(off);
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
    // Window offset -> (link index, intra-link offset), walking FORWARD from this
    // cursor's own anchor. The reads that use it (header/trailer stitching) sit at
    // small offsets, so this is about one step; bulk payload goes through
    // for_each_span, which seeks once then walks forward.
    [[nodiscard]] std::pair<std::size_t, std::size_t> seek(std::size_t off) const noexcept {
        std::size_t li = li_;
        std::size_t intra = intra_ + off;
        while (li < links_.size() && intra >= links_[li].length) {
            intra -= links_[li].length;
            ++li;
        }
        // Precondition, enforced in debug builds (zero release cost; fuzz + sanitizer CI
        // catches a violation): `off` must name a byte the chain actually holds. The walk
        // above leaves `li == links_.size()` on a violation, which makes it an out-of-range
        // subscript in byte_at — the form the sanitizers DO see, rather than a real-but-
        // wrong byte they cannot — while for_each_span's `li < links_.size()` guard still
        // stops cleanly on it.
        assert(li < links_.size() && "rope_cursor: offset is past the end of the link chain");
        return {li, intra};
    }

    std::span<const view::view_t> links_;
    std::size_t li_ = 0;     // link holding the cursor's first byte
    std::size_t intra_ = 0;  // offset of that byte within link li_
    std::size_t size_ = 0;   // bytes available from the anchor
};

}  // namespace tr::wire::grammar

namespace tr::wire {

/**
 * @brief The cheap INGRESS check (CONTEXT.md §Validation timing): top-level
 *        header + total-size anchor + the whole-frame trailer CRC, one linear scan.
 *
 * Everything ingress is allowed to verify, and nothing more: `parse_header` on
 * the root (bounds, `type == 0x00` reject, reserved-bit reject, `LL` width,
 * trailer sizing, and — when `opt.CR` is set — the trailer CRC, a LINEAR
 * link-by-link scan that never needs the tree) plus the `total == size` anchor.
 * No descent: a malformed child TLV surfaces its error where that level is
 * CONSUMED (per-TLV verify-at-access, ADR-0053), never at ingress. The strict
 * whole-tree walk remains available as the opt-in `validate_rope`.
 *
 * @param r The reassembled frame. Every link MUST be HOST (@ref
 *          view::rope_t::all_host) — a device link cannot be CPU-read to verify a
 *          CRC and is rejected with `FRAME_INVALID`.
 * @return `{}` when the root header, size anchor and (if present) root CRC hold;
 *         otherwise the `err_t` the grammar rejects with.
 */
[[nodiscard]] std::expected<void, err_t> check_frame(const view::rope_t& r);

/**
 * @brief STRICTLY validate one whole TLV frame delivered as a scatter-gather rope
 *        (ADR-0048 §1) — the opt-in eager whole-tree walk.
 *
 * Applies the exact `grammar::parse_header` grammar — bounds, `type == 0x00`
 * reject, reserved-bit reject, `LL` width, trailer sizing, the two-region CRC,
 * and trailing-bytes reject — over EVERY level of the rope's links WITHOUT
 * flattening. Iterative (no recursion) via the one `grammar::walk`; nesting
 * depth is bounded by this host validator's heap-spilled walk stack (RFC-0006 —
 * no depth constant). NOT an ingress step (ingress is `check_frame`): this is
 * the strict mode a verify-all-then-apply consumer or a differential test opts
 * into (ADR-0053 §4).
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
