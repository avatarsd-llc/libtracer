// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// L1 view: a zero-copy (segment, offset, length) window onto real bytes. A view
// borrows its segment via SegmentPtr, so copying a view is a clone (refcount
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

namespace tracer {

// A single contiguous window into one segment. Copyable; copy == clone (bumps
// the segment refcount). The common case is one link; ropes (rope.hpp) chain
// several. Invariant: offset + length <= owner->bytes.size().
struct View {
    SegmentPtr owner;
    std::size_t offset = 0;
    std::size_t length = 0;

    // A view covering the whole segment.
    [[nodiscard]] static View over(SegmentPtr seg) noexcept {
        const std::size_t n = seg ? seg->bytes.size() : 0;
        return View{std::move(seg), 0, n};
    }

    [[nodiscard]] bool empty() const noexcept { return length == 0; }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        if (!owner) return {};
        return std::span<const std::byte>(owner->bytes.data() + offset, length);
    }

    // A narrower window into the same segment (shares ownership — a refcount
    // bump, no copy). Precondition: sub_offset + sub_length <= length.
    [[nodiscard]] View subview(std::size_t sub_offset, std::size_t sub_length) const {
        return View{owner, offset + sub_offset, sub_length};
    }
};

// Cast a flat view to a TLV (the M1 decoder, zero-copy). The returned Tlv
// borrows the view's bytes, so keep the view — and thus its segment — alive
// while using the Tlv. This is the lifetime guarantee M1 left to the caller.
[[nodiscard]] inline std::expected<Tlv, Error> view_as_tlv(const View& v) {
    return decode(v.bytes());
}

}  // namespace tracer
