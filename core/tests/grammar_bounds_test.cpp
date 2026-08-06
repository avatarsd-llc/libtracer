/**
 * @file
 * @brief Unit tests for the grammar's total-encoded-size bound (grammar.hpp, #921).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The bug this pins was **invisible at the host's width**. A wire `length` is up
 * to `0xFFFFFFFF`, so `header + length + ts + crc` overflows a **32-bit**
 * `std::size_t` — the primary embedded target's (ESP32-C6 / rv32) — and the old
 * `static_cast<std::size_t>(...)` narrowed the sum *before* the `avail < total`
 * compare, so a hostile frame presented a `total` of 17, sailed past
 * FRAME_TRUNCATED, and handed `walk` a payload span far past the buffer.
 *
 * On a 64-bit host that same frame is rejected either way, so a plain
 * `parse_header` test proves nothing (the 2026-07-31 vacuous-guard lesson). The
 * proof therefore runs @ref tr::wire::grammar::total_size_fits at
 * `Size = std::uint32_t` — the *same* function template, the *same* arithmetic
 * the rv32 build compiles, only the width pinned. Those cases are `constexpr`,
 * so the 32-bit arithmetic is folded by the compiler and merely *reported* at
 * run time. The 64-bit `parse_header` leg below is documentation of the
 * platform-specificity, not the proof: it passes with and without the fix — as
 * was confirmed by reverting the fix and watching only the 32-bit leg redden.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "libtracer/error.hpp"
#include "libtracer/grammar.hpp"

namespace {

int g_failures = 0;

/** @brief Report one assertion, tallying failures for the process exit code. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief The bound at an explicit @p Size width, with the out-param folded away. */
template <class Size>
[[nodiscard]] constexpr bool fits(Size avail, Size header, std::uint32_t length, Size ts_size,
                                  Size crc_size) noexcept {
    Size total = 0;
    return tr::wire::grammar::total_size_fits(avail, header, length, ts_size, crc_size, total);
}

/** @brief The total the bound hands back (only meaningful when it holds). */
template <class Size>
[[nodiscard]] constexpr Size fits_total(Size avail, Size header, std::uint32_t length, Size ts_size,
                                        Size crc_size) noexcept {
    Size total = 0;
    (void)tr::wire::grammar::total_size_fits(avail, header, length, ts_size, crc_size, total);
    return total;
}

using u32 = std::uint32_t;

/**
 * @brief The 32-bit-`size_t` leg — the non-vacuous proof.
 *
 * Every constant below is folded at compile time at rv32's width; `check` only
 * reports the verdict.
 */

/**
 * @brief The issue's exact vector: header 6 (LL set) + length 0xFFFFFFFF + ts 8 + crc 4.
 *
 * The true sum is 0x1'0000'0011; narrowed to 32 bits it is 0x11 (17), which "fits"
 * any real buffer. Nothing that large fits a 32-byte one.
 */
constexpr bool kHostileFits32 = fits<u32>(32u, 6u, 0xFFFFFFFFu, 8u, 4u);

/**
 * @brief The trailer step on its own: the body fits the buffer exactly, so only the
 *        timestamp + CRC bytes push it over.
 *
 * Pins the third subtraction, which a bound that stopped at `length <= avail - header`
 * would miss.
 */
constexpr bool kTrailerOverrunFits32 = fits<u32>(32u, 6u, 26u, 8u, 4u);
constexpr bool kTrailerExactFits32 =
    fits<u32>(32u, 6u, 14u, 8u, 4u); /**< The trailer that exactly reaches the end. */

/**
 * @brief The helper establishes `avail >= header` itself rather than trusting the caller,
 *        so a short buffer can never make the first subtraction wrap.
 */
constexpr bool kShorterThanHeaderFits32 = fits<u32>(4u, 6u, 0u, 0u, 0u);

/**
 * @brief The same wrap with no trailer at all: 6 + 0xFFFFFFFA narrows to exactly 0.
 *
 * `avail < 0` is false for every buffer — the guard degenerates to unconditional
 * acceptance.
 */
constexpr bool kWrapToZeroFits32 = fits<u32>(64u, 6u, 0xFFFFFFFAu, 0u, 0u);

/** @brief The wrap-to-one case, to show it is the arithmetic and not one magic constant. */
constexpr bool kWrapToOneFits32 = fits<u32>(1u, 6u, 0xFFFFFFFBu, 0u, 0u);

/**
 * @brief The largest frame a 32-bit target can honestly hold must still be ACCEPTED —
 *        the fix must reject the wrap, not the boundary.
 */
constexpr bool kMaxHonestFits32 = fits<u32>(0xFFFFFFFFu, 6u, 0xFFFFFFF9u, 0u, 0u);
constexpr u32 kMaxHonestTotal32 =
    fits_total<u32>(0xFFFFFFFFu, 6u, 0xFFFFFFF9u, 0u, 0u); /**< ...and its exact total. */

/** @brief One byte past it: the true sum is 2^32, which no 32-bit buffer can hold. */
constexpr bool kOnePastMaxFits32 = fits<u32>(0xFFFFFFFFu, 6u, 0xFFFFFFFAu, 0u, 0u);

/** @brief Ordinary in-range traffic is unaffected at 32-bit width. */
constexpr bool kSmallFits32 = fits<u32>(64u, 4u, 8u, 0u, 4u);
constexpr u32 kSmallTotal32 = fits_total<u32>(64u, 4u, 8u, 0u, 4u);
constexpr bool kExactFits32 = fits<u32>(16u, 4u, 8u, 0u, 4u);
constexpr bool kOneShortFits32 = fits<u32>(15u, 4u, 8u, 0u, 4u);

/** @brief Assemble one TLV header with a declared body @p length and no real body. */
std::vector<std::byte> hostile_frame(std::uint8_t type, std::uint8_t opt, std::uint32_t length,
                                     std::size_t buffer_bytes) {
    std::vector<std::byte> b(buffer_bytes, std::byte{0});
    b[0] = std::byte{type};
    b[1] = std::byte{opt};
    b[2] = static_cast<std::byte>(length & 0xFFu);
    b[3] = static_cast<std::byte>((length >> 8) & 0xFFu);
    b[4] = static_cast<std::byte>((length >> 16) & 0xFFu);
    b[5] = static_cast<std::byte>((length >> 24) & 0xFFu);
    return b;
}

}  // namespace

int main() {
    std::printf("grammar — total-encoded-size bound (#921):\n");

    std::printf(" 32-bit size_t (rv32 / ESP32-C6) — the non-vacuous leg:\n");
    check(!kHostileFits32, "length=0xFFFFFFFF + ts + crc does NOT fit a 32-byte buffer");
    check(!kWrapToZeroFits32, "a sum that narrows to 0 is rejected");
    check(!kWrapToOneFits32, "a sum that narrows to 1 is rejected");
    check(kMaxHonestFits32, "the largest honestly-representable frame still fits");
    check(kMaxHonestTotal32 == 0xFFFFFFFFu, "...and reports the exact total");
    check(!kOnePastMaxFits32, "one byte past the 32-bit ceiling is rejected");
    check(!kTrailerOverrunFits32, "a trailer that overruns the buffer is rejected");
    check(kTrailerExactFits32, "...and a trailer that exactly reaches the end is not");
    check(!kShorterThanHeaderFits32, "avail < header is rejected, not wrapped");
    check(kSmallFits32 && kSmallTotal32 == 16u, "ordinary traffic fits, total = 16");
    check(kExactFits32, "avail == total fits");
    check(!kOneShortFits32, "avail == total - 1 does not");

    std::printf(" 64-bit size_t — same vector via parse_header (documents, not proves):\n");
    {
        // type=0x01, opt = TS|CR|LL (0x38): header 6, ts 8, crc 4 (CRC-32C).
        const auto buf = hostile_frame(0x01, 0x38, 0xFFFFFFFFu, 32);
        const tr::wire::grammar::span_cursor cur{std::span<const std::byte>(buf)};
        const auto h = tr::wire::grammar::parse_header(cur);
        check(!h && h.error() == tr::wire::err_t::FRAME_TRUNCATED,
              "the hostile frame is FRAME_TRUNCATED on this host too");
    }
    {
        // A well-formed opaque TLV: header 4, length 3, no trailer — total 7.
        const std::array<std::byte, 7> b{std::byte{0x01}, std::byte{0x00}, std::byte{0x03},
                                         std::byte{0x00}, std::byte{0xAA}, std::byte{0xBB},
                                         std::byte{0xCC}};
        const tr::wire::grammar::span_cursor cur{std::span<const std::byte>(b)};
        const auto h = tr::wire::grammar::parse_header(cur);
        check(h.has_value() && h->total == 7 && h->header == 4 && h->length == 3,
              "a well-formed TLV still parses (total = 7)");
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
