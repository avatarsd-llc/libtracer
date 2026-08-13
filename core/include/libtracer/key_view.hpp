/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * key_view — navigation over a canonical PATH-payload key. A vertex-map key is
 * the concatenated NAME-TLV encodings of its path (docs/reference/02 §dispatch
 * key; ADR-0020): each segment is a NAME TLV — a 4-byte header (type=NAME, opt=0,
 * u16 length) followed by the segment's payload bytes. The key IS these bytes, so
 * every ancestor/descendant/child relation is a byte operation and no string form
 * is ever materialised. This module is the single locus for that walking, which
 * the L4 graph previously open-coded across ~six sites (graph.cpp).
 *
 * The framing decode itself lives in ONE function — `key_view_t::record_end` — and
 * every walk here is a loop over it. It is public because the walk was ALSO
 * hand-rolled outside this header, at graph.cpp's Composite `segment_end`, at the
 * router's `subscribe_toward` mount descent, and at transport_vertex.cpp (a copy of
 * `last_segment`): a navigation module that keeps its own primitive private is a
 * module callers route around (#888).
 *
 * Key invariant (why byte-prefix ⇒ descendant): two *valid* keys can share a byte
 * prefix only where it lands on a NAME-segment boundary — a differing length
 * header would break the byte match one record earlier — so a strict byte-prefix
 * of a valid key is exactly a strict ancestor of it.
 *
 * Zero-length records (#932, decided once): an empty-payload NAME segment is
 * ILLEGAL — path syntax rejects empty segments (docs/reference/03-addressing.md:
 * `/sensor//temp` is invalid, not a spelling of `/sensor/temp`), and `path.hpp`
 * refuses one at parse time — so EVERY walker here treats a `len == 0` record as
 * malformed framing: `record_end` reports it as ragged (the single locus means
 * the rule is enforced ONCE and every walk inherits it), so the record walks
 * stop at it exactly as they stop at a ragged length, and `split_levels` fails.
 * Before the rule, `is_ancestor_of`/`split_levels` called an empty-name segment
 * a valid level that `child_record_under` denied was a child.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv.hpp"

/**
 * @file
 * @brief L2/L3 (`tr::wire`) canonical-key navigation `key_view_t`.
 */

namespace tr::wire {

/**
 * @brief A read-only view over a canonical PATH-payload key (concatenated NAME
 *        TLVs), with the ancestor / descendant / segment navigation the graph
 *        dispatch and ACL-inheritance walks need. Cheap to copy (wraps a span);
 *        borrows the key bytes, which must outlive it.
 */
class key_view_t {
   public:
    /** @brief An empty key view (the root). */
    constexpr key_view_t() noexcept = default;
    /** @brief View over the canonical key bytes @p key (concatenated NAME TLVs). */
    constexpr explicit key_view_t(std::span<const std::byte> key) noexcept : key_(key) {}

    /**
     * @brief One well-framed NAME record of a key: where it sits, and what it carries.
     *
     * `begin` is the offset of its 4-byte header, `end` one past its last payload byte —
     * so `end` is also where the NEXT record starts, and the whole record's encoding is
     * `bytes().subspan(begin, end - begin)`.
     */
    struct record_t {
        std::size_t begin = 0;              /**< Offset of the record's 4-byte header. */
        std::size_t end = 0;                /**< One past the record's last payload byte. */
        std::span<const std::byte> payload; /**< The NAME segment's payload bytes. */
    };

    /**
     * @brief One past the last byte of the well-framed NAME record starting at byte offset
     *        @p at — THE single locus of the 4-byte-header / u16-length framing decode this
     *        whole module is about (#888), and so also where the NEXT record starts.
     * @return 0 when the bytes at @p at are ragged: no room for a header, a length that
     *         runs past the key's end, or an illegal zero-length payload (see the file
     *         header — an empty NAME segment is malformed framing, decided once, here).
     *         A well-framed record ends at @p at + 5 or later, so 0 is unambiguous.
     *         Every walk here stops there — a malformed tail is never half-decoded.
     *
     * A bare offset, not a @ref record_t, because this is what the per-NAME-record loops of
     * the Composite descent (`graph_t::find_ptr`) want, and they want it INLINE: returning
     * the fuller record grew the decode past the inliner's budget and put a call in that
     * loop, which is a vertex-resolution cost this refactor must not introduce.
     *
     * The record's TYPE byte is NOT checked (a key is NAME records by construction, and the
     * hand-rolled walks this replaces did not check it either); @ref child_record_under is
     * the one caller that additionally demands `type_t::NAME`.
     */
    [[nodiscard]] std::size_t record_end(std::size_t at) const noexcept {
        if (at + 4 > key_.size()) return 0;
        const std::size_t len = detail::load_le<std::uint16_t>(key_.subspan(at + 2, 2));
        return len == 0 || at + 4 + len > key_.size() ? 0 : at + 4 + len;
    }

    /**
     * @brief The whole well-framed NAME record starting at byte offset @p at — @ref
     *        record_end with the bounds and the payload span worked out.
     * @retval std::nullopt Exactly when @ref record_end reports ragged.
     */
    [[nodiscard]] std::optional<record_t> record_from(std::size_t at) const noexcept {
        const std::size_t e = record_end(at);
        if (e == 0) return std::nullopt;
        return record_t{.begin = at, .end = e, .payload = payload_in(at, e)};
    }

    /**
     * @brief A resumable INDEXED walk over a key's NAME records — `at(i)` without the
     *        rescan-from-zero an indexed accessor would otherwise pay per call.
     *
     * Non-allocating: its whole state is the borrowed span, two offsets and one record. It
     * exists for the strip-K mount descent (`child_registry_t::longest_prefix` and
     * `fwd_router_t::subscribe_toward`), which asks for segment `i` by index and mostly
     * ascends. An ask BEHIND the walk restarts from the first record — the same answer, at
     * the cost of the rescan a forward ask avoids.
     */
    class record_cursor_t {
       public:
        /** @brief A cursor over @p key, positioned before its first record. */
        explicit record_cursor_t(key_view_t key) noexcept : key_(key.bytes()) {}

        /**
         * @brief The @p i-th (0-based) well-framed NAME record.
         * @retval std::nullopt The key has no @p i-th record — it ended, or a record at or
         *                      before @p i is ragged.
         */
        [[nodiscard]] std::optional<record_t> at(std::size_t i) noexcept {
            if (walked_ > 0 && i == walked_ - 1) return last_;  // the record just handed out
            if (i < walked_) {
                walked_ = 0;
                pos_ = 0;
            }
            while (walked_ <= i) {
                const std::optional<record_t> r = key_view_t{key_}.record_from(pos_);
                if (!r) return std::nullopt;
                last_ = *r;
                pos_ = r->end;
                ++walked_;
            }
            return last_;
        }

        /**
         * @brief Where the run of the first @p n records ENDS: 0 for @p n == 0, the key's
         *        size when fewer than @p n records are well-framed.
         *
         * The offset a strip-K descent slices its residual at — `key.subspan(end_of(k))` is
         * everything below the mount, and `end_of(k) >= size` is "the address named the
         * mount exactly, with nothing below it".
         */
        [[nodiscard]] std::size_t end_of(std::size_t n) noexcept {
            if (n == 0) return 0;
            const std::optional<record_t> r = at(n - 1);
            return r ? r->end : key_.size();
        }

       private:
        std::span<const std::byte> key_;
        record_t last_;
        std::size_t pos_ = 0;    /**< Byte offset of the next unwalked record. */
        std::size_t walked_ = 0; /**< How many records are behind `pos_`. */
    };

    /** @brief The underlying key bytes. */
    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept { return key_; }
    /** @brief True at the root (no NAME segments). */
    [[nodiscard]] constexpr bool empty() const noexcept { return key_.empty(); }

    /**
     * @brief The last NAME segment's payload — the vertex's own name; empty at the
     *        root. Walks records to the end; stops early on a malformed length (a
     *        ragged record, or an illegal zero-length one — see the file header).
     */
    [[nodiscard]] std::span<const std::byte> last_segment() const noexcept {
        std::span<const std::byte> last;
        for (std::size_t at = 0, e = record_end(0); e != 0; at = e, e = record_end(e))
            last = payload_in(at, e);
        return last;
    }

    /**
     * @brief The parent key: this key with its last NAME encoding dropped (empty
     *        at the root). The ADR-0020 inheritance walk derives ancestor keys by
     *        iterating this — the key is the concatenated NAME encodings, so no
     *        string form is needed. Stops early on malformed framing, as
     *        @ref last_segment does.
     */
    [[nodiscard]] key_view_t parent() const noexcept {
        std::size_t last_start = 0;
        for (std::size_t at = 0, e = record_end(0); e != 0; at = e, e = record_end(e))
            last_start = at;
        return key_view_t{key_.first(last_start)};
    }

    /**
     * @brief True iff this key is a strict ancestor of @p other — a
     *        segment-boundary byte-prefix of it (so @p other is a descendant).
     */
    [[nodiscard]] bool is_ancestor_of(key_view_t other) const noexcept {
        return other.key_.size() > key_.size() &&
               std::equal(key_.begin(), key_.end(), other.key_.begin());
    }

    /**
     * @brief If this key is a *direct* child of @p parent — exactly one more
     *        well-framed NAME record beyond it — return that trailing record (the
     *        child's own canonical NAME encoding); otherwise @c std::nullopt. A
     *        deeper descendant (more than one further record) yields @c nullopt,
     *        as does an illegal zero-length record (see the file header) — the
     *        `rest.size() <= 4` guard below.
     */
    [[nodiscard]] std::optional<std::span<const std::byte>> child_record_under(
        key_view_t parent) const noexcept {
        if (!parent.is_ancestor_of(*this)) return std::nullopt;
        const std::span<const std::byte> rest = key_.subspan(parent.key_.size());
        if (rest.size() <= 4) return std::nullopt;  // no room for one payload-bearing record
        if (static_cast<type_t>(std::to_integer<std::uint8_t>(rest[0])) != type_t::NAME)
            return std::nullopt;
        // Ragged reports 0, which no `rest.size() > 4` can equal, so one compare covers both
        // "malformed" and "deeper descendant".
        if (key_view_t{rest}.record_end(0) != rest.size()) return std::nullopt;
        return rest;
    }

    /**
     * @brief Append each ancestor-prefix level to @p out, shallowest-first (the
     *        last element equals the whole key) — the `mkdir -p` creation order.
     * @return false, appending nothing, if the NAME framing is ragged (records do
     *         not tile the key exactly), any record carries an illegal zero-length
     *         payload (see the file header), or the key is empty; true otherwise.
     */
    [[nodiscard]] bool split_levels(std::vector<key_view_t>& out) const {
        const std::size_t start = out.size();
        std::size_t i = 0;
        for (std::size_t e = record_end(0); e != 0; e = record_end(e)) {
            i = e;
            out.push_back(key_view_t{key_.first(i)});
        }
        if (i != key_.size() || out.size() == start) {
            out.resize(start);
            return false;
        }
        return true;
    }

   private:
    /**
     * @brief The payload of the record spanning `[at, end)` — its 4-byte header stripped.
     *
     * Within this module the header WIDTH is written exactly twice — here and in
     * @ref record_end — and every walk, here and at the call sites this header serves, is
     * expressed in those two.
     */
    [[nodiscard]] std::span<const std::byte> payload_in(std::size_t at,
                                                        std::size_t end) const noexcept {
        return key_.subspan(at + 4, end - at - 4);
    }

    std::span<const std::byte> key_;
};

}  // namespace tr::wire
