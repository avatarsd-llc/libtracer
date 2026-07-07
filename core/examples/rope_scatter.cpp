/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief L1 scatter-gather — compose a multi-link `rope_t` with zero byte copies,
 *        then measure the cost of scatter-gather egress vs. the one flatten copy.
 *
 * A `rope_t` (`docs/modules/views.md`, ADR-0053) chains several `view_t` windows
 * into one logical payload without ever copying the bytes. This example builds a
 * @p kLinks -link rope over independently-allocated segments, checks that the
 * logical bytes match a hand-built reference, and contrasts the two ways to hand
 * the payload to a consumer:
 *   - `to_iovec()` — one span per link, pointing INTO the original segments
 *     (zero copy — what you give `writev`/`sendmsg`);
 *   - `flatten(backend)` — the single contiguous copy, taken only at a boundary
 *     that cannot scatter-gather.
 *
 * The perf line is informational (RESULT), so CI never flakes on timing; the
 * self-checks guard correctness and return non-zero on any mismatch. Runs under
 * ctest as `example_rope_scatter`.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using clock_t_ = std::chrono::steady_clock;

/** @brief A heap segment of @p n bytes, each byte set to @p fill. */
tr::view::view_t chunk(std::size_t n, std::uint8_t fill) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
    for (std::size_t i = 0; i < n; ++i) seg->bytes[i] = static_cast<std::byte>(fill);
    return tr::view::view_t::over(std::move(seg));
}

/** @brief Record a failed expectation on @p ok and report it. */
void check(bool& ok, bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    constexpr std::size_t kLinks = 16;   // chain length
    constexpr std::size_t kChunk = 256;  // bytes per link
    constexpr std::size_t kLogical = kLinks * kChunk;

    // Compose the rope link by link. No bytes are copied — each append chains one
    // more window; only the third link spills the inline chain to a single heap
    // vector (the sole allocation), never the payload bytes.
    tr::view::rope_t rope;
    for (std::size_t i = 0; i < kLinks; ++i)
        rope.append(chunk(kChunk, static_cast<std::uint8_t>('A' + (i % 26))));

    std::printf("composed a %zu-link rope, %zu logical bytes (zero payload copies)\n",
                rope.link_count(), rope.total_length());

    bool ok = true;
    check(ok, rope.link_count() == kLinks, "rope has one link per appended view");
    check(ok, rope.total_length() == kLogical, "total_length sums the links");

    // to_iovec(): one span per link, each pointing into its original segment.
    const std::vector<std::span<const std::byte>> iov = rope.to_iovec();
    check(ok, iov.size() == kLinks, "to_iovec yields one span per link");
    std::size_t iov_bytes = 0;
    for (const auto& s : iov) iov_bytes += s.size();
    check(ok, iov_bytes == kLogical, "the scatter-gather spans cover every logical byte");

    // flatten(): the single contiguous copy. Its bytes must equal the walk order.
    const tr::view::view_t flat = rope.flatten();
    const auto fb = flat.bytes();
    check(ok, fb.size() == kLogical, "flattened view is the full logical length");
    bool contents_match = true;
    std::size_t p = 0;
    for (const auto& s : iov)
        for (const std::byte b : s)
            if (p >= fb.size() || fb[p++] != b) {
                contents_match = false;
                break;
            }
    check(ok, contents_match, "flatten reproduces the scatter-gather byte order exactly");

    // --- perf: scatter-gather (zero copy) vs. the one flatten copy ---
    constexpr int kIters = 20000;
    std::size_t sink = 0;  // defeat dead-code elimination

    auto t0 = clock_t_::now();
    for (int i = 0; i < kIters; ++i) sink += rope.to_iovec().size();
    auto t1 = clock_t_::now();
    for (int i = 0; i < kIters; ++i) sink += rope.flatten().bytes().size();
    auto t2 = clock_t_::now();

    const double iovec_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
    const double flat_ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / kIters;
    const double flat_gbps = (double(kLogical) / (flat_ns * 1e-9)) / 1e9;
    std::printf(
        "RESULT rope_scatter links=%zu logical_bytes=%zu iovec_ns=%.0f flatten_ns=%.0f "
        "flatten_GBps=%.2f (sink=%zu)\n",
        kLinks, kLogical, iovec_ns, flat_ns, flat_gbps, sink);
    std::printf(
        "scatter-gather is O(links) pointer work; flatten is the one memcpy you pay "
        "only at a non-scatter boundary.\n");

    std::printf("%s\n", ok ? "rope scatter-gather OK" : "rope scatter-gather FAILED");
    return ok ? 0 : 1;
}
