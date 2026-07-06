/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/rope.hpp"

#include <cstring>

namespace tr::view {

view_t rope_t::flatten(mem::mem_backend_t& backend) const {
    // A DEVICE link is not CPU-addressable, so a host memcpy would fault — refuse
    // to flatten a heterogeneous rope on the CPU (docs/adr/0024).
    if (!all_host()) return view_t{};
    const std::size_t n = total_length();
    segment_t* seg = backend.alloc(n, mem::alloc_hint_t::NONE);
    if (seg == nullptr) return view_t{};
    std::size_t pos = 0;
    for (const view_t& l : links()) {
        const auto b = l.bytes();
        if (!b.empty()) std::memcpy(seg->bytes.data() + pos, b.data(), b.size());
        pos += b.size();
    }
    return view_t{segment_ptr_t::adopt(seg), 0, n};
}

}  // namespace tr::view
