/**
 * @file
 * @brief Self-test for every estimator and calibrator in `bench_common.hpp` (#1804, #1904):
 *        picosecond resolution, the 20 µs window floor, the sample floor, both batch
 *        calibrators, the percentile accumulator, the reservation estimate, the DCE canary and
 *        the clock floor.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The estimator's outputs are timings, so most of what can be asserted is a property rather
 * than a value: every window cleared the floor, the sample count respects its own bounds, and
 * the work was not deleted, and a stall that misleads the calibration is recovered from
 * rather than aborting the run. The one exact check is the arithmetic: a per-op figure is
 * `window * 1000 / batch` in picoseconds, kept fractional — the integer division it replaced
 * turned a 3.906 ns operation into 3 ns.
 *
 * Needs no libtracer and no network. Not named bench_* for the reason test_compose_record
 * gives.
 *
 *     ./build/test_batch_timing       # exit 0 = every expectation held
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "bench_common.hpp"

namespace {

int g_failed = 0;

/** @brief Record one expectation; print it when it fails. */
void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++g_failed;
    }
}

/** @brief A few-nanosecond operation the compiler cannot delete. */
volatile std::uint64_t g_sink = 0;
void tiny_op() { g_sink = g_sink + 1; }

/** @brief Calls of @ref stall_once_op so far. */
std::size_t g_calls = 0;

/**
 * @brief A tiny operation whose FIRST call stalls for 60 µs — a preemption inside the
 *        calibration's batch-1 window, which makes batch 1 look long enough.
 */
void stall_once_op() {
    if (g_calls++ == 0) {
        const std::uint64_t until = bench::now_ns() + 60'000;
        while (bench::now_ns() < until) {
        }
    }
    g_sink = g_sink + 1;
}

/** @brief Wall time one @ref spin_op takes, at least. */
constexpr std::uint64_t kSpinNs = 1000;

/**
 * @brief An operation of known cost: it spins until @ref kSpinNs have passed. Its per-op
 *        figure therefore cannot read below 1 000 000 ps, which pins the unit and the scale.
 */
void spin_op() {
    const std::uint64_t until = bench::now_ns() + kSpinNs;
    while (bench::now_ns() < until) {
    }
}

/** @brief Whether @p n is a power of two (the calibrators only ever double). */
constexpr bool pow2(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

}  // namespace

int main() {
    // --- The window calibrator: the batch is a function of the operation's cost. ----------
    // A 1 us operation needs 32 to fill a 20 us window (16 fall short by the window's own
    // definition). A stall can only make a batch look LONGER, so it can only pick a smaller
    // batch: the largest of three calibrations is the undisturbed answer.
    std::size_t wb = 0;
    for (int i = 0; i < 3; ++i)
        wb = std::max(wb, bench::calibrate_batch_for_window(spin_op, bench::kMinBatchWindowNs));
    expect(wb == 32, "a 1 us operation fills a 20 us window at a batch of 32");
    // Work that costs nothing never fills the window: the calibrator saturates at kMaxBatch,
    // which time_batches turns into an abort instead of a figure the clock dominates.
    expect(bench::calibrate_batch_for_window([] {}) == bench::kMaxBatch,
           "deleted work saturates the window calibrator");

    // --- The plateau calibrator (ungated benches only): a power of two, in range. ---------
    const std::size_t pb = bench::calibrate_batch(spin_op);
    expect(pow2(pb) && pb <= bench::kMaxBatch, "the plateau calibrator returns a doubling");

    // --- Picosecond scale on an operation of known cost. -----------------------------------
    const bench::batch_timing_t k = bench::time_batches(spin_op, 0);
    expect(k.p50_ps >= static_cast<double>(kSpinNs) * 1000.0,
           "a 1 us operation reads at least 1 000 000 ps");
    expect(k.p50_ps < static_cast<double>(kSpinNs) * 1000.0 * 1.5,
           "a 1 us operation reads under 1.5 us (unit is ps, not ns or fs)");
    expect(k.mean_ps >= k.p50_ps * 0.5, "the mean is on the p50's scale");
    expect(pow2(k.batch), "the timed batch is a doubling");

    // --- The percentile accumulator: its convention, exactly. ------------------------------
    bench::Latency lat;
    lat.reserve(100);
    for (std::uint64_t v = 100; v >= 1; --v) lat.add(v);  // 1..100, added out of order
    const bench::Latency::Summary ls = lat.summarize();
    expect(ls.p50 == 51, "p50 is the order statistic at floor(0.5 n)");
    expect(ls.p99 == 100, "p99 is the order statistic at floor(0.99 n)");
    expect(ls.mean == 50, "the mean is the integer mean (5050 / 100)");
    expect(ls.max == 100 && ls.p999 == 100, "p999 at n = 100 is the maximum");
    expect(ls.n == 100 && !ls.tail_ok, "100 samples do not carry a tail");
    expect(bench::Latency{}.summarize().n == 0, "an empty collector summarizes to zeros");
    bench::Latency big;
    for (std::size_t i = 0; i < bench::kTailSampleFloor; ++i) big.add(i);
    expect(big.summarize().tail_ok, "the tail floor's sample count carries a tail");

    // --- The reservation estimate: bounded, and never zero. --------------------------------
    expect(bench::samples_for_budget(tiny_op, 0) == 64, "a zero budget reserves the slack only");
    expect(bench::samples_for_budget([] {}, ~std::uint64_t{0} / 4) == bench::kMaxReservedSamples,
           "an unbounded estimate is capped");

    // Exact arithmetic: 1000 ns over a batch of 256 is 3906.25 ps, not 3 ns.
    expect(bench::per_op_ps(1000, 256) == 3906.25, "per_op_ps keeps the fraction");
    expect(bench::per_op_ps(20'000, 1) == 20'000'000.0, "per_op_ps of a batch of one");

    // Budget 0: the loop still takes exactly the sample floor, every window over the floor.
    const bench::batch_timing_t a = bench::time_batches(tiny_op, 0);
    expect(a.samples == bench::kMinBatchSamples, "a zero budget still takes the sample floor");
    expect(a.min_window_ns >= bench::kMinBatchWindowNs, "every window clears the 20 us floor");
    expect(a.batch >= 1 && a.batch < bench::kMaxBatch, "the calibrated batch is in range");
    // The DCE canary: an operation that does work reads above zero.
    expect(a.p50_ps > 0.0 && a.mean_ps > 0.0, "a real operation does not read as zero");
    expect(a.ops_per_s > 0.0, "throughput is counted");
    // The p50 is one window over its batch, so it cannot be below the floor's share.
    expect(a.p50_ps * static_cast<double>(a.batch) >=
               static_cast<double>(bench::kMinBatchWindowNs) * 1000.0,
           "p50 x batch is at least the window floor");

    // An operation budget ends the loop too, but never before the sample floor.
    const bench::batch_timing_t b = bench::time_batches(tiny_op, 1'000'000'000ULL, 1);
    expect(b.samples == bench::kMinBatchSamples, "max_ops stops at the sample floor");

    // A time budget runs past the floor when there is time for it.
    const bench::batch_timing_t c = bench::time_batches(tiny_op, 20'000'000ULL);
    expect(c.samples > bench::kMinBatchSamples, "a 20 ms budget takes more than the floor");

    // A stall that misleads the calibration must not abort the run (review of #1845): the
    // first timed window comes in short, the batch doubles until the windows clear the floor,
    // and the samples restart at that batch.
    const bench::batch_timing_t s = bench::time_batches(stall_once_op, 0);
    expect(s.recalibrations > 0, "a misled calibration is re-calibrated upward");
    expect(s.batch > 1, "the recovered batch is larger than the misled one");
    expect(s.min_window_ns >= bench::kMinBatchWindowNs, "every kept window clears the floor");
    expect(s.samples == bench::kMinBatchSamples, "samples restart at the recovered batch");

    // A staged loop (#1905): the stage is called, untimed, with the coming window's batch
    // before every window, and the op consumes exactly what it built. Staging costs 50 ns per
    // operation here and the op a few ns, so a rate that counted the staging would read under
    // 20 M ops/s; the windows alone read hundreds of millions.
    std::size_t staged = 0;
    bool short_stage = false;
    const bench::batch_timing_t st = bench::time_staged_batches(
        [&](std::size_t n) {
            if (staged != 0) short_stage = true;  // the last window left values unused
            const std::uint64_t until = bench::now_ns() + n * 50;
            while (bench::now_ns() < until) {
            }
            staged = n;
        },
        [&] {
            if (staged == 0)
                short_stage = true;
            else
                --staged;
            tiny_op();
        },
        5'000'000ULL, std::numeric_limits<std::size_t>::max());
    expect(!short_stage, "every window consumes exactly the batch its stage built");
    expect(st.min_window_ns >= bench::kMinBatchWindowNs, "staged windows clear the floor too");
    expect(st.ops_per_s > 1e8, "a staged rate is over the timed windows, not the staging");

    // The `dce-canary` verdict (#1805): kept work scales with its length, deleted work does not.
    expect(bench::dce_canary_holds(10'000, 75'000, 8, 64), "a 7.5x ratio on 8x work holds");
    expect(!bench::dce_canary_holds(300, 310, 8, 64), "a collapsed ratio (deleted work) fails");
    expect(!bench::dce_canary_holds(0, 0, 8, 64), "a zero reading fails, never divides");
    // ...and the real canary, timed the way the bench row times it, holds on this build.
    const bench::dce_canary_t dce = bench::measure_dce_canary(5'000'000ULL);
    expect(dce.holds, "the dce-canary chains scale with their length");

    const bench::clock_floor_t f = bench::measure_clock_floor();
    expect(f.res_ns > 0.0, "clock_getres reports a resolution");
    expect(f.sample_ns > 0.0, "a timed sample costs something");

    std::printf("batch=%zu p50=%.3f ns window>=%llu ns | clock res %.3f ns, %.3f ns/sample\n",
                a.batch, a.p50_ps / 1e3, static_cast<unsigned long long>(a.min_window_ns), f.res_ns,
                f.sample_ns);
    std::printf("%s\n", g_failed == 0 ? "OK" : "FAILED");
    return g_failed == 0 ? 0 : 1;
}
