/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * route_handle per-link-lock contention microbenchmark.
 *
 * A `graph.write` fanning out to a remote (ws) subscriber calls
 * `route_handle_t::ensure_egress(out_link, route)` on the writer's thread; once a
 * flow is advertised, that call is a steady-state READ (a linear scan of the
 * link's egress table for the matching route → its label). Today every such read
 * takes the link's EXCLUSIVE `std::mutex`, so many producers publishing to remote
 * subscribers on ONE hot link serialize on it. This bench measures whether that
 * matters: T threads each hammer `ensure_egress` on the same (already-advertised)
 * `(link, route)` flow — the reuse-read hot path — as T scales 1 → 128.
 *
 * The contention signature is per-thread throughput collapsing while aggregate
 * plateaus: the per-link mutex serialising the reads. It shows clearly on a
 * 24-core host — per-thread 37M -> 46K ops/s (~800x), aggregate declining past T=2.
 *
 * FINDING (measured 2026-07-05, back-to-back same host): the obvious "cheap fix",
 * a per-link `std::shared_mutex` so reuse-reads run concurrently, does NOT help —
 * it is slightly-to-24%-WORSE (T=1 36.4M -> 27.8M; T=8 784K -> 534K; T=128 ~equal).
 * The critical section is a ~1-entry linear scan (nanoseconds), so the LOCK ITSELF
 * is the contended cacheline, and shared_mutex's lock_shared/unlock_shared do an
 * atomic RMW on a shared reader-count that is equally contended (and heavier
 * single-thread) — concurrent-reader locks only pay off when the critical section
 * is long enough to overlap. So the exclusive mutex stays. The only things that
 * would actually scale here avoid touching shared state on the read path: a
 * seqlock (versioned lock-free reads) or a per-producer thread-local (route ->
 * label) cache. Both are larger/riskier, and this is an EXTREME fan-in workload
 * (many producer threads publishing to ONE downstream link); deferred until a real
 * one is confirmed. Measure before optimising — and re-measure the optimisation.
 *
 * DIAGNOSTIC (thread-contention numbers are runner-dependent), NOT a CI gate — not
 * wired into perf.yml's regression gate, exactly like bench_fanout_clone_storm.
 *
 * Output: the shared bench RESULT contract (bench_common.hpp) —
 *   mode=route_handle_egress, fanout=T, pub_per_s=per-thread ops/s,
 *   deliv_per_s=aggregate ops/s, latency fields = per-thread ns/op.
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/route_handle.hpp"

namespace {

// Producer-thread counts to sweep (fixed, not clamped to hardware_concurrency, so
// a real many-core host measures the tail; a small runner shows onset + oversubscribe).
constexpr std::size_t kThreads[] = {1, 2, 4, 8, 16, 32, 64, 128};

constexpr auto kWindow = std::chrono::milliseconds(250);  // wall-clock per point
constexpr std::size_t kRouteBytes = 32;                   // one representative route PATH.

// Keep the label reads live past the optimizer (the mutex acquire/scan can't be
// removed on its own, but this makes the intent explicit).
std::atomic<std::size_t> g_sink{0};

}  // namespace

int main() {
    tr::net::route_handle_t h;
    const std::vector<std::byte> route(kRouteBytes, std::byte{0x5A});
    // Advertise the flow ONCE so every worker call below is a steady-state reuse
    // READ (the linear scan under the per-link mutex), the producer hot path.
    (void)h.ensure_egress("b", route);

    for (const std::size_t T : kThreads) {
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
                const auto [label, fresh] = h.ensure_egress("b", route);  // reuse read under t.m
                acc += label;
                (void)fresh;
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
        bench::emit("libtracer", "route_handle_egress", kRouteBytes, /*fanout=*/T, /*endpoints=*/1,
                    per_thread_ops_s, agg_ops_s, /*mb_per_s=*/0.0, lat);
    }
    return static_cast<int>(g_sink.load() & 0x7f);
}
