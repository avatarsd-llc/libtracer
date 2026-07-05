/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Fan-out clone storm — the segment-refcount contention microbenchmark.
 *
 * The load-bearing claim of libtracer fan-out (README, ADR-0038): delivering one
 * value to N subscribers is N refcount bumps, not N copies. On a many-core host a
 * hot vertex fanned out across cores turns that into N cores hammering ONE
 * segment's atomic refcount — a single cacheline bounced between L1s. This is the
 * #1 open question the architecture review flagged for the 128-core target
 * (docs/research/2026-07-04-architecture-deepening-review.md, "128-core scaling"):
 * measure it before designing anything.
 *
 * The bench parks T threads on one shared value view (one segment, one refcount)
 * and has each clone+release it (`view_t copy = hot;` — a `segment_ptr_t` copy =
 * relaxed inc; its destruction = acq_rel dec) in a tight time-boxed loop, i.e.
 * the exact per-subscriber delivery primitive with T standing in for fan-out
 * width. It reports aggregate and per-thread clone+release throughput as T scales
 * 1 -> 128. The contention signature is per-thread throughput collapsing while
 * aggregate plateaus: that is the refcount cacheline saturating. This is a
 * DIAGNOSTIC (run it on the real many-core box; ADR-0032 nightly row), not a CI
 * gate — thread-contention numbers are runner-dependent, so it is not wired into
 * perf.yml's regression gate.
 *
 * Output: the shared bench RESULT contract (bench_common.hpp) —
 *   mode=clone_storm, fanout=T, pub_per_s=per-thread ops/s, deliv_per_s=aggregate
 *   ops/s, latency fields = effective per-thread ns/op (uniform in a tight loop).
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/view.hpp"

using tr::view::view_t;

namespace {

// Fan-out widths to sweep. Fixed (not clamped to hardware_concurrency) so the run
// on a real 128-core host measures the tail; on a small CI runner the high points
// show the oversubscribed regime, and the 1->cores points show contention onset.
constexpr std::size_t kFanouts[] = {1, 2, 4, 8, 16, 32, 64, 128};

// Wall-clock per fan-out point. Time-boxed (not a fixed op count) so the total run
// stays bounded regardless of how slow contention makes each op.
constexpr auto kWindow = std::chrono::milliseconds(250);

constexpr std::size_t kSegBytes = 64;  // one representative sample segment.

// Sink for the clones' field reads: keeps the loop body (and thus the clone) live
// past the optimizer. The refcount RMWs are atomics on a shared object and are not
// removable on their own, but this makes the intent explicit and defeats hoisting.
// Atomic (not a bare `volatile`) so the once-per-thread fold is race-free under TSan.
std::atomic<std::size_t> g_sink{0};

}  // namespace

int main() {
    // One hot value view: a single segment with a single refcount, shared by every
    // cloner — the "delivered sample" of a wide fan-out.
    std::vector<std::byte> bytes(kSegBytes, std::byte{0xAB});
    const view_t hot = tr::view::over_bytes(bytes).value_or(view_t{});

    for (const std::size_t T : kFanouts) {
        std::atomic<std::uint64_t> ready{0};
        std::atomic<bool> go{false};
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> total_ops{0};

        auto worker = [&]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
            }
            std::uint64_t local = 0;
            std::size_t acc = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const view_t clone = hot;  // clone: refcount inc; dtor: refcount dec
                acc += clone.length;
                ++local;
            }
            g_sink.fetch_add(acc, std::memory_order_relaxed);
            total_ops.fetch_add(local, std::memory_order_relaxed);
        };

        std::vector<std::thread> workers;
        workers.reserve(T);
        for (std::size_t i = 0; i < T; ++i) workers.emplace_back(worker);
        while (ready.load(std::memory_order_acquire) < T) {
        }

        const std::uint64_t t0 = bench::now_ns();
        go.store(true, std::memory_order_release);
        std::this_thread::sleep_for(kWindow);
        stop.store(true, std::memory_order_relaxed);
        for (auto& w : workers) w.join();
        const std::uint64_t t1 = bench::now_ns();

        const double secs = static_cast<double>(t1 - t0) / 1e9;
        const std::uint64_t ops = total_ops.load();
        const double agg_ops_s = secs > 0 ? static_cast<double>(ops) / secs : 0.0;
        const double per_thread_ops_s = agg_ops_s / static_cast<double>(T);
        const std::uint64_t ns_per_op =
            per_thread_ops_s > 0 ? static_cast<std::uint64_t>(1e9 / per_thread_ops_s) : 0;

        const bench::Latency::Summary lat{ns_per_op, ns_per_op, ns_per_op};
        bench::emit("libtracer", "clone_storm", kSegBytes, /*fanout=*/T, /*endpoints=*/1,
                    per_thread_ops_s, agg_ops_s, /*mb_per_s=*/0.0, lat);
    }
    return g_sink.load() == ~std::size_t{0} ? 1 : 0;  // consume the sink; never taken.
}
