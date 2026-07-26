/**
 * @file
 * @brief The forward-demux baseline: what one FWD forward hop costs, and how much of that
 *        is the registry scan (ADR-0061's one open acceptance condition).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0061 proposes replacing the flat bare-NAME demux with a per-module strip-K
 * descent. Its hot-path cost was UNCONFIRMED and unmeasurable: `bench_forward_heap`
 * times nothing (it counts allocations) and `bench_fanout_clone_storm` measures
 * refcount contention, so no bench relates forward-hop cost to registry size.
 *
 * This is the BASELINE, deliberately landed against TODAY's flat `by_name` so it needs
 * no strip-K code — it is what the ADR is accepted (or rejected) against, and the
 * yardstick the strip-K PR must not regress.
 *
 * **Two axes, because the two terms move in opposite directions.** Sweeping registry
 * size alone would measure only the term strip-K *shrinks* and miss the one it *adds*:
 *
 *  - `fixed` — the target child is registered FIRST, so `by_name` hits on its first
 *    compare and the scan is ~free. This isolates the constant per-hop cost: header
 *    peek, offset dispatch, head rebuild, egress. **This is the term strip-K grows**,
 *    by a `segment[0]=="net"` literal compare plus a module match — both independent of
 *    registry size. The acceptance question is whether this number has headroom for two
 *    more segment compares.
 *  - `scan` — the target child is registered LAST, so `by_name` walks the whole table.
 *    `scan(N) - fixed(N)` is the scan's marginal cost at N links. **This is the term
 *    strip-K shrinks**: today's `by_name` scans every link and then asks every bus child
 *    to `peer_link`, whereas the per-module key and the per-endpoint `resolve_peer`
 *    narrow both passes to one module's members.
 *
 * Emits one RESULT row per (mode, N) in bench_common's shared format, so collate.py and
 * the perf history pick it up unchanged. `fanout` carries N (registered children) and
 * `endpoints` carries the target's 1-based scan position.
 *
 * NOT a wall-clock throughput number: every hop is driven synchronously on this thread
 * with a capture transport, so the reported latency is pure router cost with no socket,
 * no thread handoff, and no allocation on the measured path.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Registry sizes swept — N = children registered on the node. */
constexpr std::size_t kLinkCounts[] = {1, 2, 4, 8, 16, 32, 64};

/**
 * @brief Per-point wall-clock budget, in seconds — the ONE tunable, and it is a policy
 *        input, not a bound: `LIBTRACER_BENCH_SECONDS` overrides it.
 *
 * How many samples that buys is derived, never declared. Longer means tighter
 * percentiles; it cannot change what is measured.
 */
constexpr double kDefaultBudgetSeconds = 1.0;

/**
 * @brief Seconds per measured point (env-overridable; non-positive/garbage ⇒ default).
 */
[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

/**
 * @brief The batch size at which per-hop cost stops falling — CALIBRATED, not chosen.
 *
 * One hop costs tens of nanoseconds, the same order as `clock_gettime` itself, so timing
 * each hop individually measures the clock rather than the router. (An early draft of
 * this bench did exactly that and reported the whole-table scan *beating* the first-hit
 * lookup — the signature of a clock-dominated window.) Batching amortizes the two clock
 * reads, but the right batch size is a property of the HOST's clock, not a constant to
 * hardcode: pick it too small on a slow-clock runner and the artifact returns silently.
 *
 * So derive it. While clock overhead still dominates, doubling the batch nearly halves
 * the apparent per-hop cost; once it is amortized, the curve plateaus. Double until the
 * improvement is under @ref kPlateau — a convergence tolerance, the one number a
 * numerical method legitimately owns — and report the batch size in the output so the
 * measurement states its own assumption instead of hiding it.
 * @param hop Runs one forward hop.
 */
constexpr double kPlateau = 0.05;  // <5% improvement from doubling ⇒ amortized

template <typename Hop>
[[nodiscard]] std::size_t calibrate_batch(Hop&& hop) {
    double prev = 0.0;
    for (std::size_t batch = 1; batch <= (1U << 20); batch *= 2) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) hop();
        const double per_hop =
            static_cast<double>(bench::now_ns() - a) / static_cast<double>(batch);
        if (prev > 0.0 && per_hop > prev * (1.0 - kPlateau)) return batch;
        prev = per_hop;
    }
    return 1U << 20;  // pathological clock — cap the calibration, not the measurement
}

/**
 * @brief A transport that only counts what it was handed — no I/O, no allocation.
 *
 * The scatter-gather `send` is overridden for the same reason `bench_forward_heap` does
 * it: the base class flattens an iov into a temporary vector, which would put a
 * measurement artifact (an allocation) inside the timed window. A real zero-copy
 * transport (sendmsg/writev/RDMA) overrides it exactly this way.
 */
struct capture_transport_t : transport_t {
    std::size_t sends = 0;
    /** @brief Push an inbound frame up this link's installed receiver, as a real one does. */
    void deliver(std::span<const std::byte> f) { rx_.deliver_borrowed(f); }
    void send(std::span<const std::byte> f) override {
        ++sends;
        sink += f.size();
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        ++sends;
        for (const auto& s : iov) sink += s.size();
    }
    /** @brief Accumulates lengths so the compiler cannot elide the egress. */
    std::size_t sink = 0;
};

/** @brief Append a NAME-only PATH TLV over @p segs. */
void emit_path(std::vector<std::byte>& out, std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
}

/** @brief FWD{ op=WRITE, dst, src, VALUE } — the frame a forward hop shrinks and grows. */
std::vector<std::byte> make_fwd(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src,
                                std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    emit_path(body, src);
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, payload);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/**
 * @brief Time one forward hop with @p links children, the target at @p target_pos.
 *
 * A frame arrives on "in" addressed to "out"; the hop strips "out" from `dst`, prepends
 * "in" to `src`, and sends on "out" — no terminus, no local vertex. Filler children
 * ("l0", "l1", …) pad the registry so the target sits at scan position @p target_pos.
 * @param links      Total forwardable children registered (N).
 * @param target_pos 1-based position of "out" among them (1 = first, `links` = last).
 * @param mode       RESULT mode tag ("fixed" or "scan").
 */
std::uint64_t run_point(std::size_t links, std::size_t target_pos, const char* mode) {
    graph_t graph;
    fwd_router_t router(graph);
    capture_transport_t in_link;
    capture_transport_t out_link;
    // Stable storage: add_child holds a reference for the transport's lifetime.
    std::vector<capture_transport_t> filler(links);

    std::size_t next_filler = 0;
    for (std::size_t i = 1; i <= links; ++i) {
        if (i == target_pos) {
            router.add_child("net/ws-client/out", out_link);
        } else {
            // Zero-padded so every filler is the SAME LENGTH AS THE TARGET ("out", giving a
            // 17-byte qualified name). Two separate things were wrong before. Unpadded,
            // "l0".."l9" were 16 bytes and "l10"+ were 17, so `by_segments`'s length
            // pre-filter rejected a varying fraction of the table before any string compare
            // and the reported "ns per link" measured that ratio rather than the scan. And a
            // filler that differs in length from the target is rejected on the length check
            // alone, so it never reaches the segment compare at all — which measures the
            // list WALK, not the scan. Matching the target's width is what makes this the
            // honest worst case the row claims to report.
            char fname[32];
            std::snprintf(fname, sizeof fname, "net/ws-client/l%02zu", next_filler);
            router.add_child(fname, filler[next_filler]);
            ++next_filler;
        }
    }

    // The INBOUND child is registered LAST, and ONLY here. A forward hop performs TWO scans
    // of the registry, not one: `by_segments` for the dst mount, and `entry_by_name` for the
    // inbound child's precomputed `src` prefix. Registering it last measures the worst case
    // for that second scan, so the two are never conflated.
    //
    // This used to `add_child` the inbound link a second time, at the TOP of the function as
    // well. The intent -- "registered last" -- therefore never took effect: `entry_by_name`
    // matched the first slot at position 1, and the second scan stayed exactly as invisible
    // as the revision this comment was written to fix. Worse, the duplicate registration was
    // itself a live registry bug (a shadow slot that survives `erase`), which is how it was
    // finally caught. Keep this to ONE call.
    router.add_child("net/ws-server/in", in_link);

    const std::byte payload[4] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                  std::byte{0xEF}};
    const std::vector<std::byte> frame =
        make_fwd({"net", "ws-client", "out", "sensor", "temp"}, {"reply"},
                 std::span<const std::byte>(payload, 4));

    // Drive the hop through the INBOUND LINK's receiver, not `router.on_frame` directly.
    // That is how a real transport delivers: `add_child` installs a receiver bound to a
    // stable per-child ctx, and the hop reads its mount run off that ctx instead of scanning
    // the registry for it. Calling `on_frame` by name takes the ctx-less entry — a path only
    // tests and SDK hosts use — so the bench was timing a routing shape production never
    // executes, and was blind to the ctx optimization entirely.
    // Drive the hop through the INBOUND LINK's receiver, not `router.on_frame` directly —
    // that is how a real transport delivers, and `add_child` wires a per-child receiver ctx
    // for it. Calling `on_frame` by name takes a ctx-less entry only tests and SDK hosts use,
    // so the bench was timing a routing shape production never executes.
    const auto hop = [&] { in_link.deliver(frame); };

    // Calibration doubles as the warm-up: it primes lazy statics and caches, and its
    // own timings are discarded.
    const std::size_t batch = calibrate_batch(hop);

    // Sample until the budget is spent — the sample COUNT falls out of the host's speed
    // rather than being declared. Each sample is one amortized batch.
    bench::Latency lat;
    const std::uint64_t deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    const std::uint64_t t0 = bench::now_ns();
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) hop();
        lat.add((bench::now_ns() - a) / batch);  // per-hop ns, clock cost amortized
        ++batches;
        total = bench::now_ns() - t0;
    }

    const double hops = static_cast<double>(batches) * static_cast<double>(batch);
    const double hops_per_s = total == 0 ? 0.0 : hops * 1e9 / static_cast<double>(total);
    // size_bytes = the frame the hop carried; fanout = N; endpoints = scan position.
    const bench::Latency::Summary s = lat.summarize();
    bench::emit("libtracer", mode, frame.size(), links, target_pos, hops_per_s, hops_per_s, 0.0, s);

    // State the measurement's own parameters: a reader can tell whether the window was
    // amortized on THIS host, rather than trusting a constant baked in on another.
    std::printf("NOTE mode=%s links=%zu batch=%zu samples=%zu\n", mode, links, batch, batches);
    if (out_link.sends == 0) std::printf("WARN mode=%s links=%zu forwarded NOTHING\n", mode, links);
    return s.p50;
}

}  // namespace

int main() {
    std::vector<std::uint64_t> fixed;
    std::vector<std::uint64_t> scan;

    // Axis 1 — fixed per-hop cost: target first, so the scan hits immediately. The term
    // strip-K ADDS (a literal + a module compare) is measured against this number.
    for (const std::size_t n : kLinkCounts) fixed.push_back(run_point(n, 1, "fwd-demux-fixed"));
    // Axis 2 — scan cost: target last, so by_name walks the whole table. The delta from
    // axis 1 at the same N is the scan's marginal cost — the term strip-K NARROWS.
    for (const std::size_t n : kLinkCounts) scan.push_back(run_point(n, n, "fwd-demux-scan"));

    // The derived answer to ADR-0061's acceptance question, so it need not be
    // reconstructed by hand from the RESULT rows.
    std::printf("\n%-8s %-14s %-14s %-14s %s\n", "links", "fixed_p50_ns", "scan_p50_ns",
                "scan_delta_ns", "ns_per_link");
    for (std::size_t i = 0; i < std::size(kLinkCounts); ++i) {
        const double delta = static_cast<double>(scan[i]) - static_cast<double>(fixed[i]);
        const std::size_t compares = kLinkCounts[i] > 1 ? kLinkCounts[i] - 1 : 1;
        std::printf("%-8zu %-14llu %-14llu %-14.1f %.2f\n", kLinkCounts[i],
                    static_cast<unsigned long long>(fixed[i]),
                    static_cast<unsigned long long>(scan[i]), delta,
                    delta / static_cast<double>(compares));
    }
    std::printf(
        "\nSUMMARY fixed_per_hop_ns=%llu (size-independent — the term strip-K ADDS,\n"
        "        a segment[0]==\"net\" literal + a module compare, is measured against this)\n",
        static_cast<unsigned long long>(fixed.empty() ? 0 : fixed[0]));
    return 0;
}
