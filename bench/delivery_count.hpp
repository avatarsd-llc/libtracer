/**
 * @file
 * @brief How a libtracer bench row states what it DELIVERED — the observed figure, and the
 *        short-count guard that says so out loud when it falls short (#1481).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The fairness audit of PR #1480 removed `pub_s * fanout` from the CHARTED `inproc` rows:
 * a delivery figure derived by arithmetic is a claim, not a measurement, and it stays true
 * only for as long as nothing is shed. The rows this header serves are the ones that
 * audit deliberately left alone — `inproc-target-*` and `inproc-remote` — and the reason
 * to finish the job is that `write()` returns SUCCESS through every shed the graph has:
 * the wide fan-out truncates to its inline prefix when the overflow reserve fails, and
 * `dispatch_edge_target` drops a delivery on an unresolvable target, a denied fan-in gate
 * or a failed nothrow clone. In each case the publish loop finishes at full speed, so the
 * derived figure keeps rising while deliveries are being lost.
 *
 * Two ways to know, and a row uses whichever it HAS:
 *
 * - **Count at the consumer.** A handler target, a remote sink, a subscriber callback —
 *   anything that runs per delivery can increment. This is direct observation and is
 *   always preferred (@ref delivered_rate takes the count).
 * - **Subtract what the graph accounted as dropped.** A `STORED_VALUE` target keeps no
 *   such counter — a delivery there terminates in the target's LKV, and adding a counting
 *   subscriber to see it would change the topology being timed and therefore the number.
 *   `graph_t::delivery_drops()` is what makes this second route sound rather than a
 *   restatement of the arithmetic: RFC-0025 §4.4 makes silence the one forbidden
 *   behaviour, so every leg that sheds one of these deliveries counts it, and
 *   `want - dropped` is a figure the run OBSERVED (@ref deliveries_from_drops).
 *
 * Header rather than a body in `bench_libtracer.cpp` so `test_delivery_count` can drive
 * the same two functions the instrument publishes through, against a topology built to
 * shed on purpose.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "libtracer/tracer.hpp"

namespace bench {

/**
 * @brief Every delivery the graph accounted as dropped between two @ref
 *        tr::graph::graph_t::delivery_drops snapshots.
 *
 * All four causes summed, because all four are deliveries this process was asked to make
 * and did not: an unresolvable target, a fan-in refusal, a failed nothrow clone, and a
 * fan-out snapshot that could not widen past its inline prefix. The counters are relaxed
 * monotonic and read one at a time, so a snapshot taken while another thread delivers can
 * tear — harmless here, where both snapshots bracket a single-threaded timed loop.
 *
 * @param a The earlier snapshot, @param b the later one.
 */
[[nodiscard]] inline std::uint64_t drops_between(
    const tr::graph::graph_t::delivery_drops_t& a,
    const tr::graph::graph_t::delivery_drops_t& b) noexcept {
    return (b.no_target - a.no_target) + (b.denied - a.denied) +
           (b.out_of_memory - a.out_of_memory) + (b.fan_out_truncated - a.fan_out_truncated);
}

/**
 * @brief Deliveries a row is known to have made, for a row whose deliveries terminate
 *        somewhere that keeps no counter.
 *
 * `want` is the arithmetic ceiling — publishes times fan-out — and this is the ONLY place
 * that number is allowed to appear in a published figure, with everything the graph
 * accounted as lost taken back out of it. Saturates at zero rather than wrapping: the
 * counters are shared by the whole graph, so a drop charged by something other than the
 * timed loop (a warmup write racing a retire, a second row in the same process) must
 * report "nothing survived", never `UINT64_MAX` deliveries.
 */
[[nodiscard]] inline std::uint64_t deliveries_from_drops(
    std::uint64_t want, const tr::graph::graph_t::delivery_drops_t& before,
    const tr::graph::graph_t::delivery_drops_t& after) noexcept {
    const std::uint64_t dropped = drops_between(before, after);
    return dropped >= want ? 0 : want - dropped;
}

/**
 * @brief The deliveries-per-second figure a row publishes, from @p got rather than @p want.
 *
 * Warns on a short count and publishes it anyway, which is the shape #1480 settled on for
 * the charted rows: dropping the row would lose a point from a long-running series over
 * exactly the event a reader most wants to see in it, so the honest lower number goes out
 * with a line on stderr beside it. The two agree exactly whenever the graph delivers in
 * full — dispatch is inline, so the publish loop IS the delivery loop — which is why this
 * moves no historical number, and why the day it does is the day it earned its keep.
 *
 * @param mode  The row's mode, so a warning names the series it belongs to.
 * @param secs  The timed window; @p got over it is the figure returned.
 */
[[nodiscard]] inline double delivered_rate(const char* mode, std::size_t S, std::size_t F,
                                           std::size_t E, std::uint64_t want, std::uint64_t got,
                                           double secs) {
    if (got < want)
        std::fprintf(stderr, "[libtracer] mode=%s S=%zu F=%zu E=%zu delivered %llu/%llu (shed)\n",
                     mode, S, F, E, static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));
    return static_cast<double>(got) / secs;
}

}  // namespace bench
