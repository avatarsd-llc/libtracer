// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// L1 rope: an ordered chain of views forming one logical byte sequence that may
// span multiple segments without copying (docs/reference/08 §ropes). A rope is
// how one TLV can be assembled from, say, a static header segment + a live DMA
// payload segment. Walk it or scatter-gather it at egress with zero copies;
// flatten it to one contiguous segment only when a flat-buffer consumer demands
// it (the single bridge-boundary copy).
//
// A single-link View is the hot path and allocates nothing; only a multi-link
// Rope allocates (one vector for the chain). A bounded/embedded small-buffer
// rope is a follow-on optimization that does not change this API.
#pragma once

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

namespace tracer {

class Rope {
   public:
    Rope() = default;
    Rope(View v) { links_.push_back(std::move(v)); }  // implicit: a view is a 1-link rope

    void append(View v) { links_.push_back(std::move(v)); }
    Rope& concat(const Rope& other) {
        links_.insert(links_.end(), other.links_.begin(), other.links_.end());
        return *this;
    }

    [[nodiscard]] std::size_t link_count() const noexcept { return links_.size(); }
    [[nodiscard]] const std::vector<View>& links() const noexcept { return links_; }

    [[nodiscard]] std::size_t total_length() const noexcept {
        std::size_t n = 0;
        for (const auto& l : links_) n += l.length;
        return n;
    }

    // Visit each link's contiguous bytes in order (parsers, serializers, CRC).
    template <class Fn>
    void walk(Fn&& fn) const {
        for (const auto& l : links_) fn(l.bytes());
    }

    // Scatter-gather egress: spans pointing into the original segments (no copy).
    // Hand the result to writev/sendmsg-style I/O for true zero-copy transmit.
    [[nodiscard]] std::vector<std::span<const std::byte>> to_iovec() const {
        std::vector<std::span<const std::byte>> iov;
        iov.reserve(links_.size());
        for (const auto& l : links_) iov.push_back(l.bytes());
        return iov;
    }

    // Materialize the rope into one contiguous segment from `backend` (one copy).
    // Returns an empty view if the backend cannot allocate. The flattened view
    // can then be cast with view_as_tlv (rope-aware zero-copy decode is M3).
    [[nodiscard]] View flatten(MemBackend& backend = mem::heap_backend()) const;

   private:
    std::vector<View> links_;
};

[[nodiscard]] inline Rope operator+(Rope lhs, const Rope& rhs) {
    lhs.concat(rhs);
    return lhs;
}

}  // namespace tracer
