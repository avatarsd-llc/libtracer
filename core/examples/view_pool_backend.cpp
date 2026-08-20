/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a bounded backend: exhaustion is a return value, not an OOM.
 *
 * `tr::mem::pool_t` carves a CALLER-OWNED slab into equal slots, threading the free list
 * through the slab itself — so total memory use is exactly the caller's array, and running
 * out is `nullptr` rather than a throw or an `abort()` (`docs/reference/09-memory-substrate.md`
 * §pressure). That is what makes it the deterministic MCU choice: the node's memory ceiling
 * is a `std::array` a reader can point at.
 *
 * The refusals are two different facts and the API keeps them apart. An oversize request is
 * a PERMANENT no — the slot payload is what it is, `max_segment_size()` says so, and no
 * retry helps. An exhausted pool is TRANSIENT backpressure, which is why
 * `rope_t::try_flatten` answers `flatten_err_t::NO_MEMORY` there: the same rope flattens once
 * a slot comes back (#917). Conflating the two is how a local OOM gets reported to a peer as
 * a malformed frame.
 *
 * Runs under ctest as `example_view_pool_backend`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    bool ok = true;
    alignas(std::max_align_t) std::array<std::byte, 512> slab{};
    tr::mem::pool_t pool{slab, 32};  // 32 usable payload bytes per slot
    std::printf("pool over a %zu-byte stack slab: %zu slots of %zu bytes\n", slab.size(),
                pool.capacity(), pool.max_segment_size());
    check(ok, pool.capacity() > 0 && pool.available() == pool.capacity(), "a fresh pool is empty");

    check(ok, !tr::view::segment_alloc(pool, pool.max_segment_size() + 1),
          "an oversize request is refused permanently — no slot can ever serve it");

    // Drain it. Each handle holds one slot until the LAST reference to it drops.
    std::vector<tr::view::segment_ptr_t> held;
    while (auto seg = tr::view::segment_alloc(pool, 8)) held.push_back(std::move(seg));
    std::printf("drained: %zu segments held, %zu slots free\n", held.size(), pool.available());
    check(ok, held.size() == pool.capacity(), "every slot handed out exactly once");
    check(ok, pool.available() == 0, "and the pool is dry");
    check(ok, !tr::view::segment_alloc(pool, 8), "a dry pool answers nullptr, never an OOM");

    // A multi-link rope needs one fresh segment to flatten into; the dry pool cannot give it.
    tr::view::rope_t rope;
    rope.append(tr::view::view_t::over(held[0]));
    rope.append(tr::view::view_t::over(held[1]));
    const auto refused = rope.try_flatten(pool);
    check(ok, !refused && refused.error() == tr::view::flatten_err_t::NO_MEMORY,
          "try_flatten names its cause: NO_MEMORY is TRANSIENT backpressure");

    held.pop_back();  // give one slot back
    check(ok, pool.available() == 1, "dropping the last reference returns the slot");
    check(ok, rope.try_flatten(pool).has_value(), "and the very same rope now flattens");
    return ok ? 0 : 1;
}
