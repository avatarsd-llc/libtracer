/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/rope.hpp"

#include <cstring>

namespace tr::view {

std::expected<view_t, flatten_err_t> rope_t::try_flatten(mem::mem_backend_t& backend) const {
    // A DEVICE link is not CPU-addressable, so a host memcpy would fault — refuse
    // to flatten a heterogeneous rope on the CPU (docs/adr/0024). A refusal of the
    // ROPE, distinct from the allocator's below (#917).
    if (!all_host()) return std::unexpected(flatten_err_t::NOT_HOST);
    const std::size_t n = total_length();
    // Zero bytes flatten to the empty view WITHOUT touching the backend: a success,
    // not a refusal (#917). The allocation is skipped so a backend that answers a
    // zero-size request with nullptr — or one already exhausted — cannot turn the
    // degenerate case into a NO_MEMORY the caller would read as backpressure.
    if (n == 0) return view_t{};
    segment_t* seg = backend.alloc(n, mem::alloc_hint_t::NONE);
    if (seg == nullptr) return std::unexpected(flatten_err_t::NO_MEMORY);
    std::size_t pos = 0;
    for (const view_t& l : links()) {
        const auto b = l.bytes();
        if (!b.empty()) std::memcpy(seg->bytes.data() + pos, b.data(), b.size());
        pos += b.size();
    }
    return view_t{segment_ptr_t::adopt(seg), 0, n};
}

}  // namespace tr::view
