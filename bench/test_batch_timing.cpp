/**
 * @file
 * @brief Self-test for the batch-row estimator in `bench_common.hpp` (#1804): picosecond
 *        resolution, the 20 µs window floor, the sample floor, and the clock floor.
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
#include <cstddef>
#include <cstdint>
#include <cstdio>

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

}  // namespace

int main() {
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

    const bench::clock_floor_t f = bench::measure_clock_floor();
    expect(f.res_ns > 0.0, "clock_getres reports a resolution");
    expect(f.sample_ns > 0.0, "a timed sample costs something");

    std::printf("batch=%zu p50=%.3f ns window>=%llu ns | clock res %.3f ns, %.3f ns/sample\n",
                a.batch, a.p50_ps / 1e3, static_cast<unsigned long long>(a.min_window_ns), f.res_ns,
                f.sample_ns);
    std::printf("%s\n", g_failed == 0 ? "OK" : "FAILED");
    return g_failed == 0 ? 0 : 1;
}
