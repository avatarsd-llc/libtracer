/**
 * @file
 * @brief Validates the shared latency accumulator's TAIL estimator against analytically known
 *        distributions — the instrument that measures the instrument.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section why Why this exists
 *
 * A tail estimator that silently truncates is worse than no tail estimator at all: it answers
 * the question with a number that looks like a measurement. Before `p999` appears on any chart
 * or in any claim, the estimator behind it has to be shown to reproduce a tail it was GIVEN.
 *
 * So this feeds @ref bench::Latency distributions whose quantiles are known by construction —
 * two-point mixtures, three-tier mixtures, dense ramps, and a single-outlier case — and prints
 * measured against analytic. Nothing here touches the network, the clock, or libtracer; a
 * failure is a defect in the estimator and nowhere else.
 *
 * @section conventions The two conventions, and why the table has three columns
 *
 * `Latency::summarize()` reads the order statistic at 0-based index `floor(p * n)`. The
 * textbook nearest-rank definition reads `ceil(p * n) - 1`. Those agree for every `p * n` that
 * is not an integer and differ by exactly one rank when it is, so the table reports the
 * measured value against BOTH: `inv-CDF` (smallest x with F(x) >= p, the definition a reader
 * assumes) and `rank+1` (what the repo convention yields). A row that matches `inv-CDF` is
 * unambiguously right; a row that matches only `rank+1` is on an exact-integer boundary and is
 * over-reporting the tail by one order statistic, which is the safe direction but must be
 * visible rather than assumed.
 *
 * @section run Running it
 *
 *     cmake --build bench/build --target bench_tail_validate -j
 *     ./bench/build/bench_tail_validate
 *
 * Exit status is the number of FAIL rows, so it can gate.
 */

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "bench_net.hpp"

namespace {

using bench::Latency;

/** @brief One weighted step of a synthetic distribution: @p count samples all worth @p ns. */
struct step_t {
    std::uint64_t ns = 0;
    std::size_t count = 0;
};

/** @brief Materialise a step distribution into a flat sample vector (ascending). */
[[nodiscard]] std::vector<std::uint64_t> materialize(const std::vector<step_t>& steps) {
    std::vector<std::uint64_t> out;
    std::size_t total = 0;
    for (const step_t& s : steps) total += s.count;
    out.reserve(total);
    for (const step_t& s : steps) out.insert(out.end(), s.count, s.ns);
    return out;
}

/**
 * @brief The analytic quantile by the inverse-CDF definition: smallest x with F(x) >= p.
 *
 * Computed from the STEP TABLE, not from the sample vector — so it is an independent
 * derivation of the answer rather than a second implementation of the thing under test.
 */
[[nodiscard]] std::uint64_t analytic_inv_cdf(const std::vector<step_t>& steps, double p) {
    std::size_t total = 0;
    for (const step_t& s : steps) total += s.count;
    double cum = 0.0;
    for (const step_t& s : steps) {
        cum += static_cast<double>(s.count) / static_cast<double>(total);
        // A tolerance is required, not optional: 0.999 has no exact binary representation, so
        // an exactly-0.999 cumulative mass can land a few ULPs below p and skip the right step.
        if (cum >= p - 1e-12) return s.ns;
    }
    return steps.empty() ? 0 : steps.back().ns;
}

/** @brief The order statistic the repo's `floor(p * n)` convention selects, from sorted data. */
[[nodiscard]] std::uint64_t rank_plus_one(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const std::size_t n = sorted.size();
    return sorted[std::min(n - 1, static_cast<std::size_t>(p * static_cast<double>(n)))];
}

int g_fail = 0;

/** @brief Print one measured-vs-analytic row and score it. */
void row(const std::string& name, const char* metric, std::uint64_t measured, std::uint64_t inv_cdf,
         std::uint64_t rank1) {
    const char* verdict = nullptr;
    if (measured == inv_cdf)
        verdict = (inv_cdf == rank1) ? "PASS" : "PASS (inv-CDF)";
    else if (measured == rank1)
        verdict = "PASS boundary +1rank";
    else {
        verdict = "**FAIL**";
        ++g_fail;
    }
    std::printf("  %-26s %-6s %14llu %14llu %14llu   %s\n", name.c_str(), metric,
                static_cast<unsigned long long>(measured), static_cast<unsigned long long>(inv_cdf),
                static_cast<unsigned long long>(rank1), verdict);
}

/**
 * @brief Push a distribution through @ref bench::Latency and check every reported quantile.
 *
 * @param shuffle   Feed the samples in random order — the estimator sorts, so a shuffled feed
 *                  must produce bit-identical output. Catches any accidental dependence on
 *                  arrival order.
 * @param split     Number of per-thread collectors to spread the samples across before
 *                  `merge()`-ing them. 1 exercises the direct path; >1 exercises the pooled
 *                  path, where a decimating merge would quietly eat the tail.
 */
void check(const std::string& name, const std::vector<step_t>& steps, bool shuffle,
           std::size_t split) {
    std::vector<std::uint64_t> flat = materialize(steps);
    std::vector<std::uint64_t> sorted = flat;
    std::sort(sorted.begin(), sorted.end());

    if (shuffle) {
        std::mt19937_64 rng(0xC0FFEEULL);
        std::shuffle(flat.begin(), flat.end(), rng);
    }

    Latency pooled;
    if (split <= 1) {
        for (const std::uint64_t v : flat) pooled.add(v);
    } else {
        std::vector<Latency> parts(split);
        for (std::size_t i = 0; i < flat.size(); ++i) parts[i % split].add(flat[i]);
        for (const Latency& p : parts) pooled.merge(p);
    }

    const Latency::Summary s = pooled.summarize();

    if (s.n != flat.size()) {
        std::printf("  %-26s n=%zu but fed %zu   **FAIL — SAMPLES LOST**\n", name.c_str(), s.n,
                    flat.size());
        ++g_fail;
    }
    row(name, "p50", s.p50, analytic_inv_cdf(steps, 0.50), rank_plus_one(sorted, 0.50));
    row(name, "p99", s.p99, analytic_inv_cdf(steps, 0.99), rank_plus_one(sorted, 0.99));
    row(name, "p999", s.p999, analytic_inv_cdf(steps, 0.999), rank_plus_one(sorted, 0.999));
    row(name, "max", s.max, sorted.back(), sorted.back());
}

/** @brief A dense 1..n ramp — every sample distinct, so an off-by-one shows up as off-by-1ns. */
[[nodiscard]] std::vector<step_t> ramp(std::size_t n) {
    std::vector<step_t> steps;
    steps.reserve(n);
    for (std::size_t i = 1; i <= n; ++i) steps.push_back({static_cast<std::uint64_t>(i), 1});
    return steps;
}

/**
 * @brief The sample-count floor, shown rather than asserted.
 *
 * Prints, for each n, the index `floor(0.999 * n)` the estimator will actually select (computed
 * the same way the estimator computes it, floating-point rounding included), how many samples
 * sit strictly above it, and whether the reported p999 collapses onto `max`.
 */
void sample_floor_table() {
    std::printf(
        "\nSAMPLE-COUNT FLOOR — what a p999 is made of at each n (ramp 1..n, so value == index+1)\n"
        "'in top 0.1%%' = n - floor(0.999*n) = samples at or above the reported p999\n"
        "%-10s %-12s %-14s %-12s %-12s %s\n",
        "n", "p999 index", "in top 0.1%", "p999", "max", "verdict");
    for (const std::size_t n : {100UL, 500UL, 1000UL, 1001UL, 4000UL, 10000UL, 40000UL, 100000UL}) {
        Latency lat;
        for (std::size_t i = 1; i <= n; ++i) lat.add(static_cast<std::uint64_t>(i));
        const Latency::Summary s = lat.summarize();
        const std::size_t idx =
            std::min(n - 1, static_cast<std::size_t>(0.999 * static_cast<double>(n)));
        const std::size_t in_tail = n - idx;
        const char* verdict = s.p999 == s.max ? "DEGENERATE — p999 IS max, a single draw"
                              : in_tail < 10  ? "THIN — top 0.1% holds fewer than 10 samples"
                                              : "OK";
        std::printf("%-10zu %-12zu %-14zu %-12llu %-12llu %s%s\n", n, idx, in_tail,
                    static_cast<unsigned long long>(s.p999), static_cast<unsigned long long>(s.max),
                    verdict, s.tail_ok ? "  [tail_ok=1]" : "  [tail_ok=0]");
    }
}

/**
 * @brief End-to-end check of the PRODUCTION wiring, not just the estimator in isolation.
 *
 * The estimator passing every synthetic above proves nothing about the path the net bench
 * actually takes: `bench::net::SubState::on_payload` reconstructs each latency from a
 * timestamp embedded in the payload, and the `RESULT_TAIL` line is formatted somewhere else
 * again. An instrument validated only at the library seam is how a correct estimator ships
 * behind a mis-wired emitter, so this drives real payloads through `SubState`, captures its
 * stdout, and parses the `RESULT_TAIL` line back.
 *
 * Latencies are injected by back-dating the payload timestamp, so the recovered value is
 * `injected + the cost of on_payload's own clock read`. That is a few tens of ns of positive
 * jitter, which is why this section checks BANDS and exact counts rather than exact
 * nanoseconds — the exact-value proof is the synthetic table above.
 */
void wiring_check() {
    std::printf("\nEND-TO-END WIRING — synthetic tail pushed through bench::net::SubState\n");

    constexpr std::size_t kN = 20000;
    constexpr std::uint64_t kFast = 100;    // ns — the bulk
    constexpr std::uint64_t kSlow = 10000;  // ns — the injected tail
    // 0.5%, deliberately NOT the 1% that would put the step exactly on the p99 index. The
    // convention's +1-rank behaviour at an exact boundary is already pinned to the nanosecond
    // by the synthetic table; repeating it here would only make this check ambiguous about
    // which of the two things it is testing. (It was 1% first, and this check correctly failed
    // on p99 — the failure was the expectation's, not the instrument's.)
    constexpr std::size_t kSlowCount = 100;

    // Redirect at the FILE DESCRIPTOR, not with freopen(..., stdout): freopen REUSES the same
    // FILE object, so a saved `FILE*` aliases the stream being redirected and closing it
    // destroys the original. dup/dup2 swaps the descriptor underneath an untouched `stdout`.
    const char* const path = "/tmp/claude-1000/judgement/tail_wiring.txt";
    std::fflush(stdout);
    const int saved_fd = ::dup(STDOUT_FILENO);
    const int cap_fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (saved_fd < 0 || cap_fd < 0) {
        std::printf("  (could not capture stdout — SKIPPED)\n");
        return;
    }
    ::dup2(cap_fd, STDOUT_FILENO);
    {
        bench::net::SubState st("validate", "synthetic");
        std::vector<std::uint8_t> buf;
        const auto feed = [&](std::uint64_t back_ns, bench::net::Phase ph) {
            bench::net::make_payload(buf, 64, ph);
            const std::uint64_t ts = bench::now_ns() - back_ns;
            for (int i = 0; i < 8; ++i) buf[static_cast<std::size_t>(i)] = (ts >> (8 * i)) & 0xFF;
            st.on_payload(std::as_bytes(std::span<const std::uint8_t>(buf)));
        };
        for (std::size_t i = 0; i < kN; ++i)
            feed(i < kN - kSlowCount ? kFast : kSlow, bench::net::kLatency);
        feed(0, bench::net::kThroughput);  // one, so the EOF branch emits at all
        feed(0, bench::net::kEof);
    }
    std::fflush(stdout);
    ::dup2(saved_fd, STDOUT_FILENO);
    ::close(saved_fd);
    ::close(cap_fd);

    std::vector<std::string> fields;
    if (std::FILE* const f = std::fopen(path, "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f) != nullptr) {
            std::string s(line);
            if (s.rfind("RESULT_TAIL\t", 0) != 0) continue;
            fields.clear();
            std::size_t start = 0;
            while (start <= s.size()) {
                const std::size_t tab = s.find('\t', start);
                const std::size_t end = tab == std::string::npos ? s.find('\n', start) : tab;
                fields.push_back(s.substr(start, end - start));
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
        }
        std::fclose(f);
    }

    if (fields.size() != 12) {
        std::printf("  RESULT_TAIL not found or wrong arity (%zu fields)   **FAIL**\n",
                    fields.size());
        ++g_fail;
        return;
    }
    const auto num = [&](std::size_t i) { return std::strtoull(fields[i].c_str(), nullptr, 10); };
    const std::uint64_t n = num(6), p50 = num(7), p99 = num(8), p999 = num(9), mx = num(10),
                        ok = num(11);
    std::string joined;
    for (const std::string& f : fields) joined += f + " ";
    std::printf("  emitted: %s\n", joined.c_str());
    const auto band = [&](const char* what, std::uint64_t v, std::uint64_t lo, std::uint64_t hi) {
        const bool good = v >= lo && v <= hi;
        if (!good) ++g_fail;
        std::printf("  %-24s %12llu   expect [%llu..%llu]   %s\n", what,
                    static_cast<unsigned long long>(v), static_cast<unsigned long long>(lo),
                    static_cast<unsigned long long>(hi), good ? "PASS" : "**FAIL**");
    };
    band("n (samples)", n, kN, kN);
    band("tail_ok", ok, 1, 1);
    band("p50 ~ fast tier", p50, kFast, kFast + 500);
    band("p99 ~ fast tier", p99, kFast, kFast + 500);
    band("p999 ~ slow tier", p999, kSlow, kSlow + 500);
    band("max ~ slow tier", mx, kSlow, kSlow + 500);
}

/**
 * @brief Floating-point index audit.
 *
 * `static_cast<size_t>(0.999 * n)` truncates, and 0.999 is not representable in binary. If the
 * product lands a hair below the intended integer the estimator silently reads one rank LOW —
 * an under-report, the unsafe direction. This prints the realized index against the exact
 * integer for the n values the harness actually uses.
 */
void fp_index_audit() {
    std::printf(
        "\nFLOATING-POINT INDEX AUDIT — does `(size_t)(p * n)` land where it should?\n"
        "%-10s %-8s %-22s %-14s %s\n",
        "n", "p", "p*n (exact rational)", "realized idx", "verdict");
    for (const std::size_t n : {4000UL, 10000UL, 20000UL, 100000UL}) {
        for (const double p : {0.99, 0.999}) {
            const auto num = static_cast<std::uint64_t>(p == 0.99 ? 99 : 999);
            const auto den = static_cast<std::uint64_t>(p == 0.99 ? 100 : 1000);
            const std::uint64_t exact = static_cast<std::uint64_t>(n) * num / den;
            const bool exact_div = (static_cast<std::uint64_t>(n) * num) % den == 0;
            const auto realized = static_cast<std::uint64_t>(p * static_cast<double>(n));
            const bool ok = realized == exact;
            if (!ok) ++g_fail;
            std::printf(
                "%-10zu %-8.3f %-22llu %-14llu %s%s\n", n, p,
                static_cast<unsigned long long>(exact), static_cast<unsigned long long>(realized),
                ok ? "ok" : "**FAIL — reads one rank LOW**", exact_div ? "  (exact boundary)" : "");
        }
    }
}

}  // namespace

int main() {
    std::printf(
        "TAIL ESTIMATOR VALIDATION — bench::Latency (bench/bench_common.hpp)\n"
        "storage: unbounded std::vector, exact order statistics (no reservoir, no histogram)\n"
        "convention: 0-based index floor(p*n), clamped to n-1\n\n"
        "  %-26s %-6s %14s %14s %14s   %s\n",
        "distribution", "metric", "measured", "inv-CDF", "rank+1", "verdict");

    // The distribution the brief names: 99% at 100 ns, 1% at 10 us. p*n is an exact integer
    // here AND the mixture steps exactly there, so this is the worst case for the convention.
    check("99%@100ns + 1%@10us", {{100, 99000}, {10000, 1000}}, false, 1);

    // The same shape moved OFF the boundary: the step sits at 99.5%, so p99 and p999 both fall
    // strictly inside a run of equal values and no convention question arises.
    check("99.5%@100ns + 0.5%@10us", {{100, 99500}, {10000, 500}}, false, 1);

    // Three tiers — a p99 and a p999 that must land on DIFFERENT values, which is the whole
    // reason for adding p999 in the first place.
    check("3-tier 99/0.9/0.1", {{100, 99000}, {1000, 900}, {10000, 100}}, false, 1);

    // Dense ramps: every sample distinct, so an index error surfaces as a 1 ns discrepancy.
    check("ramp 1..100000", ramp(100000), false, 1);
    check("ramp 1..99991 (no bdry)", ramp(99991), false, 1);

    // One catastrophic outlier in 100 000. p999 must NOT see it (it is 1-in-100 000), but max
    // must — this is the direct "did the estimator throw the tail away" test.
    check("99999@100ns + 1@1s", {{100, 99999}, {1000000000, 1}}, false, 1);

    // Same data, shuffled arrival order, and the same data spread across 24 per-thread
    // collectors then merged. Both must reproduce the unshuffled single-collector answer.
    check("99.5/0.5 shuffled", {{100, 99500}, {10000, 500}}, true, 1);
    check("99.5/0.5 merged x24", {{100, 99500}, {10000, 500}}, true, 24);
    check("3-tier merged x24", {{100, 99000}, {1000, 900}, {10000, 100}}, true, 24);

    fp_index_audit();
    wiring_check();
    sample_floor_table();

    std::printf("\n%d FAIL row(s).\n", g_fail);
    return g_fail;
}
