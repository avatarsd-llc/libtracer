/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a rope is an ordered chain of views; assembly is chaining, never copy.
 *
 * A **rope** is the L1 composite: an ordered chain of views over possibly different segments,
 * forming one logical byte sequence (`CONTEXT.md` §Rope / assembly). A static header segment
 * plus a live DMA payload segment become one payload by CHAINING them — `append` links, it
 * does not `memcpy`, and `total_length` sums what the links already hold.
 *
 * Two spellings matter to a first-contact reader. `only()` is the consumer's explicit "this
 * value is one segment" — the single-link hot path, zero copy. And a rope keeps its first two
 * links in inline storage, so a one- or two-link chain allocates nothing at all; the third
 * link spills the chain to the heap. That threshold is a cost tuning knob, not a limit —
 * nothing refuses a longer rope.
 *
 * Runs under ctest as `example_view_rope_compose`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdio>
#include <optional>

#include "libtracer/tracer.hpp"

namespace {

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief A fresh 4-byte heap segment filled with @p fill, as a whole-segment view. */
tr::view::view_t block(std::byte fill) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(4);
    for (std::byte& b : seg->bytes) b = fill;
    return tr::view::view_t::over(std::move(seg));
}

}  // namespace

int main() {
    bool ok = true;

    tr::view::rope_t one{block(std::byte{0xAA})};  // a view converts to a one-link rope
    check(ok, one.link_count() == 1, "one link");
    check(ok, one.total_length() == 4, "four logical bytes");
    check(ok, one.only().bytes()[0] == std::byte{0xAA},
          "only() is the single-segment fast path — zero copy, no flatten");

    one.append(block(std::byte{0xBB}));
    check(ok, one.link_count() == 2 && one.total_length() == 8,
          "appending chains a second segment into the same logical sequence");
    check(ok, one.all_host(), "both links are CPU-addressable");

    // The third link spills the chain to the heap. An optimization threshold, not a bound.
    one.append(block(std::byte{0xCC}));
    std::printf("rope: %zu links, %zu logical bytes, still zero byte copies\n", one.link_count(),
                one.total_length());
    check(ok, one.link_count() == 3 && one.total_length() == 12,
          "a third link chains just as well");

    tr::view::rope_t tail{block(std::byte{0xDD})};
    const tr::view::rope_t joined = one + tail;  // concat = chaining, both operands intact
    check(ok, joined.link_count() == 4 && joined.total_length() == 16,
          "operator+ chains two ropes");
    check(ok, one.link_count() == 3, "leaving the left operand's own chain untouched");
    return ok ? 0 : 1;
}
