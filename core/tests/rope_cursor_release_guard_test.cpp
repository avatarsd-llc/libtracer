/**
 * @file
 * @brief The cursor window guarantee in a RELEASE build (#986) — the gate that
 *        cannot pass vacuously under `NDEBUG`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `rope_cursor_assert_test` proves the debug preconditions abort; by construction it
 * proves nothing about shipped code, because `assert` compiles out. This file is its
 * complement and the reason #986 was filed: `for_each_span`'s violation is the one the
 * sibling guards cannot see — an overshooting feed walks bytes the chain genuinely
 * holds, so nothing faults, no sanitizer fires, and the caller is handed real bytes
 * from the wrong place and told it succeeded.
 *
 * `NDEBUG` is forced ON before any include, so every cursor read this TU instantiates
 * is the shipped one with its assert compiled out. What remains must therefore be the
 * production behaviour: the feed is CLAMPED to the window (no wrong byte is ever
 * served) and the cursor LATCHES, which `parse_header` answers as `FRAME_TRUNCATED`.
 *
 * Reverting either half reddens this file rather than skipping it: without the clamp
 * the overshoot cases see 5 bytes where the window holds 3 (3 checks), and without the
 * latch the grammar consumes a short CRC feed as if it were whole (4 checks). Both were
 * confirmed by reverting each half against this file before it was committed.
 *
 * The guarantee is ROPE-ONLY, by measurement — see @ref test_span_cursor_is_constexpr_clean.
 */

#ifndef NDEBUG
#define NDEBUG 1
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "libtracer/grammar.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/rope_decode.hpp"
#include "libtracer/view.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::grammar::rope_cursor;
using tr::wire::grammar::span_cursor;

/** @brief A borrowed view over @p bytes (the caller's storage must outlive it). */
view_t borrowed_view(std::span<std::byte> bytes) { return view_t::over(tr::view::borrow(bytes)); }

/** @brief Total bytes @p cur feeds for `[off, n)`, and whether it latched. */
struct feed_result_t {
    std::size_t bytes = 0; /**< @brief Bytes actually handed to the callback. */
    bool poisoned = false; /**< @brief The cursor's latch after the feed. */
};

/** @brief Feed `[off, n)` from @p cur and report what came out. */
template <class Cursor>
feed_result_t feed(const Cursor& cur, std::size_t off, std::size_t n) {
    feed_result_t r;
    cur.for_each_span(off, n, [&](std::span<const std::byte> s) { r.bytes += s.size(); });
    r.poisoned = cur.poisoned();
    return r;
}

/**
 * @brief The in-contract feeds are untouched — no clamp, no latch (#986).
 *
 * The guarantee is worthless if it fires on legal traffic: every frame the grammar
 * parses feeds exactly `off + n == size()` at the trailer, which is in contract.
 */
void test_in_contract_feeds_are_unchanged() {
    std::printf("release build: in-contract feeds neither clamp nor latch (#986):\n");
    std::array<std::byte, 3> a{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}};
    std::array<std::byte, 2> b{std::byte{0x20}, std::byte{0x21}};
    rope_t r(borrowed_view(a));
    r.append(borrowed_view(b));
    const rope_cursor cur{r};

    const feed_result_t whole = feed(cur, 0, 5);
    check(whole.bytes == 5 && !whole.poisoned, "the whole window feeds 5 bytes and does not latch");

    const rope_cursor narrowed = cur.region(0, 3);
    const feed_result_t exact = feed(narrowed, 0, 3);
    check(exact.bytes == 3 && !exact.poisoned,
          "a feed that exactly fills a narrowed window is in "
          "contract");

    const rope_cursor tail = cur.region(3, 2);
    const feed_result_t crossed = feed(tail, 0, 2);
    check(crossed.bytes == 2 && !crossed.poisoned, "a window opening mid-chain feeds its 2 bytes");

    const feed_result_t empty = feed(cur, 5, 0);
    check(empty.bytes == 0 && !empty.poisoned,
          "a zero-length feed at the window end is legal and does not latch");
}

/**
 * @brief The defect #986 names: a feed past a NARROWED window, in a release build.
 *
 * Every byte the overshoot would walk is in the chain, so this is the case with no
 * fault, no sanitizer report and — before the fix — no signal of any kind.
 */
void test_overshoot_is_clamped_not_served() {
    std::printf("release build: a feed past a narrowed window is clamped and latches (#986):\n");
    std::array<std::byte, 3> a{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}};
    std::array<std::byte, 2> b{std::byte{0x20}, std::byte{0x21}};
    rope_t r(borrowed_view(a));
    r.append(borrowed_view(b));

    // The issue's own repro: two links 3+2, region(0, 3), for_each_span(0, 5).
    // Pre-fix release behaviour was "fed 5 bytes from a 3-byte window, returned normally".
    const rope_cursor narrowed = rope_cursor{r}.region(0, 3);
    const feed_result_t over = feed(narrowed, 0, 5);
    check(over.bytes == 3, "the overshoot is clamped to the window (was: 5 bytes, silently)");
    check(over.poisoned, "and the cursor latches so a boundary can answer for it");

    // Bytes that ARE in the chain but past the window must not be reachable by offset
    // either — the clamp is on the window, not on the chain.
    const rope_cursor front = rope_cursor{r}.region(0, 2);
    const feed_result_t off_over = feed(front, 1, 4);
    check(off_over.bytes == 1 && off_over.poisoned,
          "a feed whose START is in-window but whose end is not is clamped to the remainder");

    // A feed starting past the window names no byte at all.
    const rope_cursor short_win = rope_cursor{r}.region(0, 2);
    const feed_result_t past = feed(short_win, 2, 1);
    check(past.bytes == 0 && past.poisoned, "a feed starting at the window end serves nothing");

    // The latch is sticky: a good feed after a bad one does not clear it, because the
    // frame that produced the short read is not made sound by a later whole one.
    const feed_result_t after = feed(short_win, 0, 2);
    check(after.bytes == 2 && after.poisoned, "a later in-contract feed does not clear the latch");
}

/**
 * @brief The contiguous twin is deliberately NOT latched, and costs nothing for it (#986).
 *
 * The asymmetry is the measured half of this issue's ruling. A `span_cursor`'s window IS
 * its whole object, so an overshoot is an out-of-range `subspan` — UB the fuzz/ASan CI
 * reports — where the rope's overshoot reads real bytes from elsewhere in the chain and
 * is reportable by nothing. Giving the span source the same clamp-and-latch measured
 * **compact-forward x0.66 deliv/s and compact-terminus x0.82** (4/4 and 3/4 interleaved
 * pairs, disjoint ranges), because a `min()`-derived `subspan` length costs the CRC feed
 * loop what a directly-derived one gives it. Under this repo's standing rule a latency
 * regression is an automatic reject, so the guarantee is spent where the defect is.
 */
void test_span_cursor_is_constexpr_clean() {
    std::printf("release build: span_cursor stays unlatched by construction (#986):\n");
    std::array<std::byte, 5> bytes{std::byte{0x10}, std::byte{0x11}, std::byte{0x12},
                                   std::byte{0x20}, std::byte{0x21}};
    const span_cursor cur{std::span<const std::byte>(bytes)};

    const feed_result_t whole = feed(cur, 0, 5);
    check(whole.bytes == 5 && !whole.poisoned, "the whole span feeds and reports no latch");

    static_assert(!span_cursor::poisoned(),
                  "span_cursor::poisoned must fold at compile time, so the shared grammar's "
                  "check costs the contiguous source nothing");
    static_assert(sizeof(span_cursor) == sizeof(std::span<const std::byte>),
                  "span_cursor must stay exactly one span wide — carrying a latch byte is what "
                  "cost compact-forward a third of its throughput");
}

/**
 * @brief The decode boundary answers `FRAME_TRUNCATED` for a latched cursor (#986).
 *
 * The end of the thread the issue asked for: the guarantee is only worth its cost if a
 * caller can act on it, and `parse_header` is where a rope frame becomes a decision.
 */
void test_parse_header_maps_latch_to_truncated() {
    std::printf("release build: parse_header answers FRAME_TRUNCATED for a clamped feed (#986):\n");

    // A minimal CRC-bearing TLV: type 0x01, OPT with CR set (CW = 1 -> a 2-byte CRC),
    // 2-byte length = 2, payload {0xAA, 0xBB}, then the CRC16 of that payload.
    std::vector<std::byte> frame;
    const auto put = [&frame](std::uint8_t v) { frame.push_back(std::byte{v}); };
    // CR is bit 4 and CW is bit 2 (`opt_t`), so a CRC-16-bearing header's opt byte is
    // 0x14. Written raw rather than through `opt_t` so the test pins the wire spelling.
    constexpr std::uint8_t kOptCrCw = 0x14;
    put(0x01);
    put(kOptCrCw);
    put(0x02);
    put(0x00);
    put(0xAA);
    put(0xBB);
    tr::crc::crc16_ccitt_state crc;
    const std::array<std::byte, 2> payload{std::byte{0xAA}, std::byte{0xBB}};
    crc.feed(std::span<const std::byte>(payload));
    const std::uint16_t sum = crc.value();
    put(static_cast<std::uint8_t>(sum & 0xFFu));
    put(static_cast<std::uint8_t>((sum >> 8) & 0xFFu));

    const span_cursor whole{std::span<const std::byte>(frame)};
    const auto good = tr::wire::grammar::parse_header(whole);
    check(good.has_value(), "the well-formed frame still parses (the guard does not over-fire)");
    check(!whole.poisoned(), "and parsing it latches nothing");

    // The same bytes through a window that stops inside the trailer. `total_size_fits`
    // already refuses this before any feed runs — asserted here as the PARITY baseline
    // the rope source must match below, not as a #986 behaviour.
    const span_cursor clipped{std::span<const std::byte>(frame).first(frame.size() - 1)};
    const auto clipped_head = tr::wire::grammar::parse_header(clipped);
    check(!clipped_head.has_value(), "a frame whose trailer is outside the window is rejected");

    // The rope source must answer the same way for the same bytes — one grammar, two
    // sources, per ADR-0048 §1.
    std::vector<std::byte> storage(frame.begin(), frame.end());
    rope_t r(borrowed_view(std::span<std::byte>(storage)));
    const rope_cursor rope_whole{r};
    const auto rope_good = tr::wire::grammar::parse_header(rope_whole);
    check(rope_good.has_value() && !rope_whole.poisoned(),
          "the rope source parses the same frame with no latch");

    const rope_cursor rope_clipped = rope_cursor{r}.region(0, storage.size() - 1);
    const auto rope_head = tr::wire::grammar::parse_header(rope_clipped);
    check(!rope_head.has_value(), "and rejects the clipped window exactly as the span source does");
}

}  // namespace

int main() {
    test_in_contract_feeds_are_unchanged();
    test_overshoot_is_clamped_not_served();
    test_span_cursor_is_constexpr_clean();
    test_parse_header_maps_latch_to_truncated();
    return tr::testing::summary("rope_cursor_release_guard");
}
