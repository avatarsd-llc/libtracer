/**
 * @file
 * @brief Shared benchmark scaffolding: a steady-clock timer, a latency-percentile accumulator, the
 *        swept dimensions (payload size, subscriber fan-out, endpoint count), and a machine-
 *        parseable RESULT line that collate.py renders into a side-by-side table.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Kept tiny and dependency-free so the libtracer and Zenoh
 * harnesses emit identical, comparable output.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

namespace bench {

using Clock = std::chrono::steady_clock;

[[nodiscard]] inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

/**
 * @brief The three swept axes (the user's matrix).
 *
 * Each sweep holds two fixed while
 * varying the third; the mixed workload combines them.
 */
inline constexpr std::size_t kSizes[] = {1, 8, 64, 1024, 8192};       // payload bytes
inline constexpr std::size_t kFanouts[] = {1, 8, 128, 1024, 8192};    // subscribers / endpoint
inline constexpr std::size_t kEndpoints[] = {1, 8, 128, 1024, 8192};  // distinct topics

/** @brief Fixed points used while sweeping a different axis. */
inline constexpr std::size_t kRefSize = 64;
inline constexpr std::size_t kRefFanout = 1;
inline constexpr std::size_t kRefEndpoints = 1;

/**
 * @brief Keep wall-clock bounded + the comparison fair: target a roughly constant number of
 *        *deliveries* per run, so high fan-out does proportionally fewer publishes.
 */
inline constexpr std::uint64_t kDeliveryBudget = 2'000'000;
inline constexpr std::uint64_t kLatencyDeliveryBudget = 200'000;

[[nodiscard]] inline std::size_t publishes_for(std::size_t fanout, std::uint64_t budget) {
    const std::uint64_t n = budget / std::max<std::uint64_t>(1, fanout);
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(n, 2000, 200000));
}

/**
 * @brief Growth factor at which the batch is deemed large enough (5%).
 *
 * Doubling the batch stops lowering the per-op figure once the clock's own cost is
 * amortized away; the first batch whose per-op cost is not at least this much better
 * than the previous one is that plateau.
 */
inline constexpr double kBatchPlateau = 0.05;

/** @brief Hard ceiling on the calibrated batch — a guard against a pathological plateau. */
inline constexpr std::size_t kMaxBatch = 1U << 20;

/**
 * @brief Smallest batch size at which per-op timing stops measuring the clock.
 *
 * An operation costing the same order as `clock_gettime` cannot be timed one at a time:
 * the two clock reads and the clock's own granularity dominate the window, and the
 * reported percentiles snap to coarse steps. Timing a BATCH of @p op and dividing
 * amortizes both away. The right batch size is host-dependent (a fast TSC needs a
 * smaller batch than a syscall-backed clock), so it is calibrated against the host's own
 * clock rather than hardcoded: double the batch until the per-op figure stops improving.
 *
 * Hoisted here from bench_compact_delivery.cpp (#553) so the in-process bench can use
 * the same calibrator the net-plane benches already do, and so there is ONE definition of
 * "large enough" across the harness.
 *
 * @param op The operation to time; called (many) times, so it must be repeatable.
 * @return The calibrated batch size, clamped to @ref kMaxBatch.
 */
template <typename Op>
[[nodiscard]] std::size_t calibrate_batch(Op&& op) {
    double prev = 0.0;
    for (std::size_t batch = 1; batch <= kMaxBatch; batch *= 2) {
        const std::uint64_t a = now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        const double per_op = static_cast<double>(now_ns() - a) / static_cast<double>(batch);
        if (prev > 0.0 && per_op > prev * (1.0 - kBatchPlateau)) return batch;
        prev = per_op;
    }
    return kMaxBatch;
}

/**
 * @brief Smallest sample count at which a p999 describes a distribution rather than one draw.
 *
 * The reported p999 is the order statistic at index `floor(0.999 * n)`, so the number of
 * samples strictly ABOVE it is `n - 1 - floor(0.999 * n)`. That count is the whole basis of
 * the estimate, and it is brutal at small `n`:
 *
 * | n      | p999 index | samples above it | what the figure actually is |
 * |--------|-----------|------------------|-----------------------------|
 * | 500    | 499       | 0                | `max(samples)` — one draw   |
 * | 1000   | 999       | 0                | `max(samples)` — one draw   |
 * | 4000   | 3996      | 3                | the 4th-largest of 4000     |
 * | 10000  | 9990      | 9                | the 10th-largest            |
 * | 100000 | 99900     | 99               | the 100th-largest           |
 *
 * For any `n <= 1000` the p999 IS the maximum — a single unbounded draw wearing a
 * percentile's name. Ten samples in the tail is the conventional minimum at which the
 * estimate stops being one sample, which puts the floor at 10 000 PER POOLED COLLECTOR.
 * @ref Latency::Summary carries `n` and `tail_ok` so no reader can take a p999 without also
 * seeing whether it cleared this bar.
 */
inline constexpr std::size_t kTailSampleFloor = 10000;

/**
 * @brief Exact-quantile latency accumulator.
 *
 * @section quantile_storage Storage — every sample is kept
 *
 * Samples go into an unbounded `std::vector` and the quantiles are read off the fully
 * sorted array. There is deliberately NO reservoir and NO histogram: both trade the tail
 * away for a bounded footprint, and the tail is the quantity being measured. A reservoir of
 * size k drops samples once `n > k`, which is precisely when a p999 starts to mean
 * something; a histogram quantizes the tail to its bucket width, which is widest exactly
 * where the tail lives. The cost of keeping everything is 8 bytes per sample — 800 KB at
 * 100 k samples, which no bench-host budget notices.
 *
 * @section quantile_convention Quantile convention
 *
 * `at(p)` returns the order statistic at 0-based index `floor(p * n)`, clamped to `n - 1`.
 * This differs from the nearest-rank definition (`ceil(p * n) - 1`) in exactly one case:
 * when `p * n` is an integer, this reads ONE ELEMENT HIGHER. The convention is kept as-is
 * because the whole recorded p50/p99 history was measured under it; the bias is toward
 * over-reporting the tail, which is the safe direction for a latency claim. It never
 * truncates: `floor(0.999 * n) <= n - 1` for every `n >= 1`, so the top of the distribution
 * is always reachable.
 */
class Latency {
   public:
    void add(std::uint64_t ns) { samples_.push_back(ns); }

    /**
     * @brief Pre-size the sample store.
     *
     * Only ever an optimisation for the CALLER's benefit, never for the numbers: when the
     * collector is fed from the thread under test (a transport's poll thread, say), a
     * `push_back` that reallocates charges a `memcpy` of the whole sample history to that
     * thread and shows up as a latency spike the transport did not cause. Reserving the
     * worst-case sample count up front removes that artefact.
     */
    void reserve(std::size_t n) { samples_.reserve(n); }

    /** @brief Absorb another collector's samples — for pooling per-thread collectors.
     *
     * A multi-threaded runner must not have one thread do the timing on everyone's behalf:
     * the instrumented thread runs a different (slower) loop than the rest, so its latency
     * and the run's throughput then describe two different workloads. Each thread keeps its
     * own collector and merges here, which keeps the timed loop identical on every thread.
     *
     * Concatenation, not decimation — the merged pool holds every sample both sides held,
     * so a tail sample seen by one thread survives into the pooled p999. */
    void merge(const Latency& other) {
        samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
    }

    /**
     * @brief One collector's distribution.
     *
     * The first three members keep their historical order and meaning: `emit()`'s RESULT
     * line and every aggregate-initialised `Summary{a, b, c}` in the harness depend on it.
     * The tail members are appended, defaulted, and surfaced on their own output line.
     */
    struct Summary {
        std::uint64_t p50 = 0, p99 = 0, mean = 0;
        std::uint64_t p999 = 0; /**< 99.9th percentile — read `tail_ok` before believing it. */
        std::uint64_t max = 0; /**< The single worst sample; p999 degenerates to this at n<=1000. */
        std::size_t n = 0;     /**< Samples behind the figures above. */
        bool tail_ok = false;  /**< `n >= kTailSampleFloor` — is the p999 worth reporting? */
    };

    [[nodiscard]] Summary summarize() {
        if (samples_.empty()) return {};
        std::sort(samples_.begin(), samples_.end());
        const std::size_t n = samples_.size();
        const auto at = [&](double p) {
            return samples_[std::min(n - 1, static_cast<std::size_t>(p * static_cast<double>(n)))];
        };
        const std::uint64_t sum =
            std::accumulate(samples_.begin(), samples_.end(), std::uint64_t{0});
        return {at(0.50), at(0.99), sum / n, at(0.999), samples_[n - 1], n, n >= kTailSampleFloor};
    }

    [[nodiscard]] std::size_t size() const { return samples_.size(); }

   private:
    std::vector<std::uint64_t> samples_;
};

/*
 * One comparable measurement. `mode` distinguishes the path / module composition
 * (libtracer inproc / inproc-borrow / loopback; zenoh inproc / net). pub_per_s is
 * the publish rate; deliv_per_s = pub_per_s * fanout (the work done); latency is
 * per-publish wall time (for inproc, includes all fan-out callbacks inline).
 */
inline void emit(const char* system, const char* mode, std::size_t size_bytes, std::size_t fanout,
                 std::size_t endpoints, double pub_per_s, double deliv_per_s, double mb_per_s,
                 const Latency::Summary& lat) {
    std::printf("RESULT\t%s\t%s\t%zu\t%zu\t%zu\t%.0f\t%.0f\t%.1f\t%llu\t%llu\t%llu\n", system, mode,
                size_bytes, fanout, endpoints, pub_per_s, deliv_per_s, mb_per_s,
                static_cast<unsigned long long>(lat.p50), static_cast<unsigned long long>(lat.p99),
                static_cast<unsigned long long>(lat.mean));
    std::fflush(stdout);
}

/**
 * @brief Tail companion to @ref emit — p999, max, and the sample count that backs them.
 *
 * @section why_own_line Why this is a SEPARATE line and not two more RESULT columns
 *
 * Every parser gates on the RESULT line's exact arity — `collate.py`, `render_compare.py`,
 * `perf_emit_benchmark.py`, `perf_gate.py` (twice) and `gen_results_page.py` all test
 * `len(f) == 12`. Appending a 13th field would not fail
 * loudly; it would make every one of them match ZERO rows, so the comparison tables would
 * render empty and `perf_gate.py` would find no points to gate — a regression gate that
 * passes because it measured nothing. A tail column is not worth that, so the tail rides its
 * own `RESULT_TAIL` tag. Every existing parser skips it on the `f[0] == "RESULT"` test,
 * including `perf_gate.py`'s `^RESULT ` memory regex (which requires a following SPACE).
 *
 * @warning This line is ALSO exactly twelve fields wide, so the arity test alone does NOT
 * separate the two tags — only the `f[0]` test does. Every parser in the tree pairs the two
 * checks today and is therefore safe, but a new parser written as `if len(f) == 12:` with no
 * tag test would silently ingest tail rows as results and read `n` where it wanted `pub/s`.
 * Test the tag first. The widths are not kept equal on purpose and must not be relied on.
 *
 * Keyed by the same (system, mode, size, fanout, endpoints) tuple as the RESULT line it
 * follows, so the two join without ambiguity. `render_compare.py` performs exactly that
 * join, and it is the only parser that reads both tags.
 *
 *     RESULT_TAIL sys mode size fan ep n p50ns p99ns p999ns maxns tail_ok
 *
 * `tail_ok` is 1 only when `n >= kTailSampleFloor`. It is printed rather than used to
 * suppress the row because a suppressed row reads as "no tail problem here", whereas
 * `tail_ok=0` next to the number says what it is: an order statistic too close to the top of
 * too small a sample to carry a claim.
 */
inline void emit_tail(const char* system, const char* mode, std::size_t size_bytes,
                      std::size_t fanout, std::size_t endpoints, const Latency::Summary& lat) {
    std::printf("RESULT_TAIL\t%s\t%s\t%zu\t%zu\t%zu\t%zu\t%llu\t%llu\t%llu\t%llu\t%d\n", system,
                mode, size_bytes, fanout, endpoints, lat.n,
                static_cast<unsigned long long>(lat.p50), static_cast<unsigned long long>(lat.p99),
                static_cast<unsigned long long>(lat.p999), static_cast<unsigned long long>(lat.max),
                lat.tail_ok ? 1 : 0);
    std::fflush(stdout);
}

/**
 * @brief Response-surface grid (system dynamics).
 *
 * Log-spaced axes: a 7x7 grid, dense enough for a smooth libtracer-vs-Zenoh curve
 * yet keeping the (zenoh-bound) wall-clock sane. Two slices: size x fanout
 * (endpoints=1, mode `inproc`) and size x endpoints (fanout=1, mode `inproc-path`).
 * `grid` emits the same mode-tagged RESULT line as the default run (see emit()), so
 * bench/render_compare.py draws the docs comparison charts from one parser.
 */
inline constexpr std::size_t kGridSizes[] = {1, 16, 64, 256, 1024, 4096, 8192};
inline constexpr std::size_t kGridFanouts[] = {1, 4, 16, 64, 256, 1024, 4096};
inline constexpr std::size_t kGridEndpoints[] = {1, 4, 16, 64, 256, 1024, 4096};
inline constexpr std::uint64_t kGridBudget = 500'000;
inline constexpr std::uint64_t kGridLatBudget = 40'000;

}  // namespace bench
