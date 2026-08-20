/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the pool's classes DO NOT SHARE, and it tells you how to size them.
 *
 * `tr::mem::pool_source_t` recycles through segregated exact-size free lists keyed on the
 * whole `(bytes, align)` pair. A freed 64-byte block therefore cannot serve a 128-byte
 * request, and a block carved for `align=8` is never handed back out for `align=64`. That
 * limitation is the price of the header-free scheme and it is measured, not assumed: replaying
 * 70,937 recorded allocations, this policy needed 26,176 B against a 23,552 B peak-live floor
 * (+11.1 %), where first-fit-with-coalescing needed 27,448 B (+16.5 %) — splitting a remainder
 * under geometric growth rarely produces the size of the next request (ADR-0067).
 *
 * So the class SPAN is a sizing decision, and the source hands the deployer both instruments
 * for making it: `classes_used()` is the number of distinct shapes actually seen, and
 * `overflowed()` counts blocks that could not be filed because the span was too small. A
 * non-zero `overflowed()` never corrupts and never leaks outside the slab — the block simply
 * stays carved — but it does mean the injected span is undersized.
 *
 * Runs under ctest as `example_mem_size_classes`; returns non-zero on any failed check.
 */

#include <array>
#include <cstddef>
#include <cstdint>
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
    std::array<tr::mem::size_class_t, 2> classes{};  // deliberately too few — see below
    tr::mem::pool_source_t<> pool{slab, classes};

    // Size is part of the key: a freed block of one size cannot serve another.
    void* const small = pool.try_alloc(64, 8);
    pool.release(small, 64, 8);
    void* const large = pool.try_alloc(128, 8);
    check(ok, large != nullptr && large != small, "a freed 64 B block does not serve a 128 B ask");
    check(ok, pool.used() == 192, "both shapes are carved — that is the +11.1 % this policy pays");

    // Alignment is part of the key too, for the same reason: a block carved on an 8-byte
    // boundary cannot be promised to an align=64 caller.
    void* const wide = pool.try_alloc(32, 64);
    check(ok, wide != nullptr && reinterpret_cast<std::uintptr_t>(wide) % 64 == 0,
          "an over-aligned request is served, and is actually 64-aligned");
    pool.release(wide, 32, 64);
    void* const narrow = pool.try_alloc(32, 8);
    check(ok, narrow != wide, "the freed align=64 block is a DIFFERENT class, so it is not reused");

    // Two class slots were injected and this run has now seen three distinct shapes, so the
    // third has nowhere to be filed. The block stays carved — bounded and safe — and is counted.
    check(ok, pool.classes_used() == classes.size(), "the class table filled at its injected size");
    pool.release(narrow, 32, 8);
    std::printf("shapes seen: %zu class slots of %zu, %zu block(s) lost to overflow\n",
                pool.classes_used(), classes.size(), pool.overflowed());
    check(ok, pool.overflowed() == 1,
          "a block of an unfiled shape is COUNTED as lost, not written somewhere unsafe");
    check(ok, pool.try_alloc(64, 8) != nullptr,
          "an overflowed table degrades the recycling; it does not break the source");
    return ok ? 0 : 1;
}
