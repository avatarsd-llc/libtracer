/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a LONG-LIVED bounded seam has to recycle, not just to bound.
 *
 * `heap_source_t` recycles but is unbounded; `bump_source_t` is bounded but never recycles.
 * A seam that outlives one unit of work — a graph's control source, a receiver's decode
 * arena — needs both, and that is `tr::mem::pool_source_t`: segregated free lists over a
 * slab the CALLER owns, with no per-block header (the seam's sized `release` makes one
 * unnecessary).
 *
 * The property to watch is `used()`. It is the slab high-water mark, not a running total:
 * once every LIVE block has been carved once, a release/alloc round costs no new bytes at
 * all. Ten thousand rounds over a 512-byte slab therefore settle where two rounds do — which
 * is exactly what the bump source in the same position cannot do (see mem-bump-source).
 *
 * Both bounds are injected, deliberately: the caller supplies the slab AND the span of
 * `size_class_t` slots, so neither the byte ceiling nor the class count is a constant inside
 * the library (ADR-0067).
 *
 * Runs under ctest as `example_mem_pool_source`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdio>

#include "libtracer/mem_source.hpp"

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
    std::array<tr::mem::size_class_t, 4> classes{};
    tr::mem::pool_source_t<> pool{slab, classes};

    // The long-lived shape: two blocks live at a time, ten thousand times over.
    bool served_every_round = true;
    for (int round = 0; round < 10000; ++round) {
        void* const a = pool.try_alloc(64, 8);
        void* const b = pool.try_alloc(64, 8);
        if (a == nullptr || b == nullptr) {
            served_every_round = false;
            break;
        }
        pool.release(a, 64, 8);
        pool.release(b, 64, 8);
    }
    check(ok, served_every_round, "10,000 alloc/release rounds on a 512-byte slab all served");
    check(ok, pool.used() == 128,
          "used() settled at the 2-block high-water — a bump source would have wanted 1.28 MB");
    std::printf("512-byte slab, 10,000 rounds: %zu bytes carved, %zu class slot(s) in use\n",
                pool.used(), pool.classes_used());

    // Recycling is LIFO on the exact shape, and the freed block is the one that comes back.
    void* const first = pool.try_alloc(64, 8);
    pool.release(first, 64, 8);
    check(ok, pool.try_alloc(64, 8) == first, "a released block is the next one handed out");
    check(ok, pool.used() == 128, "and reuse carves nothing new");

    // Bounded is still bounded: the slab is the ceiling, and reaching it is a value.
    int more = 0;
    while (pool.try_alloc(64, 8) != nullptr) ++more;
    check(ok, more == 7,
          "seven more came out — with the one in hand, exactly 8 × 64 B in 512: no header");
    check(ok, pool.try_alloc(64, 8) == nullptr,
          "a full slab answers nullptr, never the platform heap and never an abort()");
    return ok ? 0 : 1;
}
