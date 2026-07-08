/**
 * @file
 * @brief Await wakeup storm — the condvar / await fan-in contention microbenchmark.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `await` (graph.cpp) parks on a per-vertex condvar until the next write bumps the
 * vertex's write sequence; a `write` bumps it and `notify_all`s. On a many-core
 * host with W subscribers all awaiting one hot vertex, each write triggers a wake
 * storm: notify_all plus W threads serially re-acquiring the vertex mutex to
 * re-check the predicate. This is the second half of the 128-core scaling
 * question the architecture review deferred to measurement
 * (docs/research/2026-07-04-architecture-deepening-review.md, "128-core scaling":
 * "await/cv wakeup scaling ... needs perf data first").
 *
 * The bench runs a steady state — one writer thread storming writes while W waiter
 * threads each loop on await — for a fixed window, and reports, as W scales
 * 1 -> 128: writer throughput (writes/s, which drops as notify_all + the contended
 * vertex lock get more expensive) and aggregate wakeups/s (the rate waiters
 * actually observe updates). Steady-state throughput is deliberately chosen over
 * single-shot latency: it needs no fragile "are all W parked yet" barrier, so it
 * is not flaky, and it is what a 128-core saturation curve is made of. A
 * DIAGNOSTIC (run on the real many-core box; ADR-0032 nightly row), not a CI gate.
 *
 * Output: the shared bench RESULT contract (bench_common.hpp) —
 *   mode=wake_storm, fanout=W, pub_per_s=writer writes/s, deliv_per_s=aggregate
 *   wakeups/s, latency fields = effective ns per write (1e9 / writes_per_s).
 */
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/view.hpp"

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::view_t;

namespace {

/**
 * @brief Waiter counts (await fan-in width) to sweep.
 *
 * Fixed, not clamped to
 * hardware_concurrency: measures the tail on a real 128-core host; on a small
 * runner the high points show the oversubscribed regime.
 */
constexpr std::size_t kWaiters[] = {1, 2, 4, 8, 16, 32, 64, 128};

constexpr auto kWindow = std::chrono::milliseconds(300);
/**
 * @brief Waiter await timeout: bounds how long a waiter lingers after `stop` before it exits.
 *
 * Short enough to keep teardown snappy; long enough that, in steady state,
 * waiters wake on writes far more often than they time out.
 */
constexpr auto kAwaitTimeout = std::chrono::milliseconds(20);

}  // namespace

int main() {
    graph_t g;
    const path_t path = *path_t::parse("/bench/wake");
    const vertex_handle_t v = g.register_vertex(path, role_t::STORED_VALUE);

    // A stable borrowed value: zero-alloc per write, so the writer measures the
    // write / notify path, not allocation.
    std::array<std::byte, 64> buf{};
    buf.fill(std::byte{0xAB});
    const auto mk = [&]() { return view_t::over(tr::view::borrow_const(buf)); };
    (void)g.write(v, mk());  // seed the LKV so awaits return a value, not NOT_FOUND.

    for (const std::size_t W : kWaiters) {
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> ready{0};
        std::atomic<std::uint64_t> wakeups{0};
        std::atomic<std::uint64_t> writes{0};

        auto waiter = [&]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!stop.load(std::memory_order_relaxed)) {
                if (g.await(v, kAwaitTimeout).has_value())
                    wakeups.fetch_add(1, std::memory_order_relaxed);
            }
        };
        auto writer = [&]() {
            ready.fetch_add(1, std::memory_order_release);
            std::uint64_t local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                (void)g.write(v, mk());
                ++local;
            }
            writes.fetch_add(local, std::memory_order_relaxed);
        };

        std::vector<std::thread> threads;
        threads.reserve(W + 1);
        for (std::size_t i = 0; i < W; ++i) threads.emplace_back(waiter);
        threads.emplace_back(writer);
        while (ready.load(std::memory_order_acquire) < W + 1) {
        }

        const std::uint64_t t0 = bench::now_ns();
        std::this_thread::sleep_for(kWindow);
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : threads) t.join();
        const std::uint64_t t1 = bench::now_ns();

        const double secs = static_cast<double>(t1 - t0) / 1e9;
        const double writes_s = secs > 0 ? static_cast<double>(writes.load()) / secs : 0.0;
        const double wakeups_s = secs > 0 ? static_cast<double>(wakeups.load()) / secs : 0.0;
        const std::uint64_t ns_per_write =
            writes_s > 0 ? static_cast<std::uint64_t>(1e9 / writes_s) : 0;

        const bench::Latency::Summary lat{ns_per_write, ns_per_write, ns_per_write};
        bench::emit("libtracer", "wake_storm", /*size_bytes=*/buf.size(), /*fanout=*/W,
                    /*endpoints=*/1, writes_s, wakeups_s, /*mb_per_s=*/0.0, lat);
    }
    return 0;
}
