/**
 * @file
 * @brief The ADR-0072 §4 gate harness: per-operation latency of a HANDLER-role vertex's
 *        value-seam read and write — the two legs the reclamation domain's announce pair
 *        sits on (#576).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Deliberately narrow: it times `graph_t::read(handle)` and `graph_t::write(handle)` on a
 * vertex whose value seam is a user callback, and nothing else. Both legs run through the
 * `handlers_slot()` branch, so any per-operation protection the seam grows shows up here
 * undiluted. A `plain` leg (a STORED_VALUE read, which has no seam at all) rides along as
 * the control: it must NOT move between arms, and a run where it does is measuring the
 * machine, not the change.
 *
 * The batch size is FIXED, not calibrated: a calibrated batch is a per-arm quantity, and
 * an A/B whose two arms amortize the clock differently is comparing two harnesses.
 *
 * Usage: `bench_seam_guard [--arm NAME] [--reps N]`. Emits one machine-parseable line per
 * (arm, leg, rep):
 *
 *     SAMPLE arm=<name> leg=<read|write|plain> rep=<i> ns_per_op=<x>
 *
 * The A/B interleaving, the median-of-11 and the core pinning are `run_seam_ab.sh`'s job —
 * this binary is one arm's one measurement, so the SAME source can be built at two
 * revisions (derive-don't-assert: the arms differ only in the library they linked).
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::view::view_t;

/** @brief Operations timed per rep, and the warm-up count ahead of the first one. */
constexpr std::size_t kBatch = 200'000;
constexpr std::size_t kWarmup = 200'000;

/** @brief A VALUE TLV carrying @p payload bytes — the same shape bench_libtracer publishes. */
std::vector<std::byte> value_tlv(std::size_t payload) {
    std::vector<std::byte> p(payload, std::byte{0xAB});
    tr::wire::tlv_t t{};
    t.type = tr::wire::type_t::VALUE;
    t.payload = p;
    return tr::wire::encode(t);
}

/** @brief Borrowed view over a stable buffer — zero alloc, so the leg times the seam. */
view_t borrowed_view(std::span<const std::byte> bytes) {
    return view_t::over(tr::view::borrow_const(bytes));
}

/**
 * @brief Warm @p op, then time @ref kBatch of it @p reps times, one SAMPLE line each.
 */
template <typename Op>
void run_leg(const char* arm, const char* leg, int reps, Op&& op) {
    for (std::size_t i = 0; i < kWarmup; ++i) op();
    for (int r = 0; r < reps; ++r) {
        const std::uint64_t t0 = bench::now_ns();
        for (std::size_t i = 0; i < kBatch; ++i) op();
        const double ns = static_cast<double>(bench::now_ns() - t0) / static_cast<double>(kBatch);
        std::printf("SAMPLE arm=%s leg=%s rep=%d ns_per_op=%.3f\n", arm, leg, r, ns);
    }
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    const char* arm = "unknown";
    int reps = 5;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--arm") == 0 && i + 1 < argc) arm = argv[++i];
        if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) reps = std::atoi(argv[++i]);
    }

    const std::vector<std::byte> tlv = value_tlv(64);
    volatile std::size_t sink = 0;

    // ---- read leg: a HANDLER vertex whose on_read supplies the value -------------------
    graph_t gr;
    tr::graph::handlers_t rh;
    rh.on_read = [&tlv]() -> tr::graph::result_t<rope_t> { return rope_t{borrowed_view(tlv)}; };
    const vertex_handle_t hr =
        gr.register_vertex(*path_t::parse("/h"), role_t::HANDLER, std::move(rh));
    run_leg(arm, "read", reps, [&] { sink += gr.read(hr).has_value() ? 1u : 0u; });

    // ---- write leg: a HANDLER vertex whose on_write consumes the value -----------------
    graph_t gw;
    std::uint64_t taken = 0;
    tr::graph::handlers_t wh;
    wh.on_write = [&taken](const rope_t&) -> tr::graph::result_t<void> {
        ++taken;
        return {};
    };
    const vertex_handle_t hw =
        gw.register_vertex(*path_t::parse("/h"), role_t::HANDLER, std::move(wh));
    run_leg(arm, "write", reps, [&] { sink += gw.write(hw, borrowed_view(tlv)).has_value(); });

    // ---- plain leg (the control): a STORED_VALUE read, which has no seam at all --------
    graph_t gp;
    const vertex_handle_t hp = gp.register_vertex(*path_t::parse("/p"), role_t::STORED_VALUE);
    (void)gp.write(hp, borrowed_view(tlv));
    run_leg(arm, "plain", reps, [&] { sink += gp.read(hp).has_value() ? 1u : 0u; });

    // Prove every leg actually reached its seam — a leg that silently returned NOT_FOUND
    // would time the error path and report a beautiful number for nothing.
    if (taken == 0 || sink == 0) {
        std::fprintf(stderr, "FATAL: a leg never reached its seam (taken=%llu sink=%zu)\n",
                     static_cast<unsigned long long>(taken), static_cast<std::size_t>(sink));
        return 1;
    }
    return 0;
}
