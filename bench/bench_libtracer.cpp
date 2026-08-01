/**
 * @file
 * @brief libtracer side of the libtracer-vs-Zenoh comparison.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Sweeps the matrix:
 *   - fan-out   1/8/128/1024/8192 subscribers on one endpoint (dispatch scaling)
 *   - payload   1..8192 bytes (per-byte cost), heap-alloc vs borrowed (zero-alloc)
 *   - endpoints 1..8192 distinct topics, write BY PATH (registry/lookup scaling)
 *   - mixed     128 topics, varied fan-out + payloads
 * Module compositions are surfaced as distinct `mode`s (inproc / inproc-borrow /
 * inproc-path / mixed / eptype-* / fold-* / inproc-mt*) — "different approaches to
 * craft libtracer". inproc is the zero-copy graph dispatch. (The `loopback` /
 * `routers-hN` ROUTER-flood modes were retired with bridge_t — ADR-0040; FWD forward
 * cost is measured by bench_forward_heap.) See bench/README.md for the caveats.
 */
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/rope.hpp"
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

/** @brief A VALUE TLV carrying `payload` bytes (so loopback exercises real encode/decode). */
std::vector<std::byte> value_tlv(std::size_t payload) {
    std::vector<std::byte> p(payload, std::byte{0xAB});
    tr::wire::tlv_t t{};
    t.type = tr::wire::type_t::VALUE;
    t.payload = p;
    return tr::wire::encode(t);
}

/** @brief Per-message owned heap view (alloc + copy each publish) — the allocating path. */
view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief Borrowed view over a stable buffer — zero alloc, zero copy (refcount handoff). */
view_t borrowed_view(std::span<const std::byte> bytes) {
    return view_t::over(tr::view::borrow_const(bytes));
}

enum class alloc_t { HEAP, BORROW };

/**
 * @brief Publish the batch-amortized twin of a quantized latency row (#553).
 *
 * Times a CALIBRATED BATCH of @p op and divides, so the two `clock_gettime` reads and the
 * clock's granularity are amortized across the batch instead of dominating one operation.
 * Emitted as a SEPARATE row under `<mode>-batch` rather than replacing the quantized one:
 * the quantized series are the primary key of long-running `gh-pages` history, and giving
 * an existing name a new meaning would silently make every point before the change
 * incomparable to every point after it.
 *
 * **This row is a LATENCY instrument only.** Every other column is left at 0, which the
 * history emitter reads as "this row does not produce that metric" and skips rather than
 * charting — the same convention the `lkv-*` constant-zero p99 fix established. Each is
 * zero for its own reason:
 *
 * - **p99** — each sample here is a MEAN over `batch` operations, so its 99th percentile
 *   is the tail of the batch means: it measures scheduling interference BETWEEN batches,
 *   not the tail of one operation. Averaging destroys exactly the quantity a p99 is read
 *   for, so publishing one under that name would be a fabricated tail. Read the quantized
 *   twin's p99 for tail shape and this row's p50/mean for magnitude.
 * - **throughput** — this arm could report its own, and it would be a real measurement,
 *   but it would be a WORSE one: the bulk phase above times an order of magnitude more
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
    std::size_t i = 0;
    const std::size_t batch = calibrate_batch([&] { op(i++); });
    const std::size_t rounds = std::max<std::size_t>(1, lat_n / batch);

    Latency lat;
    for (std::size_t r = 0; r < rounds; ++r) {
        const auto a = now_ns();
        for (std::size_t b = 0; b < batch; ++b) op(i++);
        lat.add((now_ns() - a) / batch);
    }

    Latency::Summary sum = lat.summarize();
    sum.p99 = 0;  // a percentile of batch means is not an operation's tail — see above

    const std::string batch_mode = std::string(mode) + "-batch";
    emit("libtracer", batch_mode.c_str(), S, F, E, 0.0, 0.0, 0.0, sum);
    std::printf(
        "NOTE mode=%s batch=%zu rounds=%zu (latency-only row: throughput and "
        "bandwidth belong to the bulk phase of `%s`)\n",
        batch_mode.c_str(), batch, rounds, mode);
    std::fflush(stdout);
}

/**
 * @brief One inproc run: E endpoints, F subscribers each, S-byte payload.
 *
 * `by_path`
 * writes through the path registry (lookup each publish) instead of the resolved
 * vertex_handle_t hot path — the honest "many topics" measurement.
 */
void run_inproc(std::size_t S, std::size_t F, std::size_t E, alloc_t alloc, bool by_path,
                const char* mode, std::uint64_t budget = kDeliveryBudget,
                std::uint64_t latbudget = kLatencyDeliveryBudget,
                std::pmr::memory_resource* mr = nullptr, bool batch_row = true) {
    // mr==nullptr keeps the default global-heap LKV (make_shared); an injected pool
    // routes the per-write LKV allocate_shared through it (ADR-0060 mr_ seam) — the
    // only difference between `inproc` and `inproc-pool` (graph.cpp store uses mr_).
    graph_t g{mr ? mr : std::pmr::get_default_resource()};
    std::vector<vertex_handle_t> verts;
    std::vector<path_t> paths;
    verts.reserve(E);
    paths.reserve(E);
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const rope_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
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

    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put(i);
    const double secs = (now_ns() - t0) / 1e9;
    const double pub_s = MSGS / secs;
    const double deliv_s = pub_s * static_cast<double>(F);
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
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        put(i);
        lat.add(now_ns() - a);
    }
    emit("libtracer", mode, S, F, E, pub_s, deliv_s, mb_s, lat.summarize());
    if (batch_row) emit_batch_row(mode, S, F, E, put, LATN);
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
                     std::uint64_t latbudget = kLatencyDeliveryBudget) {
    std::pmr::unsynchronized_pool_resource pool;
    // No `-batch` twin (#553): what these rows are FOR is the pooled-vs-heap LKV
    // comparison, and that is gated by the `lkv` same-run throughput ratio
    // (perf_gate.py lkv_ratio_gate), not by a latency percentile. A batch twin here
    // would be ten more series with no chart reading them.
    run_inproc(S, F, E, alloc, by_path, mode, budget, latbudget, &pool, false);
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
    std::vector<std::byte> name_bytes;
    for (char c : target_segment)
        name_bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    tr::wire::tlv_t name{.type = tr::wire::type_t::NAME, .payload = name_bytes};
    tr::wire::tlv_t path{.type = tr::wire::type_t::PATH};
    path.opt.pl = true;
    path.children.push_back(name);
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
            h.on_write = [&recv](const rope_t&) -> tr::graph::result_t<void> {
                recv.fetch_add(1, std::memory_order_relaxed);
                return {};
            };
            (void)g.register_vertex(*path_t::parse("/" + seg), role_t::HANDLER, std::move(h));
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
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put(i);
    const double secs = (now_ns() - t0) / 1e9;
    const double pub_s = MSGS / secs;
    const double deliv_s = pub_s * static_cast<double>(F);
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;
    if (kind == target_kind_t::HANDLER && recv.load() == 0) {
        std::fprintf(stderr, "SKIP mode=%s fanout=%zu: no delivery reached a handler target\n",
                     mode, F);
        return;
    }

    Latency lat;
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        put(i);
        lat.add(now_ns() - a);
    }
    emit("libtracer", mode, S, F, 1, pub_s, deliv_s, mb_s, lat.summarize());
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
    auto cb = [&](const rope_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    for (std::size_t f = 0; f < F; ++f) (void)g.subscribe(path, cb);
    const std::vector<std::byte> tlv = value_tlv(S);
    (void)g.write(v, owned_view(tlv));  // store ONCE — the timed ops below move no bytes
    const auto put = [&]() { g.propagate(v); };

    const std::size_t MSGS = publishes_for(F, budget);
    const std::size_t LATN = publishes_for(F, latbudget);
    for (std::size_t i = 0; i < 1000; ++i) put();  // warmup

    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put();
    const double secs = (now_ns() - t0) / 1e9;
    const double pub_s = MSGS / secs;
    const double deliv_s = pub_s * static_cast<double>(F);
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    Latency lat;
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
                       nullptr, false);
    // Deliver-only fan sweep at the reference payload (the comparison charts' fixed
    // size): propagate touches no payload bytes, so a full size sweep would be flat.
    for (std::size_t F : kGridFanouts) run_inproc_deliver(kRefSize, F, kGridBudget, kGridLatBudget);
    for (std::size_t S : kGridSizes)
        for (std::size_t E : kGridEndpoints)
            run_inproc(S, 1, E, alloc_t::HEAP, true, "inproc-path", kGridBudget, kGridLatBudget,
                       nullptr, false);
}

/** @brief Mixed workload: 128 topics with varied fan-out (1..16) and payloads (1..8192). */
void run_mixed() {
    graph_t g;
    constexpr std::size_t E = 128;
    std::vector<vertex_handle_t> verts;
    std::vector<std::size_t> fan;
    std::vector<std::vector<std::byte>> tlvs;
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const rope_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
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

    std::uint64_t deliveries = 0;
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) {
        const std::size_t e = i % E;
        (void)g.write(verts[e], owned_view(tlvs[e]));
        deliveries += fan[e];
    }
    const double secs = (now_ns() - t0) / 1e9;

    Latency lat;
    for (std::size_t i = 0; i < 20000; ++i) {
        const std::size_t e = i % E;
        const auto a = now_ns();
        (void)g.write(verts[e], owned_view(tlvs[e]));
        lat.add(now_ns() - a);
    }
    emit("libtracer", "mixed", 0, total_fan / E, E, MSGS / secs, deliveries / secs, 0.0,
         lat.summarize());
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
            [](void* ctx, const rope_t&) {
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

    const double pub_s = static_cast<double>(T) * MSGS / secs;  // F=1 => deliv==pub
    const double deliv_s = pub_s;
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    // --- Latency phase: per-op timing under the same parallel load. ---
    std::atomic<std::size_t> ready2{0};
    std::atomic<bool> go2{false};
    std::vector<std::thread> lthreads;
    lthreads.reserve(T);
    for (std::size_t t = 0; t < T; ++t) {
        worker_t* w = ws[t].get();
        lthreads.emplace_back([w, &ready2, &go2]() {
            w->lat.reserve(LATN);
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
    for (auto& w : ws)
        for (std::uint64_t ns : w->lat) lat.add(ns);

    const std::string mode = "inproc-mt" + std::to_string(T);
    emit("libtracer", mode.c_str(), S, 1, T, pub_s, deliv_s, mb_s, lat.summarize());
}

// ep-type (endpoint-dispatch-class) axis (#96 / ADR-0032). On ONE fixed workload
// (size=64, fan=1, ep=1) we compare the three dispatch CLASSES a write can take to
// an endpoint, emitting one RESULT line per class with `mode` tagging the class:
//
//   eptype-lean        minimal sink: a plain in-process write+deliver to a
//                      STORED_VALUE vertex, heap-allocated view per publish.
//                      Same path as the existing `inproc` mode.
//   eptype-lean-cached the zero-alloc loaned / out_cache read path: a borrowed view
//                      (zero alloc, zero copy — a refcount handoff). Same path as
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

/** @brief The STREAM-role class: time write+deliver where each write retains history. */
void run_eptype_stream() {
    constexpr std::size_t S = 64;
    graph_t g;
    const path_t path = *path_t::parse("/bench/stream");
    auto v = g.register_vertex(path, role_t::STREAM);
    g.set_history_depth(v, 16);  // a real bounded ring: retention work on every write
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const rope_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    (void)g.subscribe(path, cb);

    const std::vector<std::byte> tlv = value_tlv(S);
    const auto put = [&]() { (void)g.write(v, owned_view(tlv)); };  // heap view: lean parity

    const std::size_t MSGS = publishes_for(1, kDeliveryBudget);
    const std::size_t LATN = publishes_for(1, kLatencyDeliveryBudget);
    for (std::size_t i = 0; i < 1000; ++i) put();  // warmup

    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) put();
    const double secs = (now_ns() - t0) / 1e9;
    const double pub_s = MSGS / secs;
    const double deliv_s = pub_s;  // fan=1 => one delivery per publish
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    Latency lat;
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        put();
        lat.add(now_ns() - a);
    }
    emit("libtracer", "eptype-stream", S, 1, 1, pub_s, deliv_s, mb_s, lat.summarize());
}

/** @brief The full ep-type sweep: lean, lean-cached, stream — all at size=64 fan=1 ep=1. */
void run_eptype() {
    // No `-batch` twins (#553): these two re-emit `inproc` / `inproc-borrow` at the
    // reference point under an endpoint-type name, so their batch twins would be a
    // duplicate measurement of `inproc-batch 64B/fan1/1ep` and its borrow counterpart.
    run_inproc(kRefSize, 1, 1, alloc_t::HEAP, false, "eptype-lean", kDeliveryBudget,
               kLatencyDeliveryBudget, nullptr, false);
    run_inproc(kRefSize, 1, 1, alloc_t::BORROW, false, "eptype-lean-cached", kDeliveryBudget,
               kLatencyDeliveryBudget, nullptr, false);
    run_eptype_stream();
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
    // op costs ~8-15 ns; timing one between two `steady_clock` reads measured the CLOCK, and
    // published p50=30 / p99=31 for EVERY width. The `lat-fold` chart was consequently four
    // identical flat lines, and the perf gate's `fold-n4` leg was dead: p50=30 with
    // LAT_REGRESS=1.15 needs 34.5, i.e. one 10 ns tick, so a real ~11 ns op had to more than
    // DOUBLE before the gate could fire. See #553 for the same defect in the other rows.
    constexpr std::size_t kBatch = 256;
    Latency lat;
    for (std::size_t r = 0; r < 800; ++r) {
        const auto a = now_ns();
        for (std::size_t i = 0; i < kBatch; ++i) sink += serialize();
        lat.add((now_ns() - a) / kBatch);
    }
    (void)sink;
    // Mode renamed `fold-n*` -> `fold-b*` because the number now means something different
    // (a batch-amortized per-op cost, not a clock-quantized single-shot). Renaming ends the
    // old series and starts a new one, which is the visible outcome a changed meaning should
    // have. Bandwidth is 0: the op reads at most 8 payload bytes per link, so the old
    // `deliv_s * 512` was ~74 GB/s of bytes never touched.
    const std::string mode = "fold-b" + std::to_string(N);
    emit("libtracer", mode.c_str(), kFoldTotal, 1, 1, pub_s, deliv_s, 0.0, lat.summarize());
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
    g.set_subject_resolver([](std::string_view caller) -> std::optional<std::vector<std::byte>> {
        if (caller.empty()) return std::nullopt;  // trusted local (the setup writes)
        std::vector<std::byte> token(caller.size());
        std::memcpy(token.data(), caller.data(), caller.size());
        return token;
    });
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
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) { /* spin until released */
            }
            for (std::size_t i = 0; i < MSGS; ++i) get();
            lats[t].reserve(LATN);
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

    constexpr std::size_t kIters = 200000;
    std::size_t exhausted = 0;
    {
        const view_t warm = src.materialize(backend);
        (void)warm;
    }  // fault-in / warm caches
    const std::uint64_t t0 = now_ns();
    for (std::size_t i = 0; i < kIters; ++i) {
        if (copy) {
            const view_t flat = src.materialize(backend);  // alloc + payload memcpy
            if (flat.empty()) ++exhausted;                 // pool exhaustion == BACKPRESSURE
            // `flat` drops here → segment_ptr_t release → backend.destroy (1 free).
        } else {
            tr::view::segment_t* seg = backend.alloc(S);  // the allocation alone
            if (seg == nullptr) {
                ++exhausted;
            } else {
                const tr::view::segment_ptr_t p = tr::view::segment_ptr_t::adopt(seg);
                // `p` drops here → release → backend.destroy (1 free).
            }
        }
    }
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    const double ops = secs > 0 ? kIters / secs : 0;
    Latency::Summary lat{};
    // Batch-amortized: the whole loop is timed as one block, so p50 == mean == the
    // per-op average and there IS no distribution to take a percentile of. p99 stays
    // 0, which the emitter reads as "not measured" and declines to record — it used
    // to publish a constant-zero p99 series per commit, which was not a measurement
    // of anything. Measuring one iteration between two clock reads is not the fix
    // either: an alloc+free costs the same order as `clock_gettime`.
    lat.p50 = lat.mean = static_cast<std::uint64_t>(secs * 1e9 / kIters);
    // Bandwidth only means something for the copy arm. The alloc-only arm moves NO
    // payload — it takes a block and gives it back — so reporting kIters*S/secs there
    // published a fabricated figure (a "151 GB/s" zero-copy allocation).
    const double mb_per_s = copy ? static_cast<double>(kIters) * S / (secs * 1e6) : 0.0;
    emit("libtracer", mode, S, 1, 1, ops, ops, mb_per_s, lat);
    return {ops, exhausted};
}

/**
 * @brief Run the ADR-0060 LKV copy-store gate across two payload sizes: pooled
 *        `value_backend` vs the default heap, for both the pure alloc/free op (the
 *        gated metric) and the end-to-end flatten. Emits the charted `lkv-alloc-*` /
 *        `lkv-store-*` series and a stderr `LKV-GATE` line (the human-visible
 *        alloc-cost ratio + the zero-exhaustion / no-fragmentation check).
 */
void run_lkv_store_gate() {
    tr::mem::mem_backend_t& heap = tr::mem::heap_backend();
    // A caller-owned slab carved into equal 2 KB slots. The loop keeps at most one
    // segment live, so a handful of slots suffice; 64 gives headroom and lets the
    // zero-exhaustion invariant (no fragmentation growth) be asserted directly.
    constexpr std::size_t kSlot = 2048, kSlots = 64;
    std::vector<std::byte> slab(kSlots * (sizeof(tr::view::segment_t) + kSlot + 64));
    tr::mem::pool_t pool(slab, kSlot);
    for (std::size_t S : {std::size_t{64}, std::size_t{1024}}) {
        const lkv_result_t ha = run_lkv_store_alloc(S, false, heap, "lkv-alloc-heap");
        const lkv_result_t pa = run_lkv_store_alloc(S, false, pool, "lkv-alloc-pool");
        run_lkv_store_alloc(S, true, heap, "lkv-store-heap");
        run_lkv_store_alloc(S, true, pool, "lkv-store-pool");
        const double ratio = ha.ops_per_s > 0 ? pa.ops_per_s / ha.ops_per_s : 0.0;
        // Zero exhaustion == no fragmentation growth (fixed slots always reclaimed).
        // Floor 2.0x: glibc's tcache makes a hot same-size malloc/free ~15 ns, so the
        // pool's O(1) free-list clears ~2.5x here — the ADR's >=10x is the ESP-IDF
        // multi_heap figure (validated on-device, the follow-up). The gate proves the
        // routing (not a heap fallback) + determinism, robustly across host allocators.
        std::fprintf(stderr, "LKV-GATE S=%4zu: pool %5.1fx heap alloc/free  exhausted=%zu  %s\n", S,
                     ratio, pa.exhausted, (ratio >= 2.0 && pa.exhausted == 0) ? "PASS" : "FAIL");
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
    const auto t0 = now_ns();
    for (std::size_t t = 0; t < T; ++t) {
        ts.emplace_back([&] {
            Latency mine;
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
    const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    // A slab comfortably larger than the max concurrent live set (each thread holds <=1).
    std::vector<std::byte> slab(64 * (64 + sizeof(tr::view::segment_t) + 64));
    for (std::size_t T : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        if (T > hw) continue;
        tr::mem::sync_pool_t pool(slab, 64);  // fresh free-list per T
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
        // enough to `clock_gettime` that per-op timing would measure the clock.
        constexpr std::size_t kBatch = 256;
        Latency lat;
        std::size_t sink = 0;
        const std::uint64_t t0 = now_ns();
        std::size_t iters = 0;
        while (now_ns() - t0 < 300000000ULL) {
            const std::uint64_t s0 = now_ns();
            for (std::size_t i = 0; i < kBatch; ++i) {
                const auto p = tr::graph::path_t::parse(a);
                sink += p.has_value() ? p->segment_count() : 0;
            }
            lat.add((now_ns() - s0) / kBatch);
            ++iters;
        }
        if (sink == 0) std::printf("WARN path-parse produced nothing\n");
        const double total_s = static_cast<double>(now_ns() - t0) / 1e9;
        const double per_s = static_cast<double>(iters * kBatch) / total_s;
        emit("libtracer", "path-parse", a.size(), segs, 1, per_s, per_s, 0.0, lat.summarize());
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "grid") {
        run_grid();
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "acl") {  // the ACL rows alone (A/B runs)
        run_acl_gated();
        run_acl_gated_mt(4);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "deliver") {  // deliver-only rows (A/B runs)
        for (std::size_t F : kFanouts) run_inproc_deliver(kRefSize, F);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "target") {  // path-target rows alone (A/B runs)
        for (std::size_t F : kFanouts)
            run_inproc_target(kRefSize, F, target_kind_t::STORED, "inproc-target-stored");
        for (std::size_t F : kFanouts)
            run_inproc_target(kRefSize, F, target_kind_t::HANDLER, "inproc-target-handler");
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "lkv") {  // ADR-0060 alloc gate alone (fast)
        run_lkv_store_gate();
        return 0;
    }
    for (std::size_t F : kFanouts)
        run_inproc(kRefSize, F, kRefEndpoints, alloc_t::HEAP, false, "inproc");
    for (std::size_t S : kSizes)
        run_inproc(S, kRefFanout, kRefEndpoints, alloc_t::HEAP, false, "inproc");
    for (std::size_t S : kSizes)
        run_inproc(S, kRefFanout, kRefEndpoints, alloc_t::BORROW, false, "inproc-borrow");
    for (std::size_t E : kEndpoints)
        run_inproc(kRefSize, kRefFanout, E, alloc_t::HEAP, true, "inproc-path");
    run_mixed();
    run_path_parse();
    // n-cores (parallel-dispatch) axis: thread counts clamped to the host CPU.
    const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    for (std::size_t T : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}})
        if (T <= hw) run_inproc_mt(T);
    // ep-type (endpoint-dispatch-class) axis: lean / lean-cached / stream.
    run_eptype();
    // ACL-gated reads with inheritance (ADR-0050 cached effective-ACE merge):
    // the uncontended gate cost + the shared-ancestor contended case.
    run_acl_gated();
    if (hw >= 4) run_acl_gated_mt(4);
    // (The `loopback` and n-routers `routers-hN` modes benchmarked the ROUTER-flood
    // bridge, retired in ADR-0040 — the net plane is explicit-source-routed FWD only.
    // FWD forward cost is measured by bench_forward_heap + the fwd_* tests.)
    // n-layer-folded (fold-depth) axis — same total bytes folded across N
    // segments (N=1 flat .. N=8 rope); cost rises with the view-chain walk.
    for (std::size_t N : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}})
        run_fold(N);
    // Deliver-only (propagate) fan sweep — the store-free counterpart of the
    // inproc fan sweep. Deliberately LAST: perf_gate.py medians duplicate row
    // instances from this default run, and inserting a new sweep ahead of the
    // gated rows shifts their thermal/turbo position on small shared runners
    // (a deterministic ~+100 ns on 2-vCPU CI — observed on PR #353's gate).
    // New sweeps append here, after every pre-existing row, for the same reason.
    for (std::size_t F : kFanouts) run_inproc_deliver(kRefSize, F);
    // ADR-0060: the write-path copy-store alloc gate — pooled value_backend vs the
    // default heap on the branch/field-write flatten. Two NEW charted series
    // (lkv-store-heap / lkv-store-pool) + a same-run ratio gate (bench/perf_gate.py).
    // Appended LAST per the row-ordering note above (never ahead of a gated row).
    run_lkv_store_gate();
    // The full 1:1 write THROUGH an injected pool `mr_` (unsynchronized_pool_resource) vs the
    // default global-heap `inproc` / `inproc-borrow`. Isolates what the pool actually does to
    // the per-write persist — which, measured, is make it ~19 ns SLOWER on this host (~104 vs
    // ~85 ns/op): the pool is a determinism/bounded-ceiling lever, not a latency one. See the
    // note on `run_inproc_pool` for the full reading and for the two claims this comment used
    // to make that measurement refuted. Two charted series to gh-pages (inproc-pool /
    // inproc-pool-borrow),
    // sweeping payload at fan=1 (where the per-publish alloc is un-amortised). Appended
    // LAST per the row-ordering note above: never ahead of a gated row.
    for (std::size_t S : kSizes)
        run_inproc_pool(S, kRefFanout, kRefEndpoints, alloc_t::HEAP, false, "inproc-pool");
    for (std::size_t S : kSizes)
        run_inproc_pool(S, kRefFanout, kRefEndpoints, alloc_t::BORROW, false, "inproc-pool-borrow");
    // ADR-0060 §2: thread-safe (spinlock) sync-pool vs the thread-safe heap under
    // T-thread contention (syncpool-mtT / heap-mtT). Tracks where the single spinlock
    // bottlenecks — the signal for the lock-free CAS upgrade. Appended LAST (never ahead
    // of a gated row); tracked to gh-pages, not gated.
    run_syncpool_gate();
    // PATH-TARGET fan-out (#619): the same 1/8/128/1024/8192 fan sweep the `inproc` rows
    // run, but with edges that carry a `target_key` instead of a callback — the leg a wire
    // `SUBSCRIBER` actually takes. Two charted series (inproc-target-stored /
    // inproc-target-handler) so the two dispatch legs are separable in the results.
    // Appended LAST per the row-ordering note above: never ahead of a gated row.
    for (std::size_t F : kFanouts)
        run_inproc_target(kRefSize, F, target_kind_t::STORED, "inproc-target-stored");
    for (std::size_t F : kFanouts)
        run_inproc_target(kRefSize, F, target_kind_t::HANDLER, "inproc-target-handler");
    return 0;
}
