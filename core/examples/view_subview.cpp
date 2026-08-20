/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `subview`: a narrower window over the SAME segment, no copy.
 *
 * A view is `{owner, offset, length}`, so narrowing it is arithmetic on the offset and the
 * length plus a refcount bump — never a memcpy (`docs/reference/08-views-and-ownership.md`).
 * This is the primitive every zero-copy slice in the library is built from: a child TLV's
 * payload, a routed path suffix, a value handed to a subscriber. The pointers prove it —
 * a sub-window's `bytes().data()` is the parent's plus the offset, in the same segment.
 *
 * Narrowing also EXTENDS lifetime: the sub-window holds its own reference, so the segment
 * outlives the wide view it came from. `view_t::bytes()` debug-asserts the window invariant
 * (`offset + length <= owner->bytes.size()`), which is why the sanitizer CI leg is where an
 * out-of-bounds window is caught at its source.
 *
 * Runs under ctest as `example_view_subview`; returns non-zero on any failed check.
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

}  // namespace

int main() {
    bool ok = true;
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(16);
    check(ok, static_cast<bool>(seg), "a 16-byte heap segment");
    if (!seg) return 1;
    for (std::size_t i = 0; i < seg->bytes.size(); ++i) seg->bytes[i] = static_cast<std::byte>(i);

    const tr::view::view_t whole = tr::view::view_t::over(seg);
    check(ok, whole.length == 16, "over() covers the whole segment");

    const tr::view::view_t mid = whole.subview(4, 8);  // bytes 4..11
    std::printf("whole=[0,16) sub=[%zu,%zu) use_count=%u\n", mid.offset, mid.offset + mid.length,
                seg.use_count());
    check(ok, mid.length == 8 && mid.offset == 4, "the sub-window is {offset, length} arithmetic");
    check(ok, mid.bytes().data() == whole.bytes().data() + 4,
          "and addresses the parent's bytes directly — zero copies");

    const tr::view::view_t inner = mid.subview(2, 4);  // bytes 6..9, relative to `mid`
    check(ok, inner.offset == 6, "nesting composes offsets against the SEGMENT, not the window");
    check(ok, inner.bytes()[0] == std::byte{6}, "so the first byte read back is byte 6");

    const std::uint_least32_t held = seg.use_count();
    check(ok, held == 4, "segment + three windows, each holding its own reference");
    check(ok, !whole.empty() && !inner.empty(), "a narrowed window is still a view over bytes");
    return ok ? 0 : 1;
}
