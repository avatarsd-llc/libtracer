/**
 * @file
 * @brief N writers on ONE vertex — a plain last-writer-wins vertex against a STREAM — the
 *        in-tree instrument for RFC-0025 §4.6.2's four-writer leg (#1485, #1495).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section why Why this bench exists
 *
 * §4.6.2 banked **4.59 M/s lock-free against 1.73 M/s** — a **0.14x** collapse — for four
 * writers on one vertex, as the second of the two readings that carried Amendment 2 ("a
 * producer never queues"). Like the 53/71 ns baseline beside it, that pair came from a
 * harness that was never committed, so nobody could re-run it. This file is the committed
 * instrument the 2026-08-24 erratum to that section says is owed.
 *
 * @section what_changed What it can and cannot reproduce
 *
 * It does not re-run the structure that figure priced. The producer-side ring — the
 * `kRingAppendProbe` heuristic, the deque append under the stripe mutex, and the second stripe
 * acquisition the drain then paid — was **deleted outright** by PR #1490, on the strength of
 * that very measurement; `vertex.hpp` records it as "GONE, not made optional". The arms here
 * are:
 *
 *   - `fanin-lockfree` — N writers on a `STORED_VALUE` vertex. The baseline.
 *   - `fanin-stream`   — N writers on a `STREAM` vertex with a bounded history depth. This is
 *     the STREAM role **as it exists today**: the retention Amendment 2 relocated to the
 *     *receiving* vertex, reached here because these writers write directly at the vertex that
 *     retains. It is NOT the machinery §4.6.2 priced, and a ratio read off this arm is not a
 *     measurement of the deleted one.
 *
 * That distinction matters because the collapse **is still there**. On the studio host, best
 * of five rounds, the four-writer arms read 7.1–14.1 M/s lock-free against 1.1–3.2 M/s on the
 * STREAM — a per-round ratio between **0.08x and 0.42x**, bracketing §4.6.2's 0.14x from a
 * different structure. Any single number quoted from that band would be a lottery ticket, and
 * quoting one is the failure the erratum corrects, so this bench prints the ratio **per round**
 * and its header states a range. The one arm that is tight enough to state as a figure is
 * **T = 1**, where the STREAM write costs 0.54–0.56x the plain one (≈141 ns against ≈78 ns per
 * op): the retention premium on an uncontended write, which no thread-scheduling luck touches.
 *
 * What the sweep is therefore good for is the standing claim underneath the amendment — that a
 * queue is the consumer's, priced where the consumer asked for it — and for locating a
 * retrograde arm. It is not evidence that many writers at one retaining vertex are cheap; they
 * are not.
 *
 * @section threads The ladder
 *
 * 1, 2, 4 and 8 writers, clamped to the host's hardware concurrency. Four is the width §4.6.2
 * cites; 1 is the reference the collapse is measured against; 2 and 8 bracket it so a
 * retrograde arm can be *located* rather than only detected.
 *
 * @section reading Output
 *
 *     RESULT libtracer fanin-<arm> 64 <threads> 1 <per-thread ops/s> <aggregate ops/s> 0.0 …
 *     RATIO_FANIN threads lockfree_ops_s stream_ops_s ratio
 *
 * The latency fields are **picoseconds** per system-wide operation (the reciprocal of the
 * aggregate rate), following `bench_contention.cpp` — a contended op can run under a
 * nanosecond per system op and the RESULT latency columns are integers. `deliv/s` is 0: no arm
 * has a subscriber, so nothing is delivered and the throughput column would otherwise report
 * the store rate under a name that means something else (#553; the same confusion the §4.6.2
 * erratum corrects).
 *
 * @section gating Registered, run on demand, NEVER a perf gate
 *
 * Multi-threaded aggregate throughput on a shared host is a property of the machine and its
 * scheduler as much as of the code — the same reason `bench_contention.cpp`,
 * `bench_lkv_slot.cpp` and every other `scaling`-surface harness here is diagnostic. The
 * per-PR gate is a same-runner ratio discipline over single-threaded points; this instrument
 * does not join it. Its ratio row is the number to quote, with `bench/host_guard.py`
 * provenance, and never a bare absolute.
 */
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tracer.hpp"

namespace {

using bench::emit;
using bench::Latency;
using bench::now_ns;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::view_t;

/** @brief Payload bytes — the reference size every other bench reports at. */
constexpr std::size_t kSize = 64;

/** @brief Writes each thread performs, enough to swamp thread start-up and the warm-up. */
constexpr std::size_t kOpsPerThread = 200'000;

/** @brief Writer counts swept, clamped to the host's hardware concurrency. */
constexpr std::size_t kWriters[] = {1, 2, 4, 8};

/** @brief Retained entries on the STREAM arm — a real bounded ring, not a degenerate one. */
constexpr std::size_t kHistoryDepth = 16;

/** @brief A VALUE TLV carrying @p payload bytes. */
[[nodiscard]] std::vector<std::byte> value_tlv(std::size_t payload) {
    std::vector<std::byte> p(payload, std::byte{0xAB});
    tr::wire::tlv_t t{};
    t.type = tr::wire::type_t::VALUE;
    t.payload = p;
    return tr::wire::encode(t);
}

/** @brief Per-op owned heap view — the allocating publish path. */
[[nodiscard]] view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief The vertex role an arm drives. */
enum class arm_t { LOCKFREE, STREAM };

/** @brief The RESULT `mode` string for @p a. */
[[nodiscard]] const char* arm_name(arm_t a) {
    return a == arm_t::LOCKFREE ? "fanin-lockfree" : "fanin-stream";
}

/**
 * @brief Drive @p threads writers at one vertex of arm @p a and emit its RESULT row.
 *
 * Threads spin on a start flag rather than sleeping, so the measured window holds no wake-up
 * latency, and the clock starts only once every writer has announced itself ready — the same
 * shape `bench_contention.cpp` uses, so the two are readable against each other.
 *
 * @return Aggregate operations per second, so the caller can state the same-run ratio.
 */
[[nodiscard]] double run(arm_t a, std::size_t threads) {
    graph_t g;
    const path_t path = *path_t::parse("/bench/fanin");
    const vertex_handle_t v =
        g.register_vertex(path, a == arm_t::STREAM ? role_t::STREAM : role_t::STORED_VALUE);
    if (a == arm_t::STREAM) (void)g.set_history_depth(v, kHistoryDepth);

    const std::vector<std::byte> tlv = value_tlv(kSize);
    std::atomic<std::uint64_t> ok{0};
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (std::size_t i = 0; i < 2000; ++i) (void)g.write(v, owned_view(tlv));  // warm-up
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { /* spin to a common start */
            }
            std::uint64_t mine = 0;
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                if (g.write(v, owned_view(tlv)).has_value()) ++mine;
            }
            ok.fetch_add(mine, std::memory_order_relaxed);
        });
    }
    while (ready.load(std::memory_order_acquire) < threads) { /* wait for the field */
    }
    const std::uint64_t t0 = now_ns();
    go.store(true, std::memory_order_release);
    for (std::thread& th : pool) th.join();
    const std::uint64_t elapsed = now_ns() - t0;

    const double ops = static_cast<double>(kOpsPerThread) * static_cast<double>(threads);
    const double agg = elapsed == 0 ? 0.0 : ops * 1e9 / static_cast<double>(elapsed);
    const std::uint64_t ps_per_op = agg == 0.0 ? 0 : static_cast<std::uint64_t>(1e12 / agg);
    emit("libtracer", arm_name(a), kSize, threads, 1, agg / static_cast<double>(threads),
         /*deliv_per_s=*/0.0, /*mb_per_s=*/0.0,
         Latency::Summary{ps_per_op, ps_per_op, ps_per_op, ps_per_op, ps_per_op, 0, false});

    const std::uint64_t published = ok.load(std::memory_order_relaxed);
    std::printf("NOTE %s threads=%zu published=%llu of %.0f\n", arm_name(a), threads,
                static_cast<unsigned long long>(published), ops);
    if (static_cast<double>(published) != ops) {
        std::printf("WARN %s threads=%zu published %llu of %.0f — writes were DECLINED\n",
                    arm_name(a), threads, static_cast<unsigned long long>(published), ops);
    }
    return agg;
}

}  // namespace

/** @brief Both arms at every admissible writer count, reporting the same-run ratio per count. */
int main() {
    const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    for (std::size_t t : kWriters) {
        if (t > hw) continue;
        // The two arms run back to back at each width, so a drift window is shared by both
        // rather than donated to one — the ratio is the quantity, and it is same-run.
        const double lockfree = run(arm_t::LOCKFREE, t);
        const double stream = run(arm_t::STREAM, t);
        std::printf("RATIO_FANIN\t%zu\t%.0f\t%.0f\t%.3f\n", t, lockfree, stream,
                    lockfree == 0.0 ? 0.0 : stream / lockfree);
        std::fflush(stdout);
    }
    return 0;
}
