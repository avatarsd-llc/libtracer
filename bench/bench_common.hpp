// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// Shared benchmark scaffolding: a steady-clock timer, a latency-percentile
// accumulator, and a machine-parseable RESULT line that run.sh collates into a
// side-by-side table. Kept tiny and dependency-free so the libtracer and Zenoh
// benchmarks emit identical, comparable output.
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

// Payload sizes (application bytes) and message counts, shared by both harnesses.
inline constexpr std::size_t kSizes[] = {8, 64, 1024, 8192};
inline constexpr std::size_t kThroughputMsgs = 100000;
inline constexpr std::size_t kLatencyMsgs = 10000;

class Latency {
   public:
    void add(std::uint64_t ns) { samples_.push_back(ns); }

    struct Summary {
        std::uint64_t p50 = 0, p99 = 0, mean = 0;
    };

    [[nodiscard]] Summary summarize() {
        if (samples_.empty()) return {};
        std::sort(samples_.begin(), samples_.end());
        const auto at = [&](double p) {
            const std::size_t idx =
                std::min(samples_.size() - 1, static_cast<std::size_t>(p * samples_.size()));
            return samples_[idx];
        };
        const std::uint64_t sum =
            std::accumulate(samples_.begin(), samples_.end(), std::uint64_t{0});
        return {at(0.50), at(0.99), sum / samples_.size()};
    }

   private:
    std::vector<std::uint64_t> samples_;
};

// One comparable measurement. `mode` distinguishes libtracer's two paths
// (inproc graph vs loopback bridge) from Zenoh's in-process path.
inline void emit(const char* system, const char* mode, std::size_t size_bytes, double msgs_per_s,
                 double mb_per_s, const Latency::Summary& lat) {
    std::printf("RESULT\t%s\t%s\t%zu\t%.0f\t%.1f\t%llu\t%llu\t%llu\n", system, mode, size_bytes,
                msgs_per_s, mb_per_s, static_cast<unsigned long long>(lat.p50),
                static_cast<unsigned long long>(lat.p99),
                static_cast<unsigned long long>(lat.mean));
    std::fflush(stdout);
}

}  // namespace bench
