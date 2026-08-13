/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/rope.hpp"

#include <cstring>

namespace tr::view {

/**
 * @brief The one flatten body, shared by @ref rope_t::flatten and
 *        @ref rope_t::try_flatten (#1250).
 */
view_t rope_t::flatten_core(mem::mem_backend_t& backend, flatten_err_t& err, bool& ok) const {
    ok = false;
    // A DEVICE link is not CPU-addressable, so a host memcpy would fault — refuse
    // to flatten a heterogeneous rope on the CPU (docs/adr/0024). A refusal of the
    // ROPE, distinct from the allocator's below (#917).
    if (!all_host()) {
        err = flatten_err_t::NOT_HOST;
        return view_t{};
    }
    ok = true;
    const std::size_t n = total_length();
    // Zero bytes flatten to the empty view WITHOUT touching the backend: a success,
    // not a refusal (#917). The allocation is skipped so a backend that answers a
    // zero-size request with nullptr — or one already exhausted — cannot turn the
    // degenerate case into a NO_MEMORY the caller would read as backpressure.
    if (n == 0) return view_t{};
    segment_t* seg = backend.alloc(n, mem::alloc_hint_t::NONE);
    if (seg == nullptr) {
        ok = false;
        err = flatten_err_t::NO_MEMORY;
        return view_t{};
    }
    std::size_t pos = 0;
    for (const view_t& l : links()) {
        const auto b = l.bytes();
        if (!b.empty()) std::memcpy(seg->bytes.data() + pos, b.data(), b.size());
        pos += b.size();
    }
    return view_t{segment_ptr_t::adopt(seg), 0, n};
}

/**
 * @brief The lossy convenience form — a refusal of either cause collapses into the
 *        empty view (#917's documented trade), with no `expected` on the path.
 */
view_t rope_t::flatten(mem::mem_backend_t& backend) const {
    flatten_err_t err{};
    bool ok = false;
    // The cause is deliberately discarded here — that IS this overload's contract,
    // and the refusal already returns the empty view this overload owes the caller.
    // The core writes its result straight into THIS function's return slot, so the
    // convenience form costs one call and not one view_t copy (#1250).
    return flatten_core(backend, err, ok);
}

/** @brief The honest form — the refusal cause survives to the caller (#917). */
std::expected<view_t, flatten_err_t> rope_t::try_flatten(mem::mem_backend_t& backend) const {
    flatten_err_t err{};
    bool ok = false;
    view_t flat = flatten_core(backend, err, ok);
    if (!ok) return std::unexpected(err);
    return flat;
}

}  // namespace tr::view
