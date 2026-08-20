/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a bump source hands out a caller buffer and never takes it back.
 *
 * `tr::mem::bump_source_t` is the nothrow twin of `std::pmr::monotonic_buffer_resource`: a
 * cursor over a span the CALLER owns. `try_alloc` moves the cursor forward, `release` on a
 * block from that span is a **no-op**, and `reset()` is the only way storage comes back —
 * all of it at once.
 *
 * That is a lifetime rule, not a performance note. A bump source is a SCOPE-LIFETIME object:
 * construct it, do one unit of work, drop it (or `reset()` it) — which is exactly what the
 * branch-write decode does with a 4 KiB stack buffer. Parked as a LONG-LIVED seam it fills
 * monotonically and then refuses everything: an 8 KiB bump source wired as a router's `rx`
 * decoded 6 frames and rejected the next 194, measured. A long-lived bounded seam wants
 * `pool_source_t`, which recycles (see mem_pool_source).
 *
 * Runs under ctest as `example_mem_bump_source`; returns non-zero on any failed check.
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
    // `alignas(64)` rather than the usual `alignas(std::max_align_t)` so the padding arithmetic
    // below is the same on every host this example runs on, not a property of one ABI.
    alignas(64) std::array<std::byte, 256> scratch{};
    tr::mem::bump_source_t bump{scratch};
    check(ok, bump.used() == 0, "a fresh bump source has carved nothing");

    void* const first = bump.try_alloc(40, 8);
    check(ok, first == scratch.data(), "the first block starts at the caller's buffer");
    check(ok, bump.used() == 40, "and the cursor moved by exactly the request — no header");

    // Alignment is padding, and padding is visible: 40 is not a multiple of 16.
    void* const wide = bump.try_alloc(16, 16);
    check(ok, wide != nullptr && bump.used() == 64,
          "an over-aligned request pays 8 bytes of padding, and used() says so");
    std::printf("two blocks: %zu of %zu buffer bytes carved\n", bump.used(), scratch.size());

    // The defining property. A bump block is not individually reclaimable, so handing one
    // back is a no-op — the cursor does not retreat and the bytes are not reusable.
    bump.release(first, 40, 8);
    check(ok, bump.used() == 64, "release() of a bump block returns NOTHING — the cursor stands");

    // reset() is the whole-buffer answer, and it is why a bump source is scope-lifetime:
    // it is only safe when every block carved from the buffer is already dead.
    bump.reset();
    check(ok, bump.used() == 0, "reset() hands the whole buffer back at once");
    check(ok, bump.try_alloc(40, 8) == first, "so the next unit of work starts at byte 0 again");
    return ok ? 0 : 1;
}
