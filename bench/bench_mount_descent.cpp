/**
 * @file
 * @brief What the mount descent's width loop costs, and the floor a single-pass design could reach.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `resolve_mount_segs` (`core/src/fwd_router.cpp`) tries key widths `k = W..1`, and every
 * iteration calls `child_registry_t::by_segments`, which walks the **whole** table. So the
 * descent is **O(min(D, W) × N)** slot visits, where W is the widest registered mount, N the live
 * child count and D the `dst` depth.
 *
 * Today `kMountPeekMax = 4` caps W at 3, so the loop runs at most three times and the term is
 * invisible. The 2026-07-30 ruling — *"path must be limited by MRU or not limited at all"* —
 * removes that cap, and the term stops being invisible.
 *
 * @section why Why this bench exists
 *
 * A design review proposed bounding the peek by the registry's own widest mount instead of a
 * constant, and rejected an **inverted single-pass descent** (one table pass, each slot matched
 * against the prefix of its own width) as not worth the machinery. That rejection was measured at
 * **W = 3** — where the loop it replaces runs exactly ONE width, so the optimisation cannot pay by
 * construction. The adversarial pass called the measurement tautological and the single pass
 * "load-bearing, not deferrable". Neither claim was measured at W > 3. This measures it.
 *
 * @section what What is measured, and what is NOT
 *
 * Two arms over the same registry, same segments, same miss/hit shape:
 *
 *  - **loop** — `for k = W..1: by_segments(seg[0..k))`, i.e. exactly what `resolve_mount_segs`
 *    does. On a miss that is W full-table scans.
 *  - **floor** — `by_segments(seg[0..W))` once. **This is not a proposed implementation.** It is
 *    the *lower bound* on any single-pass design: one full-table scan is the least such a design
 *    can do, because it must still visit every slot once.
 *
 * `loop − floor` is therefore the **headroom** available to a single-pass redesign — an upper
 * bound on what it could win, not a prediction of what it would win. A real inverted pass also
 * pays a per-slot width compare the floor arm does not, so it lands somewhere above the floor.
 *
 * If headroom is near zero the redesign cannot pay and the ruling is affordable with the loop as
 * it stands. If headroom is large, the redesign is worth building and measuring for real.
 *
 * @note Deliberately drives the SHIPPED `child_registry_t`, not a model of it. A model arm is how
 *       this project last overstated a win by 5×, and every A/B figure here comes from one binary
 *       on one table.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/child_registry.hpp"
#include "libtracer/transport.hpp"

namespace {

using tr::net::child_registry_t;

/** @brief A link that does nothing — the registry only needs an address. */
struct null_link_t : tr::net::transport_t {
    void send(std::span<const std::byte>) override {}
};

/** @brief Mount widths swept. 3 is today's ceiling; 6 and 12 are the uncapped regime. */
constexpr std::size_t kWidths[] = {3, 6, 12};
/** @brief Live child counts. 16 is where the registry digest was said to make the scan ~0. */
constexpr std::size_t kCounts[] = {1, 16, 64};

[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return 0.5;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : 0.5;
}

/**
 * @brief A mount name of @p w segments, distinct per @p i.
 *
 * Length-matched across the sweep so the per-slot length pre-filter in `by_segments` rejects at
 * the same rate at every width — otherwise a wider mount would look faster purely because its
 * candidates fail earlier.
 */
[[nodiscard]] std::string mount_name(std::size_t w, std::size_t i) {
    std::string s;
    for (std::size_t k = 0; k < w; ++k) {
        if (k != 0) s.push_back('/');
        s += "s";
        s += std::to_string((i * 31 + k) % 97);
    }
    return s;
}

struct row_t {
    std::size_t w = 0;
    std::size_t n = 0;
    std::uint64_t loop_ns = 0;
    std::uint64_t floor_ns = 0;
};

/**
 * @brief Time both arms for (@p w, @p n) on a MISS — the worst case, and the one that matters.
 *
 * A miss is what makes the loop run all W widths; a hit at the widest width exits after one. A
 * frame addressed to a local vertex misses every mount by definition, so this is not a contrived
 * shape — it is every terminus-bound frame the node receives.
 */
[[nodiscard]] row_t measure(std::size_t w, std::size_t n, double seconds) {
    child_registry_t reg;
    std::vector<null_link_t> links(n);
    for (std::size_t i = 0; i < n; ++i) reg.add(mount_name(w, i), links[i]);

    // Segments that match NO registered mount: the terminus-bound case.
    std::vector<std::string> owned;
    for (std::size_t k = 0; k < w; ++k) owned.push_back("z" + std::to_string(k));
    std::vector<std::string_view> seg;
    for (const std::string& s : owned) seg.emplace_back(s);

    std::size_t sink = 0;
    const auto loop_arm = [&] {
        for (std::size_t k = w; k >= 1; --k) {
            const auto* c = reg.by_segments(std::span<const std::string_view>(seg.data(), k));
            sink += (c != nullptr);
        }
    };
    const auto floor_arm = [&] {
        const auto* c = reg.by_segments(std::span<const std::string_view>(seg.data(), w));
        sink += (c != nullptr);
    };

    const auto time_it = [&](auto&& op) -> std::uint64_t {
        for (int i = 0; i < 1000; ++i) op();  // warm
        bench::Latency lat;
        const auto deadline = static_cast<std::uint64_t>(seconds * 1e9);
        const std::uint64_t t0 = bench::now_ns();
        constexpr std::size_t kBatch = 256;
        while (bench::now_ns() - t0 < deadline) {
            const std::uint64_t a = bench::now_ns();
            for (std::size_t i = 0; i < kBatch; ++i) op();
            lat.add((bench::now_ns() - a) / kBatch);
        }
        return lat.summarize().p50;
    };

    // Arms interleaved and order-alternated across the sweep: two builds, or two separated
    // timing windows, leave thermal drift in a comparison whose effect is the same size.
    row_t r{w, n, 0, 0};
    if ((w + n) % 2 == 0) {
        r.loop_ns = time_it(loop_arm);
        r.floor_ns = time_it(floor_arm);
    } else {
        r.floor_ns = time_it(floor_arm);
        r.loop_ns = time_it(loop_arm);
    }
    // Keep both arms live against -O3 DCE without printing: the compiler cannot prove the
    // comparison false, so neither by_segments call can be elided.
    if (sink == static_cast<std::size_t>(-1)) std::fputs("", stderr);
    return r;
}

}  // namespace

int main() {
    const double s = budget_seconds();
    std::printf(
        "Mount descent: the k=W..1 width loop vs ONE full-table scan (the single-pass floor)\n"
        "miss case (terminus-bound frame) — every width is tried, which is the worst case\n\n");
    std::printf("%-6s %-6s %-12s %-12s %-12s %s\n", "W", "N", "loop_ns", "floor_ns", "headroom",
                "loop/floor");

    std::vector<row_t> rows;
    for (const std::size_t w : kWidths) {
        for (const std::size_t n : kCounts) {
            const row_t r = measure(w, n, s);
            rows.push_back(r);
            const double head = static_cast<double>(r.loop_ns) - static_cast<double>(r.floor_ns);
            const double ratio =
                r.floor_ns == 0 ? 0.0
                                : static_cast<double>(r.loop_ns) / static_cast<double>(r.floor_ns);
            std::printf("%-6zu %-6zu %-12llu %-12llu %-12.0f %.2fx\n", r.w, r.n,
                        static_cast<unsigned long long>(r.loop_ns),
                        static_cast<unsigned long long>(r.floor_ns), head, ratio);
            std::printf(
                "RESULT\tlibtracer\tmount-descent-W%zu-N%zu\t0\t1\t1\t0\t0\t0.0\t%llu\t%llu\t%"
                "llu\n",
                r.w, r.n, static_cast<unsigned long long>(r.loop_ns),
                static_cast<unsigned long long>(r.loop_ns),
                static_cast<unsigned long long>(r.loop_ns));
        }
    }

    std::printf(
        "\nSUMMARY `headroom` = loop - floor, the MOST a single-pass descent could win. The floor\n"
        "        arm is ONE full-table scan; it is a lower bound, not a proposed implementation,\n"
        "        because any single-pass design must still visit every slot once. A real inverted\n"
        "        pass also pays a per-slot width compare, so it lands somewhere ABOVE the "
        "floor.\n");
    std::printf(
        "        READ IT THIS WAY: headroom near zero at W>3 means the width loop is not the\n"
        "        term, the inverted single pass cannot pay, and uncapping the descent costs\n"
        "        little. Headroom large and growing with W means the opposite, and the redesign\n"
        "        the 2026-07-30 review deferred should be built and measured for real.\n");
    std::printf(
        "        W=3 is today's shipped ceiling (kMountPeekMax=4). Any conclusion drawn ONLY\n"
        "        from that row is tautological -- the loop runs one width there.\n");
    return 0;
}
