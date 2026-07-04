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
 * Key invariant (why byte-prefix ⇒ descendant): two *valid* keys can share a byte
 * prefix only where it lands on a NAME-segment boundary — a differing length
 * header would break the byte match one record earlier — so a strict byte-prefix
 * of a valid key is exactly a strict ancestor of it.
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

    /** @brief The underlying key bytes. */
    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept { return key_; }
    /** @brief True at the root (no NAME segments). */
    [[nodiscard]] constexpr bool empty() const noexcept { return key_.empty(); }

    /**
     * @brief The last NAME segment's payload — the vertex's own name; empty at the
     *        root. Walks records to the end; stops early on a malformed length.
     */
    [[nodiscard]] std::span<const std::byte> last_segment() const noexcept {
        std::span<const std::byte> last;
        std::size_t i = 0;
        while (i + 4 <= key_.size()) {
            const std::size_t len = detail::load_le<std::uint16_t>(key_.subspan(i + 2, 2));
            if (i + 4 + len > key_.size()) break;
            last = key_.subspan(i + 4, len);
            i += 4 + len;
        }
        return last;
    }

    /**
     * @brief The parent key: this key with its last NAME encoding dropped (empty
     *        at the root). The ADR-0020 inheritance walk derives ancestor keys by
     *        iterating this — the key is the concatenated NAME encodings, so no
     *        string form is needed.
     */
    [[nodiscard]] key_view_t parent() const noexcept {
        std::size_t last_start = 0;
        std::size_t i = 0;
        while (i + 4 <= key_.size()) {
            const std::size_t len = detail::load_le<std::uint16_t>(key_.subspan(i + 2, 2));
            if (i + 4 + len > key_.size()) break;
            last_start = i;
            i += 4 + len;
        }
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
     *        deeper descendant (more than one further record) yields @c nullopt.
     */
    [[nodiscard]] std::optional<std::span<const std::byte>> child_record_under(
        key_view_t parent) const noexcept {
        if (!parent.is_ancestor_of(*this)) return std::nullopt;
        const std::span<const std::byte> rest = key_.subspan(parent.key_.size());
        if (rest.size() <= 4) return std::nullopt;  // no room for one payload-bearing record
        if (static_cast<type_t>(std::to_integer<std::uint8_t>(rest[0])) != type_t::NAME)
            return std::nullopt;
        const std::size_t len = detail::load_le<std::uint16_t>(rest.subspan(2, 2));
        if (rest.size() != 4 + len) return std::nullopt;  // deeper descendant, not a child
        return rest;
    }

    /**
     * @brief Append each ancestor-prefix level to @p out, shallowest-first (the
     *        last element equals the whole key) — the `mkdir -p` creation order.
     * @return false, appending nothing, if the NAME framing is ragged (records do
     *         not tile the key exactly) or the key is empty; true otherwise.
     */
    [[nodiscard]] bool split_levels(std::vector<key_view_t>& out) const {
        const std::size_t start = out.size();
        std::size_t i = 0;
        while (i + 4 <= key_.size()) {
            const std::size_t len = detail::load_le<std::uint16_t>(key_.subspan(i + 2, 2));
            if (i + 4 + len > key_.size()) break;
            i += 4 + len;
            out.push_back(key_view_t{key_.first(i)});
        }
        if (i != key_.size() || out.size() == start) {
            out.resize(start);
            return false;
        }
        return true;
    }

   private:
    std::span<const std::byte> key_;
};

}  // namespace tr::wire
