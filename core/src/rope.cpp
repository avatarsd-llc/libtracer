// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/rope.hpp"

#include <cstring>

namespace tracer {

View Rope::flatten(MemBackend& backend) const {
    const std::size_t n = total_length();
    Segment* seg = backend.alloc(n, 0);
    if (seg == nullptr) return View{};
    std::size_t pos = 0;
    for (const auto& l : links_) {
        const auto b = l.bytes();
        if (!b.empty()) std::memcpy(seg->bytes.data() + pos, b.data(), b.size());
        pos += b.size();
    }
    return View{SegmentPtr::adopt(seg), 0, n};
}

}  // namespace tracer
