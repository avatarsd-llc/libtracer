/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * L1 view: a zero-copy (segment, offset, length) window onto real bytes. A view
 * borrows its segment via segment_ptr_t, so copying a view is a clone (refcount
 * bump) and the bytes stay alive as long as any view references them. The
 * load-bearing claim of L1 (docs/reference/08 §what L1 is): "a TLV is a cast
 * from a view" — the cast itself (view_as_tlv) lives at L2 (frame.hpp /
 * tr::wire), since it produces a tlv_t; L1 must not depend upward on L2.
 */
#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "libtracer/segment.hpp"

/**
 * @file
 * @brief L1 (`tr::view`) zero-copy window `view_t`.
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

    /** @brief The window's bytes as a read-only span (empty if unowned).
     * @warning For a @ref is_device window the span addresses non-CPU memory —
     *          do not dereference it on the CPU (docs/adr/0024).
     */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        if (!owner) return {};
        return std::span<const std::byte>(owner->bytes.data() + offset, length);
    }

    /** @brief True when this window's bytes are CPU-addressable (HOST). Unowned ⇒ host. */
    [[nodiscard]] bool is_host() const noexcept {
        return !owner || owner->space == mem::mem_space_t::HOST;
    }
    /** @brief True when this window's bytes are non-CPU (DEVICE, e.g. GPU memory). */
    [[nodiscard]] bool is_device() const noexcept { return !is_host(); }

    /**
     * @brief A narrower window into the same segment (shares ownership — a
     *        refcount bump, no copy).
     * @note Precondition: `sub_offset + sub_length <= length`.
     */
    [[nodiscard]] view_t subview(std::size_t sub_offset, std::size_t sub_length) const {
        return view_t{owner, offset + sub_offset, sub_length};
    }
};

}  // namespace tr::view
