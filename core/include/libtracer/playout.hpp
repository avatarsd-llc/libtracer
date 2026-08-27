/**
 * @file
 * @brief The REFERENCE PLAYOUT helper — the receiver-side derivation RFC-0025 §4.7 asks the
 *        library to ship, and nothing else.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0025 §4.2.1 (Amendment 1) names three clocks: **wire/TX** time on the outermost frame's
 * trailer (always `TF=0`), **SAMPLE** time in the batch's payload `TIME` (`0x0C`) child, and
 * **PLAYOUT** time, which is *never transmitted* — the consumer derives it. This header is
 * that derivation, written once so every consumer spells it the same way.
 *
 * **What it does — exactly two things.**
 *
 * 1. **Per-sample timestamps.** `t(i)` for every frame in a batch: `base + i * dt_ns` on a
 *    UNIFORM stream, at **0 bytes per sample** (the §4.3 descriptor's `dt_ns` is a reader's
 *    fact the producer already declared), and `base + offsets[i]` on a NON-UNIFORM one, out
 *    of the packed `i32` run. The arithmetic itself is @ref tr::wire::batch_view_t's; what
 *    this adds is the walk, the clock comparison and the sequence bookkeeping around it.
 * 2. **Late/gap flagging against a caller-supplied "now".** A sample is LATE when the
 *    caller's clock reading has passed its sample time by more than the caller's budget
 *    (@ref tr::wire::playout_clock_t). A `tr::flow::address_shift_gap` the caller reports —
 *    the in-order shed signal a STREAM ring drain hands back — surfaces in the report and
 *    RESETS the sequence expectation. Loss is never masked.
 *
 * **What it REFUSES to do, deliberately: reorder, de-jitter, interpolate, pace.** Those are
 * consumer policy, and a recorder wants none of them (§4.7). Pacing in particular *schedules*,
 * and RFC-0005's ban — "rate caps, flush intervals, dirty tracking and timers are explicitly
 * not libtracer concerns" — stands **unqualified** (RFC-0025 §4.1.3, Amendment 4). Nothing
 * here sleeps, arms a timer or reads a clock: the "now" it compares against is a number the
 * application passes in, taken on the application's own thread from the application's own
 * clock, above the graph. A helper that scheduled would be disqualified.
 *
 * **It is a pure value-consuming derivation.** It holds no buffers — the only state it needs
 * is a sequence expectation, and that lives in a @ref tr::wire::playout_cursor_t the CALLER
 * declares and owns (two scalars; no allocation, ever). The batch it walks is the caller's
 * decoded value. Receiver-side only: a producer never queues (§4.6.1, Amendment 2).
 *
 * **Its conformance surface is host tests only.** A receiver-side derivation emits no bytes,
 * so there is nothing for a vector to pin — the reading §7 items 2/4/5/6/7/9 already carry.
 *
 * Header-only, and in `tr::wire` because a batch is a value: an MCU pays this header's
 * footprint only if it includes it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include "libtracer/batch.hpp"
#include "libtracer/tlv.hpp"

namespace tr::wire {

namespace detail {

/**
 * @brief `a - b` in nanoseconds, SATURATED at the `i64` ends instead of wrapping.
 *
 * Lateness is a difference of two epoch stamps, and two stamps far enough apart overflow it.
 * A wrapped difference would turn a hopelessly late sample into an early one — the one answer
 * worse than "unrepresentably late" — so the ends clamp.
 */
[[nodiscard]] constexpr std::int64_t saturating_sub_ns(std::int64_t a, std::int64_t b) noexcept {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (b < 0 && a > kMax + b) return kMax;
    if (b > 0 && a < kMin + b) return kMin;
    return a - b;
}

}  // namespace detail

/**
 * @brief The APPLICATION's clock reading and lateness budget — the whole of what this helper
 *        knows about time that is not in the batch.
 *
 * Passed in, never taken: the helper reads no clock and arms no timer (RFC-0005's ban, kept
 * unqualified by RFC-0025 Amendment 4). Whatever thread the application samples its clock on
 * is the thread this derivation runs on.
 */
struct playout_clock_t {
    /** @brief The caller's "now", in the SAME epoch as the batch's `TIME` base — nanoseconds
     *         since the Unix epoch. Mixing epochs produces nonsense, silently, because there
     *         is nothing on the wire to check it against. */
    std::int64_t now_ns = 0;
    /** @brief How far past its sample time a frame may be and still not be flagged LATE. The
     *         caller's budget — a de-jitter depth, a display deadline, a recorder's `INT64_MAX`
     *         for "never late". Default `0`: late the instant its sample time has passed. */
    std::int64_t late_after_ns = 0;
};

/**
 * @brief One sample frame, with its derived time and the caller's verdict on it.
 *
 * Handed to the visitor by reference, valid only for that call: @ref frame borrows out of the
 * `tlv_t` the batch decoded from, which the caller owns and must keep alive.
 */
struct playout_sample_t {
    /** @brief Position of this frame within the batch, in frame order. */
    std::size_t index = 0;
    /** @brief The sample frame itself — a borrowed child of the caller's decoded batch. */
    const tlv_t* frame = nullptr;
    /** @brief The DERIVED sample time, ns since the Unix epoch. Meaningful only when
     *         @ref time_known; `0` otherwise, never a fabricated plausible value. */
    std::int64_t sample_time_ns = 0;
    /** @brief `now - t(i)`, saturated: positive = the sample time has passed, negative = it is
     *         still ahead. `0` when the time is unknown. */
    std::int64_t lateness_ns = 0;
    /** @brief False when the derivation would leave the representable ns epoch — reported,
     *         never wrapped into a time that looks fine. */
    bool time_known = false;
    /** @brief `lateness_ns > playout_clock_t::late_after_ns`. A FLAG, not a disposition: this
     *         helper never drops, holds or reorders a late sample — what to do with one is the
     *         application's policy. */
    bool late = false;
};

/**
 * @brief How this batch sits against the sequence the cursor expected.
 *
 * Three states, and no fourth: this helper reports continuity, it does not diagnose it. The
 * expected time travels in the report (@ref playout_report_t::expected_sample_time_ns) so a
 * caller can measure drift or under-run for itself, without the helper opining on either.
 */
enum class playout_continuity_t : std::uint8_t {
    /** @brief The cursor had no expectation — its first batch, or the first after a gap
     *         reset it. */
    FIRST,
    /** @brief Continuous with what the cursor last saw, and no loss was reported. */
    CONTINUOUS,
    /** @brief A `tr::flow::address_shift_gap` was reported at or before this batch: entries
     *         the consumer would have seen are MISSING. The expectation is discarded. */
    GAP,
};

/**
 * @brief The CALLER's sequence expectation across batches — the only state this derivation
 *        carries, and the caller owns it.
 *
 * Two scalars on the caller's stack (or in the caller's own object). No allocation, no
 * library-internal buffer, no hidden lifetime: declare one per stream being played out, pass
 * it to every @ref playout_batch call for that stream, and it is as long-lived as the caller
 * makes it. A fresh cursor is a fresh stream.
 */
struct playout_cursor_t {
    /** @brief Where the next sample was expected. On a UNIFORM stream, the last derived
     *         sample time plus the descriptor's `dt_ns`. On a NON-UNIFORM one no rate is
     *         declared, so no forward projection exists and this is the last sample's own
     *         time — a lower bound, not a prediction. Meaningful only when @ref primed. */
    std::int64_t next_sample_time_ns = 0;
    /** @brief True once a batch has established an expectation, and false again the moment a
     *         reported gap discards it. */
    bool primed = false;

    /** @brief Discard the expectation — what a reported gap does, exposed so a caller can do
     *         it explicitly on a re-subscribe, an epoch change or any other reason of its own
     *         that the helper has no business inferring. */
    constexpr void forget() noexcept {
        next_sample_time_ns = 0;
        primed = false;
    }
};

/**
 * @brief What one @ref playout_batch call derived — the counts and the continuity verdict.
 *
 * `[[nodiscard]]`-returned on purpose: @ref continuity and @ref gaps are the only place loss
 * surfaces, and a helper whose loss report can be dropped by accident would be masking it.
 */
struct playout_report_t {
    /** @brief How this batch sat against the cursor's expectation. */
    playout_continuity_t continuity = playout_continuity_t::FIRST;
    /** @brief The gap count the CALLER reported for this batch, passed straight through — the
     *         `tr::flow::address_shift_gap` census of RFC-0025 §4.4/§4.5. Non-zero means
     *         entries are missing immediately before this batch. */
    std::uint64_t gaps = 0;
    /** @brief Sample frames visited — the batch's whole length; nothing is skipped. */
    std::size_t samples = 0;
    /** @brief How many of them were flagged @ref playout_sample_t::late. */
    std::size_t late = 0;
    /** @brief How many had no representable sample time (@ref playout_sample_t::time_known
     *         false). */
    std::size_t undated = 0;
    /** @brief The time the cursor expected this batch's first sample at. Meaningful only when
     *         @ref expected_valid — which a gap, and the first batch, both leave false. */
    std::int64_t expected_sample_time_ns = 0;
    /** @brief Whether @ref expected_sample_time_ns says anything. */
    bool expected_valid = false;
};

/**
 * @brief Walk @p batch in frame order, deriving each sample's time and flagging it against
 *        @p clock — the reference playout derivation of RFC-0025 §4.7.
 *
 * Visits every frame exactly once, in order, calling `on_sample(const playout_sample_t&)`.
 * **In order and complete**: no reordering, no dropping, no interpolation, no waiting between
 * samples. If the application wants to pace playout it does so in its own thread around this
 * call — this function returns as fast as it can walk the batch.
 *
 * Nothing is retained. The visitor sees each sample while the caller's decoded batch is alive
 * and this helper keeps no reference afterwards.
 *
 * **Loss.** @p gaps_before is what the caller's drain reported (`vertex_t::drain_unflushed`'s
 * `gap_before` out-param, or the equivalent on whatever seam delivered the batch). Non-zero
 * makes the report @ref playout_continuity_t::GAP and **resets @p cursor's expectation** — a
 * discontinuity is surfaced to the caller, never smoothed over.
 *
 * @param cursor      The caller's sequence expectation for this stream; updated in place.
 * @param batch       The decoded batch (@ref read_batch), whose backing `tlv_t` must outlive
 *                    this call.
 * @param clock       The application's "now" and lateness budget.
 * @param gaps_before Shed points reported immediately before this batch; `0` for none.
 * @param on_sample   Visitor, invoked once per sample frame in order.
 * @return The derivation's counts and continuity verdict.
 */
template <class on_sample_t>
[[nodiscard]] inline playout_report_t playout_batch(playout_cursor_t& cursor,
                                                    const batch_view_t& batch,
                                                    const playout_clock_t& clock,
                                                    std::uint64_t gaps_before,
                                                    on_sample_t&& on_sample) {
    playout_report_t report{};
    report.gaps = gaps_before;
    if (gaps_before != 0) {
        cursor.forget();
        report.continuity = playout_continuity_t::GAP;
    } else if (cursor.primed) {
        report.continuity = playout_continuity_t::CONTINUOUS;
        report.expected_sample_time_ns = cursor.next_sample_time_ns;
        report.expected_valid = true;
    }

    std::int64_t last_ns = 0;
    bool have_last = false;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        playout_sample_t sample{};
        sample.index = i;
        sample.frame = &batch.samples[i];
        if (const std::optional<std::int64_t> t = batch.sample_time_ns(i); t.has_value()) {
            sample.time_known = true;
            sample.sample_time_ns = *t;
            sample.lateness_ns = detail::saturating_sub_ns(clock.now_ns, *t);
            sample.late = sample.lateness_ns > clock.late_after_ns;
            last_ns = *t;
            have_last = true;
        } else {
            ++report.undated;
        }
        if (sample.late) ++report.late;
        ++report.samples;
        on_sample(std::as_const(sample));
    }

    // Re-prime from the last DERIVED time, not from the base plus a count: an undated tail
    // must not shift the expectation to a time no sample was actually at.
    if (have_last) {
        const std::uint64_t dt = batch.dt_ns;
        // Two arms, because the derived time can be NEGATIVE: a non-uniform batch is
        // `base + offsets[i]` and §4.2.1 permits a negative i32 offset, so `read_batch`'s
        // `base_ns >= 0` guarantee does not survive the addition. Spelling the headroom as
        // `int64max - last_ns` would then be signed overflow — UB on peer-supplied bytes
        // (#1580). Mirrors the guarded addition of `batch_view_t::sample_time_ns`.
        const std::uint64_t headroom =
            last_ns >= 0
                ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - last_ns)
                : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
                      (0u - static_cast<std::uint64_t>(last_ns));
        if (dt > headroom) {
            cursor.forget();  // the next sample would sit past the representable epoch
        } else {
            // `last_ns + dt` is in range by the guard above, so the unsigned sum carries the
            // right two's-complement bits — and, unlike the signed spelling, it stays defined
            // for a `dt` that alone exceeds `int64max` (reachable only when `last_ns < 0`).
            cursor.next_sample_time_ns =
                static_cast<std::int64_t>(static_cast<std::uint64_t>(last_ns) + dt);
            cursor.primed = true;
        }
    }
    return report;
}

/**
 * @brief The counts-only spelling — the same derivation with no visitor, for a caller that
 *        wants the lateness and continuity census and not the samples.
 *
 * Identical in every other respect to the visiting overload above, including the gap
 * handling: a non-zero @p gaps_before still surfaces as @ref playout_continuity_t::GAP and
 * still resets @p cursor.
 *
 * @param cursor      The caller's sequence expectation for this stream; updated in place.
 * @param batch       The decoded batch, whose backing `tlv_t` must outlive this call.
 * @param clock       The application's "now" and lateness budget.
 * @param gaps_before Shed points reported immediately before this batch; `0` for none.
 * @return The derivation's counts and continuity verdict.
 */
[[nodiscard]] inline playout_report_t playout_batch(playout_cursor_t& cursor,
                                                    const batch_view_t& batch,
                                                    const playout_clock_t& clock,
                                                    std::uint64_t gaps_before) {
    return playout_batch(cursor, batch, clock, gaps_before,
                         [](const playout_sample_t&) noexcept {});
}

}  // namespace tr::wire
