/**
 * @file
 * @brief libtracer side of the libtracer-vs-Zenoh comparison.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Sweeps the matrix:
 *   - fan-out   1/8/128/1024/8192 subscribers on one endpoint (dispatch scaling), plus the
 *               mid arms 16/32/64/256/512 on `inproc` (#844 — `kFanoutsMid`)
 *   - payload   1..8192 bytes (per-byte cost), heap-alloc vs borrowed (no payload copy)
 *   - endpoints 1..8192 distinct topics, write BY PATH (registry/lookup scaling)
 *   - mixed     128 topics, varied fan-out + payloads
 * Module compositions are surfaced as distinct `mode`s (inproc / inproc-borrow /
 * inproc-path / mixed / eptype-* / fold-* / inproc-mt*) — "different approaches to
 * craft libtracer". inproc is the zero-copy graph dispatch. (The `loopback` /
 * `routers-hN` ROUTER-flood modes were retired with bridge_t — ADR-0040; FWD forward
 * cost is measured by bench_forward_heap.) See bench/README.md for the caveats.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "bench_process.hpp"
#include "delivery_count.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tracer.hpp"

using namespace bench;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::view::view_t;

namespace {

// The mid fan-out ladder (#844) is pinned to the engine constant it exists to bracket.
// `kFanoutsMid` lives in bench_common.hpp, which is deliberately dependency-free (the Zenoh
// harness includes it too), so the tie to `vertex_t::kInlineFanout` is asserted HERE, in the
// only translation unit that sees both. The first mid arm must sit strictly past the inline
// capacity and no further than one doubling beyond it: that is the width at which a cost
// paid only on the overflow path has its largest relative signature (it decays as 1/F), and
// it is precisely the width the coarse 1/8/128/1024 ladder skips. If `kInlineFanout` moves,
// this ladder must move with it or the sweep silently stops sampling the transition.
static_assert(kFanoutsMid[0] > tr::graph::vertex_t::kInlineFanout,
              "the first mid fan-out arm must spill past kInlineFanout — below it the "
              "snapshot never leaves the stack buffer and the overflow path is unsampled");
static_assert(kFanoutsMid[0] <= 2 * tr::graph::vertex_t::kInlineFanout,
              "the first mid fan-out arm must be within one doubling of kInlineFanout — "
              "further out, an overflow-path-only cost is already amortized toward noise");

/** @brief A VALUE TLV carrying `payload` bytes (so loopback exercises real encode/decode). */
std::vector<std::byte> value_tlv(std::size_t payload) {
    std::vector<std::byte> p(payload, std::byte{0xAB});
    tr::wire::tlv_t t{};
    t.type = tr::wire::type_t::VALUE;
    t.payload = p;
    std::vector<std::byte> out = tr::wire::encode(t);
    // The Zenoh harness sizes its payload from bench::value_wire_bytes (#1809); a header that
    // drifts from it would compare unequal byte counts under one row name.
    if (out.size() != bench::value_wire_bytes(payload)) {
        std::fprintf(stderr, "value_tlv(%zu): encoded %zu bytes, value_wire_bytes says %zu\n",
                     payload, out.size(), bench::value_wire_bytes(payload));
        std::abort();
    }
    return out;
}

/** @brief Per-message owned heap view (alloc + copy each publish) — the allocating path. */
view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/**
 * @brief Borrowed view over a stable buffer: no payload copy, but not allocation-free.
 *
 * `borrow_const` allocates the segment header that points at the buffer, and the write then
 * stores a block for the value, so a borrowed write still makes two allocations (#1805).
 */
view_t borrowed_view(std::span<const std::byte> bytes) {
    return view_t::over(tr::view::borrow_const(bytes));
}

enum class alloc_t { HEAP, BORROW };

/** @brief Which rows one @ref run_inproc call publishes for its point. */
enum class rows_t {
    BOTH,      /**< The clock-quantized row and its `<mode>-batch` twin (the default). */
    QUANTIZED, /**< The quantized row alone: rows whose twin would have no reader. */
    BATCH,     /**< The `<mode>-batch` twin alone (#1808): a point whose quantized row is
                    already published by another family. */
};

/**
 * @brief Publish the batch-amortized twin of a quantized latency row (#553).
 *
 * Times @p op in window-calibrated batches (@ref bench::time_batches, #1804) and reports the
 * per-op time to the picosecond, so the two `clock_gettime` reads and the clock's granularity
 * are amortized across a window of at least 20 µs instead of dominating one operation.
 * Emitted as a SEPARATE row under `<mode>-batch` rather than replacing the quantized one:
 * the quantized series are the primary key of long-running `gh-pages` history, and giving
 * an existing name a new meaning would silently make every point before the change
 * incomparable to every point after it.
 *
 * **This row is a LATENCY instrument only.** Every other column is left at 0, which the
 * history emitter reads as "this row does not produce that metric" and skips rather than
 * charting. Each is zero for its own reason:
 *
 * - **p99** — each sample here is a MEAN over `batch` operations, so its 99th percentile
 *   is the tail of the batch means: it measures scheduling interference BETWEEN batches,
 *   not the tail of one operation. Read the quantized twin's p99 for tail shape and this
 *   row's p50/mean for magnitude.
 * - **throughput** — the bulk phase of the quantized row times an order of magnitude more
 *   work over a longer window and is already the authoritative figure for this exact
 *   point. Publishing a second, weaker estimate of one quantity is how a reader ends up
 *   with two numbers for one thing and no rule for which to trust.
 * - **`mb_s`** — bandwidth belongs to that same bulk phase, for the same reason.
 *
 * @param mode    The quantized row's mode; this row publishes as `<mode>-batch`.
 * @param op      The operation to time, indexed like the quantized loop's.
 * @param lat_n   Operation budget, matched to the quantized arm so both cost the same work.
 */
template <typename Op>
void emit_batch_row(const char* mode, std::size_t S, std::size_t F, std::size_t E, Op&& op,
                    std::size_t lat_n) {
    constexpr std::uint64_t kBudgetNs = 10'000'000'000ULL;  // a backstop; lat_n ends the loop
    std::size_t i = 0;
    const bench::batch_timing_t t = bench::time_batches([&] { op(i++); }, kBudgetNs, lat_n);
    const std::string batch_mode = std::string(mode) + "-batch";
    bench::emit_batch("libtracer", batch_mode.c_str(), S, F, E, 0.0, 0.0, 0.0, t);
}

/**
 * @brief One inproc run: E endpoints, F subscribers each, S-byte payload.
 *
 * `by_path`
 * writes through the path registry (lookup each publish) instead of the resolved
 * vertex_handle_t hot path — the honest "many topics" measurement.
 *
 * **What a HEAP row times includes the producer.** Each timed op builds the value it writes
 * (`owned_view`: one segment allocation and an S-byte copy) and then writes it, so above
 * 1 KiB part of the size slope is the producer's copy, not the library's. It stays inside the
 * timed region on purpose (#1805): the Zenoh rows these are drawn against copy the payload on
 * `put` too, and the history keyed `inproc` was recorded with it. The labels on the page and
 * in `docs/methodology.md` say so; `inproc-borrow` is the row without the copy.
 */
void run_inproc(std::size_t S, std::size_t F, std::size_t E, alloc_t alloc, bool by_path,
                const char* mode, std::uint64_t budget = kDeliveryBudget,
                std::uint64_t latbudget = kLatencyDeliveryBudget,
                tr::mem::block_source_t* src = nullptr, rows_t rows = rows_t::BOTH) {
    // src==nullptr keeps the process-default source, which folds back onto the global-heap
    // LKV (make_shared) exactly as before #873 phase 1; an injected source routes the
    // per-write LKV allocate_shared through the graph's internal pmr adapter over it — the
    // only difference between `inproc` and `inproc-pool`.
    graph_t g{src != nullptr ? *src : tr::mem::heap_source()};
    std::vector<vertex_handle_t> verts;
    std::vector<path_t> paths;
    verts.reserve(E);
    paths.reserve(E);
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const tr::graph::value_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    for (std::size_t e = 0; e < E; ++e) {
        path_t path = *path_t::parse("/bench/v" + std::to_string(e));
        auto v = g.register_vertex(path, role_t::STORED_VALUE);
        for (std::size_t f = 0; f < F; ++f) (void)g.subscribe(path, cb);
        verts.push_back(v);
        paths.push_back(std::move(path));
    }
    const std::vector<std::byte> tlv = value_tlv(S);
    const auto mk = [&]() { return alloc == alloc_t::HEAP ? owned_view(tlv) : borrowed_view(tlv); };
    const auto put = [&](std::size_t i) {
        if (by_path)
            (void)g.write(paths[i % E], mk());
        else
            (void)g.write(verts[i % E], mk());
    };

    const std::size_t MSGS = publishes_for(F, budget);
    const std::size_t LATN = publishes_for(F, latbudget);
    for (std::size_t i = 0; i < 1000; ++i) put(i);  // warmup

    if (rows == rows_t::BATCH) {
        emit_batch_row(mode, S, F, E, put, LATN);
        return;
    }
    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put(i);
    const double secs = (now_ns() - t0) / 1e9;

    // COUNT the deliveries; do not ASSERT them. This row is charted against a Zenoh row
    // whose delivery figure is `received / elapsed`, with a stderr warning when the count
    // comes up short. Publishing `pub_s * F` here would put a measured number on one side
    // of that chart and an arithmetic one on the other — and the arithmetic side is ours.
    //
    // The two agree EXACTLY whenever the graph delivers in full, so no charted number
    // moves: dispatch is inline (graph.cpp `fan_out` -> `dispatch_edge` runs every
    // callback on this thread before `write()` returns), which is also why no wait is
    // needed here the way the Zenoh side needs one. They diverge only where the
    // arithmetic would be a false claim — `vertex.hpp`'s wide-fan-out snapshot truncates
    // to its inline prefix when the overflow `try_reserve` fails, and the HANDLER and
    // STREAM legs shed an entire fan-out on a clone or ring-append failure. In every one
    // of those cases `write()` still returns success, so the publish loop finishes at full
    // speed and `pub_s * F` would report deliveries that never happened. The count-and-warn
    // itself now lives in `delivery_count.hpp`, shared with the `inproc-target-*` and
    // `inproc-remote` rows #1480 left on the arithmetic and #1481 finished.
    const std::uint64_t want = static_cast<std::uint64_t>(MSGS) * F;
    const double pub_s = MSGS / secs;
    const double deliv_s =
        delivered_rate(mode, S, F, E, want, recv.load(std::memory_order_relaxed), secs);
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    // THE p50/p99 COLUMNS OF THIS ROW ARE CLOCK-QUANTIZED. Read `<mode>-batch` for small deltas.
    //
    // This times ONE operation between two `steady_clock` reads. An in-process write costs
    // ~70-85 ns and the clock's own granularity plus the two reads is a large fraction of
    // that, so the reported percentiles snap to coarse steps: run the binary and the p50s for
    // these rows cluster on 30 / 70 / 80 / 90 / 100 / 120 rather than spreading. Anything
    // under roughly 10 ns is INVISIBLE here — a real 6% improvement to the write path measured
    // 100 ns before and 100 ns after, while the throughput column moved 87 -> 80 ns/op.
    //
    // The row is KEPT AS IS ON PURPOSE (#553). These rows feed long-running gh-pages series
    // keyed by their name, and changing what one measures while keeping its name would make
    // every historical point incomparable to every later one. So the quantized series
    // continues unbroken and the honest measurement is published ALONGSIDE it, below.
    Latency lat;
    lat.reserve(LATN);
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        put(i);
        lat.add(now_ns() - a);
    }
    emit("libtracer", mode, S, F, E, pub_s, deliv_s, mb_s, lat.summarize());
    if (rows == rows_t::BOTH) emit_batch_row(mode, S, F, E, put, LATN);
}

/**
 * @brief `inproc` write path through an INJECTED pool `mr_` (modes `inproc-pool` /
 *        `inproc-pool-borrow`), vs the default global-heap `inproc` / `inproc-borrow`.
 *
 * The ONLY difference from `run_inproc` is the graph's LKV allocator: a
 * `std::pmr::unsynchronized_pool_resource` (frees + reuses fixed-size blocks — a real
 * deployment, not a monotonic best-case) instead of the process heap. The pool outlives the
 * graph: `run_inproc` completes synchronously before `pool` destructs.
 *
 * **The pool is a DETERMINISM lever, not a latency one — and on a host it is SLOWER.**
 * Measured here: ~104 ns/op pooled against ~85 ns/op on the default heap. glibc's tcache
 * serves a hot same-size malloc/free in tens of nanoseconds and a general-purpose pool cannot
 * beat that; the same inversion measures on the terminus path (295 ns heap vs 309 ns pooled).
 * The reason to inject one is a bounded, deterministic ceiling — which is what the 16KB target
 * needs — and on an MCU allocator, where a round-trip costs hundreds of nanoseconds, the
 * comparison flips. That is a property of the HOST allocator, not of the seam.
 *
 * This comment used to claim the opposite: that the fan-1-vs-Zenoh gap is "malloc-dominated"
 * and that "the ~180 ns delta is the LKV persist". Both are refuted by measurement. A leaf
 * write makes exactly ONE allocation (the 104-byte `allocate_shared` of the LKV rope), and
 * removing it outright by injecting the pool buys under a nanosecond — the write path is not
 * allocation-bound at all. It is bound by a short chain of serializing atomics: removing ~164
 * instructions per op from it changed the cycle count by zero, because the out-of-order machine
 * was already hiding that work at IPC ~5.
 */
void run_inproc_pool(std::size_t S, std::size_t F, std::size_t E, alloc_t alloc, bool by_path,
                     const char* mode, std::uint64_t budget = kDeliveryBudget,
                     std::uint64_t latbudget = kLatencyDeliveryBudget,
                     rows_t rows = rows_t::QUANTIZED) {
    // Since #873 phase 1 the graph takes ONE `block_source_t`, so the pooled arm is a
    // `tr::mem::pool_source_t` over a caller-owned slab rather than a
    // `std::pmr::unsynchronized_pool_resource`. Same shape of instrument — a real recycling
    // free-list pool, not a monotonic best-case — and it is now the shipping type rather
    // than a std stand-in. Sized far above the steady state (the loop keeps a bounded number
    // of LKV blocks live and every replacement is recycled by exact size class); a refusal
    // would reach the graph's pmr adapter as a `bad_alloc`, so the arm asserts there was
    // none rather than leaving that to chance.
    std::vector<std::byte> slab(4u * 1024u * 1024u);
    std::vector<tr::mem::size_class_t> classes(64);
    tr::mem::pool_source_t<> pool{slab, classes};
    // The `inproc-pool` family publishes the quantized rows alone. Their window-calibrated
    // twin, `inproc-pool-batch` (#1808), is a family of its own, appended last, so adding it
    // moved no existing row: the quantized pool rows still run in the process they always did.
    run_inproc(S, F, E, alloc, by_path, mode, budget, latbudget, &pool, rows);
    if (pool.stats().refused != 0) {
        std::fprintf(stderr, "FAIL: %s pooled arm exhausted its slab (%zu refusals)\n", mode,
                     pool.stats().refused);
    }
}

/** @brief Which local vertex kind a path-target edge re-dispatches INTO. */
enum class target_kind_t { STORED, HANDLER };

/**
 * @brief A SUBSCRIBER TLV naming a single-segment target path (the wire subscribe form).
 *
 * The graph-test idiom: a `SUBSCRIBER` whose `PATH` child is the target key. This is what
 * gives the edge a non-null `target_key` — the thing `g.subscribe(path, callback)` cannot
 * produce, and therefore the thing the rest of this file never measures.
 */
view_t subscriber_tlv(std::string_view target_segment) {
    std::vector<std::byte> body;
    (void)tr::wire::emit_path_segment(body, target_segment);
    tr::wire::tlv_t path{.type = tr::wire::type_t::PATH, .payload = body};  // packed, PL = 0
    tr::wire::tlv_t sub{.type = tr::wire::type_t::SUBSCRIBER};
    sub.opt.pl = true;
    sub.children.push_back(path);
    return owned_view(tr::wire::encode(sub));
}

/**
 * @brief The PATH-TARGET fan-out sweep (#619) — modes `inproc-target-stored` /
 *        `inproc-target-handler`.
 *
 * Every other fan-out row in this file subscribes with `g.subscribe(path, callback)`, whose
 * edges carry a NULL `target_key`. Those exercise exactly one of `dispatch_edge`'s three
 * legs: `e.callback(...)`. The leg that carries the WIRE semantics — a remote `SUBSCRIBER`
 * written into `:subscribers[]` names a target PATH, not a callback — is
 * `dispatch_edge_target`: a registry `find_ptr(target_key)`, the fan-in ACL gate, a nothrow
 * rope clone, and the target's own `store_value`. Until this row, no bench in the suite
 * touched it at any width, so the published fan-out curves described the convenience API
 * (ADR-0049's own word for it: sugar) and not the specified one.
 *
 * That blind spot had already cost something concrete: #618 deleted TWO heap allocations
 * per delivery from this leg, and the suite could not see the change, because its edges
 * never had a target key to copy.
 *
 * F edges each go to their OWN target vertex, mirroring the callback rows' F independent
 * consumers — F edges into ONE target would measure a different topology (and a single hot
 * LKV). @p kind picks what the delivery lands in, because the two costs are different and
 * neither is "the" answer: `STORED` measures dispatch + the target's store (LKV publish,
 * history, await wake), `HANDLER` measures dispatch + a user handler that does nothing.
 * Read against the `inproc` row at the same fan-out for the callback leg's cost.
 */
void run_inproc_target(std::size_t S, std::size_t F, target_kind_t kind, const char* mode,
                       std::uint64_t budget = kDeliveryBudget,
                       std::uint64_t latbudget = kLatencyDeliveryBudget) {
    graph_t g;
    std::atomic<std::uint64_t> recv{0};
    for (std::size_t f = 0; f < F; ++f) {
        const std::string seg = "t" + std::to_string(f);
        if (kind == target_kind_t::HANDLER) {
            tr::graph::handlers_t h;
            h.on_write = {[](void* c, const tr::graph::value_t&,
                             const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
                              static_cast<std::atomic<std::uint64_t>*>(c)->fetch_add(
                                  1, std::memory_order_relaxed);
                              return {};
                          },
                          &recv};
            (void)g.register_vertex(*path_t::parse("/" + seg), role_t::HANDLER, h);
        } else {
            (void)g.register_vertex(*path_t::parse("/" + seg), role_t::STORED_VALUE);
        }
    }
    const path_t src_path = *path_t::parse("/bench/target-src");
    vertex_handle_t src = g.register_vertex(src_path, role_t::STORED_VALUE);

    // Subscribe the wire way, once per target. If any edge fails to admit, every number
    // below would describe a smaller fan-out than its own label claims, so refuse to emit.
    const path_t sub_path = *path_t::parse("/bench/target-src:subscribers[]");
    std::size_t admitted = 0;
    for (std::size_t f = 0; f < F; ++f)
        if (g.write(sub_path, subscriber_tlv("t" + std::to_string(f))).has_value()) ++admitted;
    if (admitted != F) {
        std::fprintf(stderr, "SKIP mode=%s fanout=%zu: admitted %zu of %zu path-target edges\n",
                     mode, F, admitted, F);
        return;
    }

    const std::vector<std::byte> tlv = value_tlv(S);
    const auto put = [&](std::size_t) { (void)g.write(src, owned_view(tlv)); };

    // Prove the leg under test is actually taken before timing it. A path-target edge that
    // resolved to nothing — a mis-built key, a fan-in ACL denial — drops its delivery
    // SILENTLY (`dispatch_edge_target` returns on a null `find_ptr` and on a denied gate),
    // so the loop below would happily report the cost of not delivering. The handler rows
    // check their counter after the bulk phase; a STORED target has no counter, so read one
    // back instead.
    put(0);
    if (kind == target_kind_t::STORED && !g.read(path_t("/t0")).has_value()) {
        std::fprintf(stderr, "SKIP mode=%s fanout=%zu: target /t0 holds no value after a write\n",
                     mode, F);
        return;
    }

    const std::size_t MSGS = publishes_for(F, budget);
    const std::size_t LATN = publishes_for(F, latbudget);
    for (std::size_t i = 0; i < 1000; ++i) put(i);  // warmup

    // The delivery counter only moves for HANDLER targets (a STORED target's delivery
    // terminates in its LKV), so this is a wiring check for the handler row alone.
    recv.store(0);
    const tr::graph::graph_t::delivery_drops_t drops0 = g.delivery_drops();
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put(i);
    const double secs = (now_ns() - t0) / 1e9;
    if (kind == target_kind_t::HANDLER && recv.load() == 0) {
        std::fprintf(stderr, "SKIP mode=%s fanout=%zu: no delivery reached a handler target\n",
                     mode, F);
        return;
    }

    // COUNT the deliveries, by whichever route this KIND has (delivery_count.hpp). #1480 took
    // `pub_s * F` off the charted `inproc` rows and deliberately left this pair on it; the
    // reason it had to go is the same here and worse. `dispatch_edge_target` drops a delivery
    // on an unresolvable target, a denied fan-in gate and a failed nothrow clone — three legs
    // the callback rows do not even have — and `write()` returns SUCCESS through all three, so
    // the arithmetic would keep reporting a full fan-out while the graph delivered less.
    //
    // HANDLER reads its own counter: direct observation, the same evidence `inproc` uses.
    // STORED has none to read — its delivery terminates in the target's LKV, and hanging a
    // counting subscriber off each target to see it would add an edge per target to the very
    // topology being timed, which is to say it would answer a different question. So that arm
    // subtracts the graph's own drop accounting from the ceiling instead. That is sound rather
    // than circular precisely because RFC-0025 §4.4 forbids a silent shed: every leg above
    // counts what it dropped, so `want - dropped` is observed, not assumed.
    const std::uint64_t want = static_cast<std::uint64_t>(MSGS) * F;
    const std::uint64_t got = kind == target_kind_t::HANDLER
                                  ? recv.load(std::memory_order_relaxed)
                                  : deliveries_from_drops(want, drops0, g.delivery_drops());
    const double pub_s = MSGS / secs;
    const double deliv_s = delivered_rate(mode, S, F, 1, want, got, secs);
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    Latency lat;
    lat.reserve(LATN);
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        put(i);
        lat.add(now_ns() - a);
    }
    emit("libtracer", mode, S, F, 1, pub_s, deliv_s, mb_s, lat.summarize());
}

/**
 * @brief The REMOTE fan-out sweep (#1448) — mode `inproc-remote`, reachable only as the
 *        isolated `fan-remote` sweep.
 *
 * The third of `dispatch_edge`'s legs, and the one no row in this file measured. `inproc`
 * subscribes with `g.subscribe(path, callback)`, whose edge carries NO cold half at all
 * (`subscriber_t::remote` stays null); `inproc-target-*` writes a `SUBSCRIBER` naming a local
 * PATH, whose cold half is null too unless a caller context is supplied. So the whole
 * remote-subscriber machinery — the link NAME, the stored return route, the fan-in caller —
 * was invisible to the fan-out curves, in exactly the way #619 found `dispatch_edge_target`
 * invisible before it.
 *
 * That blind spot has cost something concrete twice now. #1442 took a deep copy of the cold
 * half off the SUBSCRIBE path and had to be priced on `bench_subscribe_index` because no
 * fan-out row could see it; #1448 took the same copy off the DELIVERY path — two `std::string`
 * copies per remote edge per write — and this row is what prices it.
 *
 * **NOT part of the default sweep, deliberately.** `perf_gate.py`, `render_history.py` and
 * `collate.py` join on the default run's `(mode, size, fan, ep)` keys, and a new row there is
 * a new series with no history. This one is reached only through `argv[1] == "fan-remote"`,
 * for A/B runs, exactly as `fan` and `target` are.
 *
 * The sink is a `{fn, ctx}` counter and nothing else. That is the point rather than a
 * shortcut: what is under test is the graph's per-edge snapshot and dispatch, not a
 * transport, and a real egress would bury the term in `sendmsg`. The counter is checked
 * before the timed phase — a remote edge whose link never populated delivers nothing and this
 * row would happily report the cost of not delivering.
 *
 * Links are named `192.168.4.N:9000`-style and therefore past the small-string buffer, which
 * is #1441's finding restated: a deployment naming links `host:port` sits on the expensive
 * side of the SSO boundary by default, and an SSO-sized link would hide most of what the
 * removed copy cost.
 */
void run_inproc_remote(std::size_t S, std::size_t F, const char* mode,
                       std::uint64_t budget = kDeliveryBudget,
                       std::uint64_t latbudget = kLatencyDeliveryBudget) {
    graph_t g;
    std::atomic<std::uint64_t> recv{0};
    struct sink_ctx_t {
        std::atomic<std::uint64_t>* n;
        std::size_t link_bytes;
    };
    sink_ctx_t sink_ctx{&recv, 0};
    {
        auto hooks = g.hooks();
        hooks.remote_delivery = {
            [](void* ctx, const tr::graph::remote_delivery_t& d, const tr::graph::value_t&) {
                auto* s = static_cast<sink_ctx_t*>(ctx);
                s->n->fetch_add(1, std::memory_order_relaxed);
                s->link_bytes += d.link.size();  // READ the borrowed spelling, don't just count
            },
            &sink_ctx};
        g.set_hooks(hooks);
    }

    const path_t src_path = *path_t::parse("/bench/remote-src");
    const vertex_handle_t src = g.register_vertex(src_path, role_t::STORED_VALUE);
    // A bare SUBSCRIBER (no PATH child) is the remote form: no local target, delivery rides
    // the return route over the link. The route is the smallest well-formed PATH TLV.
    const view_t sub_tlv = owned_view(
        std::vector<std::byte>{std::byte{0x04}, std::byte{0x40}, std::byte{0x00}, std::byte{0x00}});
    const view_t route_tlv = owned_view(
        std::vector<std::byte>{std::byte{0x06}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}});
    std::size_t admitted = 0;
    for (std::size_t f = 0; f < F; ++f) {
        std::string link =
            "192.168." + std::to_string(f / 250) + "." + std::to_string(f % 250) + ":9000";
        if (g.subscribe_wire(src, sub_tlv, route_tlv, std::move(link)).has_value()) ++admitted;
    }
    if (admitted != F) {
        std::fprintf(stderr, "SKIP mode=%s fanout=%zu: admitted %zu of %zu remote edges\n", mode, F,
                     admitted, F);
        return;
    }

    const std::vector<std::byte> tlv = value_tlv(S);
    const auto put = [&](std::size_t) { (void)g.write(src, owned_view(tlv)); };

    recv.store(0);
    put(0);
    if (recv.load() == 0) {
        std::fprintf(stderr, "SKIP mode=%s fanout=%zu: no remote delivery reached the sink\n", mode,
                     F);
        return;
    }

    const std::size_t MSGS = publishes_for(F, budget);
    const std::size_t LATN = publishes_for(F, latbudget);
    for (std::size_t i = 0; i < 1000; ++i) put(i);  // warmup

    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put(i);
    const double secs = (now_ns() - t0) / 1e9;

    // COUNT the deliveries the sink saw (delivery_count.hpp), where this row used to publish
    // `pub_s * F` and then REFUSE to emit unless the sink's tally matched it exactly. Both
    // halves of that were wrong in the same way. The number was arithmetic, so the tally was
    // the only thing standing between a shed and a false figure; and dropping the whole row on
    // a short count deletes the point from the series over exactly the event a reader wants to
    // find in it. Publishing what the sink counted needs no such backstop: a shed lowers the
    // figure it should lower, and says so on stderr. The pre-timing zero check above stays —
    // that one is a WIRING check ("did this leg run at all"), not a delivery figure.
    const std::uint64_t want = static_cast<std::uint64_t>(MSGS) * F;
    const double pub_s = MSGS / secs;
    const double deliv_s =
        delivered_rate(mode, S, F, 1, want, recv.load(std::memory_order_relaxed), secs);
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    Latency lat;
    lat.reserve(LATN);
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        put(i);
        lat.add(now_ns() - a);
    }
    emit("libtracer", mode, S, F, 1, pub_s, deliv_s, mb_s, lat.summarize());
    // The sink's borrowed-link read must not be optimized away — it is the only thing that
    // makes this row exercise the shared record rather than a pointer nobody dereferences.
    if (sink_ctx.link_bytes == 0)
        std::fprintf(stderr, "NOTE mode=%s fanout=%zu: sink read no link bytes\n", mode, F);
}

/**
 * @brief Deliver-only row (mode `inproc-deliver`): RFC-0008's edge-transition
 *        primitive, timed alone.
 *
 * The value is stored ONCE (a single `write` before the timed loops); each measured
 * op is `graph_t::propagate(v)` — deliver the current last-known-value to the F
 * subscribers. No per-op store, no segment allocation, no memcpy, no await/readiness
 * sequence bump. This is the semantic analogue of Zenoh's transient put (delivery
 * only) and the true apples-to-apples row; the `inproc` row's `write` does strictly
 * more work per op (assign/store + readiness bump + deliver).
 */
void run_inproc_deliver(std::size_t S, std::size_t F, std::uint64_t budget = kDeliveryBudget,
                        std::uint64_t latbudget = kLatencyDeliveryBudget) {
    graph_t g;
    const path_t path = *path_t::parse("/bench/deliver");
    auto v = g.register_vertex(path, role_t::STORED_VALUE);
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const tr::graph::value_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    for (std::size_t f = 0; f < F; ++f) (void)g.subscribe(path, cb);
    const std::vector<std::byte> tlv = value_tlv(S);
    (void)g.write(v, owned_view(tlv));  // store ONCE — the timed ops below move no bytes
    const auto put = [&]() { (void)g.propagate(v); };

    const std::size_t MSGS = publishes_for(F, budget);
    const std::size_t LATN = publishes_for(F, latbudget);
    for (std::size_t i = 0; i < 1000; ++i) put();  // warmup

    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put();
    const double secs = (now_ns() - t0) / 1e9;

    // Counted, not asserted — see the same block in `run_inproc`. `propagate` fans out
    // through the identical inline `dispatch_edge` path, so the count and `pub_s * F`
    // agree whenever nothing is shed, and this row is charted against Zenoh's counted one.
    const std::uint64_t want = static_cast<std::uint64_t>(MSGS) * F;
    const double pub_s = MSGS / secs;
    const double deliv_s =
        delivered_rate("inproc-deliver", S, F, 1, want, recv.load(std::memory_order_relaxed), secs);
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    Latency lat;
    lat.reserve(LATN);
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        put();
        lat.add(now_ns() - a);
    }
    emit("libtracer", "inproc-deliver", S, F, 1, pub_s, deliv_s, mb_s, lat.summarize());
}

/**
 * @brief Response-surface grid (system dynamics): size x fanout (endpoints=1, mode `inproc`) and
 *        size x endpoints (fanout=1, write-by-path, mode `inproc-path`).
 *
 * Emits the standard mode-tagged RESULT line (same 12-field shape as the default
 * run) so one parser feeds both the terminal table and the docs comparison charts.
 */
void run_grid() {
    // No `-batch` twins here (#553). The grid is the ENGINE-COMPARISON surface: every row
    // is drawn against a Zenoh row measured the same way, and there is no batch-amortized
    // Zenoh arm to compare one against. A batch twin would also double a 7x7 grid on both
    // sweeps — 98 extra rows whose only reader would be a chart with nothing beside it.
    for (std::size_t S : kGridSizes)
        for (std::size_t F : kGridFanouts)
            run_inproc(S, F, 1, alloc_t::HEAP, false, "inproc", kGridBudget, kGridLatBudget,
                       nullptr, rows_t::QUANTIZED);
    // Deliver-only fan sweep at the reference payload (the comparison charts' fixed
    // size): propagate touches no payload bytes, so a full size sweep would be flat.
    for (std::size_t F : kGridFanouts) run_inproc_deliver(kRefSize, F, kGridBudget, kGridLatBudget);
    for (std::size_t S : kGridSizes)
        for (std::size_t E : kGridEndpoints)
            run_inproc(S, 1, E, alloc_t::HEAP, true, "inproc-path", kGridBudget, kGridLatBudget,
                       nullptr, rows_t::QUANTIZED);
    // The dense payload sweep (#1890), at fan-out 1 only: the payload charts' slice.
    for (std::size_t S : bench::sweep_extra(kGridSizes))
        run_inproc(S, 1, 1, alloc_t::HEAP, false, "inproc", bench::ladder_budget(S, kGridBudget),
                   bench::ladder_budget(S, kGridLatBudget), nullptr, rows_t::QUANTIZED);
}

/** @brief Mixed workload: 128 topics with varied fan-out (1..16) and payloads (1..8192). */
void run_mixed() {
    graph_t g;
    constexpr std::size_t E = 128;
    std::vector<vertex_handle_t> verts;
    std::vector<std::size_t> fan;
    std::vector<std::vector<std::byte>> tlvs;
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const tr::graph::value_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    std::size_t total_fan = 0;
    for (std::size_t e = 0; e < E; ++e) {
        path_t path = *path_t::parse("/bench/m" + std::to_string(e));
        auto v = g.register_vertex(path, role_t::STORED_VALUE);
        const std::size_t F = std::size_t{1} << (e % 5);  // 1,2,4,8,16
        for (std::size_t f = 0; f < F; ++f) (void)g.subscribe(path, cb);
        verts.push_back(v);
        fan.push_back(F);
        total_fan += F;
        tlvs.push_back(value_tlv(kSizes[e % 5]));
    }
    constexpr std::size_t MSGS = 100000;
    for (std::size_t i = 0; i < 1000; ++i) (void)g.write(verts[i % E], owned_view(tlvs[i % E]));

    // `want` is the arithmetic ceiling, summed outside the timed loop; the published figure is
    // what the subscribers COUNTED (#1805).
    std::uint64_t want = 0;
    for (std::size_t i = 0; i < MSGS; ++i) want += fan[i % E];
    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) {
        const std::size_t e = i % E;
        (void)g.write(verts[e], owned_view(tlvs[e]));
    }
    const double secs = (now_ns() - t0) / 1e9;
    const double deliv_s = delivered_rate("mixed", 0, total_fan / E, E, want,
                                          recv.load(std::memory_order_relaxed), secs);

    constexpr std::size_t kMixedLatN = 20000;
    Latency lat;
    lat.reserve(kMixedLatN);
    for (std::size_t i = 0; i < kMixedLatN; ++i) {
        const std::size_t e = i % E;
        const auto a = now_ns();
        (void)g.write(verts[e], owned_view(tlvs[e]));
        lat.add(now_ns() - a);
    }
    emit("libtracer", "mixed", 0, total_fan / E, E, MSGS / secs, deliv_s, 0.0, lat.summarize());
}

/**
 * @brief n-cores (parallel-dispatch) axis (#96 / ADR-0032).
 *
 * T independent publisher
 * threads, each driving its OWN graph + endpoint with the zero-copy in-process
 * path, measured for AGGREGATE throughput + per-op latency under load. Each
 * thread reuses one borrowed view over a stable per-thread buffer, so the timed
 * loop allocates nothing (no cross-thread allocator contention) — what scales is
 * dispatch itself. Fixed per-thread work, so more cores => more aggregate work.
 */
void run_inproc_mt(std::size_t T) {
    constexpr std::size_t S = 64;
    constexpr std::size_t MSGS = 2'000'000;  // per-thread fixed work (throughput phase)
    constexpr std::size_t LATN = 200'000;    // per-thread samples (latency phase)

    // Each thread owns everything it touches: its own graph, vertex, subscriber
    // counter, payload buffer, and the single reused borrowed view.
    struct worker_t {
        graph_t g;
        std::optional<vertex_handle_t> v;
        std::vector<std::byte> buf;
        view_t view;
        std::atomic<std::uint64_t> recv{0};
        std::vector<std::uint64_t> lat;
    };
    std::vector<std::unique_ptr<worker_t>> ws;
    ws.reserve(T);
    const std::vector<std::byte> tlv = value_tlv(S);
    for (std::size_t t = 0; t < T; ++t) {
        auto w = std::make_unique<worker_t>();
        w->buf = tlv;  // per-thread copy => per-thread segment, no shared refcount
        w->v = w->g.register_vertex(*path_t::parse("/bench/mt"), role_t::STORED_VALUE);
        (void)w->g.subscribe(
            *path_t::parse("/bench/mt"),
            [](void* ctx, const tr::graph::value_t&) {
                static_cast<worker_t*>(ctx)->recv.fetch_add(1, std::memory_order_relaxed);
            },
            w.get());
        w->view = borrowed_view(w->buf);
        ws.push_back(std::move(w));
    }

    // --- Throughput phase: all threads start together, run fixed work, join. ---
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(T);
    for (std::size_t t = 0; t < T; ++t) {
        worker_t* w = ws[t].get();
        threads.emplace_back([w, &ready, &go]() {
            for (std::size_t i = 0; i < 1000; ++i) (void)w->g.write(*w->v, w->view);  // warmup
            w->recv.store(0, std::memory_order_relaxed);
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) { /* spin until released */
            }
            for (std::size_t i = 0; i < MSGS; ++i) (void)w->g.write(*w->v, w->view);
        });
    }
    while (ready.load(std::memory_order_acquire) < T) { /* wait for all warmed up */
    }
    const auto t0 = now_ns();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    const double secs = (now_ns() - t0) / 1e9;

    const double pub_s = static_cast<double>(T) * MSGS / secs;
    // COUNTED (#1805): each worker's subscriber counted its own deliveries, reset before the
    // release, so the sum is what the throughput phase delivered across every graph.
    std::uint64_t got = 0;
    for (const auto& w : ws) got += w->recv.load(std::memory_order_relaxed);
    const std::string mode = "inproc-mt" + std::to_string(T);
    const double deliv_s =
        delivered_rate(mode.c_str(), S, 1, T, static_cast<std::uint64_t>(T) * MSGS, got, secs);
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    // --- Latency phase: per-op timing under the same parallel load. ---
    std::atomic<std::size_t> ready2{0};
    std::atomic<bool> go2{false};
    std::vector<std::thread> lthreads;
    lthreads.reserve(T);
    for (std::size_t t = 0; t < T; ++t) {
        worker_t* w = ws[t].get();
        lthreads.emplace_back([w, &ready2, &go2]() {
            w->lat.resize(LATN);  // reserve AND touch (#1803): no page fault mid-sample
            w->lat.clear();
            ready2.fetch_add(1, std::memory_order_acq_rel);
            while (!go2.load(std::memory_order_acquire)) { /* spin */
            }
            for (std::size_t i = 0; i < LATN; ++i) {
                const auto a = now_ns();
                (void)w->g.write(*w->v, w->view);
                w->lat.push_back(now_ns() - a);
            }
        });
    }
    while (ready2.load(std::memory_order_acquire) < T) { /* wait */
    }
    go2.store(true, std::memory_order_release);
    for (auto& th : lthreads) th.join();

    Latency lat;
    lat.reserve(T * LATN);
    for (auto& w : ws)
        for (std::uint64_t ns : w->lat) lat.add(ns);

    emit("libtracer", mode.c_str(), S, 1, T, pub_s, deliv_s, mb_s, lat.summarize());
}

// ep-type (endpoint-dispatch-class) axis (#96 / ADR-0032). On ONE fixed workload
// (size=64, fan=1, ep=1) we compare the three dispatch CLASSES a write can take to
// an endpoint, emitting one RESULT line per class with `mode` tagging the class:
//
//   eptype-lean        minimal sink: a plain in-process write+deliver to a
//                      STORED_VALUE vertex, heap-allocated view per publish.
//                      Same path as the existing `inproc` mode.
//   eptype-lean-cached the loaned / out_cache path: a borrowed view (no payload copy;
//                      a segment header and a stored-value block per write). Same path as
//                      the existing `inproc-borrow` mode.
//   eptype-stream      a STREAM-role vertex: each write appends to the bounded
//                      history ring (retention work) *then* fans out — strictly more
//                      work than lean. Allocation parity with lean (heap view) so the
//                      delta isolates the history-retention cost.
//
// (Naming is provisional — see bench/README.md "ep-type axis" for the map; the class
// boundaries are what matter, the labels can be refined later.)
//
// lean / lean-cached reuse the existing inproc paths via run_inproc(), re-emitted
// under the eptype-* tag (the original inproc / inproc-borrow lines still print too).

/**
 * @brief The STREAM-role class: time write+deliver where each write retains history.
 * @param S The payload; 64 B is the gated reference row, the rest are the ladder (#1806).
 */
void run_eptype_stream(std::size_t S) {
    // The same fixture as the #1808 STREAM rows: a 16-deep ring, one counting subscriber.
    stream_fixture_t fx;
    const std::vector<std::byte> tlv = value_tlv(S);
    const auto put = [&]() { (void)fx.g.write(fx.v, owned_view(tlv)); };  // heap view: lean parity

    const std::size_t MSGS = publishes_for(1, ladder_budget(S, kDeliveryBudget));
    const std::size_t LATN = publishes_for(1, ladder_budget(S, kLatencyDeliveryBudget));
    for (std::size_t i = 0; i < 1000; ++i) put();  // warmup

    fx.recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put();
    const double secs = (now_ns() - t0) / 1e9;
    const double pub_s = MSGS / secs;
    // COUNTED at the subscriber (#1805). A best-effort ring that cannot fund an entry sheds
    // it and `write()` still succeeds, so `pub_s` was a claim that held only while nothing shed.
    const double deliv_s = delivered_rate("eptype-stream", S, 1, 1, MSGS,
                                          fx.recv.load(std::memory_order_relaxed), secs);
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    Latency lat;
    lat.reserve(LATN);
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        put();
        lat.add(now_ns() - a);
    }
    emit("libtracer", "eptype-stream", S, 1, 1, pub_s, deliv_s, mb_s, lat.summarize());
}

/**
 * @brief The full ep-type sweep: lean, lean-cached, stream — all at size=64 fan=1 ep=1 — then
 *        the STREAM leg over the rest of the payload ladder (#1806).
 */
void run_eptype() {
    // No `-batch` twins (#553): these two re-emit `inproc` / `inproc-borrow` at the
    // reference point under an endpoint-type name, so their batch twins would be a
    // duplicate measurement of `inproc-batch 64B/fan1/1ep` and its borrow counterpart.
    run_inproc(kRefSize, 1, 1, alloc_t::HEAP, false, "eptype-lean", kDeliveryBudget,
               kLatencyDeliveryBudget, nullptr, rows_t::QUANTIZED);
    run_inproc(kRefSize, 1, 1, alloc_t::BORROW, false, "eptype-lean-cached", kDeliveryBudget,
               kLatencyDeliveryBudget, nullptr, rows_t::QUANTIZED);
    for (std::size_t S : kPayloadLadder) run_eptype_stream(S);
}

/**
 * @brief n-layer-folded (fold-depth) axis (#96 / ADR-0032) — the LAST axis.
 *
 * How does the
 * L0/L1 zero-copy COMPOSITION cost scale with how many memory layers a value is
 * FOLDED across? We hold the TOTAL bytes CONSTANT (kFoldTotal) and sweep the fold
 * depth N: the same value is built as a rope of N borrowed views over N segments —
 * N=1 is one flat segment, N=8 is an 8-link rope of identical total bytes. Per op we
 * serialize the folded value for egress the way a transport does: build the
 * scatter-gather descriptor (rope_t::to_iovec — spans into the N segments, no copy)
 * and walk it. Because the bytes are fixed and only the fold depth changes, the delta
 * isolates the view-chain walk / scatter-gather cost: more folds => more links to
 * gather => higher per-op cost (and lower throughput). (The naming "n-layer-folded" /
 * "fold depth" is provisional — see bench/README.md "n-layer-folded axis".)
 */
void run_fold(std::size_t N) {
    constexpr std::size_t kFoldTotal = 512;  // total bytes, CONSTANT across N (isolate fold)
    const std::size_t seg_bytes = kFoldTotal / N;

    // Stable per-segment buffers; the rope BORROWS them, so the timed loop allocates
    // nothing for the value — what it pays is purely the fold-depth walk/gather.
    std::vector<std::vector<std::byte>> bufs(N, std::vector<std::byte>(seg_bytes, std::byte{0xAB}));
    tr::view::rope_t rope;
    for (auto& b : bufs) rope.append(borrowed_view(b));

    // One egress-serialize op: gather the rope into a scatter-gather iovec, then walk the
    // links (the view-chain walk a transport / codec performs to ship the rope).
    //
    // The span table is REUSED, via the nothrow `try_to_iovec`, exactly as the real terminus
    // egress does. The old loop called `to_iovec()`, which does `reserve(link_count())` and
    // therefore one malloc PER OP — measured at 1.00 allocations/op, 47-70% of the timed
    // work. That contradicted this function's own docstring ("the timed loop allocates
    // nothing ... purely the fold-depth walk/gather") and meant the fold-width axis was
    // mostly charting a constant malloc.
    std::vector<std::span<const std::byte>> iov;
    const auto serialize = [&]() -> std::size_t {
        if (!rope.try_to_iovec(iov)) return 0;
        std::size_t acc = 0;
        for (const auto& sp : iov)
            acc += sp.size() + (sp.empty() ? 0u : std::to_integer<std::size_t>(sp[0]));
        return acc;
    };

    volatile std::size_t sink = 0;
    constexpr std::size_t MSGS = 2'000'000;                      // throughput phase
    for (std::size_t i = 0; i < 1000; ++i) sink += serialize();  // warmup

    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) sink += serialize();
    const double secs = (now_ns() - t0) / 1e9;
    const double pub_s = MSGS / secs;
    const double deliv_s = pub_s;  // fan=1 => one egress per publish

    // BATCH-AMORTIZED latency, like `run_path_parse` below and the net-plane benches. A fold
    // op costs ~3-15 ns; timing one between two `steady_clock` reads measured the CLOCK, and
    // published p50=30 / p99=31 for EVERY width (#553). The fixed 256-op batch that replaced
    // it was still a ~1 µs window, 25x under the timing floor, and its integer division
    // stepped a 3 ns row in whole nanoseconds. Now the batch is sized by window (>= 20 µs,
    // asserted) and the result is kept in picoseconds (#1804). No p99: a percentile of batch
    // means is not an operation's tail.
    constexpr std::uint64_t kFoldBudgetNs = 50'000'000;
    const bench::batch_timing_t t =
        bench::time_batches([&] { sink += serialize(); }, kFoldBudgetNs);
    (void)sink;
    // Mode renamed `fold-n*` -> `fold-b*` because the number now means something different
    // (a batch-amortized per-op cost, not a clock-quantized single-shot). Renaming ends the
    // old series and starts a new one, which is the visible outcome a changed meaning should
    // have. Bandwidth is 0: the op reads at most 8 payload bytes per link, so the old
    // `deliv_s * 512` was ~74 GB/s of bytes never touched.
    const std::string mode = "fold-b" + std::to_string(N);
    bench::emit_batch("libtracer", mode.c_str(), kFoldTotal, 1, 1, pub_s, deliv_s, 0.0, t);
}

/**
 * @brief ACL-gated data ops with inheritance (ADR-0050): a depth-4 tree whose root and mid level
 *        carry INHERIT ACEs, a subject resolver installed, and every op arriving under a granted
 *        caller context — so each op pays the full effective-ACL check.
 *
 * The measured op is the GATED READ (the gate plus the lock-free LKV load — the
 * leanest gated data op, so the gate's cost is what the row sees). Two modes:
 *
 *   acl-inherit-d4      one thread, one leaf — the uncontended gate cost.
 *   acl-inherit-d4-mtT  T threads, each gating reads on its OWN leaf under the
 *                       SHARED ancestor chain — what the ADR-0050 cached merge
 *                       buys: pre-cache every op locked each shared ancestor's
 *                       mutex (cross-core cacheline traffic on hot composites);
 *                       post-cache an op touches only its own vertex's lock.
 */
namespace acl_bench {

/** @brief Install a subject resolver mapping a non-empty caller to its own bytes. */
void install_resolver(graph_t& g) {
    {
        auto hooks = g.hooks();
        hooks.subject_resolver = {
            [](void*,
               std::string_view caller) -> std::expected<std::vector<std::byte>, tr::wire::err_t> {
                // The empty (local) context is settled as trusted before the resolver runs (#905),
                // so the setup writes never arrive here.
                std::vector<std::byte> token(caller.size());
                std::memcpy(token.data(), caller.data(), caller.size());
                return token;
            },
            nullptr};
        g.set_hooks(hooks);
    }
}

/** @brief Write a single INHERIT ALLOW ACE for subject "peer" onto `path`:acl. */
void install_acl(graph_t& g, const char* path, std::uint32_t mask) {
    const std::vector<tr::graph::ace_t> aces{
        tr::graph::ace_t{.type = tr::graph::ace_type_t::ALLOW,
                         .flags = tr::graph::kAceInherit,
                         .subject = {reinterpret_cast<const std::byte*>("peer"),
                                     reinterpret_cast<const std::byte*>("peer") + 4},
                         .access_mask = mask,
                         .expires_ns = 0}};
    (void)g.write(*path_t::parse(path), owned_view(tr::graph::encode_acl(aces)));
}

/** @brief Build the depth-4 gated tree: /acl(/hub(/dev(/leaf0..N-1))) + INHERIT ACEs. */
std::vector<vertex_handle_t> build_tree(graph_t& g, std::size_t leaves) {
    using tr::graph::acl_right_t;
    (void)g.register_vertex(*path_t::parse("/acl"), role_t::STORED_VALUE);
    (void)g.register_vertex(*path_t::parse("/acl/hub"), role_t::STORED_VALUE);
    (void)g.register_vertex(*path_t::parse("/acl/hub/dev"), role_t::STORED_VALUE);
    std::vector<vertex_handle_t> out;
    out.reserve(leaves);
    for (std::size_t i = 0; i < leaves; ++i)
        out.push_back(g.register_vertex(*path_t::parse("/acl/hub/dev/leaf" + std::to_string(i)),
                                        role_t::STORED_VALUE));
    // INHERIT grants on the root and the mid level, so the effective merge spans
    // multiple ancestor lists (READ for the measured op, WRITE for the seeds).
    install_acl(g, "/acl:acl",
                static_cast<std::uint32_t>(acl_right_t::READ) |
                    static_cast<std::uint32_t>(acl_right_t::WRITE));
    install_acl(g, "/acl/hub:acl", static_cast<std::uint32_t>(acl_right_t::READ));
    const std::vector<std::byte> tlv = value_tlv(64);
    for (vertex_handle_t v : out) (void)g.write(v, owned_view(tlv), "peer");  // seed via the gate
    return out;
}

}  // namespace acl_bench

/** @brief The single-threaded row: the uncontended per-op gate cost (mode acl-inherit-d4). */
void run_acl_gated() {
    constexpr std::size_t S = 64;
    graph_t g;
    acl_bench::install_resolver(g);
    const vertex_handle_t leaf = acl_bench::build_tree(g, 1)[0];

    volatile std::size_t sink = 0;
    const auto get = [&]() { sink += g.read(leaf, "peer").has_value() ? 1u : 0u; };

    constexpr std::size_t MSGS = 2'000'000;
    constexpr std::size_t LATN = 200'000;
    for (std::size_t i = 0; i < 1000; ++i) get();  // warmup

    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) get();
    const double ops_s = MSGS / ((now_ns() - t0) / 1e9);

    Latency lat;
    lat.reserve(LATN);
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        get();
        lat.add(now_ns() - a);
    }
    (void)sink;
    emit("libtracer", "acl-inherit-d4", S, 1, 1, ops_s, ops_s, ops_s * static_cast<double>(S) / 1e6,
         lat.summarize());
}

/**
 * @brief The contended row (mode acl-inherit-d4-mtT): T reader threads, one leaf each, all gated
 *        through the SAME ancestor chain — the shared-composite hot case.
 */
void run_acl_gated_mt(std::size_t T) {
    constexpr std::size_t S = 64;
    graph_t g;
    acl_bench::install_resolver(g);
    const std::vector<vertex_handle_t> leaves = acl_bench::build_tree(g, T);

    constexpr std::size_t MSGS = 1'000'000;  // per-thread fixed work (throughput phase)
    constexpr std::size_t LATN = 100'000;    // per-thread samples (latency phase)

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::vector<std::uint64_t>> lats(T);
    std::vector<std::thread> threads;
    threads.reserve(T);
    for (std::size_t t = 0; t < T; ++t) {
        threads.emplace_back([&, t]() {
            const vertex_handle_t leaf = leaves[t];
            volatile std::size_t sink = 0;
            const auto get = [&]() { sink += g.read(leaf, "peer").has_value() ? 1u : 0u; };
            for (std::size_t i = 0; i < 1000; ++i) get();  // warmup
            // Reserve AND touch before the release (#1803): this used to reserve after the
            // timed MSGS phase had started, inside the throughput window.
            lats[t].resize(LATN);
            lats[t].clear();
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) { /* spin until released */
            }
            for (std::size_t i = 0; i < MSGS; ++i) get();
            for (std::size_t i = 0; i < LATN; ++i) {
                const auto a = now_ns();
                get();
                lats[t].push_back(now_ns() - a);
            }
            (void)sink;
        });
    }
    while (ready.load(std::memory_order_acquire) < T) { /* wait for warmup */
    }
    const auto t0 = now_ns();
    go.store(true, std::memory_order_release);
    for (std::thread& th : threads) th.join();
    // Throughput counts the fixed MSGS phase plus the latency samples (all gated ops).
    const double secs = (now_ns() - t0) / 1e9;
    const double ops_s = static_cast<double>(T) * (MSGS + LATN) / secs;

    Latency lat;
    lat.reserve(T * LATN);
    for (const std::vector<std::uint64_t>& per : lats)
        for (std::uint64_t ns : per) lat.add(ns);
    const std::string mode = "acl-inherit-d4-mt" + std::to_string(T);
    emit("libtracer", mode.c_str(), S, 1, T, ops_s, ops_s, ops_s * static_cast<double>(S) / 1e6,
         lat.summarize());
}

/** @brief One LKV copy-store measurement's outcome. */
struct lkv_result_t {
    double ops_per_s = 0;      /**< @brief alloc+free pairs per second. */
    std::size_t exhausted = 0; /**< @brief Backpressure hits (must be 0 for the pool). */
};

/**
 * @brief ADR-0060: the write-path copy-store allocation in isolation.
 *
 * **Read the mode names carefully — `lkv-store-*` is NOT the LKV slot.** The "store" is
 * the write path's *copy-store*: the segment the value is copied INTO, i.e. exactly
 * `rope_t::materialize` (backend alloc + payload `memcpy`) and nothing downstream of it.
 * No `graph_t`, no vertex, no last-known-value publish is anywhere in this loop. A
 * regression here therefore reads as "the rope-to-contiguous copy got slower", and #1250
 * is the case that proves the distinction is load-bearing: a 48% step on these rows was
 * triaged as an LKV-store regression when the defect was the shape of `rope_t::flatten`.
 * The names are deliberately NOT being corrected — they are the primary key of the
 * `gh-pages` history series, and renaming them would end those series (see the `fold-n*`
 * -> `fold-b*` note in bench/README.md for what that costs).
 *
 * Two variants of the exact allocation `graph_t` routes through its `value_backend_`
 * at the branch/field write sites (`graph.cpp` 825/1017):
 *   - @p copy `false` — the pure `backend.alloc` + `backend.destroy` op the ADR
 *     gate names ("alloc/free throughput"), isolated from the payload copy. Mode
 *     `lkv-alloc-*`; this is the gated ratio (a pooled O(1) free-list vs the
 *     default heap's malloc/free).
 *   - @p copy `true` — the full `materialize()` flatten (alloc + the payload
 *     `memcpy`) the write path actually performs. Mode `lkv-store-*`; the memcpy is
 *     backend-independent, so this end-to-end ratio is smaller than the alloc-only
 *     one — the honest wall-clock the write path gains on this host.
 * Every iteration allocates then drops one owned segment (one alloc + one free via
 * `segment_ptr_t`), so both variants exercise the reclaim path graph relies on.
 */
lkv_result_t run_lkv_store_alloc(std::size_t S, bool copy, tr::mem::mem_backend_t& backend,
                                 const char* mode) {
    // A 2-link rope of S total bytes forces the flatten — a single-link rope would
    // materialize zero-copy and never touch the backend (the fast path this bench is
    // deliberately NOT measuring). Borrowed links keep the source alloc-free.
    std::vector<std::byte> a(S - S / 2, std::byte{0xAB});
    std::vector<std::byte> b(S / 2, std::byte{0xCD});
    rope_t src{borrowed_view(a)};
    src.append(borrowed_view(b));

    // 200 000 iterations up to 8 KiB, as always; fewer above it (#1806), so a 64 KiB copy
    // costs about what an 8 KiB one does.
    const std::size_t iters = ladder_budget(S, 200000);
    std::size_t exhausted = 0;
    {
        const view_t warm = src.materialize(backend);
        (void)warm;
    }  // fault-in / warm caches
    const std::uint64_t t0 = now_ns();
    for (std::size_t i = 0; i < iters; ++i) {
        if (copy) {
            const view_t flat = src.materialize(backend);  // alloc + payload memcpy
            if (flat.empty()) ++exhausted;                 // pool exhaustion == BACKPRESSURE
            // Kept (#1805): nothing reads the copy, so without this the memcpy into a block
            // freed at once is a dead store the optimizer may drop with its alloc/free pair.
            bench::do_not_optimize(flat);
            // `flat` drops here → segment_ptr_t release → backend.destroy (1 free).
        } else {
            tr::view::segment_t* seg = backend.alloc(S);  // the allocation alone
            if (seg == nullptr) {
                ++exhausted;
            } else {
                const tr::view::segment_ptr_t p = tr::view::segment_ptr_t::adopt(seg);
                bench::do_not_optimize(p);  // an unread alloc/free pair is elidable (#1805)
                // `p` drops here → release → backend.destroy (1 free).
            }
        }
    }
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    const double ops = secs > 0 ? iters / secs : 0;
    // ONE metric (#1804): the whole loop is timed as one block, so there is exactly one
    // measurement here — operations per second. It used to be published three ways (as
    // throughput, and as a p50 and a mean that were both `1e9 / ops` truncated to whole
    // nanoseconds), which the gate then counted as three legs agreeing with each other. The
    // latency columns are 0, read everywhere as "not measured"; the history still charts the
    // same figure as ns/delivery. Measuring one iteration between two clock reads is not the
    // fix either: an alloc+free costs the same order as `clock_gettime`.
    const Latency::Summary lat{};
    // Bandwidth only means something for the copy arm. The alloc-only arm moves NO
    // payload — it takes a block and gives it back — so reporting iters*S/secs there
    // published a fabricated figure (a "151 GB/s" zero-copy allocation).
    const double mb_per_s = copy ? static_cast<double>(iters) * S / (secs * 1e6) : 0.0;
    emit("libtracer", mode, S, 1, 1, ops, ops, mb_per_s, lat);
    return {ops, exhausted};
}

/**
 * @brief Run the ADR-0060 LKV copy-store rows across two payload sizes: pooled
 *        `value_backend` vs the default heap, for both the pure alloc/free op and the
 *        end-to-end flatten. Emits the charted `lkv-alloc-*` / `lkv-store-*` series and a
 *        stderr `LKV-RATIO` line (the alloc-cost ratio and the pool's exhaustion count),
 *        which is reported only: the pool's no-heap claim is gated by LKV-ROUTE in
 *        `bench_forward_heap` (#1695).
 */
void run_lkv_store_rows() {
    tr::mem::mem_backend_t& heap = tr::mem::heap_backend();
    // A caller-owned slab carved into equal 2 KB slots. The loop keeps at most one
    // segment live, so a handful of slots suffice; 64 gives headroom and lets the
    // zero-exhaustion invariant (no fragmentation growth) be asserted directly.
    //
    // The slab's alignment is stated, not inherited (#1745). As a `std::vector` it was a
    // ~134 KiB heap block that glibc maps, so its base sat at page + 16, and the reused slot's
    // payload landed on a 64-byte line only because `pool_t`'s 48-byte header made it add up.
    // A static with an explicit `alignas(64)` makes the slot layout the same on every run and
    // every runner, whatever the heap did first.
    constexpr std::size_t kSlot = 2048, kSlots = 64;
    constexpr std::size_t kSlabBytes = kSlots * (sizeof(tr::view::segment_t) + kSlot + 64);
    alignas(64) static std::byte slab[kSlabBytes];
    tr::mem::pool_t pool(std::span<std::byte>(slab, kSlabBytes), kSlot, 64);
    // The payload ladder's larger values (#1806) need a slot of 64 KiB; four slots are enough
    // for a loop that keeps one segment live. A separate pool, so the 2 KB-slot rows above
    // keep the exact slot layout their history was recorded on.
    constexpr std::size_t kBigSlot = 65536, kBigSlots = 4;
    constexpr std::size_t kBigSlabBytes = kBigSlots * (sizeof(tr::view::segment_t) + kBigSlot + 64);
    alignas(64) static std::byte big_slab[kBigSlabBytes];
    tr::mem::pool_t big_pool(std::span<std::byte>(big_slab, kBigSlabBytes), kBigSlot, 64);
    // 64 B and 1 KiB first, in their historical order; then the rest of the ladder.
    std::vector<std::size_t> sizes{64, 1024};
    for (std::size_t S : kPayloadLadder)
        if (S != 64 && S != 1024) sizes.push_back(S);
    for (std::size_t S : sizes) {
        tr::mem::mem_backend_t& pooled = S <= kSlot
                                             ? static_cast<tr::mem::mem_backend_t&>(pool)
                                             : static_cast<tr::mem::mem_backend_t&>(big_pool);
        const lkv_result_t ha = run_lkv_store_alloc(S, false, heap, "lkv-alloc-heap");
        const lkv_result_t pa = run_lkv_store_alloc(S, false, pooled, "lkv-alloc-pool");
        run_lkv_store_alloc(S, true, heap, "lkv-store-heap");
        run_lkv_store_alloc(S, true, pooled, "lkv-store-pool");
        const double ratio = ha.ops_per_s > 0 ? pa.ops_per_s / ha.ops_per_s : 0.0;
        // Reported, not judged (#1695): speed is not the pool's acceptance criterion, and the
        // ratio moves with the host allocator (1.4x-6.5x at 64 B on healthy code).
        std::fprintf(stderr, "LKV-RATIO S=%4zu: pool %5.1fx heap alloc/free  exhausted=%zu\n", S,
                     ratio, pa.exhausted);
    }
}

/**
 * @brief Put this process's heap into one fixed, reproducible AGED state (#1803).
 *
 * A long-running node does not allocate from an empty heap: its free lists hold whatever
 * earlier work left behind. That state used to reach the heap rows only by accident — as the
 * residue of every row ahead of them in the sweep — so it changed whenever a row was added or
 * moved. Here it is built on purpose, from a fixed seed: 16384 blocks of 16..2063 bytes
 * (spanning the 64 B and 1 KiB bins the `lkv-*-heap` rows use), every other one freed, the
 * rest held live until exit. The same binary therefore always measures the same aged heap.
 */
void age_heap() {
    constexpr std::size_t kBlocks = 16384;
    static std::vector<void*> survivors;  // held to exit: the fragmentation IS the state
    std::vector<void*> blocks(kBlocks);
    survivors.reserve(kBlocks / 2);
    std::minstd_rand rng(0x1803);  // fixed seed: one aged state, not a different one per run
    for (void*& b : blocks) b = std::malloc(16 + rng() % 2048);
    for (std::size_t i = 0; i < kBlocks; ++i) {
        if (i % 2 == 0)
            survivors.push_back(blocks[i]);
        else
            std::free(blocks[i]);
    }
}

/**
 * @brief The `lkv-*-heap` rows again, on an AGED heap (#1803): `lkv-alloc-heap-aged` and
 *        `lkv-store-heap-aged`.
 *
 * The un-suffixed `lkv-*-heap` rows run first thing in a fresh process (@ref
 * run_lkv_store_rows in the `lkv` family), so they are the FRESH-heap variant; their names are
 * kept because they are gated keys and history series. This family ages the heap first
 * (@ref age_heap) and reports the same two operations under `-aged` names, so a change that
 * only hurts on a fragmented heap — the shape of the 1 KiB regression — has a row of its own.
 * The pool rows have no aged twin: the pool carves a static slab and never touches the heap.
 */
void run_lkv_aged() {
    age_heap();
    tr::mem::mem_backend_t& heap = tr::mem::heap_backend();
    for (std::size_t S : {std::size_t{64}, std::size_t{1024}}) {
        run_lkv_store_alloc(S, false, heap, "lkv-alloc-heap-aged");
        run_lkv_store_alloc(S, true, heap, "lkv-store-heap-aged");
    }
}

/**
 * @brief One cliff row per size: a segment allocated from @p backend and returned, nothing
 *        else (#1806).
 *
 * The operation is the `lkv-alloc-*` one, `backend.alloc(S)` then `destroy` through the owning
 * `segment_ptr_t`, but timed through @ref bench::time_batches: an alloc/free costs 10-40 ns,
 * which a per-operation clock read cannot resolve and a single bulk window reports as one
 * figure. Each row is a batch row (p50 and mean of the window means, in ns to the picosecond;
 * no p99). The family runs in its own fresh process, so the first row sees an unaged heap and
 * every later row sees only what the rows below it in size left behind.
 *
 * The gate reads each row twice: against main at the same size, and against the next smaller
 * size (`perf_gate.py`'s cliff check), so a new size-class cliff fails at whatever size it
 * appears; one main already has is reported, not failed.
 *
 * @param backend The backend under test: the process heap or a pool with a 64 KiB slot.
 * @param mode    The row's mode: `cliff-alloc-heap` or `cliff-alloc-pool`.
 */
void run_cliff(tr::mem::mem_backend_t& backend, const char* mode) {
    constexpr std::uint64_t kCliffBudgetNs = 20'000'000;  // 20 ms: ~500 windows of 40 us
    // The header is computed the way the heap backend computes it, not read from the #1768
    // trait, so the ladder is the same on a build that predates that trait or reverts it.
    constexpr std::size_t kHeader =
        tr::mem::segment_header_bytes(tr::mem::heap_backend_t::kBlockAlign);
    for (std::size_t S : bench::cliff_sizes(kHeader)) {
        std::size_t exhausted = 0;
        const auto op = [&] {
            tr::view::segment_t* const seg = backend.alloc(S);
            if (seg == nullptr) {
                ++exhausted;
                return;
            }
            const tr::view::segment_ptr_t p = tr::view::segment_ptr_t::adopt(seg);
        };
        const bench::batch_timing_t t = bench::time_batches(op, kCliffBudgetNs);
        bench::emit_batch("libtracer", mode, S, 1, 1, t.ops_per_s, t.ops_per_s, 0.0, t);
        if (exhausted != 0)
            std::printf("WARN mode=%s size=%zu: %zu allocations failed\n", mode, S, exhausted);
    }
}

/**
 * @brief The cliff family on the process-default heap backend (`cliff-alloc-heap`).
 *
 * Fresh heap only. A fenced, aged variant was tried and dropped: on the reference host
 * (glibc 2.39) a request past the per-thread-cache ceiling costs about 6 ns more than a cache
 * hit in a fresh heap and in a fenced one alike, so a second set of 49 rows bought no
 * coverage. The `lkv-*-heap-aged` rows keep an aged heap state for the 64 B and 1 KiB rows.
 */
void family_cliff_heap() { run_cliff(tr::mem::heap_backend(), "cliff-alloc-heap"); }

/**
 * @brief The cliff family on a pool (`cliff-alloc-pool`): one 64 KiB slot fits every size, so
 *        the row should be flat across the ladder; a step in it is a pool regression.
 */
void family_cliff_pool() {
    constexpr std::size_t kSlot = 65536 + 64, kSlots = 4;
    constexpr std::size_t kSlabBytes = kSlots * (sizeof(tr::view::segment_t) + kSlot + 64);
    alignas(64) static std::byte slab[kSlabBytes];
    tr::mem::pool_t pool(std::span<std::byte>(slab, kSlabBytes), kSlot, 64);
    run_cliff(pool, "cliff-alloc-pool");
}

// --- rows for the logic that changed and is coming (#1808) ---------------------------------

/** @brief Entries one spill cycle queues ahead of its write: two past the in-frame slots. */
constexpr std::size_t kSpillBacklog = tr::graph::vertex_t::ring_take_t::kInline + 2;
static_assert(kSpillBacklog + 1 <= 16, "the spill window must fit the fixture's 16-deep ring");

/**
 * @brief `stream-w<T>` (#1808): T writer threads on ONE STREAM vertex, the admission #1713
 *        fused into one stripe-lock section, measured as it runs under contention.
 *
 * Each writer makes a fixed number of 64 B writes (a heap view per write, the `eptype-stream`
 * parity). Throughput is aggregate writes per second; the delivery rate is COUNTED at the one
 * subscriber, after a trailing covering sweep, so a shed or a lost entry lowers it. Latency is
 * one write between two clock reads on every writer, merged, like `inproc-mt<T>`. The thread
 * count lives in the mode name only.
 */
void run_stream_mt(std::size_t T) {
    constexpr std::size_t S = 64;
    constexpr std::size_t MSGS = 200'000;  // per writer (throughput phase)
    constexpr std::size_t LATN = 50'000;   // per writer (latency phase)
    stream_fixture_t fx;
    const std::vector<std::byte> tlv = value_tlv(S);
    const auto put = [&] { (void)fx.g.write(fx.v, owned_view(tlv)); };
    for (std::size_t i = 0; i < 1000; ++i) put();  // warmup

    std::vector<std::vector<std::uint64_t>> lats(T);
    const auto phase = [&](bool timed) {
        std::atomic<std::size_t> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> ts;
        ts.reserve(T);
        for (std::size_t t = 0; t < T; ++t) {
            ts.emplace_back([&, t] {
                std::vector<std::uint64_t>& mine = lats[t];
                mine.resize(LATN);  // reserve AND touch before the release (#1803)
                mine.clear();
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (!go.load(std::memory_order_acquire)) { /* spin until released */
                }
                if (!timed) {
                    for (std::size_t i = 0; i < MSGS; ++i) put();
                    return;
                }
                for (std::size_t i = 0; i < LATN; ++i) {
                    const auto a = now_ns();
                    put();
                    mine.push_back(now_ns() - a);
                }
            });
        }
        while (ready.load(std::memory_order_acquire) < T) { /* wait */
        }
        const auto t0 = now_ns();
        go.store(true, std::memory_order_release);
        for (std::thread& th : ts) th.join();
        return (now_ns() - t0) / 1e9;
    };

    fx.recv.store(0);
    const double secs = phase(false);
    (void)fx.g.propagate(fx.v);  // a covering sweep: whatever a racing take left behind
    const std::uint64_t want = static_cast<std::uint64_t>(T) * MSGS;
    const std::string mode = "stream-w" + std::to_string(T);
    const double pub_s = static_cast<double>(want) / secs;
    const double deliv_s = delivered_rate(mode.c_str(), S, 1, 1, want, fx.recv.load(), secs);
    (void)phase(true);
    Latency lat;
    lat.reserve(T * LATN);
    for (const std::vector<std::uint64_t>& per : lats)
        for (std::uint64_t ns : per) lat.add(ns);
    emit("libtracer", mode.c_str(), S, 1, 1, pub_s, deliv_s, deliv_s * static_cast<double>(S) / 1e6,
         lat.summarize());
}

/**
 * @brief One batch-timed STREAM cycle with its delivery rate counted at the subscriber.
 *
 * The cycle is timed through @ref bench::time_batches. Its deliveries are COUNTED over every
 * cycle that ran, calibration included, and scale the cycle rate into the delivery column, so
 * an entry the window lost lowers the figure instead of being assumed.
 */
template <typename Cycle>
void emit_stream_cycle(const char* mode, stream_fixture_t& fx, Cycle&& cycle,
                       std::size_t per_cycle) {
    constexpr std::uint64_t kBudgetNs = 200'000'000;
    std::uint64_t cycles = 0;
    fx.recv.store(0);
    const bench::batch_timing_t t = bench::time_batches(
        [&] {
            cycle();
            ++cycles;
        },
        kBudgetNs);
    const std::uint64_t want = cycles * per_cycle;
    const std::uint64_t got = fx.recv.load(std::memory_order_relaxed);
    if (got != want)
        std::fprintf(stderr, "WARN mode=%s delivered %llu of %llu entries\n", mode,
                     static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
    const double per = cycles != 0 ? static_cast<double>(got) / static_cast<double>(cycles) : 0.0;
    bench::emit_batch("libtracer", mode, 64, 1, 1, t.ops_per_s, t.ops_per_s * per, 0.0, t);
}

/**
 * @brief The single-writer STREAM cycles (#1808): `stream-spill` and `stream-defer`.
 *
 *  - `stream-spill`: @ref kSpillBacklog `assign`s queue entries without delivering, then one
 *    `write` admits its own entry and takes the whole window. The window is wider than
 *    `ring_take_t::kInline`, so the take spills to its one heap vector (#1713's overflow arm).
 *  - `stream-defer`: `ring_take_t::kInline` `assign`s with delivery DEFERRED, then one
 *    covering `propagate` delivers them: the in-frame take, with no write in the window.
 *
 * Each row's p50 and mean are per CYCLE, not per entry; the delivery column is entries per
 * second, counted. The refused-spill deferral (a window held back because its spill could not
 * be allocated) is an exact-count row in `bench_forward_heap`, which can refuse the allocation.
 */
void run_stream_cycles() {
    const std::vector<std::byte> tlv = value_tlv(64);
    {
        stream_fixture_t fx;
        emit_stream_cycle(
            "stream-spill", fx,
            [&] {
                for (std::size_t i = 0; i < kSpillBacklog; ++i)
                    (void)fx.g.assign(fx.v, owned_view(tlv));
                (void)fx.g.write(fx.v, owned_view(tlv));
            },
            kSpillBacklog + 1);
    }
    {
        stream_fixture_t fx;
        constexpr std::size_t kDeferred = tr::graph::vertex_t::ring_take_t::kInline;
        emit_stream_cycle(
            "stream-defer", fx,
            [&] {
                for (std::size_t i = 0; i < kDeferred; ++i)
                    (void)fx.g.assign(fx.v, owned_view(tlv));
                (void)fx.g.propagate(fx.v);
            },
            kDeferred);
    }
}

/**
 * @brief `route-handle-egress-mt<T>` (#1808, advisory): T producer threads on ONE advertised
 *        `(link, route)` flow of a `route_handle_t`, the steady-state reuse read a compacted
 *        delivery's egress takes per write.
 *
 * `bench_route_handle_contention` sweeps this to T=128 on a wall-clock window; this is its
 * T=1/2/4 slice in the default sweep, on fixed work per thread so every point runs the same
 * operations. One metric: aggregate reads per second. The per-op columns are 0, because one
 * read costs about what a clock read does and a mean derived from the rate is not a
 * measurement of its own (#1804). Charted, not gated.
 */
void run_route_handle_mt(std::size_t T) {
    constexpr std::size_t kOps = 500'000;  // per thread
    constexpr std::size_t kRouteBytes = 32;
    tr::net::route_handle_t h;
    const std::vector<std::byte> route(kRouteBytes, std::byte{0x5A});
    (void)h.ensure_egress("b", route);  // advertise once: every timed call is a reuse read
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::atomic<std::size_t> sink{0};
    std::vector<std::thread> ts;
    ts.reserve(T);
    for (std::size_t t = 0; t < T; ++t) {
        ts.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) { /* spin until released */
            }
            std::size_t acc = 0;
            for (std::size_t i = 0; i < kOps; ++i) acc += h.ensure_egress("b", route).first;
            sink.fetch_add(acc, std::memory_order_relaxed);
        });
    }
    while (ready.load(std::memory_order_acquire) < T) { /* wait */
    }
    const auto t0 = now_ns();
    go.store(true, std::memory_order_release);
    for (std::thread& th : ts) th.join();
    const double secs = (now_ns() - t0) / 1e9;
    if (sink.load() == 0) std::fprintf(stderr, "WARN route-handle reads returned no label\n");
    const double ops = static_cast<double>(T * kOps) / secs;
    const std::string mode = "route-handle-egress-mt" + std::to_string(T);
    emit("libtracer", mode.c_str(), kRouteBytes, 1, 1, ops, ops, 0.0, Latency::Summary{});
}

/**
 * @brief The allocation seam's two decisions, timed (#1808): size-class selection and the
 *        fallback to an upstream source.
 *
 *  - `seam-class-c<C>`: a `pool_source_t` whose class table holds C classes, the 64 B one
 *    LAST, so each 64 B `try_alloc` + `release` walks all C to find its class. C = 1, 8, 32:
 *    the cost of choosing a class as the table grows.
 *  - `seam-fallback`: a `bump_source_t` whose buffer is full, over an upstream
 *    `pool_source_t`: each 64 B block is refused by the bump, served by the upstream, and
 *    released back there. `seam-direct` is the same block from the upstream alone, so the
 *    pair's difference is what the fallback costs.
 *
 * Batch rows at 64 B; the size-classed host pool (#1777) will be judged on the same rows.
 */
void run_alloc_seam() {
    constexpr std::uint64_t kBudgetNs = 100'000'000;
    constexpr std::size_t S = 64, kAlign = alignof(std::max_align_t);
    const auto alloc_free = [](tr::mem::block_source_t& src) {
        return [&src] {
            void* const p = src.try_alloc(S, kAlign);
            if (p != nullptr) src.release(p, S, kAlign);
        };
    };
    for (std::size_t C : {std::size_t{1}, std::size_t{8}, std::size_t{32}}) {
        std::vector<std::byte> slab(64 * 1024);
        std::vector<tr::mem::size_class_t> classes(C);
        tr::mem::pool_source_t<> pool{slab, classes};
        for (std::size_t i = 1; i < C; ++i) {  // the other classes first, so 64 B is the last
            const std::size_t n = S + 16 * i;
            pool.release(pool.try_alloc(n, kAlign), n, kAlign);
        }
        pool.release(pool.try_alloc(S, kAlign), S, kAlign);
        if (pool.classes_used() != C)
            std::fprintf(stderr, "WARN seam-class-c%zu: %zu classes in use\n", C,
                         pool.classes_used());
        const bench::batch_timing_t t = bench::time_batches(alloc_free(pool), kBudgetNs);
        const std::string mode = "seam-class-c" + std::to_string(C);
        bench::emit_batch("libtracer", mode.c_str(), S, 1, 1, t.ops_per_s, t.ops_per_s, 0.0, t);
    }
    std::vector<std::byte> slab(64 * 1024);
    std::vector<tr::mem::size_class_t> classes(4);
    tr::mem::pool_source_t<> upstream{slab, classes};
    alignas(64) std::array<std::byte, 64> full{};
    tr::mem::bump_source_t bump{full, upstream};
    while (bump.used() + 8 <= full.size() && bump.try_alloc(8, 8) != nullptr) {
    }  // fill the buffer: from here on every 64 B request falls back to the upstream
    const bench::batch_timing_t direct = bench::time_batches(alloc_free(upstream), kBudgetNs);
    bench::emit_batch("libtracer", "seam-direct", S, 1, 1, direct.ops_per_s, direct.ops_per_s, 0.0,
                      direct);
    const bench::batch_timing_t fb = bench::time_batches(alloc_free(bump), kBudgetNs);
    bench::emit_batch("libtracer", "seam-fallback", S, 1, 1, fb.ops_per_s, fb.ops_per_s, 0.0, fb);
    if (upstream.refused() != 0)
        std::fprintf(stderr, "WARN seam rows: the upstream refused %zu requests\n",
                     upstream.refused());
}

/** @brief `stream-mt` family (#1808): `stream-w1`, `-w2`, `-w4`, capped by the affinity mask. */
void family_stream_mt() {
    const std::size_t hw = bench::usable_cpus();
    for (std::size_t T : {std::size_t{1}, std::size_t{2}, std::size_t{4}})
        if (T <= hw) run_stream_mt(T);
}

/** @brief `route-handle` family (#1808): T = 1, 2, 4, capped by the affinity mask. */
void family_route_handle() {
    const std::size_t hw = bench::usable_cpus();
    for (std::size_t T : {std::size_t{1}, std::size_t{2}, std::size_t{4}})
        if (T <= hw) run_route_handle_mt(T);
}

/**
 * @brief `inproc-pool-batch` (#1808): the window-calibrated twin of every heap-view
 *        `inproc-pool` row, in its own process so the quantized pool rows did not move.
 */
void family_inproc_pool_batch() {
    for (std::size_t S : kSizes)
        run_inproc_pool(S, kRefFanout, kRefEndpoints, alloc_t::HEAP, false, "inproc-pool",
                        kDeliveryBudget, kLatencyDeliveryBudget, rows_t::BATCH);
}

/**
 * @brief `dce-canary` (#1805): proof that this build still times the work its rows name.
 *
 * Two rows, `dce-canary/8` and `dce-canary/64` (the size column is the chain's step count):
 * a dependent mixing chain, timed through @ref bench::time_batches like every batch row. Kept
 * work takes about 8x longer at 64 steps; deleted work does not. If the long chain does not
 * take at least 4x the short one, the family exits 2 and the sweep fails. What it proves is
 * that the timed chain itself was not deleted; it does NOT prove that
 * @ref bench::do_not_optimize is load-bearing (on GCC 13 the chain survives with that clobber
 * blanked), so the sinks that rely only on the clobber are not covered by it.
 */
void family_dce_canary() {
    const bench::dce_canary_t c = bench::measure_dce_canary(100'000'000ULL);
    bench::emit_batch("libtracer", "dce-canary", bench::kDceSmallK, 1, 1, c.small.ops_per_s, 0.0,
                      0.0, c.small);
    bench::emit_batch("libtracer", "dce-canary", bench::kDceBigK, 1, 1, c.big.ops_per_s, 0.0, 0.0,
                      c.big);
    if (!c.holds) {
        std::fprintf(stderr,
                     "DCE-CANARY FAIL: %zu steps %.3f ns, %zu steps %.3f ns — the measured work "
                     "was optimized away\n",
                     bench::kDceSmallK, c.small.p50_ps / 1e3, bench::kDceBigK, c.big.p50_ps / 1e3);
        std::exit(2);
    }
}

}  // namespace

/**
 * @brief ADR-0060 §2: concurrent alloc+free through a shared backend at T threads.
 *
 * The thread-safe sync pool (spinlock) vs the thread-safe default heap — tracks whether
 * the pool's O(1) locked free-list beats `malloc` under contention, and WHERE the single
 * spinlock starts to bottleneck (the signal that motivates the lock-free index+tag CAS
 * upgrade held in ADR-0060 §2). Aggregate ops/s across T threads + thread-0 latency.
 */
void run_syncpool_mt(std::size_t T, tr::mem::mem_backend_t& backend, const char* base) {
    constexpr std::size_t S = 64;
    constexpr std::size_t kOpsPerThread = 200000;
    std::atomic<std::uint64_t> done{0};
    Latency lat0;  // thread 0 only writes it; read after join (happens-before)
    std::vector<std::thread> ts;
    // EVERY thread is instrumented, not just thread 0. Instrumenting one thread meant the
    // published throughput came from a cheaper loop (T-1 threads skipping two clock reads
    // per op) than the one whose latency was published — two different workloads in one
    // row, with the throughput inflated ~1.4-1.7x relative to the latency's conditions.
    std::mutex lat_m;
    // One collector per thread, reserved and touched BEFORE t0 (#1803), so neither the
    // reservation nor its page faults land inside the timed window.
    std::vector<Latency> mines(T);
    for (Latency& m : mines) m.reserve(kOpsPerThread);
    lat0.reserve(T * kOpsPerThread);
    ts.reserve(T);
    const auto t0 = now_ns();
    for (std::size_t t = 0; t < T; ++t) {
        ts.emplace_back([&, t] {
            Latency& mine = mines[t];
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                const std::uint64_t a = now_ns();
                tr::view::segment_t* raw = backend.alloc(S);
                if (raw != nullptr) {
                    const tr::view::segment_ptr_t p = tr::view::segment_ptr_t::adopt(raw);
                }  // p drops -> destroy (locked for the pool) on this thread
                mine.add(now_ns() - a);
                done.fetch_add(1, std::memory_order_relaxed);
            }
            const std::lock_guard g(lat_m);
            lat0.merge(mine);
        });
    }
    for (auto& th : ts) th.join();
    const double secs = (now_ns() - t0) / 1e9;
    const double ops = secs > 0 ? done.load() / secs : 0;
    const std::string mode = base + std::to_string(T);
    // fan=1, ep=1, and bandwidth=0 — all three deliberately.
    //
    // This runner allocates a segment and drops it. It delivers NOTHING and copies NO bytes,
    // so the row must not claim otherwise. It previously emitted the thread count in the
    // FAN-OUT column (a series literally named `.../fan4/1ep` for 4 threads and zero
    // subscribers) and `ops * S` as bandwidth for a loop that moves zero bytes. The thread
    // count now lives only in the mode name, where it is not mistakable for a topology.
    //
    // The rate still lands in the pub/deliv columns because `emit`'s 12-field shape is fixed
    // and shared with every other bench; what changed is the MODE NAME — `poolalloc-` /
    // `heapalloc-` rather than `syncpool-` / `heap-`, so the charted series reads as an
    // allocator rate instead of a delivery rate. That renames the series, which ends the old
    // ones and starts new ones: the correct, VISIBLE outcome when a row's meaning was wrong,
    // as opposed to silently re-valuing a name readers already trust.
    emit("libtracer", mode.c_str(), S, 1, 1, ops, ops, 0.0, lat0.summarize());
}

/** @brief The sync-pool vs heap MT contention sweep (charted to gh-pages, not gated). */
void run_syncpool_gate() {
    const std::size_t hw = bench::usable_cpus();
    // A slab comfortably larger than the max concurrent live set (each thread holds <=1).
    std::vector<std::byte> slab(64 * (64 + sizeof(tr::view::segment_t) + 64));
    for (std::size_t T : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        if (T > hw) continue;
        tr::mem::synchronized_pool_t<> pool(slab, 64);  // fresh free-list per T
        run_syncpool_mt(T, pool, "poolalloc-mt");
        run_syncpool_mt(T, tr::mem::heap_backend(), "heapalloc-mt");
    }
}

/**
 * @brief What `path_t::parse` itself costs — the tax EVERY path-keyed operation pays.
 *
 * Nothing measured this. `inproc-path` parses its addresses once into a vector and reuses the
 * `path_t`s, so it times the registry lookup on an already-parsed key; the parse was invisible
 * to the whole suite. That mattered, because `parse` built its canonical PATH-TLV payload by
 * geometric doubling — a two-segment address walked a 1→2→4→8→16 realloc chain, four throwaway
 * blocks per call — and the natural API use (`g.write(*path_t::parse("/a/b"), v)`) pays it per
 * operation.
 *
 * Swept over segment COUNT rather than payload size, because the realloc chain grew with the
 * number of `emit_name` appends, not with the bytes. Reported under `size_bytes` = address
 * length so the row keeps the standard 12-field shape and flows into the history store.
 */
void run_path_parse() {
    static constexpr const char* kAddrs[] = {
        "/a",
        "/bench/v0001",
        "/net/ws-server/up/peer0",
        "/a/b/c/d/e/f/g/h",
    };
    for (const char* addr : kAddrs) {
        const std::string_view a{addr};
        std::size_t segs = 0;
        for (const char c : a) {
            if (c == '/') ++segs;
        }
        // Batch-amortized for the same reason the net-plane benches are: one parse is close
        // enough to `clock_gettime` that per-op timing would measure the clock. The batch is
        // sized by window (>= 20 µs, asserted) and the result kept in picoseconds (#1804).
        constexpr std::uint64_t kBudgetNs = 300000000ULL;
        std::size_t sink = 0;
        const bench::batch_timing_t t = bench::time_batches(
            [&] {
                const auto p = tr::graph::path_t::parse(a);
                sink += p.has_value() ? p->segment_count() : 0;
                bench::do_not_optimize(p);  // the parsed path, not only its count (#1805)
            },
            kBudgetNs);
        if (sink == 0) std::printf("WARN path-parse produced nothing\n");
        bench::emit_batch("libtracer", "path-parse", a.size(), segs, 1, t.ops_per_s, t.ops_per_s,
                          0.0, t);
    }
}

namespace {

/** @brief `acl`: the ACL-gate rows alone (A/B runs) — uncontended, then contended at 4. */
void run_mode_acl() {
    run_acl_gated();
    run_acl_gated_mt(4);
}

/** @brief `deliver`: the deliver-only (`propagate`) fan sweep alone (A/B runs). */
void run_mode_deliver() {
    for (std::size_t F : kFanouts) run_inproc_deliver(kRefSize, F);
}

/** @brief `target`: the two path-target dispatch legs alone (A/B runs). */
void run_mode_target() {
    for (std::size_t F : kFanouts)
        run_inproc_target(kRefSize, F, target_kind_t::STORED, "inproc-target-stored");
    for (std::size_t F : kFanouts)
        run_inproc_target(kRefSize, F, target_kind_t::HANDLER, "inproc-target-handler");
}

/**
 * @brief `fan`: the `inproc` fan-out LADDER alone, ascending.
 *
 * Coarse (`kFanouts`) merged with the mid arms (`kFanoutsMid`) so one short run reads as a
 * single curve (#844). This is the A/B mode for anything that touches dispatch width:
 * measured on this host the ladder runs in ~1.7 s against ~30 s for the default sweep,
 * nearly all of which is off this axis — which is what made a hand-added mid arm the path
 * of least resistance for #841's verify (that line was scaffolding and was never
 * committed). Ascending here rather than coarse-then-mid because nothing joins on this
 * mode's row ORDER — only the default run feeds perf_gate.py and the history store, and
 * that run keeps every pre-existing ordinal exactly where it was.
 */
void run_mode_fan() {
    constexpr std::size_t kLadder[] = {1, 8, 16, 32, 64, 128, 256, 512, 1024, 8192};
    static_assert(std::size(kLadder) == std::size(kFanouts) + std::size(kFanoutsMid),
                  "kLadder must be the union of kFanouts and kFanoutsMid — an arm added "
                  "to either without being added here would be missing from `fan` runs");
    for (std::size_t F : kLadder)
        run_inproc(kRefSize, F, kRefEndpoints, alloc_t::HEAP, false, "inproc");
}

/**
 * @brief `fan-remote`: the same ladder over REMOTE wire subscribers (#1448).
 *
 * The `fan` ladder's edges are local callbacks and carry no cold half, so they price the
 * dispatch WIDTH and nothing about the remote-subscriber record. This is the arm that does.
 * The ladder is shared with @ref run_mode_fan on purpose — the two rows are meant to be read
 * side by side at the same fan-out.
 */
void run_mode_fan_remote() {
    constexpr std::size_t kLadder[] = {1, 8, 16, 32, 64, 128, 256, 512, 1024, 8192};
    static_assert(std::size(kLadder) == std::size(kFanouts) + std::size(kFanoutsMid),
                  "kLadder must be the union of kFanouts and kFanoutsMid — it is read against "
                  "the `fan` ladder point for point");
    for (std::size_t F : kLadder) run_inproc_remote(kRefSize, F, "inproc-remote");
}

/**
 * @brief #1485 addendum C — the TOPIC-COUNT arm, in both address spellings (`topics`).
 *
 * `topics-bound` writes through a pre-bound @ref tr::graph::vertex_handle_t; `topics-addr` writes
 * through a pre-parsed @ref tr::graph::path_t, so the registry resolution lands inside every
 * timed iteration. Both reach the same one subscriber per topic, at the same payload.
 *
 * This pair exists because the published topic-count comparison did not have it. The libtracer
 * row was `inproc-path` — resolve per write — and the Zenoh row of the same name published
 * through a declared `Publisher`, which resolves nothing per put. So a resolution term sat
 * inside one arm and nowhere in the other, and the reported narrowing of the margin across the
 * topic ladder could not be attributed to either engine's topic scaling. `bench_zenoh topics`
 * emits the matching pair; run them from `run_topics.sh`, which alternates the engines and runs
 * both arm orders.
 *
 * The batch twin is suppressed on these rows (`rows_t::QUANTIZED`): the whole point is a
 * cross-engine comparison, the Zenoh side has no batch twin to compare against, and ten more
 * unread series is not what a new arm should cost.
 */
void run_mode_topics() {
    for (std::size_t E : kTopicLadder) {
        run_inproc(kRefSize, kRefFanout, E, alloc_t::HEAP, false, "topics-bound", kDeliveryBudget,
                   kLatencyDeliveryBudget, nullptr, rows_t::QUANTIZED);
        run_inproc(kRefSize, kRefFanout, E, alloc_t::HEAP, true, "topics-addr", kDeliveryBudget,
                   kLatencyDeliveryBudget, nullptr, rows_t::QUANTIZED);
    }
}

/** @brief @ref run_mode_topics with the two arms swapped — the order flip (`topics-rev`). */
void run_mode_topics_rev() {
    for (std::size_t E : kTopicLadder) {
        run_inproc(kRefSize, kRefFanout, E, alloc_t::HEAP, true, "topics-addr", kDeliveryBudget,
                   kLatencyDeliveryBudget, nullptr, rows_t::QUANTIZED);
        run_inproc(kRefSize, kRefFanout, E, alloc_t::HEAP, false, "topics-bound", kDeliveryBudget,
                   kLatencyDeliveryBudget, nullptr, rows_t::QUANTIZED);
    }
}

/** @brief One selectable isolated sweep: its `argv[1]` spelling and the runner behind it. */
struct bench_mode_t {
    std::string_view name; /**< What `argv[1]` must equal to select this sweep. */
    void (*run)();         /**< The sweep this mode runs, and nothing else. */
};

/**
 * @brief Every mode `argv[1]` accepts — the ONE source dispatch and the usage text share.
 *
 * The dispatch used to be a chain of `if (argv[1] == "...") { ...; return 0; }` tests with
 * no terminating else, so an argument matching none of them fell through into the DEFAULT
 * sweep and exited 0 (#1040). That is a measurement hazard rather than a CLI nit: these
 * modes exist for A/B runs, an A/B run builds one arm from a DIFFERENT commit, and a mode
 * added after that commit does not exist in it — so the candidate ran one axis for seconds
 * while the baseline ran every axis for minutes, both emitting well-formed RESULT rows
 * under the same `(mode, size, fan, ep)` keys for a harness to join on.
 *
 * A table rather than a chain because the usage text is now GENERATED from it: a mode that
 * is dispatchable is a mode that is listed, and a name that is listed is a name that runs
 * its sweep, with no second list to keep in step.
 */
constexpr bench_mode_t kModes[] = {
    {"grid", run_grid},
    {"acl", run_mode_acl},
    {"deliver", run_mode_deliver},
    {"target", run_mode_target},
    {"fan", run_mode_fan},
    {"lkv", run_lkv_store_rows},
    {"fan-remote", run_mode_fan_remote},
    {"topics", run_mode_topics},
    {"topics-rev", run_mode_topics_rev},
};

/*
 * The default sweep, as FAMILIES (#1803). Each family below is one block of what used to be
 * `main`'s single in-process sweep, unchanged in content and in row order, so the transcript
 * keeps every row, every ordinal and every line shape it had. What changed is the process: the
 * default run starts each family as its own child process (`--family <name>`), under the
 * pinned allocator tunables, so no family inherits the heap another one aged.
 */

/**
 * @brief `inproc` fan-out sweep at the reference payload (gated `inproc/64/1024/1`).
 *
 * Starts past fan-out 1 (#1805): `inproc/64/1/1` is the payload sweep's reference row, and
 * this family used to print it too, so the gate and the history medianed two processes' runs
 * of one point under one key. The `fan` A/B mode still runs the whole ladder.
 */
void family_inproc_fan() {
    for (std::size_t F : kFanouts)
        if (F != kRefFanout) run_inproc(kRefSize, F, kRefEndpoints, alloc_t::HEAP, false, "inproc");
}

/** @brief `inproc` payload sweep at the reference fan-out (gated `inproc/64/1/1`). */
void family_inproc_size() {
    for (std::size_t S : kSizes)
        run_inproc(S, kRefFanout, kRefEndpoints, alloc_t::HEAP, false, "inproc");
    // The payload ladder's rows above and around 1 KiB (#1806), after every existing row.
    for (std::size_t S : bench::ladder_extra())
        run_inproc(S, kRefFanout, kRefEndpoints, alloc_t::HEAP, false, "inproc",
                   bench::ladder_budget(S, kDeliveryBudget),
                   bench::ladder_budget(S, kLatencyDeliveryBudget));
    // The dense comparison sweep (#1890), after those: Zenoh's `inproc-size` runs the same.
    for (std::size_t S : bench::sweep_extra(kSizes, bench::kPayloadLadder))
        run_inproc(S, kRefFanout, kRefEndpoints, alloc_t::HEAP, false, "inproc",
                   bench::ladder_budget(S, kDeliveryBudget),
                   bench::ladder_budget(S, kLatencyDeliveryBudget));
}

/** @brief `inproc-borrow` payload sweep (gated `inproc-borrow/64/1/1`). */
void family_inproc_borrow() {
    for (std::size_t S : kSizes)
        run_inproc(S, kRefFanout, kRefEndpoints, alloc_t::BORROW, false, "inproc-borrow");
    for (std::size_t S : bench::ladder_extra())  // the payload ladder (#1806)
        run_inproc(S, kRefFanout, kRefEndpoints, alloc_t::BORROW, false, "inproc-borrow",
                   bench::ladder_budget(S, kDeliveryBudget),
                   bench::ladder_budget(S, kLatencyDeliveryBudget));
}

/** @brief `inproc-path` topic-count sweep, write by path (gated `inproc-path/64/1/8192`). */
void family_inproc_path() {
    for (std::size_t E : kEndpoints)
        run_inproc(kRefSize, kRefFanout, E, alloc_t::HEAP, true, "inproc-path");
}

/** @brief n-cores (parallel-dispatch) axis: thread counts clamped to the CPUs the bench may use. */
void family_inproc_mt() {
    const std::size_t hw = bench::usable_cpus();
    for (std::size_t T : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}})
        if (T <= hw) run_inproc_mt(T);
}

/**
 * @brief ACL-gated reads with inheritance (ADR-0050 cached effective-ACE merge): the
 *        uncontended gate cost, then the shared-ancestor contended case where 4 usable CPUs
 *        exist.
 */
void family_acl() {
    run_acl_gated();
    if (bench::usable_cpus() >= 4) run_acl_gated_mt(4);
}

/**
 * @brief n-layer-folded (fold-depth) axis: the same total bytes folded across N segments
 *        (N=1 flat .. N=8 rope); cost rises with the view-chain walk.
 */
void family_fold() {
    for (std::size_t N : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}})
        run_fold(N);
}

/**
 * @brief The full 1:1 write THROUGH an injected pool `mr_` vs the default global-heap
 *        `inproc` / `inproc-borrow` (see @ref run_inproc_pool for the reading).
 */
void family_inproc_pool() {
    for (std::size_t S : kSizes)
        run_inproc_pool(S, kRefFanout, kRefEndpoints, alloc_t::HEAP, false, "inproc-pool");
    for (std::size_t S : kSizes)
        run_inproc_pool(S, kRefFanout, kRefEndpoints, alloc_t::BORROW, false, "inproc-pool-borrow");
}

/**
 * @brief MID fan-out arms (#844): 16 / 32 / 64 / 256 / 512 on the `inproc` mode — the two
 *        gaps in `kFanouts` where the cost model kinks. Same `mode` string as the coarse
 *        ladder, so they land on the SAME charted series (a denser curve, not a new one).
 */
void family_inproc_fan_mid() {
    for (std::size_t F : kFanoutsMid)
        run_inproc(kRefSize, F, kRefEndpoints, alloc_t::HEAP, false, "inproc");
}

/** @brief Which family set a family belongs to — see @ref kFamilies. */
enum class family_set_t {
    SINGLE, /**< One thread does the timed work: no row waits on another of the bench's own. */
    MULTI,  /**< Starts worker threads (plus a coordinating main thread that spins). */
};

/** @brief One family of the default sweep: its `--family` spelling, its runner, its set. */
struct bench_family_t {
    std::string_view name; /**< What `--family` must equal to select this family. */
    void (*run)();         /**< The rows this family emits, and nothing else. */
    family_set_t set;      /**< SINGLE- or MULTI-threaded — what `--family-set` selects on. */
};

/**
 * @brief The default sweep, family by family, in its historical row order.
 *
 * ORDER IS LOAD-BEARING for the transcript, not for the numbers: every consumer joins rows by
 * `(mode, size, fan, ep)`, but the default run's ordinals were kept stable for years and new
 * families append at the END, never ahead of a gated row. Since each family now runs in its
 * own process, the order no longer changes any row's VALUE — which is what
 * `LIBTRACER_BENCH_FAMILY_SEED` (a shuffled order) exists to check.
 *
 * Notes carried over from the in-process sweep:
 *   - `deliver` (the store-free counterpart of the inproc fan sweep) was deliberately last
 *     among the original rows; everything after it is appended in the order it was added.
 *   - `lkv` is the ADR-0060 copy-store gate on a FRESH heap; its two 64 B `lkv-store-*` rows
 *     are perf_gate.py POINTS (#1250), and `perf_gate.py` runs the same function through the
 *     `lkv` mode, i.e. in the same process shape, for the pool/heap ratio check.
 *   - `syncpool` is ADR-0060 §2's sync-pool vs heap under T-thread contention (charted, not
 *     gated).
 *   - `target` is the PATH-TARGET fan-out (#619), the leg a wire `SUBSCRIBER` takes.
 *   - `lkv-aged` (#1803) is new and appended last: the `lkv-*-heap` rows on an aged heap.
 *   - `cliff-heap` and `cliff-pool` (#1806) come after it: alloc-only rows over the
 *     allocator-cliff ladder (`bench::cliff_sizes`), one family per backend.
 *   - #1808's families come last: `stream` (the spill and deferral cycles), `stream-mt`
 *     (1, 2 and 4 writers on one STREAM vertex), `route-handle` (the egress reuse read at
 *     T = 1, 2, 4), `alloc-seam` (class selection and the upstream fallback) and
 *     `inproc-pool-batch` (the window-calibrated twin of the `inproc-pool` rows).
 *   - `dce-canary` (#1805) comes after them: two rows proving the sink clobber still keeps
 *     timed work.
 *   - `topics` (#1809) is last of all: the `topics` mode's bound/by-path pair, so the Zenoh
 *     topic-count chart has its libtracer rows in every default run. Charted, not gated.
 *
 * The SET column splits the sweep for the perf gate's measurement-condition check. A MULTI
 * family runs T workers on the pinned CPUs while its main thread spins waiting for them, so
 * the bench's OWN threads queue behind each other and raise its own cgroup's CPU pressure.
 * The condition check reads that pressure at the NEXT invocation's launch and used to mark
 * the gate INCONCLUSIVE for it. So perf_gate.py times the two sets as separate invocations,
 * both compared A/B: `--family-set single` judged on foreign time and pressure, then
 * `--family-set multi` judged on foreign time only. Foreign CPU time on the bench CPUs, which
 * is how a real intruder shows, is scored on both.
 *   - There is no `loopback` or `routers-hN` family: those modes benchmarked the ROUTER-flood
 *     bridge, retired in ADR-0040 — the net plane is explicit-source-routed FWD only, and its
 *     forward cost is measured by bench_forward_heap and the fwd_* tests.
 */
constexpr bench_family_t kFamilies[] = {
    {"inproc-fan", family_inproc_fan, family_set_t::SINGLE},
    {"inproc-size", family_inproc_size, family_set_t::SINGLE},
    {"inproc-borrow", family_inproc_borrow, family_set_t::SINGLE},
    {"inproc-path", family_inproc_path, family_set_t::SINGLE},
    {"mixed", run_mixed, family_set_t::SINGLE},
    {"path-parse", run_path_parse, family_set_t::SINGLE},
    {"inproc-mt", family_inproc_mt, family_set_t::MULTI},
    {"eptype", run_eptype, family_set_t::SINGLE},
    {"acl", family_acl, family_set_t::MULTI},
    {"fold", family_fold, family_set_t::SINGLE},
    {"deliver", run_mode_deliver, family_set_t::SINGLE},
    {"lkv", run_lkv_store_rows, family_set_t::SINGLE},
    {"inproc-pool", family_inproc_pool, family_set_t::SINGLE},
    {"syncpool", run_syncpool_gate, family_set_t::MULTI},
    {"target", run_mode_target, family_set_t::SINGLE},
    {"inproc-fan-mid", family_inproc_fan_mid, family_set_t::SINGLE},
    {"lkv-aged", run_lkv_aged, family_set_t::SINGLE},
    {"cliff-heap", family_cliff_heap, family_set_t::SINGLE},
    {"cliff-pool", family_cliff_pool, family_set_t::SINGLE},
    {"stream", run_stream_cycles, family_set_t::SINGLE},
    {"stream-mt", family_stream_mt, family_set_t::MULTI},
    {"route-handle", family_route_handle, family_set_t::MULTI},
    {"alloc-seam", run_alloc_seam, family_set_t::SINGLE},
    {"inproc-pool-batch", family_inproc_pool_batch, family_set_t::SINGLE},
    {"dce-canary", family_dce_canary, family_set_t::SINGLE},
    // Last, so no earlier family's rows or ordinals move: the `topics-bound` / `topics-addr`
    // pair in the default transcript, for the Zenoh topic charts (#1809). Ungated.
    {"topics", run_mode_topics, family_set_t::SINGLE},
};

/**
 * @brief The family run order: declared order, or a seeded shuffle of it.
 *
 * `LIBTRACER_BENCH_FAMILY_SEED=<n>` shuffles the order deterministically from @p n and prints
 * it on stderr. It exists to TEST the isolation (#1803): with every family in its own process,
 * a shuffled run must leave every row inside its A/A spread.
 */
std::vector<const bench_family_t*> family_order() {
    std::vector<const bench_family_t*> order;
    order.reserve(std::size(kFamilies));
    for (const bench_family_t& f : kFamilies) order.push_back(&f);
    const char* seed = std::getenv("LIBTRACER_BENCH_FAMILY_SEED");
    if (seed != nullptr && *seed != '\0') {
        std::mt19937 rng(static_cast<std::mt19937::result_type>(std::strtoul(seed, nullptr, 10)));
        std::shuffle(order.begin(), order.end(), rng);
        std::fprintf(stderr, "FAMILY-ORDER seed=%s:", seed);
        for (const bench_family_t* f : order)
            std::fprintf(stderr, " %.*s", static_cast<int>(f->name.size()), f->name.data());
        std::fprintf(stderr, "\n");
    }
    return order;
}

/**
 * @brief The default sweep: every family, each in a fresh child process (#1803).
 *
 * With @p only set, runs just the families of that set (`--family-set`), in the same order.
 * Stops at the first family that fails and returns non-zero, so a crash in one family is a
 * failed run rather than a transcript silently missing that family's rows.
 */
int run_default_sweep(const char* argv0, const family_set_t* only = nullptr) {
    for (const bench_family_t* f : family_order()) {
        if (only != nullptr && f->set != *only) continue;
        if constexpr (!kFamilyProcesses) {
            f->run();  // no child processes on this platform: the old in-process sweep
            continue;
        }
        const int rc = run_family_process(argv0, f->name);
        if (rc != 0) {
            std::fprintf(stderr, "error: family '%.*s' failed (status %d)\n",
                         static_cast<int>(f->name.size()), f->name.data(), rc);
            return 1;
        }
    }
    return 0;
}

/** @brief The usage text, on stderr, listing every entry of @ref kModes and @ref kFamilies. */
void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [mode]\n"
                 "  no mode:  the full default sweep (what CI and perf_gate.py run)\n"
                 "  modes:    ",
                 argv0 != nullptr ? argv0 : "bench_libtracer");
    for (const bench_mode_t& m : kModes)
        std::fprintf(stderr, "%s%.*s", &m == &kModes[0] ? "" : " | ",
                     static_cast<int>(m.name.size()), m.name.data());
    std::fprintf(stderr,
                 "  (one isolated sweep each, for A/B runs)\n"
                 "  --family NAME: one family of the default sweep, in this process\n"
                 "  --family-set single|multi: the default sweep, one set of families\n"
                 "  --families: list every family and its set (no rows)\n"
                 "  families: ");
    for (const bench_family_t& f : kFamilies)
        std::fprintf(stderr, "%s%.*s", &f == &kFamilies[0] ? "" : " | ",
                     static_cast<int>(f.name.size()), f.name.data());
    std::fprintf(stderr, "\n");
}

}  // namespace

int main(int argc, char** argv) {
    // Fixed allocator state (#1803): re-exec under the pinned GLIBC_TUNABLES, before any
    // output. Every family child inherits them from here.
    pin_allocator_state(argv);
    if (argc > 2 && std::string_view{argv[1]} == "--family") {
        const std::string_view want{argv[2]};
        for (const bench_family_t& f : kFamilies) {
            if (f.name != want) continue;
            // stderr, like the MODE marker: stdout stays the RESULT stream.
            std::fprintf(stderr, "FAMILY %.*s\n", static_cast<int>(f.name.size()), f.name.data());
            const std::size_t start_kb = bench::rss_kb();
            f.run();
            bench::emit_family_rss(f.name, start_kb);
            return 0;
        }
        std::fprintf(stderr, "error: unknown family '%s'\n", argv[2]);
        print_usage(argv[0]);
        return 2;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--families") {
        // The capability probe perf_gate.py runs before it asks for `--family-set`: a binary
        // without family sets refuses this as an unknown mode, exits 2, and is swept whole.
        for (const bench_family_t& f : kFamilies)
            std::printf("%.*s\t%s\n", static_cast<int>(f.name.size()), f.name.data(),
                        f.set == family_set_t::MULTI ? "multi" : "single");
        return 0;
    }
    if (argc > 2 && std::string_view{argv[1]} == "--family-set") {
        const std::string_view want{argv[2]};
        if (want == "single" || want == "multi") {
            const family_set_t only = want == "multi" ? family_set_t::MULTI : family_set_t::SINGLE;
            bench::emit_clock_floor();  // the run's clock floor, ahead of its rows (#1804)
            return run_default_sweep(argv[0], &only);
        }
        std::fprintf(stderr, "error: unknown family set '%s'\n", argv[2]);
        print_usage(argv[0]);
        return 2;
    }
    if (argc > 1) {
        const std::string_view want{argv[1]};
        for (const bench_mode_t& m : kModes) {
            if (m.name != want) continue;
            // The marker goes to STDERR, ahead of the first row: stdout is the RESULT
            // stream perf_gate.py, perf_emit_benchmark.py and collate.py parse, and the
            // acceptance rule for this fix is that every run's stdout keeps the row set,
            // row order and line shape it had before. stderr already carries the harness's
            // own `SKIP mode=...` and `LKV-RATIO ...` lines, so an A/B driver that wants to
            // assert it got the arm it asked for reads it there.
            std::fprintf(stderr, "MODE %.*s\n", static_cast<int>(m.name.size()), m.name.data());
            bench::emit_clock_floor();
            m.run();
            return 0;
        }
        // Unrecognised: say so and exit non-zero WITHOUT emitting a row. Silence plus a
        // zero status is what let a typo (`taget`) and a cross-commit mode both answer
        // with the full default sweep.
        std::fprintf(stderr, "error: unknown mode '%s'\n", argv[1]);
        print_usage(argv[0]);
        return 2;
    }
    // The default sweep: every family of @ref kFamilies, each in its own fresh process. The
    // clock floor is measured once, here in the parent, ahead of every family's rows (#1804).
    bench::emit_clock_floor();
    return run_default_sweep(argv[0]);
}
