/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The lazy rope-backed decode view (ADR-0053 §1): what a rope-delivered frame
 * BECOMES on the decode side. A tlv_view_t is one TLV whose bytes live in a
 * rope — it holds the parsed header facts plus a refcounted subrope of the
 * frame, and NOTHING that is not accessed is ever decoded: children are
 * materialized one header at a time (children_t::next), a payload handed
 * onward stays the subrope it already is, and materialize() -> tlv_t is the
 * single explicit copy point.
 *
 * Validation is fully lazy (ADR-0053 §4): over() anchors the bounds (root
 * header + exact-total check) with the CRC walk DEFERRED, child headers are
 * grammar-checked as they are stepped over (containment comes from the bounds
 * anchor + per-header total checks), and integrity (the per-TLV CRC trailer)
 * is checked by whichever consumer accesses a TLV — verify(). An endpoint
 * whose members form one transaction runs verify-all-then-apply before
 * mutating state (ADR-0053 §4, torn-application note).
 *
 * Like rope_decode.hpp this is its own translation unit: a span-only MCU
 * target never instantiates the lazy tier (ADR-0048 §1 / ADR-0047 rule).
 */
#pragma once

#include <cstddef>
#include <expected>
#include <optional>

#include "libtracer/error.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/rope_decode.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief The lazy rope-backed decode view `tr::wire::tlv_view_t` (ADR-0053).
 */

namespace tr::wire {

/**
 * @brief One TLV whose bytes live in a rope — the lazy decode-side node (ADR-0053 §1).
 *
 * Holds the (CRC-deferred) parsed header plus a refcounted subrope of the
 * inbound frame; copying a `tlv_view_t` bumps segment refcounts, never bytes.
 * The view — and any child view or subrope taken from it — keeps exactly its
 * own links' segments alive (@ref view::rope_t::subrope) and may outlive the
 * transport read loop: this is the owning delivery tier, the scoped revision
 * of the ADR-0041 §2 span-arena contract (which itself is untouched).
 *
 * `tlv_t` remains the eager encode-side / materialized representation;
 * @ref materialize is the one explicit copy from this tier into it.
 */
class tlv_view_t {
   public:
    /**
     * @brief The ingress bounds anchor (ADR-0053 §4): adopt @p frame as one lazy TLV.
     *
     * Parses the root header with the CRC walk **deferred** and requires
     * `total == frame.total_length()` — a handful of byte reads plus an
     * O(links) size sum, no payload walk, no copy. With the root bounds
     * anchored, every later child materialization is containment-checked
     * against its parent's region, so lazy access is memory-safe without
     * ever touching bytes nobody reads.
     *
     * @param frame The reassembled inbound frame (the rope is adopted —
     *              refcounted links, no copy). Every link must be HOST
     *              (@ref view::rope_t::all_host): rejected `FRAME_INVALID`
     *              otherwise, exactly as @ref validate_rope.
     * @return The root view, or the grammar's `err_t` (`FRAME_TRUNCATED` /
     *         `FRAME_INVALID` — including trailing bytes).
     */
    [[nodiscard]] static std::expected<tlv_view_t, err_t> over(view::rope_t frame);

    /** @brief The TLV type code. */
    [[nodiscard]] type_t type() const noexcept { return hdr_.type; }
    /** @brief The decoded `opt` bits. */
    [[nodiscard]] opt_t opt() const noexcept { return hdr_.opt; }
    /** @brief True for a structured TLV (`opt.PL` — body = children region). */
    [[nodiscard]] bool structured() const noexcept { return hdr_.opt.pl; }
    /** @brief Body (payload / children region) length in bytes. */
    [[nodiscard]] std::size_t body_size() const noexcept { return hdr_.length; }

    /**
     * @brief This TLV's full wire bytes (header + body + trailer) as a rope.
     *
     * The forwarding primitive: a hop that routed on the PATH prefix hands
     * this (or a @ref body subrope) to the next transport as-is — zero decode,
     * zero copy, the links refcount-alive across the hop (ADR-0053 §3).
     */
    [[nodiscard]] const view::rope_t& wire() const noexcept { return wire_; }

    /**
     * @brief The body region as a refcounted subrope (payload of an opaque
     *        TLV; the packed children region of a structured one).
     *
     * Never validated, never copied — a structured payload delivered to a
     * consumer that does not descend stays exactly these bytes ("up to the
     * consumer to deal with it", ADR-0053 §1).
     */
    [[nodiscard]] view::rope_t body() const { return wire_.subrope(hdr_.header, hdr_.length); }

    /**
     * @brief Lazy forward iteration over a structured TLV's children (ADR-0053 §1).
     *
     * Each @ref next parses exactly ONE child header (CRC deferred, so a
     * skipped sibling's payload is never walked) and yields that child as its
     * own `tlv_view_t` subrope. A grammar error in a child surfaces here — to
     * whoever is iterating — and only for the child it belongs to; siblings
     * already yielded are unaffected (partial consumption, ADR-0053 §4).
     */
    class children_t {
       public:
        /**
         * @brief The next child, `std::nullopt` when the region is exhausted,
         *        or the child's grammar `err_t`.
         *
         * After an error the iterator is poisoned (further calls return the
         * same error): child boundaries beyond a malformed header are
         * unknowable.
         */
        [[nodiscard]] std::expected<std::optional<tlv_view_t>, err_t> next();

        /**
         * @brief True when the region is cleanly consumed (@ref next would
         *        yield `std::nullopt`; `false` while poisoned).
         */
        [[nodiscard]] bool exhausted() const noexcept {
            return !poisoned_ && pos_ == body_.total_length();
        }

       private:
        friend class tlv_view_t;
        explicit children_t(view::rope_t body) : body_(std::move(body)) {}
        view::rope_t body_;
        std::size_t pos_ = 0;
        std::optional<err_t> poisoned_{};
    };

    /** @brief Begin lazy child iteration (an empty range for an opaque TLV). */
    [[nodiscard]] children_t children() const {
        return children_t(structured() ? body() : view::rope_t{});
    }

    /**
     * @brief Check THIS TLV's integrity — its CRC trailer — now (ADR-0053 §4).
     *
     * The access-time integrity point: walks this TLV's body ++ timestamp
     * bytes link-by-link (no copy) against the stored trailer. A TLV with no
     * CRC trailer verifies trivially. Covers nested children byte-for-byte
     * (they are the body), so an endpoint applying a multi-member write as
     * one transaction calls this once — verify-all-then-apply.
     */
    [[nodiscard]] std::expected<void, err_t> verify() const;

    /**
     * @brief The decoded timestamp trailer, if `opt.TS` (a bounded stitched read).
     */
    [[nodiscard]] std::optional<timestamp_t> timestamp() const;

    /**
     * @brief A materialized view: the flat copy plus the eager tree borrowing it.
     *
     * `root` borrows `flat`'s segment bytes (stable across moves — the segment
     * is refcounted heap memory), so keep the pair together, exactly like
     * @ref view_as_tlv's "keep the view alive" contract.
     */
    struct materialized_t {
        view::view_t flat; /**< @brief The single contiguous copy of the wire bytes. */
        tlv_t root;        /**< @brief The eager tree; borrows @ref flat's bytes. */
    };

    /**
     * @brief The single explicit copy point (ADR-0053 §1): flatten + eager decode.
     *
     * Everything lazy access deferred is paid here, once, by the consumer that
     * asked for it: one contiguous copy, the full grammar walk INCLUDING every
     * CRC trailer, and the @ref kMaxDepth cap — byte-identical to
     * `decode(flatten(wire()))`.
     *
     * @param backend Where the flat segment is allocated.
     * @return The flat copy + eager tree, or the grammar's `err_t`
     *         (`FRAME_INVALID` when @p backend cannot allocate the segment).
     */
    [[nodiscard]] std::expected<materialized_t, err_t> materialize(
        mem::mem_backend_t& backend = mem::heap_backend()) const;

   private:
    tlv_view_t(view::rope_t wire, grammar::header_t hdr) : wire_(std::move(wire)), hdr_(hdr) {}

    view::rope_t wire_;      // this TLV's full bytes: refcounted links, never copies
    grammar::header_t hdr_;  // parsed with crc_check_t::DEFER
};

}  // namespace tr::wire
