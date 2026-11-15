/**
 * @file
 * @brief Retire and revive one vertex in a loop with no `collect()` (#2845): the cycle rate as
 *        the park fills.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Each cycle registers `/a` as a stored value with an `on_children` handler (after the first
 * cycle, a revive in place) and retires it, which parks the vertex's value seam until the next
 * `collect()`. None is called, as with an embedder that collects rarely, so the park gains one
 * entry per cycle. A park that grows to exactly the size each retire needs copies itself on
 * every retire, and the rate falls as it fills; a park that grows geometrically keeps the rate
 * flat.
 *
 * Rows: cycles per second in each window of `kWindow` cycles, the best of `kRounds` fresh
 * graphs, then the last window's rate over the first's. Diagnostic, not a perf gate. Run it
 * pinned, A/B against the base tree.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "bench_common.hpp"
#include "libtracer/tracer.hpp"

namespace {

/** @brief Windows per round. */
constexpr int kWindows = 10;
/** @brief Cycles per window. */
constexpr int kWindow = 5000;
/** @brief Fresh graphs; each window reports its best. */
constexpr int kRounds = 5;

}  // namespace

int main() {
    std::array<double, kWindows> best{};
    const tr::graph::path_t path("/a");
    tr::graph::handlers_t h;
    // An inert listing: what gives the vertex a value seam to park.
    h.on_children = tr::graph::thunk([]() -> tr::graph::result_t<tr::view::view_t> {
        return std::unexpected(tr::graph::status_t::NOT_FOUND);
    });
    for (int round = 0; round < kRounds; ++round) {
        tr::graph::graph_t g;
        for (double& rate : best) {
            const std::uint64_t t0 = bench::now_ns();
            for (int i = 0; i < kWindow; ++i) {
                const auto v =
                    g.register_vertex_key(path.key(), tr::graph::role_t::STORED_VALUE, h);
                if (!v || !g.retire(*v)) {
                    std::fprintf(stderr, "bench_retire_park: a cycle was refused\n");
                    return 1;
                }
            }
            const double ns = static_cast<double>(bench::now_ns() - t0);
            rate = std::max(rate, kWindow * 1e9 / ns);
        }
        // Every cycle parked one seam, or the rows measure something else.
        if (g.parked_seam_count() != static_cast<std::size_t>(kWindows) * kWindow) {
            std::fprintf(stderr, "bench_retire_park: %zu seams parked, expected %d\n",
                         g.parked_seam_count(), kWindows * kWindow);
            return 1;
        }
    }
    std::printf("%-8s %10s %12s\n", "window", "parked", "cycles/s");
    for (int w = 0; w < kWindows; ++w)
        std::printf("%-8d %10d %12.0f\n", w, (w + 1) * kWindow, best[w]);
    std::printf("last/first %.3f\n", best.back() / best.front());
    return 0;
}
