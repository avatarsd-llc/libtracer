/**
 * @file
 * @brief The mount-resolve hot path, A/B-able across BINARIES: one forward hop through a
 *        registry of N mounts of width W (#523).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section why Why this exists, and why it is shaped this way
 *
 * The mount-width lift replaces a `k = W..1` descent — which re-scanned the whole registry at
 * every width, O(W×N) — with ONE pass that matches each slot against the prefix of its own
 * width. The question the maintainer put on it is not "is the wide case faster" (it must be)
 * but **"is the case that already worked, W ≤ 3, unchanged"**. A regression there is a reject,
 * not a trade.
 *
 * So this bench is deliberately written to compile UNCHANGED against both the pre-lift and the
 * post-lift tree: it touches only `fwd_router_t::add_child` / `on_frame`, whose spellings are
 * source-compatible across the change. Two binaries, one source, interleaved runs on one
 * machine — the A/B a single-binary "old arm vs new arm" cannot give, because it cannot see
 * what the change did to the slot layout, the stack frame, or the inlining around the pass.
 *
 * @section what What is timed
 *
 * ONE `on_frame` of a `FWD` addressed through the LAST-registered mount, which is the worst
 * case for any single pass (every slot is visited) and the honest one for the old loop too.
 * The whole hop is timed — peek, descent, head rebuild, egress — because that, not the
 * descent in isolation, is what a regression would be paid on.
 *
 * @section instrument The reachability instrument, and breaking the line
 *
 * Every cell reports `hits` — how many frames actually reached the intended egress link. A
 * mount bench that resolves NOTHING still produces beautiful numbers: the frame simply falls
 * through to the terminus, and the terminus path is a different (and cheaper, on a miss)
 * amount of work. `hits == iterations` is what says the timed line is the line.
 *
 * `LIBTRACER_BENCH_BREAK=1` breaks it on purpose: the frame is addressed one segment WIDE of
 * the registered mount, so no mount matches. Run it once and read the instrument — `hits`
 * must drop to zero and `miss` must take every frame. A build where the "broken" run still
 * reports hits is measuring something other than what it says.
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
#include "libtracer/tracer.hpp"

namespace {

/** @brief An egress link that counts the frames the router hands it. */
struct counting_link_t : tr::net::transport_t {
    std::size_t sent = 0; /**< @brief Frames this endpoint received. */
    void send(std::span<const std::byte>) override { ++sent; }
    void send(std::span<const std::span<const std::byte>>) override { ++sent; }
};

/** @brief One measured cell: a registry of @ref n mounts, each @ref w segments wide. */
struct cell_t {
    std::size_t w = 0;         /**< @brief Mount width (segments per key). */
    std::size_t n = 0;         /**< @brief Live children in the registry. */
    std::uint64_t p50 = 0;     /**< @brief Median per-hop latency, ns. */
    std::uint64_t p99 = 0;     /**< @brief p99 per-hop latency, ns. */
    std::size_t hits = 0;      /**< @brief Frames that reached the intended egress link. */
    std::size_t iters = 0;     /**< @brief Frames driven. */
    std::size_t key_bytes = 0; /**< @brief Joined length of the matched mount key. */
};

/** @brief The widths swept. 1..3 is today's shipped regime; 8 and 12 are the lift's. */
constexpr std::size_t kWidths[] = {1, 2, 3, 8, 12};
/** @brief Registry sizes. */
constexpr std::size_t kCounts[] = {8, 64};

/** @brief Seconds per timed window; `LIBTRACER_BENCH_SECONDS` overrides. */
[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return 0.35;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : 0.35;
}

/** @brief True when the run should deliberately address NO registered mount. */
[[nodiscard]] bool break_the_line() {
    const char* const env = std::getenv("LIBTRACER_BENCH_BREAK");
    return env != nullptr && env[0] == '1';
}

/**
 * @brief Mount @p i's segments at width @p w — length-matched across the sweep.
 *
 * Every segment is the same length at every width, so the joined-length pre-filter in the
 * registry rejects at the same rate everywhere; otherwise a wider mount would look faster
 * purely because its candidates fail earlier.
 */
[[nodiscard]] std::vector<std::string> mount_segments(std::size_t w, std::size_t i) {
    std::vector<std::string> segs;
    for (std::size_t k = 0; k < w; ++k) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "s%02zu", (i * 31 + k) % 97);
        segs.emplace_back(buf);
    }
    return segs;
}

/** @brief Segments joined by `/`. */
[[nodiscard]] std::string join(const std::vector<std::string>& segs) {
    std::string out;
    for (const std::string& s : segs) {
        if (!out.empty()) out.push_back('/');
        out += s;
    }
    return out;
}

/** @brief `["a","b"]` as a PATH TLV appended to @p out. */
void emit_path(std::vector<std::byte>& out, const std::vector<std::string>& segs) {
    std::vector<std::byte> body;
    for (const std::string& s : segs) tr::wire::emit_name(body, s);
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{.pl = true}, body);
}

/** @brief A `FWD{WRITE, dst, src=[origin], payload}` frame. */
[[nodiscard]] std::vector<std::byte> make_fwd(const std::vector<std::string>& dst) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    emit_path(body, std::vector<std::string>{"origin"});
    const std::byte payload[8] = {};
    tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(payload, 8));
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, tr::wire::type_t::FWD, tr::wire::opt_t{.pl = true}, body);
    return frame;
}

/** @brief Time one (@p w, @p n) cell. */
[[nodiscard]] cell_t measure(std::size_t w, std::size_t n, double seconds, bool broken) {
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router{graph};
    std::vector<counting_link_t> links(n);
    counting_link_t inbound;
    for (std::size_t i = 0; i < n; ++i) router.add_child(join(mount_segments(w, i)), links[i]);
    router.add_child("in", inbound);

    // Address the LAST-registered mount: the worst case for a single pass (every slot is
    // visited) and an honest one for the width loop too (it scans the whole table per width).
    const std::vector<std::string> target = mount_segments(w, n - 1);
    std::vector<std::string> dst;
    if (broken) {
        // Address something NO registered mount prefixes: the frame falls to the terminus and
        // the egress counter must stay flat.
        dst.emplace_back("zzz");
    } else {
        dst = target;
    }
    dst.emplace_back("leaf");
    const std::vector<std::byte> frame = make_fwd(dst);
    const std::span<const std::byte> f(frame);

    counting_link_t& sink = links[n - 1];
    const auto op = [&] { router.on_frame("in", f); };

    for (int i = 0; i < 2000; ++i) op();  // warm
    const std::size_t warm_hits = sink.sent;

    bench::Latency lat;
    std::size_t iters = 0;
    const auto deadline = static_cast<std::uint64_t>(seconds * 1e9);
    const std::uint64_t t0 = bench::now_ns();
    constexpr std::size_t kBatch = 128;
    while (bench::now_ns() - t0 < deadline) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < kBatch; ++i) op();
        lat.add((bench::now_ns() - a) / kBatch);
        iters += kBatch;
    }
    const auto s = lat.summarize();
    cell_t c;
    c.w = w;
    c.n = n;
    c.p50 = s.p50;
    c.p99 = s.p99;
    c.hits = sink.sent - warm_hits;
    c.iters = iters;
    c.key_bytes = join(target).size();
    return c;
}

}  // namespace

int main() {
    const double s = budget_seconds();
    const bool broken = break_the_line();
    std::printf("Mount resolve: ONE forward hop through a registry of N mounts of width W\n");
    if (broken)
        std::printf(
            "LINE BROKEN ON PURPOSE (LIBTRACER_BENCH_BREAK=1): the dst matches NO mount.\n"
            "Read the instrument: `hits` must be 0 in every cell. If it is not, this bench is\n"
            "not measuring the mount descent and no number below means anything.\n");
    std::printf("\n%-4s %-4s %-10s %-10s %-10s %-10s %s\n", "W", "N", "p50_ns", "p99_ns", "hits",
                "iters", "key_bytes");
    for (const std::size_t w : kWidths) {
        for (const std::size_t n : kCounts) {
            // W>3 at N=8 is not in the ruled cell list; the deep regime is measured at N=64,
            // where the pass cost is visible at all.
            if (w > 3 && n != 64) continue;
            const cell_t c = measure(w, n, s, broken);
            std::printf("%-4zu %-4zu %-10llu %-10llu %-10zu %-10zu %zu\n", c.w, c.n,
                        static_cast<unsigned long long>(c.p50),
                        static_cast<unsigned long long>(c.p99), c.hits, c.iters, c.key_bytes);
            std::printf("CELL\tW%zu\tN%zu\t%llu\t%llu\t%zu\t%zu\n", c.w, c.n,
                        static_cast<unsigned long long>(c.p50),
                        static_cast<unsigned long long>(c.p99), c.hits, c.iters);
            std::printf(
                "RESULT\tlibtracer\tmount-resolve-W%zu-N%zu\t0\t1\t1\t0\t0\t0.0\t%llu\t%llu\t%"
                "llu\n",
                c.w, c.n, static_cast<unsigned long long>(c.p50),
                static_cast<unsigned long long>(c.p50), static_cast<unsigned long long>(c.p99));
        }
    }
    std::printf(
        "\nREAD IT THIS WAY: the W<=3 rows are the SHIPPED shapes and must not regress — a\n"
        "regression there is a reject, not a trade. The W=8/12 rows are the shapes the lift\n"
        "makes reachable at all; before it they were unroutable, so there is no 'before'\n"
        "number for them, only a 'does it cost what the W<=3 rows cost' question.\n");
    return 0;
}
