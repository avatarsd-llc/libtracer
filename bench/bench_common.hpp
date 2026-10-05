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
#include <cstdlib>
#include <ctime>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace bench {

using Clock = std::chrono::steady_clock;

[[nodiscard]] inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

/**
 * @brief Make the compiler treat @p value as read, and every store before it as needed (#1805).
 *
 * A timed loop whose result nothing reads is dead code to the optimizer: a copy into a block
 * that is freed unread, or a parse whose result only feeds a counter the loop never prints,
 * may be deleted, and the row then times an empty loop. The empty `asm` takes the object's
 * address and clobbers memory, so the object and everything it points to must exist at this
 * point. It emits no instruction. The `dce-canary` row checks it still works.
 */
template <typename T>
inline void do_not_optimize(const T& value) noexcept {
    asm volatile("" : : "r"(&value) : "memory");
}

/**
 * @brief The `dce-canary` verdict: does timed work that grows K-fold time at least half as
 *        much longer?
 *
 * The canary times one dependent chain of @p small_k steps and one of @p big_k steps, each
 * kept by @ref do_not_optimize. Work the compiler kept scales with its length; work it deleted
 * costs the empty loop's floor at both lengths, so the ratio collapses to about 1. Half the
 * ideal ratio is the bar, so loop overhead and the clock floor cannot fail a healthy build.
 *
 * @param small_ps Per-op time of the short chain, in picoseconds.
 * @param big_ps   Per-op time of the long chain, in picoseconds.
 * @param small_k  Steps in the short chain.
 * @param big_k    Steps in the long chain.
 */
[[nodiscard]] constexpr bool dce_canary_holds(double small_ps, double big_ps, std::size_t small_k,
                                              std::size_t big_k) {
    if (small_ps <= 0 || big_ps <= 0 || small_k == 0 || big_k <= small_k) return false;
    const double ideal = static_cast<double>(big_k) / static_cast<double>(small_k);
    return big_ps / small_ps >= ideal / 2;
}

/**
 * @brief CPUs this process may run on: its affinity mask, not the host's CPU count.
 *
 * A multi-threaded row sized from `hardware_concurrency()` puts its threads on however few
 * CPUs the bench is pinned to. They then queue behind each other, and the run reads as
 * CONTENDED (own-cgroup pressure) instead of measuring contention between real cores.
 */
[[nodiscard]] inline std::size_t usable_cpus() {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0) return static_cast<std::size_t>(n);
    }
#endif
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
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

/**
 * @brief The payload ladder every data-path family is swept over (#1806).
 *
 * 64 B is the reference point. 984 and 985 B sit either side of the host heap's small-block
 * boundary for one segment: 984 B plus the 48 B segment header is 1032 B, glibc's largest
 * per-thread-cache request, and 985 B is the first payload past it (#1768). 1 KiB, 4 KiB,
 * 16 KiB and 64 KiB are the standing rows above 1 KiB, so a cost that only appears with
 * large values has a row in every family. A family that already sweeps @ref kSizes runs the
 * ladder sizes it does not already have AFTER its existing rows (@ref ladder_extra), so no
 * existing row moves.
 */
inline constexpr std::size_t kPayloadLadder[] = {64, 984, 985, 1024, 4096, 16384, 65536};

/**
 * @brief The allocator-cliff ladder (#1806): every payload size the cliff family times, and
 *        every size `bench_forward_heap` counts the heap backend's draws at.
 *
 * Two parts, ascending and without duplicates:
 *   - **the fine band**, 960 to 1096 B in steps of 8, plus the first payload past the heap's
 *     one-block boundary. 984 B plus the 48 B segment header is 1032 B, glibc's largest
 *     per-thread-cache request, so 985 B is the first payload past it. That boundary is where
 *     RFC-0028 slice 10 doubled the 1 KiB heap rows (#1768);
 *   - **the power-of-two band**, 2^k and 2^k ± @p header for 2^k from 64 B to 64 KiB, the
 *     sizes where a size-classed allocator changes class.
 *
 * @param header The heap backend's padded segment header (48 B on a 64-bit host).
 */
[[nodiscard]] inline std::vector<std::size_t> cliff_sizes(std::size_t header) {
    constexpr std::size_t kGlibcTcacheMax = 1032;  // glibc's largest per-thread-cache request
    std::vector<std::size_t> out;
    for (std::size_t s = 960; s <= 1096; s += 8) out.push_back(s);
    out.push_back(kGlibcTcacheMax - header + 1);
    for (std::size_t p = 64; p <= 65536; p *= 2) {
        out.push_back(p - header);
        out.push_back(p);
        out.push_back(p + header);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/** @brief Whether @p s is in @ref kSizes, so a ladder sweep can skip a row already emitted. */
[[nodiscard]] constexpr bool in_sizes(std::size_t s) {
    for (std::size_t k : kSizes)
        if (k == s) return true;
    return false;
}

/**
 * @brief The ladder sizes a @ref kSizes sweep does not already emit, in ladder order.
 * @return 984, 985, 4096, 16384 and 65536 for today's @ref kSizes.
 */
[[nodiscard]] inline std::vector<std::size_t> ladder_extra() {
    std::vector<std::size_t> out;
    for (std::size_t s : kPayloadLadder)
        if (!in_sizes(s)) out.push_back(s);
    return out;
}

/**
 * @brief Scale an operation budget down for a payload above 8 KiB, so the 16 KiB and 64 KiB
 *        ladder rows (#1806) cost about what the 8 KiB row does instead of 2-8x more.
 *
 * At or below 8 KiB the budget is returned unchanged, so no existing row's sample count
 * moves. Above it the budget shrinks in proportion to the payload, with a floor of 2000
 * operations, which is @ref publishes_for's own floor.
 */
[[nodiscard]] inline std::uint64_t ladder_budget(std::size_t size, std::uint64_t budget) {
    constexpr std::size_t kFullBudgetBytes = 8192;
    constexpr std::uint64_t kCap = 200'000;  // publishes_for's ceiling at fan-out 1
    if (size <= kFullBudgetBytes) return budget;
    const std::uint64_t scaled = std::min(budget, kCap) * kFullBudgetBytes / size;
    return std::max<std::uint64_t>(2000, scaled);
}

/**
 * @brief Fan-out widths filling the two widest gaps in `kFanouts`, chosen where the
 *        dispatch cost model is expected to BREAK (#844).
 *
 * A publish costs roughly `fixed + F * marginal` (measured on this host: ~118 ns + ~16 ns
 * per delivery on `inproc`), and two arms already determine a straight line. Extra arms
 * therefore earn their keep only where the line has a KINK, and two bands were picked on
 * that basis:
 *
 *   * **16 and 32 — just past the inline/overflow boundary.** `vertex_t::kInlineFanout` is
 *     8, so fan-8 is the LAST width that still snapshots into the raw stack buffer and
 *     fan-9 is the first that spills to the overflow vector (`graph.cpp`'s wide-fan-out
 *     path). The coarse ladder samples that boundary from below (8) and then jumps 16x
 *     past it (128). A per-publish cost paid ONLY on the overflow path decays as 1/F, so
 *     its relative signature is largest just past the boundary and is amortized toward
 *     noise by fan-128. MEASURED: against a build with the per-publish overflow allocation
 *     deliberately reintroduced, fan-16 read 1.101x on the mean and fan-32 1.068x (12/15
 *     and 11/15 interleaved pairs), while fan-1 / 8 / 64 / 128 / 1024 / 8192 all stayed
 *     inside a same-window A/A null. See bench/README.md for the full table.
 *   * **64, 256 and 512 — curve density across the two octave gaps.** RATIONALE, NOT A
 *     MEASUREMENT: no defect is on record that only these arms catch, and fan-64 did NOT
 *     resolve the overflow step above. They are here so the 8 -> 128 and 128 -> 1024 gaps
 *     are sampled at <= 2x steps, which is what lets a wide-band step be LOCATED rather
 *     than only detected — the edge array the fan-out loop streams is
 *     `F * sizeof(edge_view_t)` and outgrows a typical L1 somewhere inside the upper gap,
 *     and #841's regression (per #844's write-up, a published entry grown to ~2x the slot
 *     it projects) was a bytes-streamed defect that the gate saw only as one number at
 *     fan-1024, with no neighbour to place it against.
 *
 * ADDITIVE: these are new `(mode, size, fanout, endpoints)` points, emitted after every
 * pre-existing row. No existing row key, value or ordinal changes — several harnesses
 * (`collate.py`, `render_history.py`, `perf_gate.py`) join on those.
 */
inline constexpr std::size_t kFanoutsMid[] = {16, 32, 64, 256, 512};

/**
 * @brief The TOPIC-COUNT ladder for the #1485 addendum-C scaling arm — decades, on BOTH engines.
 *
 * The pre-existing @ref kEndpoints ladder is octave-spaced and already reaches 8192, and the
 * #1480 fairness audit read its topic-count curve off exactly that. This ladder is not a denser
 * re-run of it: it exists because that audit compared two DIFFERENT operations, and the modes it
 * fed are not separable after the fact. libtracer's `inproc-path` row writes **by address** — a
 * registry resolution inside every timed iteration — while `bench_zenoh`'s row of the same name
 * publishes through a **declared `Publisher`**, which is the bound form and resolves nothing per
 * put. Whatever the audit's 1.57x -> 1.21x narrowing was, part of it is that asymmetry rather
 * than either engine's topic scaling.
 *
 * So the `topics-*` arms sweep this ladder in BOTH spellings on BOTH engines — `topics-bound`
 * (pre-bound handle / declared publisher) and `topics-addr` (resolve the address inside every
 * operation). That makes the comparison a 2x2 which can be read either way round, and makes the
 * resolution term visible as its own difference instead of hidden inside one arm.
 *
 * Three decades, not five: every point pays a full population build on both engines, and this
 * ladder's job is to show a SLOPE across decades, which three points spanning four of them do.
 */
inline constexpr std::size_t kTopicLadder[] = {1, 100, 10000};

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
 * No gated row uses it any more (#1804): every batch row of `bench_libtracer`,
 * `bench_compact_delivery` and `bench_forward_demux` times through @ref time_batches, which
 * sizes its batch by window. It stays for the ungated benches that still call it.
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
 * @brief Timed-window floor (ns) at which the clock's own cost stops moving the per-op figure.
 *
 * Measured on the pinned bench host against `fwd-rope-hop/79/1/1`, a ~238 ns operation, by
 * forcing the batch instead of calibrating it: batch 1 reads **260 ns**, batch 2 **250 ns**,
 * batch 4 **242 ns**, and batch 8 and above **236–240 ns**. The gap is the sample's own two
 * `now_ns()` reads plus the loop, ~**22 ns per sample** on this host. Divided over a window of
 * this length that term is ~0.11 % of the reported figure, an order of magnitude below the
 * ~1 % same-binary agreement the benches are read at.
 */
inline constexpr std::uint64_t kMinBatchWindowNs = 20'000;

/**
 * @brief Calibrate a batch by TARGET WINDOW instead of by plateau — the deterministic twin
 *        of @ref calibrate_batch (#1358).
 *
 * @section why_not_plateau Why the plateau rule is not safe to gate behind
 *
 * @ref calibrate_batch stops at the first batch whose per-op figure is not at least
 * `kBatchPlateau` better than the previous batch's. That test compares two *timed* quantities,
 * so the machine gets a vote in the answer: one disturbed measurement at batch 1 or 2 makes the
 * next doubling look like it did not help, and the calibrator latches a batch far below the one
 * it would have picked a second later. The chosen batch is then a **discrete, per-execution
 * lottery** — and because the clock term is charged per SAMPLE rather than per op, the batch
 * the lottery lands on shifts the reported number: on the pinned host `bench_forward_rope`'s
 * `links=1` point reads 260 / 250 / 242 / ~238 ns at batch 1 / 2 / 4 / >=8. Repeated executions
 * of the same binary were observed latching batches of 2, 4, 8, 16 and 32 on that point, so a
 * same-binary A/A comparison could differ by up to ~8 % with **nothing but the calibrator**
 * between the two arms, in two clusters rather than a spread. Contamination makes it worse in
 * exactly the wrong way: a busy neighbour perturbs the very comparison the plateau rule turns
 * on, so the lottery is most biased precisely when the host is least trustworthy.
 *
 * @section how_window Why a window floor fixes it
 *
 * The plateau rule is only ever a proxy for "the window is long enough that the clock does not
 * matter". Asking that question directly — keep doubling until the measured window reaches
 * @p min_window_ns — removes the comparison, and with it the lottery: the batch becomes a
 * function of the operation's own cost, which is stable to ~1 %, so repeated executions pick
 * the SAME batch. It also self-scales across a sweep's arms, which a hardcoded floor cannot:
 * a 238 ns hop needs 128 iterations to fill the window and a 4.3 µs hop needs 8, and each
 * arm keeps the largest sample count its cost allows rather than paying the slowest arm's batch.
 *
 * @ref calibrate_batch is deliberately left in place and unchanged. Its callers' points are
 * banked in the perf history, and re-calibrating them would step every one of those series for
 * a reason that has nothing to do with the code under test; migration is per-bench and
 * deliberate.
 *
 * @param op             The operation to time; called (many) times, so it must be repeatable.
 * @param min_window_ns  Shortest acceptable timed window; defaults to @ref kMinBatchWindowNs.
 * @return The smallest power-of-two batch whose window reaches @p min_window_ns, clamped to
 *         @ref kMaxBatch.
 */
template <typename Op>
[[nodiscard]] std::size_t calibrate_batch_for_window(
    Op&& op, std::uint64_t min_window_ns = kMinBatchWindowNs) {
    for (std::size_t batch = 1; batch < kMaxBatch; batch *= 2) {
        const std::uint64_t a = now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        if (now_ns() - a >= min_window_ns) return batch;
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
    void reserve(std::size_t n) {
        // Every timed loop reserves before its first sample (#1803), and the reservation is
        // TOUCHED here, not just requested: a reserved-but-untouched vector still page-faults
        // on first write, which would move the faults into the timed window instead of
        // removing them.
        const std::size_t had = samples_.size();
        samples_.reserve(had + n);
        samples_.resize(had + n);
        samples_.resize(had);
    }

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

/** @brief Upper bound on a deadline loop's reservation: 32 MiB of samples. */
inline constexpr std::size_t kMaxReservedSamples = std::size_t{1} << 22;

/**
 * @brief How many samples a deadline-bounded loop will take, for @ref Latency::reserve.
 *
 * A loop that samples until a time budget runs out has no declared count, so it is
 * estimated: the fastest of three untimed probes of one sample's work, divided into the
 * budget, doubled for slack and capped at @ref kMaxReservedSamples. The probes double as
 * warm-up. An under-estimate costs one vector growth inside the loop, which is what every
 * such loop paid before this existed (#1803).
 *
 * @param sample One sample's work (one batch).
 * @param budget_ns The loop's time budget.
 * @return The sample count to reserve.
 */
template <class Sample>
[[nodiscard]] std::size_t samples_for_budget(Sample&& sample, std::uint64_t budget_ns) {
    std::uint64_t best = ~std::uint64_t{0};
    for (int i = 0; i < 3; ++i) {
        const std::uint64_t a = now_ns();
        sample();
        best = std::min(best, now_ns() - a);
    }
    const std::uint64_t est = 2 * budget_ns / std::max<std::uint64_t>(1, best) + 64;
    return static_cast<std::size_t>(std::min<std::uint64_t>(est, kMaxReservedSamples));
}

/**
 * @brief Window the batch calibrator aims for in @ref time_batches: twice the floor.
 *
 * @ref calibrate_batch_for_window stops at the first power-of-two batch whose window reaches
 * its target, so a target equal to the floor leaves the timed windows a few percent above it,
 * and one fast window could dip under. Aiming at twice the floor puts every timed window at
 * 40–80 µs, so the floor assertion in @ref time_batches holds unless the operation gets twice
 * as fast between calibration and timing.
 */
inline constexpr std::uint64_t kBatchWindowTargetNs = 2 * kMinBatchWindowNs;
static_assert(kMinBatchWindowNs >= 20'000, "a batch window under 20 us lets the clock back in");

/**
 * @brief One batch row's per-operation figures, in picoseconds (#1804).
 *
 * Each sample is one timed window divided by its batch, kept as a `double` in picoseconds.
 * The integer `window_ns / batch` it replaces truncated a 3.9 ns operation to 3 ns and moved
 * in whole-nanosecond steps, which on a 3–20 ns row is a 5–30 % grain. There is no p99 here
 * on purpose: a percentile of batch means measures interference between batches, not the
 * tail of one operation.
 */
struct batch_timing_t {
    double p50_ps = 0;               /**< @brief Median per-op time over the samples. */
    double mean_ps = 0;              /**< @brief Mean per-op time over the samples. */
    double ops_per_s = 0;            /**< @brief Operations over the whole timed loop. */
    std::size_t batch = 0;           /**< @brief Operations per timed window. */
    std::size_t samples = 0;         /**< @brief Timed windows behind the figures. */
    std::uint64_t min_window_ns = 0; /**< @brief Shortest timed window (>= the floor). */
    std::size_t recalibrations = 0;  /**< @brief Times a short window doubled the batch. */
};

/**
 * @brief One window's per-operation time in picoseconds: `window_ns * 1000 / batch`, kept
 *        fractional (#1804).
 */
[[nodiscard]] constexpr double per_op_ps(std::uint64_t window_ns, std::size_t batch) {
    return static_cast<double>(window_ns) * 1000.0 / static_cast<double>(batch);
}

/** @brief Fewest windows a batch row takes, whatever its budget, so its p50 is a median. */
inline constexpr std::size_t kMinBatchSamples = 16;

/**
 * @brief Abort the run: a batch window came in under @ref kMinBatchWindowNs at @ref kMaxBatch.
 *
 * A short window at a smaller batch is a misled calibration, and @ref time_batches recovers
 * from it by doubling the batch. At @ref kMaxBatch there is nothing left to double: the
 * operation costs almost nothing, which means the compiler deleted the work or the harness is
 * broken, so the bench stops rather than publish a figure the clock dominates.
 *
 * @param window_ns The window that was too short.
 * @param batch     The batch it timed.
 */
[[noreturn]] inline void window_floor_breached(std::uint64_t window_ns, std::size_t batch) {
    std::fprintf(stderr,
                 "FATAL batch window %llu ns < floor %llu ns (batch %zu): the clock is back in "
                 "the figure\n",
                 static_cast<unsigned long long>(window_ns),
                 static_cast<unsigned long long>(kMinBatchWindowNs), batch);
    std::fflush(stdout);
    std::abort();
}

/**
 * @brief Time @p op in window-calibrated batches and report per-op picoseconds (#1804).
 *
 * The one timing loop every batch row uses. The batch comes from @ref
 * calibrate_batch_for_window aimed at @ref kBatchWindowTargetNs (the calibration doubles as
 * warm-up), and every kept window is at least @ref kMinBatchWindowNs. A window under it
 * means a stall misled the calibration into too small a batch: the batch is doubled and the
 * samples restart, and only a short window at @ref kMaxBatch aborts the run. Sampling stops once @p
 * budget_ns is spent or @p max_ops operations have run, whichever comes first, but never before
 * @ref kMinBatchSamples windows.
 *
 * @param op        The operation; called many times, so it must be repeatable.
 * @param budget_ns Time budget for the timed loop.
 * @param max_ops   Operation budget for the timed loop.
 * @return The per-op figures; see @ref batch_timing_t.
 */
template <typename Op>
[[nodiscard]] batch_timing_t time_batches(
    Op&& op, std::uint64_t budget_ns,
    std::size_t max_ops = std::numeric_limits<std::size_t>::max()) {
    std::size_t batch = calibrate_batch_for_window(op, kBatchWindowTargetNs);
    const auto window = [&] {
        for (std::size_t i = 0; i < batch; ++i) op();
    };
    // Reserved and touched before timing, like Latency::reserve (#1803).
    const std::size_t want =
        std::max(kMinBatchSamples, std::min(samples_for_budget(window, budget_ns),
                                            max_ops / std::max<std::size_t>(1, batch) + 1));
    std::vector<double> ps(want);
    ps.clear();

    std::uint64_t min_window = std::numeric_limits<std::uint64_t>::max();
    std::size_t ops = 0;
    std::uint64_t total = 0;
    std::size_t recalibrations = 0;
    std::uint64_t t0 = now_ns();
    while (ps.size() < kMinBatchSamples || (total < budget_ns && ops < max_ops)) {
        const std::uint64_t a = now_ns();
        window();
        const std::uint64_t w = now_ns() - a;
        if (w < kMinBatchWindowNs) {
            // The calibration was misled — one stall inside a short calibration window makes
            // a small batch look long enough. Re-calibrate upward and start the samples over,
            // so every kept window clears the floor at ONE batch size. Only a batch that has
            // reached kMaxBatch and still runs short is a harness defect (or deleted work).
            if (batch >= kMaxBatch) window_floor_breached(w, batch);
            batch *= 2;
            ++recalibrations;
            ps.clear();
            min_window = std::numeric_limits<std::uint64_t>::max();
            ops = 0;
            total = 0;
            t0 = now_ns();
            continue;
        }
        min_window = std::min(min_window, w);
        ps.push_back(per_op_ps(w, batch));
        ops += batch;
        total = now_ns() - t0;
    }

    batch_timing_t t;
    t.batch = batch;
    t.recalibrations = recalibrations;
    t.samples = ps.size();
    t.min_window_ns = min_window;
    t.ops_per_s = total > 0 ? static_cast<double>(ops) * 1e9 / static_cast<double>(total) : 0.0;
    t.mean_ps = std::accumulate(ps.begin(), ps.end(), 0.0) / static_cast<double>(ps.size());
    // Same order statistic as Latency::summarize (index floor(0.5 * n)), so a batch row's p50
    // keeps the convention its history was recorded under.
    std::sort(ps.begin(), ps.end());
    t.p50_ps = ps[std::min(ps.size() - 1, ps.size() / 2)];
    return t;
}

/** @brief Steps in the `dce-canary` short chain. */
inline constexpr std::size_t kDceSmallK = 8;
/** @brief Steps in the `dce-canary` long chain: eight times the short one. */
inline constexpr std::size_t kDceBigK = 64;

/**
 * @brief @p k dependent mixing steps on @p x: work no compiler can fold into fewer steps.
 *
 * Each step needs the previous one's result (a shift-xor then a multiply), so the chain
 * cannot be vectorized or collapsed into a closed form; its only way to get cheaper is to be
 * deleted, which is what the canary watches for.
 */
[[nodiscard]] constexpr std::uint64_t dce_chain(std::uint64_t x, std::size_t k) {
    for (std::size_t i = 0; i < k; ++i) {
        x ^= x >> 29;
        x *= 0xbf58476d1ce4e5b9ULL;
    }
    return x;
}

/** @brief Both `dce-canary` timings and their verdict (@ref dce_canary_holds). */
struct dce_canary_t {
    batch_timing_t small; /**< @brief The @ref kDceSmallK chain. */
    batch_timing_t big;   /**< @brief The @ref kDceBigK chain. */
    bool holds = false;   /**< @brief The long chain timed at least half its ideal ratio. */
};

/**
 * @brief Time the two canary chains the way every batch row is timed, each result kept only by
 *        @ref do_not_optimize, and judge them.
 * @param budget_ns Timing budget per chain.
 */
[[nodiscard]] inline dce_canary_t measure_dce_canary(std::uint64_t budget_ns) {
    std::uint64_t x = 0x9e3779b97f4a7c15ULL;
    const auto chain = [&](std::size_t k) {
        return time_batches(
            [&] {
                x = dce_chain(x, k);
                do_not_optimize(x);
            },
            budget_ns);
    };
    dce_canary_t c;
    c.small = chain(kDceSmallK);
    c.big = chain(kDceBigK);
    c.holds = dce_canary_holds(c.small.p50_ps, c.big.p50_ps, kDceSmallK, kDceBigK);
    return c;
}

/**
 * @brief The clock floor of this host: the clock's resolution and what one sample costs.
 *
 * Recorded with every run (#1804) so a reader can judge how fine a row can be at all.
 * `res_ns` is `clock_getres(CLOCK_MONOTONIC)`, the clock `std::chrono::steady_clock` reads
 * on Linux. `sample_ns` is measured: the cost of the two @ref now_ns reads that bracket one
 * timed sample, averaged over a run of back-to-back pairs, best of eight runs. A per-op row
 * pays it once per operation; a batch row pays it once per window.
 */
struct clock_floor_t {
    double res_ns = 0;    /**< @brief Reported clock resolution. */
    double sample_ns = 0; /**< @brief Measured cost of one sample's pair of clock reads. */
};

/** @brief Measure @ref clock_floor_t on this host, now. */
[[nodiscard]] inline clock_floor_t measure_clock_floor() {
    clock_floor_t f;
    timespec res{};
    if (clock_getres(CLOCK_MONOTONIC, &res) == 0)
        f.res_ns = static_cast<double>(res.tv_sec) * 1e9 + static_cast<double>(res.tv_nsec);
    constexpr std::size_t kPairs = 4096;
    volatile std::uint64_t sink = 0;
    double best = std::numeric_limits<double>::max();
    for (int run = 0; run < 8; ++run) {
        const std::uint64_t a = now_ns();
        for (std::size_t i = 0; i < kPairs; ++i) {
            const std::uint64_t s0 = now_ns();
            sink = sink + (now_ns() - s0);
        }
        best = std::min(best, static_cast<double>(now_ns() - a) / static_cast<double>(kPairs));
    }
    f.sample_ns = best;
    return f;
}

/**
 * @brief Print this host's clock floor as one `CLOCK` line (#1804).
 *
 *     CLOCK res_ns sample_ns
 *
 * Tab-separated, on stdout ahead of the run's rows. Every RESULT parser tests the first
 * field, so the line is skipped by all of them; `host_guard.py stamp --clock-from` reads it
 * into the store's host descriptor, which the performance page shows on every point.
 */
inline void emit_clock_floor() {
    const clock_floor_t f = measure_clock_floor();
    std::printf("CLOCK\t%.3f\t%.3f\n", f.res_ns, f.sample_ns);
    std::fflush(stdout);
}

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
 * @brief Publish a batch row (#1804): p50 and mean in nanoseconds to the picosecond, no p99.
 *
 * Same 12-column RESULT line as @ref emit, so every parser joins it unchanged; the two
 * latency columns carry three decimals (picoseconds) instead of whole nanoseconds, and every
 * parser reads them as floats. The p99 column is 0, which the history emitter and the gate
 * read as "this row does not produce that metric". A `NOTE` line follows with the batch, the
 * sample count and the shortest window, so the row states its own resolution.
 */
inline void emit_batch(const char* system, const char* mode, std::size_t size_bytes,
                       std::size_t fanout, std::size_t endpoints, double pub_per_s,
                       double deliv_per_s, double mb_per_s, const batch_timing_t& t) {
    std::printf("RESULT\t%s\t%s\t%zu\t%zu\t%zu\t%.0f\t%.0f\t%.1f\t%.3f\t0\t%.3f\n", system, mode,
                size_bytes, fanout, endpoints, pub_per_s, deliv_per_s, mb_per_s, t.p50_ps / 1e3,
                t.mean_ps / 1e3);
    std::printf(
        "NOTE mode=%s size=%zu fan=%zu ep=%zu batch=%zu samples=%zu min_window_ns=%llu "
        "recalibrations=%zu\n",
        mode, size_bytes, fanout, endpoints, t.batch, t.samples,
        static_cast<unsigned long long>(t.min_window_ns), t.recalibrations);
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
