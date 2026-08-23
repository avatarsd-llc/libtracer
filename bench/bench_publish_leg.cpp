/**
 * @file
 * @brief The PRODUCER-SIDE PUBLISH leg, isolated: `assign` (state only) against `write`
 *        (state then deliver), at 0 and 1 subscribers — the in-tree instrument for the two
 *        legs RFC-0025 §4.6.2 banked from a throwaway harness (#1485, #1495).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section why Why this bench exists
 *
 * RFC-0025 §4.6.2 quoted a **53 ns / 71 ns** baseline write at 0 / 1 subscribers as the basis
 * for its **+54 %** producer-ring premium. The 2026-08-24 erratum to that section demoted both
 * absolutes to a host-stamped basis, because the host, governor and compiler behind them were
 * never recorded and are not recoverable — and because **no bench in this tree measured that
 * leg at all**. Every gated point runs a whole `graph_t::write`; none of them isolates the
 * state half. This file is that missing instrument, so the parenthetical can be re-banked from
 * something a reader can re-run.
 *
 * @section arms The four arms, and what separates them
 *
 * The erratum's other correction was definitional: two figures were read as the same quantity
 * when they time different operations. So the two operations are run side by side here, in one
 * process, over the same value and the same vertex:
 *
 *   - `publish-assign` — @ref tr::graph::graph_t::assign. RFC-0008's STATE transition and
 *     nothing else: swap the LKV (lock-free), bump the write sequence, wake awaiters, mark the
 *     vertex for the next covering sweep. **Sends nothing.** This is the `vertex_t::store` leg
 *     the RFC's 53 ns names; `store()` itself is private to `graph_t`, and `assign` is the
 *     public verb that reaches it without fanning out.
 *   - `publish-write` — @ref tr::graph::graph_t::write. `assign` **then deliver**: the same
 *     state transition plus the fan-out loop. At fan 0 the loop finds no edges, so the
 *     `write` − `assign` difference at fan 0 is the cost of *asking*; at fan 1 it is the cost
 *     of one real delivery.
 *
 * Each arm runs at fan 0 (no subscriber at all) and fan 1 (one in-process callback), which is
 * the 0/1-subscriber axis the RFC's pair of numbers sweeps. Note that a subscriber changes
 * `assign` too, and not by accident: with nobody observing at or above the vertex, `assign`
 * skips marking it pending, so `publish-assign` fan 0 → fan 1 prices exactly that bookkeeping.
 *
 * @section reading What the RESULT columns mean here
 *
 *     RESULT libtracer publish-assign 64 <fan> 1 <ops/s> <deliv/s> <MB/s> p50 p99 mean
 *
 * `deliv/s` is **0 on every `assign` row and on `publish-write` at fan 0**, following the #553
 * latency-only convention: those arms deliver nothing, and publishing the store rate in a
 * column named "deliveries" is how a definitional confusion of exactly this kind starts. The
 * operation rate is in the `pub/s` column for every arm. Latency is per-op over a
 * window-floored calibrated batch (`calibrate_batch_for_window`, #1358) — the op runs near
 * 80 ns on the studio host, which is inside the clock's own grain if timed one at a time.
 *
 * @section gating Registered, charted, NOT gated — and why
 *
 * This is a **registered instrument** (`bench/gen_results_page.py`'s `INSTRUMENTS`), run on
 * demand, with its provenance stamped by `bench/host_guard.py`. It is deliberately **not** in
 * `perf_gate.py`'s `POINTS`: every arm here is a strict subset of `inproc/64/1/1`, which is
 * already gated, so a pullback in the publish leg fails that point too. The registry's own
 * rule for admitting a point — that it not be a re-emission of an already-gated path — is what
 * keeps it out. What this bench is for is *attribution*: which half of a write moved.
 *
 * Nothing here is a cap. RFC-0025 §4.6.2's absolutes are a basis for percentages on one host,
 * never a conformance bar, and the "53–55 ns" acceptance range that once circulated appears in
 * no RFC — see that document's 2026-08-24 erratum, §Caps that never existed here.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tracer.hpp"

namespace {

using bench::calibrate_batch_for_window;
using bench::emit;
using bench::Latency;
using bench::now_ns;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::view_t;

/** @brief Payload bytes, matching the reference point every other bench reports at. */
constexpr std::size_t kSize = 64;

/** @brief Operations in the bulk-timed throughput window. */
constexpr std::size_t kBulkOps = 400'000;

/** @brief How long each arm's latency window runs, in nanoseconds. */
constexpr std::uint64_t kLatencyWindowNs = 300'000'000;

/** @brief A VALUE TLV carrying @p payload bytes — the same shape `bench_libtracer` writes. */
[[nodiscard]] std::vector<std::byte> value_tlv(std::size_t payload) {
    std::vector<std::byte> p(payload, std::byte{0xAB});
    tr::wire::tlv_t t{};
    t.type = tr::wire::type_t::VALUE;
    t.payload = p;
    return tr::wire::encode(t);
}

/** @brief Per-op owned heap view — the allocating publish path, as `inproc` runs it. */
[[nodiscard]] view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief Which half of a write an arm drives. */
enum class verb_t { ASSIGN, WRITE };

/** @brief The RESULT `mode` string for @p v. */
[[nodiscard]] const char* verb_name(verb_t v) {
    return v == verb_t::ASSIGN ? "publish-assign" : "publish-write";
}

/**
 * @brief Run one (verb, subscriber-count) arm and emit its RESULT row.
 *
 * A fresh `graph_t` per arm, so no arm inherits another's registry, edges or allocator state.
 * The non-vacuity check is the write sequence: an arm whose publishes were all declined would
 * otherwise report a very fast nothing.
 *
 * @param verb Which verb to time.
 * @param subs 0 or 1 in-process subscribers on the vertex under test.
 */
void run(verb_t verb, std::size_t subs) {
    graph_t g;
    const path_t path = *path_t::parse("/bench/publish");
    const vertex_handle_t v = g.register_vertex(path, role_t::STORED_VALUE);

    std::atomic<std::uint64_t> received{0};
    // Function scope, NOT the `if` below: the `F&` subscribe overload binds the callback by
    // reference, so a lambda declared inside the branch would be destroyed while the edge
    // still pointed at it — and the arm would time a dangling call rather than a delivery.
    auto cb = [&](const tr::view::rope_t&) { received.fetch_add(1, std::memory_order_relaxed); };
    if (subs != 0) (void)g.subscribe(path, cb);

    const std::vector<std::byte> tlv = value_tlv(kSize);
    std::atomic<std::uint64_t> ok{0};
    const auto op = [&] {
        const bool good = verb == verb_t::ASSIGN ? g.assign(v, owned_view(tlv)).has_value()
                                                 : g.write(v, owned_view(tlv)).has_value();
        if (good) ok.fetch_add(1, std::memory_order_relaxed);
    };

    for (std::size_t i = 0; i < 10'000; ++i) op();  // warm the allocator and the branch history

    // Bulk window first: one timer around many operations, which is the instrument that is
    // independent of the clock's per-sample cost.
    const std::uint64_t before = ok.load(std::memory_order_relaxed);
    const std::uint64_t t0 = now_ns();
    for (std::size_t i = 0; i < kBulkOps; ++i) op();
    const std::uint64_t bulk_ns = now_ns() - t0;
    const std::uint64_t published = ok.load(std::memory_order_relaxed) - before;
    const double ops_per_s =
        bulk_ns == 0 ? 0.0 : static_cast<double>(kBulkOps) * 1e9 / static_cast<double>(bulk_ns);

    // Then the per-op distribution, sampled over a window-floored batch.
    const std::size_t batch = calibrate_batch_for_window(op);
    Latency lat;
    const std::uint64_t l0 = now_ns();
    while (now_ns() - l0 < kLatencyWindowNs) {
        const std::uint64_t a = now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        lat.add((now_ns() - a) / batch);
    }

    // `assign` delivers nothing, and neither does a `write` with no edges: publishing the
    // operation rate under a column named "deliveries" is the definitional confusion the
    // §4.6.2 erratum corrects. Zero means "this row does not measure throughput" (#553).
    const bool delivers = verb == verb_t::WRITE && subs != 0;
    const double deliv_s = delivers ? ops_per_s : 0.0;
    const double mb_s = deliv_s * static_cast<double>(kSize) / 1e6;
    emit("libtracer", verb_name(verb), kSize, subs, 1, ops_per_s, deliv_s, mb_s, lat.summarize());

    std::printf("NOTE %s fan=%zu batch=%zu published=%llu delivered=%llu\n", verb_name(verb), subs,
                batch, static_cast<unsigned long long>(published),
                static_cast<unsigned long long>(received.load(std::memory_order_relaxed)));
    if (published != kBulkOps) {
        std::printf("WARN %s fan=%zu published %llu of %zu — the arm measured a DECLINED path\n",
                    verb_name(verb), subs, static_cast<unsigned long long>(published), kBulkOps);
    }
    if (delivers && received.load(std::memory_order_relaxed) == 0) {
        std::printf("WARN %s fan=%zu delivered NOTHING — the subscriber never fired\n",
                    verb_name(verb), subs);
    }
}

}  // namespace

/** @brief Both verbs at both subscriber counts, one process, arms interleaved by verb. */
int main() {
    for (std::size_t subs : {std::size_t{0}, std::size_t{1}}) {
        run(verb_t::ASSIGN, subs);
        run(verb_t::WRITE, subs);
    }
    return 0;
}
