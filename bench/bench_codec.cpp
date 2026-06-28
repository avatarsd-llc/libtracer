/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * C++ codec perf bench — the `lang` axis (C++ core, cpp-core) of the ranged
 * cross-core perf matrix (ADR-0032, issue #96). It measures decode+encode
 * roundtrip latency and throughput over the SHARED conformance vectors under
 * tests/conformance/vectors/v1/ (the "vector data" workload — real structured
 * TLVs, not synthetic scalars), so a C++-vs-TS-vs-Rust surface can be rendered
 * over the SAME inputs.
 *
 * Output: one machine-parseable RESULT line per vector, in the SAME
 * tab-separated column contract as the TS bench
 * (bindings/typescript/packages/core/bench/perf.mjs) and the Rust bench
 * (bindings/rust/examples/perf.rs), produced via bench_common.hpp's emit():
 *
 *   RESULT \t system \t mode \t size \t fanout \t endpoints \t pub_s \t
 *          deliv_s \t mb_s \t p50ns \t p99ns \t meanns
 *
 * with system="cpp-core", mode="codec", fanout=1, endpoints=1. pub_s and deliv_s
 * are roundtrips/sec (one roundtrip == one decode + one encode); mb_s is
 * bytes/sec/1e6; p50/p99/mean are per-roundtrip nanoseconds. The methodology
 * (warmup, ~100ms throughput phase, individually-timed latency samples) mirrors
 * the TS / Rust benches so the three cores are directly comparable.
 *
 *   bench_codec [vectors_dir]
 * The vectors dir is taken from argv[1], else the compiled-in repo default.
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/tracer.hpp"

#ifndef LIBTRACER_BENCH_DEFAULT_VECTORS
#define LIBTRACER_BENCH_DEFAULT_VECTORS "tests/conformance/vectors/v1"
#endif

using namespace bench;

namespace {

namespace fs = std::filesystem;

// Methodology constants — identical to perf.mjs / perf.rs so the cores compare.
constexpr std::uint64_t kTargetNs = 100'000'000;  // ~100 ms throughput phase
constexpr std::size_t kLatencySamples = 20'000;   // individually-timed roundtrips
constexpr std::size_t kWarmup = 5'000;
constexpr std::size_t kProbeN = 2'000;
constexpr std::size_t kMaxIters = 50'000'000;

std::vector<std::byte> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::ranges::transform(raw, out.begin(), [](char c) {
        return static_cast<std::byte>(static_cast<unsigned char>(c));
    });
    return out;
}

// One decode+encode roundtrip (the unit of work). Returns the re-encoded size so
// the caller can fold it into a sink the optimizer cannot elide.
[[nodiscard]] std::size_t roundtrip(std::span<const std::byte> input) {
    const auto dec = tr::wire::decode(input);
    if (!dec) return 0;  // conformance vectors decode; guard keeps the bench honest
    return tr::wire::encode(*dec).size();
}

struct measure_t {
    double pub_s = 0;
    double mb_s = 0;
    Latency::Summary lat;
};

// Measure a single vector: a tight throughput loop (>= kTargetNs) for the rate,
// then a batch of individually-timed roundtrips for the latency percentiles.
measure_t measure(std::span<const std::byte> input) {
    volatile std::size_t sink = 0;

    for (std::size_t i = 0; i < kWarmup; ++i) sink += roundtrip(input);  // warm caches

    // Calibrate iteration count from a short probe so the timed loop ~= kTargetNs.
    auto t0 = now_ns();
    for (std::size_t i = 0; i < kProbeN; ++i) sink += roundtrip(input);
    const std::uint64_t probe = std::max<std::uint64_t>(1, now_ns() - t0);
    const std::uint64_t per_op = std::max<std::uint64_t>(1, probe / kProbeN);
    std::size_t iters = static_cast<std::size_t>(kTargetNs / per_op);
    iters = std::clamp(iters, kProbeN, kMaxIters);

    // Throughput phase: one tight loop, measure total wall time.
    t0 = now_ns();
    for (std::size_t i = 0; i < iters; ++i) sink += roundtrip(input);
    const double secs = (now_ns() - t0) / 1e9;
    const double pub_s = iters / secs;
    const double mb_s = iters * static_cast<double>(input.size()) / secs / 1e6;

    // Latency phase: time individual roundtrips for the percentiles.
    Latency lat;
    for (std::size_t i = 0; i < kLatencySamples; ++i) {
        const auto a = now_ns();
        sink += roundtrip(input);
        lat.add(now_ns() - a);
    }
    (void)sink;
    return {pub_s, mb_s, lat.summarize()};
}

// Recursively collect (relative-posix-path, absolute-path) for every input.bin.
std::vector<std::pair<std::string, fs::path>> find_inputs(const fs::path& root) {
    std::vector<std::pair<std::string, fs::path>> cases;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (e.path().filename() != "input.bin") continue;
        cases.emplace_back(fs::relative(e.path().parent_path(), root).generic_string(), e.path());
    }
    // Sort by POSIX relative path so the row order matches the TS / Rust harnesses.
    std::sort(cases.begin(), cases.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return cases;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path vroot = argc > 1 ? fs::path{argv[1]} : fs::path{LIBTRACER_BENCH_DEFAULT_VECTORS};
    if (!fs::is_directory(vroot)) {
        std::fprintf(stderr, "no vectors dir at %s\n", vroot.string().c_str());
        return 2;
    }

    const auto cases = find_inputs(vroot);
    if (cases.empty()) {
        std::fprintf(stderr, "no input.bin vectors found under %s\n", vroot.string().c_str());
        return 2;
    }

    std::vector<std::tuple<std::string, std::size_t, measure_t>> rows;
    rows.reserve(cases.size());
    for (const auto& [rel, abs] : cases) {
        const std::vector<std::byte> input = read_file(abs);
        const measure_t m = measure(input);
        // RESULT line — identical 12-field column contract to bench_common.hpp /
        // perf.mjs / perf.rs: pub_s == deliv_s (roundtrips/sec); mb_s = bytes/sec/1e6.
        emit("cpp-core", "codec", input.size(), 1, 1, m.pub_s, m.pub_s, m.mb_s, m.lat);
        rows.emplace_back(rel, input.size(), m);
    }

    // Human-readable footer (per-vector ns/op) to stderr so RESULT stdout stays
    // clean for collate.py / gen_results_page.py-style tooling.
    std::fprintf(stderr, "\n# cpp-core codec bench — %zu vectors (%s)\n", rows.size(),
                 vroot.string().c_str());
    std::fprintf(stderr, "# %-34s%7s%12s%9s%9s%10s\n", "vector", "size", "ns/op", "p50", "p99",
                 "M op/s");
    for (const auto& [rel, size, m] : rows) {
        std::fprintf(stderr, "# %-34s%6zuB%12llu%9llu%9llu%10.2f\n", rel.c_str(), size,
                     static_cast<unsigned long long>(m.lat.mean),
                     static_cast<unsigned long long>(m.lat.p50),
                     static_cast<unsigned long long>(m.lat.p99), m.pub_s / 1e6);
    }
    return 0;
}
