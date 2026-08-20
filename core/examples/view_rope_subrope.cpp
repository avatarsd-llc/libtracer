/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — `subrope` re-links a byte range; `to_iovec` hands it to the DMA.
 *
 * A rope's sub-range is taken by TRIMMING the covering links with `view_t::subview` and
 * re-chaining them, so a window whose start falls mid-link costs one arithmetic pass and a
 * few refcount bumps — no bytes move (`docs/reference/08-views-and-ownership.md`). This is
 * the region primitive of the lazy decode tier: a child TLV, a routed path suffix, or a
 * payload handed to the next hop is a **subrope** of the inbound frame, never a copy of it.
 * It also narrows ownership — the sub-rope keeps alive exactly the segments its window
 * touches, and no others.
 *
 * The egress half is `to_iovec`: one span per link, straight into `writev`/`sendmsg`-style
 * scatter-gather. `try_to_iovec` is its nothrow twin, refilling a caller's vector so a
 * per-send table costs no allocation and cannot abort the node on a fragmented heap.
 *
 * Runs under ctest as `example_view_rope_subrope`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

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
    tr::view::rope_t frame;
    frame.append(block(std::byte{0xA0}));
    frame.append(block(std::byte{0xB0}));
    frame.append(block(std::byte{0xC0}));
    check(ok, frame.link_count() == 3 && frame.total_length() == 12, "three 4-byte links");

    // [2, 9): starts two bytes into link 0 and stops one byte into link 2.
    const tr::view::rope_t region = frame.subrope(2, 7);
    std::printf("subrope(2,7): %zu links, %zu bytes, no memcpy\n", region.link_count(),
                region.total_length());
    check(ok, region.total_length() == 7, "the window is exactly the bytes asked for");
    check(ok, region.link_count() == 3, "spread over the three links it overlaps");
    check(ok, region.links()[0].length == 2, "the first link is trimmed to its tail");
    check(ok, region.links()[2].length == 1, "and the last to its head");
    check(ok, region.links()[0].bytes().data() == frame.links()[0].bytes().data() + 2,
          "the trimmed link still addresses the ORIGINAL segment's bytes");

    // walk() visits each link's contiguous bytes in order — what a parser or a CRC does.
    std::vector<std::byte> seen;
    region.walk([&seen](std::span<const std::byte> chunk) {
        seen.insert(seen.end(), chunk.begin(), chunk.end());
    });
    check(ok, seen.size() == 7 && seen[0] == std::byte{0xA0} && seen[6] == std::byte{0xC0},
          "walk() sees one logical byte sequence across three segments");

    std::vector<std::span<const std::byte>> iov;
    check(ok, region.try_to_iovec(iov), "try_to_iovec fills the caller's table, nothrow");
    check(ok, iov.size() == region.link_count(), "one span per link — the scatter-gather list");
    check(ok, region.to_iovec().size() == iov.size(),
          "to_iovec is the same list, freshly allocated");
    return ok ? 0 : 1;
}
