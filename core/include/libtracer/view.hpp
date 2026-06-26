// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// L1 view: a zero-copy (segment, offset, length) window onto real bytes. A view
// borrows its segment via segment_ptr_t, so copying a view is a clone (refcount
// bump) and the bytes stay alive as long as any view references them. The
// load-bearing claim of L1 (docs/reference/08 §what L1 is): "a TLV is a cast
// from a view" — view_as_tlv runs the M1 decoder over a view's bytes, no copy.
#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <utility>

#include "libtracer/frame.hpp"
#include "libtracer/segment.hpp"

/**
 * @file
 * @brief L1 (`tr::view`) zero-copy window `view_t` and the TLV-as-cast helper.
 */

namespace tr::view {

/**
 * @brief A single contiguous window into one @ref segment_t.
 *
 * Copyable; copy == clone (bumps the segment refcount). The common case is one
 * link; ropes (rope.hpp) chain several.
 * @note Invariant: `offset + length <= owner->bytes.size()`.
 */
struct view_t {
    segment_ptr_t owner;    /**< @brief The segment whose bytes this view borrows. */
    std::size_t offset = 0; /**< @brief Byte offset of the window within the segment. */
    std::size_t length = 0; /**< @brief Window length in bytes. */

    /** @brief A view covering the whole of @p seg. */
    [[nodiscard]] static view_t over(segment_ptr_t seg) noexcept {
        const std::size_t n = seg ? seg->bytes.size() : 0;
        return view_t{std::move(seg), 0, n};
    }

    /** @brief True when the window is empty. */
    [[nodiscard]] bool empty() const noexcept { return length == 0; }

    /** @brief The window's bytes as a read-only span (empty if unowned). */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        if (!owner) return {};
        return std::span<const std::byte>(owner->bytes.data() + offset, length);
    }

    /**
     * @brief A narrower window into the same segment (shares ownership — a
     *        refcount bump, no copy).
     * @note Precondition: `sub_offset + sub_length <= length`.
     */
    [[nodiscard]] view_t subview(std::size_t sub_offset, std::size_t sub_length) const {
        return view_t{owner, offset + sub_offset, sub_length};
    }
};

/**
 * @brief Cast a flat view to a TLV (the M1 decoder, zero-copy).
 *
 * The returned `tlv_t` borrows the view's bytes, so keep the view — and thus its
 * segment — alive while using the `tlv_t`. This is the lifetime guarantee M1 left
 * to the caller.
 */
[[nodiscard]] inline std::expected<tlv_t, error_t> view_as_tlv(const view_t& v) {
    return decode(v.bytes());
}

}  // namespace tr::view
