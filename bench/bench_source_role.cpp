/**
 * @file
 * @brief The RETENTION-ROLE write asymmetry (#1505): what one write costs at a vertex that
 *        retains a last-known-value versus one that retains nothing.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_libtracer`'s `inproc-target-stored` / `inproc-target-handler` pair sweeps the role of
 * the DELIVERY TARGET — the far end of a path-target edge. Nothing in the suite sweeps the role
 * of the vertex being WRITTEN, which is where the asymmetry #1505 reports lives:
 *
 *   - a retaining role (`STORED_VALUE`) delivers the pointer `store_value` just published, so
 *     the fan-out leg reclones nothing (`graph.cpp`'s `deliver_vertex(v, **stored)` tail);
 *   - a `HANDLER` retains nothing, so it builds a nothrow `try_clone_rope(notify, value)`
 *     BEFORE `store_value` and delivers from that — one `try_reserve` plus one refcount RMW
 *     per link, per write, **paid even at fan-out ZERO** — and on clone OOM the entire
 *     fan-out is shed, counted at own-subs width, while the write still returns success.
 *
 * So the sanctioned "callbacks only, nothing stored" mode is the one that pays an extra copy
 * on the hot write path. This bench prices that: the same write, the same subscribers, the
 * same value, with only the WRITTEN vertex's role changed.
 *
 * Rows use the 12-field `RESULT` contract of `bench_common.hpp`, with the `ep` column carrying
 * the value's LINK COUNT rather than an endpoint count — the axis that matters here, because
 * `rope_t::kInline` is 2 and a clone past it reaches the allocator:
 *
 *   - `src-role-stored` / `src-role-handler` — per-write cost, fan 0/1/4, links 1 and 4.
 *   - `src-role-clone` — the clone term ALONE (`try_reserve` + `concat`, the body of
 *     `graph.cpp`'s `try_clone_rope`), swept over link count. The inline/spill knee is the
 *     shed-on-OOM exposure boundary: below it the clone touches no allocator and cannot fail.
 *
 * The heap half — how many allocations one write costs at each role, which is what makes the
 * shed exposure a count rather than an argument — lives in `bench_source_role_alloc`.
 *
 * DIAGNOSTIC, not a perf gate: a decision input for #1505's ruling. Its rows are deliberately
 * absent from the default `bench_libtracer` sweep whose keys `perf_gate.py` and the `gh-pages`
 * history join on, where a new row would be a series with no history.
 */

#include "bench_source_role.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "bench_common.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tracer.hpp"

using namespace bench;
using bench_role::kBytes;
using bench_role::register_src;
using bench_role::value_fixture_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;

namespace {

/** @brief The swept link counts: 1 and 4 straddle `rope_t::kInline` (2). */
constexpr std::size_t kLinks[] = {1, 4};
/** @brief The swept subscriber widths. Zero is the load-bearing arm — see @ref run_role. */
constexpr std::size_t kFans[] = {0, 1, 4};

/**
 * @brief The clone term alone: the body of `graph.cpp`'s `try_clone_rope`.
 *
 * Reproduced rather than called because that helper lives in an anonymous namespace — but it
 * is two public `rope_t` calls, so the reproduction cannot drift in substance. Both branches
 * of `try_reserve` matter: while the chain fits inline it touches no allocator at all, and
 * past `kInline` it grows a `std::vector<view_t>` that can fail — and that failure is the one
 * that sheds a whole fan-out.
 */
[[nodiscard]] bool clone_rope(rope_t& dst, const rope_t& src) noexcept {
    if (!dst.try_reserve(src.link_count())) return false;
    dst.concat(src);
    return true;
}

/**
 * @brief One (role, fan, links) point: per-write cost with only the WRITTEN vertex's role
 *        changed.
 *
 * @param handler `true` registers the written vertex as `HANDLER`, `false` as `STORED_VALUE`.
 * @param fan     Callback subscribers on the written vertex. **Fan 0 is the sharpest arm**:
 *                the retaining role dispatches nothing, while the HANDLER still pays the
 *                notify clone in full before discovering there is nobody to notify — so at
 *                fan 0 the gap between the two arms is the clone term and little else.
 *                Callback edges deliberately, not path-target ones: the target leg has a
 *                per-edge clone of its own (`dispatch_edge_target`) that would swamp the
 *                source-role term this row exists to isolate.
 * @param links   Links in the written value; 1 stays inside `rope_t::kInline`, 4 spills.
 */
void run_role(bool handler, std::size_t fan, std::size_t links, const char* mode) {
    graph_t g;
    const path_t src = *path_t::parse("/bench/src");
    std::atomic<std::uint64_t> hits{0};
    const vertex_handle_t v = register_src(g, src, handler, hits);
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const rope_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    for (std::size_t f = 0; f < fan; ++f) (void)g.subscribe(src, cb);

    const value_fixture_t fx{links};
    const auto put = [&](std::size_t) { (void)g.write(v, fx.make()); };

    // Prove the arm under test is wired before timing it. A HANDLER whose `on_write` never
    // ran would be timing a refusal, and a subscriber that never fired would be timing a
    // fan-out this row's label claims and does not have.
    put(0);
    if (handler && hits.load() == 0) {
        std::fprintf(stderr, "SKIP mode=%s fan=%zu links=%zu: handler never ran\n", mode, fan,
                     links);
        return;
    }
    if (fan > 0 && recv.load() == 0) {
        std::fprintf(stderr, "SKIP mode=%s fan=%zu links=%zu: no delivery reached a subscriber\n",
                     mode, fan, links);
        return;
    }

    // Deliveries-per-write is `fan`, but the budget must not collapse at fan 0 — that arm
    // publishes at the same rate as fan 1 and is the one the clone term is read off.
    const std::size_t denom = std::max<std::size_t>(fan, 1);
    const std::size_t MSGS = publishes_for(denom, kDeliveryBudget);
    const std::size_t LATN = publishes_for(denom, kLatencyDeliveryBudget);
    for (std::size_t i = 0; i < 2000; ++i) put(i);  // warmup

    const std::uint64_t t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put(i);
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    const double pub_s = static_cast<double>(MSGS) / secs;
    const double deliv_s = pub_s * static_cast<double>(fan);
    const double mb_s = deliv_s * static_cast<double>(kBytes * links) / 1e6;

    Latency lat;
    for (std::size_t i = 0; i < LATN; ++i) {
        const std::uint64_t a = now_ns();
        put(i);
        lat.add(now_ns() - a);
    }
    const Latency::Summary s = lat.summarize();
    emit("libtracer", mode, kBytes * links, fan, links, pub_s, deliv_s, mb_s, s);
    emit_tail("libtracer", mode, kBytes * links, fan, links, s);
}

/** @brief The clone term alone, swept over link count — mode `src-role-clone`. */
void run_clone_term(std::size_t links) {
    const value_fixture_t fx{links};
    const rope_t value = fx.make();
    constexpr std::size_t kN = 200000;
    for (std::size_t i = 0; i < 2000; ++i) {  // warmup
        rope_t dst;
        (void)clone_rope(dst, value);
    }
    std::size_t ok = 0;
    const std::uint64_t t0 = now_ns();
    for (std::size_t i = 0; i < kN; ++i) {
        rope_t dst;
        ok += clone_rope(dst, value) ? 1 : 0;
    }
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    if (ok != kN) {
        std::fprintf(stderr, "SKIP mode=src-role-clone links=%zu: %zu of %zu clones failed\n",
                     links, kN - ok, kN);
        return;
    }
    Latency lat;
    for (std::size_t i = 0; i < 100000; ++i) {
        const std::uint64_t a = now_ns();
        rope_t dst;
        (void)clone_rope(dst, value);
        lat.add(now_ns() - a);
    }
    // Throughput only; there is no delivery and no wire, so the delivery and bandwidth
    // columns are left at 0 — the convention the history emitter reads as "this row does not
    // produce that metric".
    emit("libtracer", "src-role-clone", kBytes * links, 0, links, static_cast<double>(kN) / secs,
         0.0, 0.0, lat.summarize());
}

}  // namespace

/** @brief Run the whole matrix; it takes well under a minute and has no arguments. */
int main() {
    for (const std::size_t links : kLinks) run_clone_term(links);
    for (const std::size_t links : kLinks)
        for (const std::size_t fan : kFans) {
            run_role(false, fan, links, "src-role-stored");
            run_role(true, fan, links, "src-role-handler");
        }
    return 0;
}
