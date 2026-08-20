/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the UPSTREAM decides whether a buffer is a hint or the bound.
 *
 * `tr::mem::bump_source_t` takes a second argument nobody has to pass, and it is the one
 * that changes what the source MEANS. Once the caller's buffer cannot fit a request, the
 * bump draws from its upstream instead:
 *
 * - the default upstream is `heap_source()`, so the buffer is a fast path and an oversize
 *   request quietly succeeds off the process heap;
 * - `null_source()` serves nothing, so the buffer IS the ceiling and an oversize request is
 *   refused by value.
 *
 * That choice is why the bump is a capability-PRESERVING substitution for
 * `std::pmr::monotonic_buffer_resource`, which also spills — but spills to a THROWING
 * resource, which under `-fno-exceptions` is the `abort()` this seam exists to remove. A
 * bounded node picks `null_source()`; `wire::decode_into` over a stack slab is the shipped
 * example (see wire-arena-decode).
 *
 * Runs under ctest as `example_mem_bump_upstream`; returns non-zero on any failed check.
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
    constexpr std::size_t kOversize = 512;  // larger than either buffer below

    // Same buffer size, same request, two upstreams — and two different node behaviours.
    alignas(std::max_align_t) std::array<std::byte, 128> soft_buf{};
    alignas(std::max_align_t) std::array<std::byte, 128> hard_buf{};
    tr::mem::bump_source_t soft{soft_buf};                          // upstream: heap_source()
    tr::mem::bump_source_t hard{hard_buf, tr::mem::null_source()};  // upstream: nothing

    void* const from_buffer = hard.try_alloc(64, 8);
    check(ok, from_buffer == hard_buf.data(), "what FITS is served from the buffer either way");
    check(ok, hard.used() == 64, "and only that is counted against the buffer");

    void* const spilled = soft.try_alloc(kOversize, 8);
    check(ok, spilled != nullptr, "with a heap upstream, an oversize request still succeeds");
    check(ok, soft.used() == 0,
          "but used() stays 0 — the block came from the UPSTREAM, not the caller's buffer");
    std::printf("default upstream: %zu-byte request served, %zu buffer bytes used\n", kOversize,
                soft.used());

    // Releasing a spilled block routes back to the upstream (a bump block's release is the
    // no-op; this one is not), so the composition leaks nothing.
    soft.release(spilled, kOversize, 8);

    check(ok, hard.try_alloc(kOversize, 8) == nullptr,
          "with null_source() upstream the buffer is the HARD bound — the answer is nullptr");
    check(ok, hard.used() == 64, "a refusal carves nothing; the earlier block is untouched");
    check(ok, hard.try_alloc(32, 8) != nullptr,
          "and the source keeps serving what still fits — a refusal is not a broken source");
    return ok ? 0 : 1;
}
