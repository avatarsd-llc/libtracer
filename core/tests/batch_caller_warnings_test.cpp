/**
 * @file
 * @brief #2393 — the obvious caller of `batch_view_t::sample_time_ns` compiles cleanly under
 *        `-Wall -Wextra -Werror` at `-O2` (`batch.hpp`).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The obvious way to read a sample time is to test the `std::optional<batch_view_t>` that
 * @ref tr::wire::read_batch or @ref tr::wire::read_packed_batch answers, then dereference it
 * behind the same short-circuit: `if (v && v->size() > 2) v->sample_time_ns(2)`. GCC 13 at
 * `-O2` reported `-Wmaybe-uninitialized` inside `sample_time_ns` for that shape, a false
 * positive (the optional is engaged on that path) that broke any `-Werror` application. This
 * target is built with `-Werror` and optimization forced on, so the gate is the BUILD: a
 * header change that brings the warning back fails to compile here. Running it then checks
 * that the time it reads is the right one.
 *
 * The record is read from a heap copy, not a constant, so the optimizer cannot fold the
 * optional's engaged flag away: that is the condition the false positive needed. The warning
 * is sensitive to the caller's exact shape, so this one is kept as found: verified to fail
 * the build on GCC 13 against the header before #2393 and to pass after it.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/batch.hpp"

namespace {

/** @brief The batch base, in ns since the Unix epoch. */
constexpr std::int64_t kBase = 1'700'000'000'000'000'000;
/** @brief Per-sample offsets of the non-uniform stream. */
constexpr std::array<std::int32_t, 3> kOffsets{0, 1500, -250};
/** @brief The descriptor's `width` for the packed shape: one u16 per sample. */
constexpr std::uint16_t kWidth = 2;

/**
 * @brief Report expectation @p what and record a failure on @p ok.
 *
 * A plain local reporter, not `tr::testing::check`: the false positive depended on how the
 * caller is optimized, and this is the shape it was found in (#2377's example).
 */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    bool ok = true;
    const std::array<std::byte, 3 * kWidth> block{};
    std::array<std::byte, 64> out{};
    const std::vector<std::byte> rec(
        out.begin(), out.begin() + static_cast<std::ptrdiff_t>(tr::wire::encode_packed_batch(
                                       out, tr::wire::batch_carriage_t::STANDALONE, kBase, kWidth,
                                       block, kOffsets)));
    const auto node = tr::wire::tlv_node_t::over(rec);
    // The same bytes read as a uniform framed batch, two children after the base: a second
    // optional view in the same condition.
    const auto framed_node = tr::wire::tlv_node_t::over(rec);

    const auto v = node ? tr::wire::read_packed_batch(*node, 0, kWidth) : std::nullopt;
    const auto f = framed_node ? tr::wire::read_batch(*framed_node, 1) : std::nullopt;

    // The issue's shape: test the optionals, then dereference behind the same test.
    bool read = false;
    if (f && v && f->size() == 2 && v->size() > 2) read = v->sample_time_ns(2) == kBase - 250;
    check(ok, read, "sample 2 of the packed view is base + offsets[2]");

    std::printf("%s\n",
                ok ? "batch_caller_warnings: all checks passed" : "batch_caller_warnings: FAILED");
    return ok ? 0 : 1;
}
