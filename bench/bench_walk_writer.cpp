/**
 * @file
 * @brief An exclusive map-lock holder's latency while N threads run subtree walks (#2846).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A graph holds `kFanout`² vertices under `/p`, so a composed read of `/p` walks all of them
 * under the shared map hold. N reader threads read `/p` back to back, retrying a
 * `BACKPRESSURE`, while the main thread registers `kWrites` fresh vertices one at a time; each
 * registration takes the map lock exclusively. Before #2846 a registration waited out every
 * overlapping walk, so its latency grew with N. After it, a walk yields to the waiting writer
 * at its next node, so the latency should stay near N = 0 whatever N is.
 *
 * Rows: `walk_writer N=<n>` with the writer's p50, p99 and max latency in microseconds, plus
 * the walks completed and yielded inside the timed window. Each write first waits a random
 * 0..one walk, so the writes sample every phase of a walk cycle; a row with no walk ending in
 * its window fails. Diagnostic, not a perf gate. Run it pinned, A/B against the base tree: it
 * uses public API only, so it builds on either. On the base tree a writer can starve under the
 * rwlock's reader preference, so run that arm under a timeout.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/tracer.hpp"

namespace {

/** @brief Children of `/p`, and children of each: `kFanout`² walked vertices. */
constexpr int kFanout = 316;
/** @brief Timed registrations per row. */
constexpr int kWrites = 200;
/** @brief The reader counts the rows sweep. */
constexpr int kReaders[] = {0, 1, 2, 4, 8};

}  // namespace

int main() {
    using tr::graph::path_t;
    using tr::graph::role_t;
    using tr::graph::status_t;
    tr::graph::graph_t g;
    const auto p = g.try_register_vertex(path_t("/p"), role_t::STORED_VALUE);
    if (!p) return 1;
    std::array<std::byte, 8> seed{};
    seed.fill(std::byte{0xAB});
    for (int i = 0; i < kFanout; ++i) {
        // A branch read composes only when the vertex has a REGISTERED child: register the
        // middle level too, not just the leaves under it.
        if (!g.try_register_vertex(path_t("/p/a" + std::to_string(i)), role_t::STORED_VALUE))
            return 1;
        for (int j = 0; j < kFanout; ++j) {
            const std::string k = "/p/a" + std::to_string(i) + "/b" + std::to_string(j);
            const auto v = g.try_register_vertex(path_t(k), role_t::STORED_VALUE);
            // A walk folds stored values: with none anywhere, the read of /p is NOT_FOUND
            // and nothing is walked.
            if (!v || !g.write(*v, tr::view::view_t::over(tr::view::borrow_const(seed)))) return 1;
        }
    }
    // One bare walk's length, best of a few: the timed writes are spread over many walk cycles
    // by waiting a random 0..walk_ns between them, so they land in the locked collect phase in
    // proportion to its share of the cycle instead of all in the unlocked emit and teardown.
    std::uint64_t walk_ns = ~std::uint64_t{0};
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t t0 = bench::now_ns();
        if (!g.read(*p)) return 1;
        walk_ns = std::min(walk_ns, bench::now_ns() - t0);
    }
    std::mt19937_64 rng(2846);
    std::uniform_int_distribution<std::uint64_t> gap(0, walk_ns);
    std::printf("%-16s %10s %10s %10s %10s %10s\n", "row", "p50_us", "p99_us", "max_us", "walks",
                "yields");
    int fresh = 0;
    for (const int n : kReaders) {
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> walks{0};
        std::atomic<std::uint64_t> yields{0};
        // Any other refusal is terminal: the row cannot measure what it claims, so the readers
        // stop and the bench fails instead of waiting forever for a walk that never ends.
        std::atomic<int> refused{0};
        std::vector<std::thread> readers;
        for (int r = 0; r < n; ++r) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    const auto v = g.read(*p);
                    if (v) {
                        walks.fetch_add(1, std::memory_order_relaxed);
                    } else if (v.error() == status_t::BACKPRESSURE) {
                        // A yield to the waiting writer; a read refused for want of a table
                        // also answers BACKPRESSURE, which the default heap source never does.
                        yields.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        refused.store(static_cast<int>(v.error()) + 1);
                        stop = true;
                        return;
                    }
                }
            });
        }
        // Let the readers get going (n walks ended) before the first timed write.
        while (n > 0 && refused.load() == 0 &&
               walks.load() + yields.load() < static_cast<std::uint64_t>(n))
            std::this_thread::yield();
        std::vector<double> us;
        us.reserve(kWrites);
        bool ok = true;
        // Contention is counted inside the timed window only.
        const std::uint64_t walks0 = walks.load();
        const std::uint64_t yields0 = yields.load();
        for (int w = 0; ok && w < kWrites; ++w) {
            for (const std::uint64_t until = bench::now_ns() + gap(rng); bench::now_ns() < until;) {
            }
            const std::string k = "/w" + std::to_string(fresh++);
            const std::uint64_t t0 = bench::now_ns();
            ok = g.try_register_vertex(path_t(k), role_t::STORED_VALUE).has_value();
            us.push_back(static_cast<double>(bench::now_ns() - t0) / 1e3);
        }
        const std::uint64_t win_walks = walks.load() - walks0;
        const std::uint64_t win_yields = yields.load() - yields0;
        stop = true;
        for (std::thread& t : readers) t.join();
        if (!ok) return 1;
        if (refused.load() != 0) {
            std::fprintf(stderr, "walk_writer N=%d: read of /p refused with status %d\n", n,
                         refused.load() - 1);
            return 2;
        }
        std::sort(us.begin(), us.end());
        char row[32];
        std::snprintf(row, sizeof row, "walk_writer N=%d", n);
        std::printf("%-16s %10.1f %10.1f %10.1f %10llu %10llu\n", row, us[us.size() / 2],
                    us[us.size() * 99 / 100], us.back(), static_cast<unsigned long long>(win_walks),
                    static_cast<unsigned long long>(win_yields));
        // No walk ended inside the window: the writes never overlapped one, so the row says
        // nothing about a writer waiting on walks. Fail it rather than print a clean number.
        if (n > 0 && win_walks + win_yields == 0) {
            std::fprintf(stderr, "walk_writer N=%d: no walk overlapped the timed writes\n", n);
            return 3;
        }
    }
    return 0;
}
