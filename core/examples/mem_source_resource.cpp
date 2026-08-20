/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the `std::pmr` adapter buys PLACEMENT and BOUNDING, not failability.
 *
 * `tr::mem::source_resource_t` is a `std::pmr::memory_resource` holding one
 * `block_source_t*`: `do_allocate` forwards to `try_alloc`, `do_deallocate` forwards to the
 * seam's SIZED `release` — `std::pmr` carries the original size and alignment into
 * `deallocate`, which is exactly the pair a header-free pool needs — and `do_is_equal` is
 * address identity. It exists for the one case a migration cannot retype away: a `std::pmr`
 * container whose element type is neither trivially copyable nor trivially destructible, so
 * `block_array_t`'s two static assertions reject it (ADR-0079).
 *
 * @warning What it does NOT deliver is the reason the block seam exists. `std::pmr`'s only
 *          exhaustion signal is a throw, so this adapter's boundary is a `std::bad_alloc` on
 *          a hosted build and a `std::abort()` under `-fno-exceptions`. A PEER-PROVOKED store
 *          therefore does not become safe by moving onto a `std::pmr` container over a
 *          bounded slab; it migrates onto `block_array_t` and fails by value (see
 *          mem-block-array). This example never provokes that boundary, because there is
 *          nothing to demonstrate there but the abort.
 *
 * The adapter runs ONE direction. Wrapping a `std::pmr::memory_resource` so it could be used
 * AS a `block_source_t` is not offered and must not be added: `allocate` is annotated
 * `returns_nonnull`, so the wrapper's null check is deleted at exactly the `-Os`/`-Oz` levels
 * the reference node ships at.
 *
 * Runs under ctest as `example_mem_source_resource`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <memory_resource>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/mem_source_pmr.hpp"

namespace {

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    bool ok = true;
    alignas(std::max_align_t) std::array<std::byte, 256> slab{};
    std::array<tr::mem::size_class_t, 4> classes{};
    tr::mem::pool_source_t<> pool{slab, classes};
    tr::mem::source_resource_t adapter{pool};
    check(ok, &adapter.source() == &pool, "the adapter names the source its bytes come from");

    {
        std::pmr::vector<int> samples{&adapter};
        samples.reserve(16);
        for (int i = 0; i < 16; ++i) samples.push_back(i);

        const auto* const bytes = reinterpret_cast<const std::byte*>(samples.data());
        check(ok, bytes >= slab.data() && bytes < slab.data() + slab.size(),
              "PLACEMENT: the container's elements live in the caller's slab, not the heap");
        check(ok, pool.used() == 64, "BOUNDING: 16 ints, 64 bytes, counted against the slab");
        std::printf("std::pmr::vector of %zu ints: %zu of %zu slab bytes\n", samples.size(),
                    pool.used(), slab.size());
    }

    // The container's destructor deallocates with the SAME (bytes, align) it allocated with,
    // which is the seam's sized-reclaim contract — so the pool files the block and reuses it.
    check(ok, pool.used() == 64, "the freed block was filed, not re-carved");
    void* const recycled = adapter.allocate(64, alignof(int));
    check(ok, recycled == static_cast<void*>(slab.data()),
          "and the very block the vector gave back is the next one the adapter hands out");
    adapter.deallocate(recycled, 64, alignof(int));

    // Address identity, not RTTI — the reference node ships -fno-rtti. Two adapters over the
    // SAME source compare UNEQUAL, so build one adapter per source and pass it around.
    tr::mem::source_resource_t second{pool};
    check(ok, !adapter.is_equal(second), "two adapters over one source are not interchangeable");
    check(ok, adapter.is_equal(adapter), "an adapter is equal only to itself");
    return ok ? 0 : 1;
}
