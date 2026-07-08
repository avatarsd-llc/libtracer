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
