/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0027 §5 — the ELEMENT CODEC of a packed `PATH` body: the one walk that reads a
 * body whose records are a free mixture of literal segments and path labels, and the
 * one splice that replaces a run of literal segments with a single label element.
 *
 * Car 2 of #1325 settled how ONE label element is spelled (`path_label.hpp`) and how any
 * record is framed and skipped (`packed_path.hpp`). What was still missing is the surface
 * between them: a reader that answers *what is this element* — segment, label, somebody
 * else's escape kind, or a refusal — for every record of a MIXED body, which RFC-0027 §5.2
 * makes the expected shape rather than an edge case.
 *
 * Two clauses of §5 are load-bearing here and are why this is a type rather than a bool:
 *
 * - **An element self-describes by its kind, never by its position** (§5.1, applying
 *   RFC-0024 amendment 2's ruling unchanged). So the walk classifies each record where it
 *   stands and never counts elements to decide what one is.
 * - **A foreign kind and a malformed label are DIFFERENT answers** (§12.5 erratum 1). A
 *   kind this host does not implement is stepped over by its declared length — that is the
 *   property a non-minting hop relies on — while a `kind = 0x16` record whose payload is not
 *   exactly one label is a malformed ADDRESS and the resolver refuses it (`tr::path::invalid`).
 *   A codec that folded the two into one "not a label" answer would let a length-only check
 *   dereference another kind's payload against the local table, which is a mis-delivery.
 *
 * **The structural clauses are exactly TWO, and no third is invented here.** §12.5 erratum 1
 * fixes them as the `kind` and the declared `len`, so a record satisfying both IS a label
 * element — including one carrying the reserved zero generation. That value cannot name a slot
 * any host minted, but the refusal it earns is §7.2's `NOT_FOUND`-class one at the deref, with
 * the sender falling back to the full-string path it still holds; classifying it as a malformed
 * address would answer `tr::path::invalid` where the spec answers `tr::path::not_found`, which
 * is a wire-surface divergence and would need an amendment rather than a codec.
 *
 * **Nothing here mints.** Emitting a label element says how a local part is SPELLED, never
 * that a mint is due: §6.2's trigger, the reply-leg rewrite, the table deref and the
 * `NOT_FOUND` answer are the forwarder's (car 4 of #1325). This header is `tr::wire` (L2/L3)
 * and knows nothing about a graph, a peer or a table — it turns wire bytes into wire values
 * and back, exactly as `path_label.hpp` and `path_ref.hpp` do.
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/packed_path.hpp"
#include "libtracer/path_label.hpp"

/**
 * @file
 * @brief The RFC-0027 §5 mixed-element codec over a packed `PATH` body: classify, walk, splice.
 */

namespace tr::wire {

/**
 * @brief What one record of a packed `PATH` body IS — the four answers a reader needs.
 *
 * There are four and not two because the two refusals differ in what a host does next
 * (RFC-0027 §12.5 erratum 1): a @ref FOREIGN record is relayed intact, a @ref MALFORMED one
 * refuses the address.
 */
enum class path_element_kind_t : std::uint8_t {
    /** @brief A literal segment record `[u8 len][utf8]` — the canonical spelling (RFC-0018). */
    SEGMENT,
    /** @brief RFC-0027's label element — an escape at `kind = 0x16` carrying one valid label. */
    LABEL,
    /** @brief An escape record of some other kind: skippable by length, never interpreted. */
    FOREIGN,
    /** @brief Not a readable element: a ragged record, or a `0x16` record that is not a label. */
    MALFORMED,
};

/**
 * @brief One classified record of a packed `PATH` body, with the bytes it occupies.
 *
 * A value, not a view onto a cursor: the walk is a `p += bytes` step over a body the caller
 * already holds, so the element carries its own offset and width and the caller may keep, skip
 * or copy it without asking the cursor anything.
 */
struct path_element_t {
    /** @brief Which of the four answers this record is. */
    path_element_kind_t kind = path_element_kind_t::MALFORMED;
    /** @brief Byte offset of the record's first byte within the body. */
    std::size_t at = 0;
    /** @brief Total bytes the record occupies, or `0` when the framing itself is ragged. */
    std::size_t bytes = 0;
    /** @brief SEGMENT: the segment's UTF-8 bytes. FOREIGN: the escape's declared payload.
     *         Empty for every other kind — a label's value is in @ref label, decoded. */
    std::span<const std::byte> payload{};
    /** @brief The escape `kind` byte, for @ref LABEL and @ref FOREIGN; `0` otherwise. */
    std::uint8_t escape_kind = 0;
    /** @brief The decoded label, meaningful only when @ref kind is @ref LABEL — and then still
     *         possibly not `valid()`, when the record carries the reserved zero generation. */
    path_label_t label{};

    /** @brief True unless this record refuses the address (@ref MALFORMED). */
    [[nodiscard]] constexpr bool ok() const noexcept {
        return kind != path_element_kind_t::MALFORMED;
    }

    /** @brief A @ref SEGMENT's payload as text (empty for every other kind). */
    [[nodiscard]] std::string_view text() const noexcept {
        return kind == path_element_kind_t::SEGMENT
                   ? std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size())
                   : std::string_view{};
    }
};

/**
 * @brief Classify the record at @p at of packed `PATH` body @p body (RFC-0027 §5.1).
 *
 * The kind decides the reading, in this order and no other: ragged framing refuses first,
 * a non-escape record is a literal segment, an escape of a kind this host does not own is
 * @ref path_element_kind_t::FOREIGN, and a `kind = 0x16` record whose payload is exactly four
 * bytes reads as @ref path_element_kind_t::LABEL. Those are §12.5 erratum 1's two structural
 * clauses and there is no third.
 *
 * @note A `LABEL` element may carry a path label that is not `valid()` — the reserved zero
 *       generation. That is deliberate and it is where this differs from car 2's
 *       `path_label_at`, which answers `nullopt` for it: a single-element reader has one
 *       answer to give, while a body reader must not turn a value-level refusal into a
 *       structural one. The reserved generation names no slot any host minted, so the deref
 *       refuses it with §7.2's `NOT_FOUND`-class answer and the sender falls back to strings —
 *       the same recovery as any stale or unknown path label, and one this codec must not
 *       pre-empt with `tr::path::invalid`.
 */
[[nodiscard]] constexpr path_element_t path_element_at(std::span<const std::byte> body,
                                                       std::size_t at) noexcept {
    const std::size_t span = packed_record_span(body, at);
    if (span == 0) return path_element_t{.kind = path_element_kind_t::MALFORMED, .at = at};
    if (!packed_record_is_escape(body, at))
        return path_element_t{.kind = path_element_kind_t::SEGMENT,
                              .at = at,
                              .bytes = span,
                              .payload = body.subspan(at + 1, span - 1)};

    const std::uint8_t kind = static_cast<std::uint8_t>(body[at + 1]);
    const std::span<const std::byte> payload =
        body.subspan(at + kPackedEscapeOverhead, span - kPackedEscapeOverhead);
    if (kind != kPackedEscapeKindLabel)
        return path_element_t{.kind = path_element_kind_t::FOREIGN,
                              .at = at,
                              .bytes = span,
                              .payload = payload,
                              .escape_kind = kind};

    if (!path_label_record_valid(kind, payload.size()))
        return path_element_t{
            .kind = path_element_kind_t::MALFORMED, .at = at, .bytes = span, .escape_kind = kind};
    // Structurally a label, whatever the VALUE is: the reserved zero generation is refused at
    // the deref with a NOT_FOUND-class answer (§7.2), never as a malformed address.
    const path_label_t label = path_label_load(payload);
    return path_element_t{.kind = path_element_kind_t::LABEL,
                          .at = at,
                          .bytes = span,
                          .payload = payload,
                          .escape_kind = kind,
                          .label = label};
}

/**
 * @brief A forward walk over the elements of a packed `PATH` body.
 *
 * The whole walk is `at += element.bytes` over @ref path_element_at, which is the property
 * RFC-0018's encoding exists for. It ends on a @ref path_element_kind_t::MALFORMED record
 * rather than trying to resynchronize: a malformed record means the ADDRESS is refused, and a
 * cursor that walked past one would be inventing the rest of somebody's route.
 */
class path_element_cursor_t {
   public:
    /** @brief Walk @p body from its first record. */
    explicit constexpr path_element_cursor_t(std::span<const std::byte> body) noexcept
        : body_(body) {}

    /** @brief True once every record has been returned (or the walk stopped on a refusal). */
    [[nodiscard]] constexpr bool done() const noexcept { return at_ >= body_.size(); }

    /** @brief Byte offset the next record starts at. */
    [[nodiscard]] constexpr std::size_t offset() const noexcept { return at_; }

    /**
     * @brief The next element, or `nullopt` at the end of the body.
     *
     * A @ref path_element_kind_t::MALFORMED element is returned once and then ends the walk,
     * so a caller sees the refusal exactly one time and cannot step past it.
     */
    [[nodiscard]] constexpr std::optional<path_element_t> next() noexcept {
        if (done()) return std::nullopt;
        const path_element_t el = path_element_at(body_, at_);
        at_ = el.ok() ? at_ + el.bytes : body_.size();
        return el;
    }

   private:
    std::span<const std::byte> body_{}; /**< @brief The packed body being walked. */
    std::size_t at_ = 0;                /**< @brief Offset of the next record. */
};

/**
 * @brief What a whole packed `PATH` body is made of — one walk, four counts.
 *
 * The counts are what a caller decides with: `labels != 0` is "this body is a frame path and
 * not a key" (`packed_path_valid_key` is the rule's other side), and `!well_formed` is the
 * `tr::path::invalid` refusal.
 */
struct path_element_census_t {
    /** @brief True iff every record framed cleanly and none refused the address. */
    bool well_formed = false;
    /** @brief Records walked, all kinds together. */
    std::size_t elements = 0;
    /** @brief Literal segment records. */
    std::size_t segments = 0;
    /** @brief RFC-0027 label elements. */
    std::size_t labels = 0;
    /** @brief Escape records of a kind this host does not own. */
    std::size_t foreign = 0;
    /** @brief Value equality over every count. */
    [[nodiscard]] friend constexpr bool operator==(const path_element_census_t&,
                                                   const path_element_census_t&) noexcept = default;
};

/**
 * @brief Walk @p body once and count what it holds (@ref path_element_census_t).
 *
 * An EMPTY body is well-formed with zero elements — it is the graph root, exactly as
 * `packed_path_valid_key` reads it.
 */
[[nodiscard]] constexpr path_element_census_t path_element_census(
    std::span<const std::byte> body) noexcept {
    path_element_census_t c{.well_formed = true};
    path_element_cursor_t cur(body);
    while (const std::optional<path_element_t> el = cur.next()) {
        ++c.elements;
        switch (el->kind) {
            case path_element_kind_t::SEGMENT:
                ++c.segments;
                break;
            case path_element_kind_t::LABEL:
                ++c.labels;
                break;
            case path_element_kind_t::FOREIGN:
                ++c.foreign;
                break;
            case path_element_kind_t::MALFORMED:
                c.well_formed = false;
                break;
        }
    }
    return c;
}

/**
 * @brief Append @p body to @p out with @p count elements from index @p first replaced by one
 *        label element — RFC-0027 amendment 6's multi-segment splice.
 *
 * One label stands for a hop's whole LOCAL PART: its entire mount run, however many segments
 * that is (§5.3.3). The record is the same 7 bytes whether the run it replaces is one segment
 * or five, which is the arithmetic §3.3 assumes and the reason a per-segment label was refused.
 *
 * **This is the spelling, not the mint.** Which run belongs to which hop, and whether a mint is
 * due at all, is the forwarder's (§6.2) — nothing here consults a table or a peer.
 *
 * @return False, appending NOTHING, when the splice has no reading: an unmintable @p label
 *         (the reserved zero generation), an empty or out-of-range run, a body that does not
 *         walk cleanly, or a run holding anything but literal segments. That last refusal is
 *         §5.3.3 with §6.1: what a label stands for is a hop's local part, and a hop's local
 *         part is the mount RUN it strips — a run of names. An element that is already a label
 *         or a foreign escape is not part of any run this hop resolved, so a splice over it
 *         would not be the rewrite §6.1 describes.
 *
 * @warning @p out MUST NOT alias @p body's storage. The appends may reallocate, which would
 *          dangle the span mid-splice. Car 4's reply-leg rewrite is the in-place-shaped caller
 *          and is exactly where that would be reached for, so it is stated here rather than
 *          discovered there: build into a fresh buffer and swap.
 */
[[nodiscard]] inline bool emit_path_labelled(std::vector<std::byte>& out,
                                             std::span<const std::byte> body, std::size_t first,
                                             std::size_t count, path_label_t label) {
    if (!label.valid() || count == 0) return false;
    // The run bound, checked BEFORE any `first + count` is formed. An element is at least two
    // bytes, so the element count never exceeds the body's byte count and this both rejects
    // every out-of-range run and makes the additions below unable to wrap. Left unguarded,
    // `{first = SIZE_MAX, count = 1}` wraps to an empty in-run window and splices a label into
    // a body it was never meant to touch — a well-formed spelling of a DIFFERENT address, which
    // is the mis-delivery class this design closes by construction everywhere else.
    if (count > body.size() || first > body.size() - count) return false;

    // Locate the run by walking, so the refusal happens before a single byte is appended.
    std::size_t index = 0;
    std::size_t run_begin = body.size();
    std::size_t run_end = body.size();
    path_element_cursor_t cur(body);
    while (const std::optional<path_element_t> el = cur.next()) {
        if (!el->ok()) return false;
        const bool in_run = index >= first && index < first + count;
        if (in_run && el->kind != path_element_kind_t::SEGMENT) return false;
        if (index == first) run_begin = el->at;
        if (index == first + count - 1) run_end = el->at + el->bytes;
        ++index;
    }
    if (first + count > index) return false;

    out.insert(out.end(), body.begin(), body.begin() + static_cast<std::ptrdiff_t>(run_begin));
    const bool spelled = emit_path_label(out, label);  // cannot fail: `label.valid()` above
    assert(spelled);
    (void)spelled;
    out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(run_end), body.end());
    return true;
}

}  // namespace tr::wire
