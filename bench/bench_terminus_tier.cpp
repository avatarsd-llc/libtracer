/**
 * @file
 * @brief Terminus resolve: the EAGER arena reader vs the LAZY view reader, on the same frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * [ADR-0053](../docs/adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md)'s 2026-07-06
 * amendment ratified the lazy `tlv_view_t` reader as serving "the **whole owning tier**,
 * single-link ropes included, so the lazy path is exercised by **every** TCP/QUIC/WS frame, not
 * only the rare fragmented ones".
 *
 * What shipped does the opposite. `fwd_router_t::on_frame_rope_impl` short-circuits a single-link
 * rope onto the span path (`fwd_router.cpp:337`, commented "the pre-ADR-0053 view path,
 * unchanged"), so the lazy reader serves ONLY multi-link ropes — precisely the rare fragmented
 * case the amendment said it must not be limited to. Every ordinary WS/TCP/UDP frame still pays
 * the eager arena decode, which `bench_forward_heap` counts at 9 allocations / 937 bytes per
 * terminus resolve.
 *
 * Nothing measured whether that short-circuit is a fast path or a missed one, because
 * `bench_forward_demux` times the FORWARD hop (which never resolves) and `bench_forward_heap`
 * counts allocations without timing them. This bench closes that gap: it drives the SAME logical
 * frame through both public `op_resolver_t::resolve` overloads — the differential-oracle pairing
 * `op_resolve_view_test` already uses for correctness — and reports latency and allocation count
 * per tier.
 *
 * Two axes, because the tiers can trade differently along each:
 *   - **payload size** — the arena copies structure eagerly; the view holds regions. Whether
 *     laziness wins should depend on how much of the frame the resolve actually touches.
 *   - **link count** — 1 link is the case in dispute (today's short-circuit); >1 is what the
 *     lazy tier serves now, and is the control showing the rope cursor's stitching cost.
 *
 * The measurement is batch-amortized and self-calibrating for the same reason
 * `bench_forward_demux` is: a resolve is close enough to `clock_gettime` that per-op timing
 * measures the clock. The batch size is derived on THIS host and printed.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Payload sizes swept — how many value bytes the resolved WRITE carries. */
constexpr std::size_t kPayloadSizes[] = {4, 64, 512, 4096, 16384, 65536};

/** @brief Link counts swept — 1 is the disputed short-circuit; >1 is what the lazy tier serves. */
constexpr std::size_t kLinkCounts[] = {1, 2, 8};

constexpr double kDefaultBudgetSeconds = 1.0;
constexpr double kPlateau = 0.05;

[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

template <typename Op>
[[nodiscard]] std::size_t calibrate_batch(Op&& op) {
    double prev = 0.0;
    for (std::size_t batch = 1; batch <= (1U << 20); batch *= 2) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        const double per_op = static_cast<double>(bench::now_ns() - a) / static_cast<double>(batch);
        if (prev > 0.0 && per_op > prev * (1.0 - kPlateau)) return batch;
        prev = per_op;
    }
    return 1U << 20;
}

// --- allocation counting (the RAM axis) --------------------------------------
// A counting global allocator, armed around exactly one resolve. Same instrument
// bench_forward_heap uses, so the two are directly comparable.
std::size_t g_allocs = 0;
std::size_t g_bytes = 0;
bool g_arm = false;

}  // namespace

void* operator new(std::size_t n) {
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    void* const p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

/** @brief A FWD{WRITE} addressed at a local vertex, carrying @p payload_bytes of value. */
std::vector<std::byte> make_write_frame(std::size_t payload_bytes) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));

    std::vector<std::byte> dst;
    for (std::string_view s : {"sensor", "temp"}) tr::wire::emit_name(dst, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, dst);

    std::vector<std::byte> src;
    tr::wire::emit_name(src, "origin");
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, src);

    std::vector<std::byte> payload(payload_bytes, std::byte{0xAB});
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload));

    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/** @brief Split @p frame into @p links roughly-equal rope links (1 ⇒ a single link). */
tr::view::rope_t rope_of(std::span<const std::byte> frame, std::size_t links) {
    tr::view::rope_t r;
    const std::size_t step = (frame.size() + links - 1) / links;
    for (std::size_t at = 0; at < frame.size(); at += step) {
        const std::size_t n = std::min(step, frame.size() - at);
        const auto v = tr::view::over_bytes(frame.subspan(at, n));
        if (v) r.append(*v);
    }
    return r;
}

void seed(graph_t& g) {
    (void)g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
}

/**
 * @brief One measured point: resolve @p frame through one tier, timed and allocation-counted.
 * @param tier "arena" (eager, span tier) or "view" (lazy, owning tier).
 */
void run_point(std::span<const std::byte> frame, std::size_t links, const char* tier) {
    // Production's terminus arena decodes into the router's INJECTED pmr resource
    // (`fwd_router.cpp:546`, `decode_into(frame, *mr_)` — ADR-0039 §1), not into the default
    // resource. An earlier revision of this bench passed `get_default_resource()`, which is
    // plain `new`: that measured an arena tier no deployment runs, and understated it.
    // A monotonic buffer over a stack slab is the shape the terminus actually has.
    std::array<std::byte, 1 << 16> slab;
    std::pmr::monotonic_buffer_resource arena_mr(slab.data(), slab.size(),
                                                 std::pmr::null_memory_resource());
    graph_t g;
    seed(g);
    tr::graph::op_resolver_t r(g);

    const bool lazy = std::strcmp(tier, "view") == 0;
    const bool flat = std::strcmp(tier, "flat+arena") == 0;

    // One resolve, with the allocator armed — the RAM axis. Counted outside the timed
    // window so the counter itself never distorts the latency number.
    g_allocs = 0;
    g_bytes = 0;
    g_arm = true;
    if (lazy) {
        const auto v = tr::wire::tlv_view_t::over(rope_of(frame, links));
        if (v) (void)r.resolve(*v, "cli");
    } else if (flat) {
        const tr::view::view_t f = rope_of(frame, links).materialize();
        arena_mr.release();
        const auto a = tr::wire::decode_into(f.bytes(), arena_mr);
        if (a) (void)r.resolve(*a, "cli");
    } else {
        arena_mr.release();
        const auto a = tr::wire::decode_into(frame, arena_mr);
        if (a) (void)r.resolve(*a, "cli");
    }
    g_arm = false;
    const std::size_t allocs = g_allocs;
    const std::size_t bytes = g_bytes;

    // The timed op includes ADOPTION (arena decode / view::over), because that is the work
    // a terminus actually does per frame — comparing only the walk would flatter the arena,
    // whose cost IS the eager decode.
    const auto op = [&] {
        if (lazy) {
            const auto v = tr::wire::tlv_view_t::over(rope_of(frame, links));
            if (v) (void)r.resolve(*v, "cli");
        } else if (flat) {
            // The alternative ADR-0053 rejected for multi-link: pay ONE copy, then read
            // contiguously. Measured rather than assumed — if it wins, the lazy tier has
            // no remaining case at all.
            const tr::view::view_t f = rope_of(frame, links).materialize();
            arena_mr.release();
            const auto a = tr::wire::decode_into(f.bytes(), arena_mr);
            if (a) (void)r.resolve(*a, "cli");
        } else {
            arena_mr.release();
            const auto a = tr::wire::decode_into(frame, arena_mr);
            if (a) (void)r.resolve(*a, "cli");
        }
    };

    const std::size_t batch = calibrate_batch(op);
    bench::Latency lat;
    const std::uint64_t t0 = bench::now_ns();
    const auto deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        lat.add((bench::now_ns() - a) / batch);
        ++batches;
        total = bench::now_ns() - t0;
    }

    const double ops = static_cast<double>(batches) * static_cast<double>(batch);
    const double ops_per_s = total == 0 ? 0.0 : ops * 1e9 / static_cast<double>(total);
    const bench::Latency::Summary s = lat.summarize();
    std::string mode = std::string("terminus-") + tier;
    bench::emit("libtracer", mode.c_str(), frame.size(), links, 1, ops_per_s, ops_per_s, 0.0, s);
    std::printf("NOTE mode=%s frame=%zu links=%zu batch=%zu samples=%zu allocs=%zu bytes=%zu\n",
                mode.c_str(), frame.size(), links, batch, batches, allocs, bytes);
}

}  // namespace

int main() {
    std::printf("# Terminus tier: eager arena reader vs lazy view reader, SAME frame (ADR-0053)\n");
    for (const std::size_t payload : kPayloadSizes) {
        const std::vector<std::byte> frame = make_write_frame(payload);
        for (const std::size_t links : kLinkCounts) {
            // The arena tier is link-count-independent by construction (it decodes contiguous
            // bytes), so it is measured once per payload, at links=1.
            if (links == 1) run_point(frame, 1, "arena");
            run_point(frame, links, "view");
            if (links > 1) run_point(frame, links, "flat+arena");
        }
    }
    std::printf(
        "\nRead: 'view' at links=1 is the case ADR-0053 ratified and the code does NOT do.\n");
    return 0;
}
