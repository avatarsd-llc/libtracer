/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * libtracer side of the libtracer-vs-Zenoh comparison. Sweeps the matrix:
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tracer.hpp"

using namespace bench;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::settings_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::view::view_t;

namespace {

// A VALUE TLV carrying `payload` bytes (so loopback exercises real encode/decode).
std::vector<std::byte> value_tlv(std::size_t payload) {
    std::vector<std::byte> p(payload, std::byte{0xAB});
    tr::wire::tlv_t t{};
    t.type = tr::wire::type_t::VALUE;
    t.payload = p;
    return tr::wire::encode(t);
}

// Per-message owned heap view (alloc + copy each publish) — the allocating path.
view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

// Borrowed view over a stable buffer — zero alloc, zero copy (refcount handoff).
view_t borrowed_view(std::span<const std::byte> bytes) {
    return view_t::over(tr::view::borrow_const(bytes));
}

enum class alloc_t { HEAP, BORROW };

// One inproc run: E endpoints, F subscribers each, S-byte payload. `by_path`
// writes through the path registry (lookup each publish) instead of the resolved
// vertex_handle_t hot path — the honest "many topics" measurement.
void run_inproc(std::size_t S, std::size_t F, std::size_t E, alloc_t alloc, bool by_path,
                const char* mode, std::uint64_t budget = kDeliveryBudget,
                std::uint64_t latbudget = kLatencyDeliveryBudget) {
    graph_t g;
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

    Latency lat;
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        put(i);
        lat.add(now_ns() - a);
    }
    emit("libtracer", mode, S, F, E, pub_s, deliv_s, mb_s, lat.summarize());
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

// Response-surface grid (system dynamics): size x fanout (endpoints=1, mode
// `inproc`) and size x endpoints (fanout=1, write-by-path, mode `inproc-path`).
// Emits the standard mode-tagged RESULT line (same 12-field shape as the default
// run) so one parser feeds both the terminal table and the docs comparison charts.
void run_grid() {
    for (std::size_t S : kGridSizes)
        for (std::size_t F : kGridFanouts)
            run_inproc(S, F, 1, alloc_t::HEAP, false, "inproc", kGridBudget, kGridLatBudget);
    // Deliver-only fan sweep at the reference payload (the comparison charts' fixed
    // size): propagate touches no payload bytes, so a full size sweep would be flat.
    for (std::size_t F : kGridFanouts) run_inproc_deliver(kRefSize, F, kGridBudget, kGridLatBudget);
    for (std::size_t S : kGridSizes)
        for (std::size_t E : kGridEndpoints)
            run_inproc(S, 1, E, alloc_t::HEAP, true, "inproc-path", kGridBudget, kGridLatBudget);
}

// Mixed workload: 128 topics with varied fan-out (1..16) and payloads (1..8192).
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

// n-cores (parallel-dispatch) axis (#96 / ADR-0032). T independent publisher
// threads, each driving its OWN graph + endpoint with the zero-copy in-process
// path, measured for AGGREGATE throughput + per-op latency under load. Each
// thread reuses one borrowed view over a stable per-thread buffer, so the timed
// loop allocates nothing (no cross-thread allocator contention) — what scales is
// dispatch itself. Fixed per-thread work, so more cores => more aggregate work.
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

// The STREAM-role class: time write+deliver where each write retains history.
void run_eptype_stream() {
    constexpr std::size_t S = 64;
    graph_t g;
    settings_t st{};
    st.history_keep_last = 16;  // a real bounded ring: retention work on every write
    const path_t path = *path_t::parse("/bench/stream");
    auto v = g.register_vertex(path, role_t::STREAM, {}, st);
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

// The full ep-type sweep: lean, lean-cached, stream — all at size=64 fan=1 ep=1.
void run_eptype() {
    run_inproc(kRefSize, 1, 1, alloc_t::HEAP, false, "eptype-lean");
    run_inproc(kRefSize, 1, 1, alloc_t::BORROW, false, "eptype-lean-cached");
    run_eptype_stream();
}

// n-layer-folded (fold-depth) axis (#96 / ADR-0032) — the LAST axis. How does the
// L0/L1 zero-copy COMPOSITION cost scale with how many memory layers a value is
// FOLDED across? We hold the TOTAL bytes CONSTANT (kFoldTotal) and sweep the fold
// depth N: the same value is built as a rope of N borrowed views over N segments —
// N=1 is one flat segment, N=8 is an 8-link rope of identical total bytes. Per op we
// serialize the folded value for egress the way a transport does: build the
// scatter-gather descriptor (rope_t::to_iovec — spans into the N segments, no copy)
// and walk it. Because the bytes are fixed and only the fold depth changes, the delta
// isolates the view-chain walk / scatter-gather cost: more folds => more links to
// gather => higher per-op cost (and lower throughput). (The naming "n-layer-folded" /
// "fold depth" is provisional — see bench/README.md "n-layer-folded axis".)
void run_fold(std::size_t N) {
    constexpr std::size_t kFoldTotal = 512;  // total bytes, CONSTANT across N (isolate fold)
    const std::size_t seg_bytes = kFoldTotal / N;

    // Stable per-segment buffers; the rope BORROWS them, so the timed loop allocates
    // nothing for the value — what it pays is purely the fold-depth walk/gather.
    std::vector<std::vector<std::byte>> bufs(N, std::vector<std::byte>(seg_bytes, std::byte{0xAB}));
    tr::view::rope_t rope;
    for (auto& b : bufs) rope.append(borrowed_view(b));

    // One egress-serialize op: gather the rope into a scatter-gather iovec, then walk
    // the links (the view-chain walk a transport / codec performs to ship the rope).
    const auto serialize = [&]() -> std::size_t {
        const auto iov = rope.to_iovec();
        std::size_t acc = 0;
        for (const auto& s : iov)
            acc += s.size() + (s.empty() ? 0u : std::to_integer<std::size_t>(s[0]));
        return acc;
    };

    volatile std::size_t sink = 0;
    constexpr std::size_t MSGS = 2'000'000;                      // throughput phase
    constexpr std::size_t LATN = 200'000;                        // latency phase
    for (std::size_t i = 0; i < 1000; ++i) sink += serialize();  // warmup

    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) sink += serialize();
    const double secs = (now_ns() - t0) / 1e9;
    const double pub_s = MSGS / secs;
    const double deliv_s = pub_s;  // fan=1 => one egress per publish
    const double mb_s = deliv_s * static_cast<double>(kFoldTotal) / 1e6;

    Latency lat;
    for (std::size_t i = 0; i < LATN; ++i) {
        const auto a = now_ns();
        sink += serialize();
        lat.add(now_ns() - a);
    }
    (void)sink;
    const std::string mode = "fold-n" + std::to_string(N);
    emit("libtracer", mode.c_str(), kFoldTotal, 1, 1, pub_s, deliv_s, mb_s, lat.summarize());
}

// ACL-gated data ops with inheritance (ADR-0050): a depth-4 tree whose root and
// mid level carry INHERIT ACEs, a subject resolver installed, and every op arriving
// under a granted caller context — so each op pays the full effective-ACL check.
// The measured op is the GATED READ (the gate plus the lock-free LKV load — the
// leanest gated data op, so the gate's cost is what the row sees). Two modes:
//
//   acl-inherit-d4      one thread, one leaf — the uncontended gate cost.
//   acl-inherit-d4-mtT  T threads, each gating reads on its OWN leaf under the
//                       SHARED ancestor chain — what the ADR-0050 cached merge
//                       buys: pre-cache every op locked each shared ancestor's
//                       mutex (cross-core cacheline traffic on hot composites);
//                       post-cache an op touches only its own vertex's lock.
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

// The single-threaded row: the uncontended per-op gate cost (mode acl-inherit-d4).
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

// The contended row (mode acl-inherit-d4-mtT): T reader threads, one leaf each,
// all gated through the SAME ancestor chain — the shared-composite hot case.
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

}  // namespace

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
    for (std::size_t F : kFanouts)
        run_inproc(kRefSize, F, kRefEndpoints, alloc_t::HEAP, false, "inproc");
    for (std::size_t S : kSizes)
        run_inproc(S, kRefFanout, kRefEndpoints, alloc_t::HEAP, false, "inproc");
    for (std::size_t S : kSizes)
        run_inproc(S, kRefFanout, kRefEndpoints, alloc_t::BORROW, false, "inproc-borrow");
    for (std::size_t E : kEndpoints)
        run_inproc(kRefSize, kRefFanout, E, alloc_t::HEAP, true, "inproc-path");
    run_mixed();
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
    return 0;
}
