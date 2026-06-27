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
 * inproc-path / loopback / mixed) — "different approaches to craft libtracer".
 * inproc is the zero-copy graph dispatch; loopback exercises encode/ROUTER/decode
 * across a thread. See bench/README.md for the (important) caveats.
 */
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/tracer.hpp"

using namespace bench;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::settings_t;
using tr::graph::vertex_t;
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

tr::net::peer_id_t peer(std::uint8_t f) {
    tr::net::peer_id_t p{};
    p.fill(static_cast<std::byte>(f));
    return p;
}

enum class alloc_t { HEAP, BORROW };

// One inproc run: E endpoints, F subscribers each, S-byte payload. `by_path`
// writes through the path registry (lookup each publish) instead of the resolved
// vertex_t* hot path — the honest "many topics" measurement.
void run_inproc(std::size_t S, std::size_t F, std::size_t E, alloc_t alloc, bool by_path,
                const char* mode, bool csv = false, std::uint64_t budget = kDeliveryBudget,
                std::uint64_t latbudget = kLatencyDeliveryBudget) {
    graph_t g;
    std::vector<vertex_t*> verts;
    std::vector<path_t> paths;
    verts.reserve(E);
    paths.reserve(E);
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const view_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    for (std::size_t e = 0; e < E; ++e) {
        path_t path = *path_t::parse("/bench/v" + std::to_string(e));
        vertex_t* v = *g.register_vertex(path, role_t::STORED_VALUE);
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
    if (csv)
        emit_csv("libtracer", S, F, E, pub_s, deliv_s, lat.summarize());
    else
        emit("libtracer", mode, S, F, E, pub_s, deliv_s, mb_s, lat.summarize());
}

// Response-surface grid (system dynamics): size x fanout (endpoints=1) and
// size x endpoints (fanout=1, write-by-path). Emits CSV for plot.py.
void run_grid() {
    emit_csv_header();
    for (std::size_t S : kGridSizes)
        for (std::size_t F : kGridFanouts)
            run_inproc(S, F, 1, alloc_t::HEAP, false, "grid", true, kGridBudget, kGridLatBudget);
    for (std::size_t S : kGridSizes)
        for (std::size_t E : kGridEndpoints)
            run_inproc(S, 1, E, alloc_t::HEAP, true, "grid", true, kGridBudget, kGridLatBudget);
}

// Loopback: two nodes + bridge; encode/ROUTER/decode + cross-thread handoff.
void run_loopback(std::size_t S) {
    tr::net::loopback_channel_t ch;
    graph_t a, b;
    tr::net::bridge_t ba(a, ch.a(), peer(0xA1));
    tr::net::bridge_t bb(b, ch.b(), peer(0xB2));
    vertex_t* va = *a.register_vertex(*path_t::parse("/bench/v"), role_t::STORED_VALUE);
    (void)b.register_vertex(*path_t::parse("/bench/remote"), role_t::STORED_VALUE);
    bb.set_mount(*path_t::parse("/bench/remote"));
    bb.set_recent_set_capacity(0);  // disable dedup: measure the data path
    std::atomic<std::uint64_t> recv{0};
    (void)b.subscribe(*path_t::parse("/bench/remote"),
                      [&](const view_t&) { recv.fetch_add(1, std::memory_order_relaxed); });
    (void)ba.export_vertex(*path_t::parse("/bench/v"));
    const std::vector<std::byte> tlv = value_tlv(S);

    const std::size_t MSGS = 100000;
    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) (void)a.write(va, owned_view(tlv));
    while (recv.load(std::memory_order_relaxed) < MSGS) std::this_thread::yield();
    const double secs = (now_ns() - t0) / 1e9;

    Latency lat;
    for (std::size_t i = 0; i < 5000; ++i) {
        const std::uint64_t before = recv.load(std::memory_order_relaxed);
        const auto start = now_ns();
        (void)a.write(va, owned_view(tlv));
        while (recv.load(std::memory_order_relaxed) == before) std::this_thread::yield();
        lat.add(now_ns() - start);
    }
    emit("libtracer", "loopback", S, 1, 1, MSGS / secs, MSGS / secs,
         MSGS * static_cast<double>(S) / secs / 1e6, lat.summarize());
    ch.shutdown();
}

// Mixed workload: 128 topics with varied fan-out (1..16) and payloads (1..8192).
void run_mixed() {
    graph_t g;
    constexpr std::size_t E = 128;
    std::vector<vertex_t*> verts;
    std::vector<std::size_t> fan;
    std::vector<std::vector<std::byte>> tlvs;
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const view_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    std::size_t total_fan = 0;
    for (std::size_t e = 0; e < E; ++e) {
        path_t path = *path_t::parse("/bench/m" + std::to_string(e));
        vertex_t* v = *g.register_vertex(path, role_t::STORED_VALUE);
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
        vertex_t* v = nullptr;
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
        w->v = *w->g.register_vertex(*path_t::parse("/bench/mt"), role_t::STORED_VALUE);
        (void)w->g.subscribe(*path_t::parse("/bench/mt"), [p = w.get()](const view_t&) {
            p->recv.fetch_add(1, std::memory_order_relaxed);
        });
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
            for (std::size_t i = 0; i < 1000; ++i) (void)w->g.write(w->v, w->view);  // warmup
            w->recv.store(0, std::memory_order_relaxed);
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) { /* spin until released */
            }
            for (std::size_t i = 0; i < MSGS; ++i) (void)w->g.write(w->v, w->view);
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
                (void)w->g.write(w->v, w->view);
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

// n-routers (bridge-hop) axis (#96 / ADR-0032). Build an H-hop CHAIN of bridges
// over in-process loopback channels: bridge_0 -> bridge_1 -> ... -> bridge_{H-1}.
// Publish at the head, deliver at the tail; every hop pays the full cross-node
// cost — router_wrap (egress) + router_unwrap + recent-set dedup + ROUTER strip
// (ingress) — plus one cross-"wire" thread handoff. Measures how end-to-end
// delivery LATENCY and aggregate THROUGHPUT degrade as a frame traverses N hops:
// the cross-node fan cost. Dedup stays ENABLED (default recent-set) so every hop
// pays the dedup probe; H stays well under kMaxHops (32).
//
// Topology for H hops (H+1 nodes, H loopback channels, 2H bridges):
//   node[0] --ch[0]--> node[1] --ch[1]--> ... --ch[H-1]--> node[H]
// Each node owns a graph with one /bench/chain vertex. node[k]'s EGRESS bridge
// exports /bench/chain onto ch[k].a(); node[k+1]'s INGRESS bridge mounts ch[k].b()
// back onto its own /bench/chain. So an intermediate node re-wraps on egress what
// it just unwrapped on ingress — a fresh hop per channel. Publishing at node[0]
// reaches node[H]'s subscriber after exactly H hops.
void run_routers(std::size_t H) {
    constexpr std::size_t S = 64;
    const path_t path = *path_t::parse("/bench/chain");

    std::vector<std::unique_ptr<graph_t>> nodes;                      // H+1 graphs
    std::vector<std::unique_ptr<tr::net::loopback_channel_t>> chans;  // H channels
    std::vector<std::unique_ptr<tr::net::bridge_t>> bridges;          // 2H bridges
    nodes.reserve(H + 1);
    chans.reserve(H);
    bridges.reserve(2 * H);
    std::atomic<std::uint64_t> recv{0};

    vertex_t* head = nullptr;
    for (std::size_t i = 0; i <= H; ++i) {
        auto g = std::make_unique<graph_t>();
        vertex_t* v = *g->register_vertex(path, role_t::STORED_VALUE);
        if (i == 0) head = v;  // resolved handle for the hot publish at the head
        nodes.push_back(std::move(g));
    }
    for (std::size_t k = 0; k < H; ++k)
        chans.push_back(std::make_unique<tr::net::loopback_channel_t>());

    // Wire each hop: egress on node[k] -> ch[k] -> ingress on node[k+1].
    for (std::size_t k = 0; k < H; ++k) {
        auto eg = std::make_unique<tr::net::bridge_t>(*nodes[k], chans[k]->a(),
                                                      peer(static_cast<std::uint8_t>(k + 1)));
        auto in = std::make_unique<tr::net::bridge_t>(*nodes[k + 1], chans[k]->b(),
                                                      peer(static_cast<std::uint8_t>(0x80 + k)));
        in->set_mount(path);            // ingress lands the unwrapped TLV at node[k+1]
        (void)eg->export_vertex(path);  // egress re-wraps every write to node[k]'s vertex
        bridges.push_back(std::move(eg));
        bridges.push_back(std::move(in));
    }

    // The tail subscriber counts end-to-end deliveries.
    (void)nodes[H]->subscribe(path,
                              [&](const view_t&) { recv.fetch_add(1, std::memory_order_relaxed); });

    const std::vector<std::byte> tlv = value_tlv(S);
    const auto publish = [&]() { (void)nodes[0]->write(head, owned_view(tlv)); };

    // Warmup: prime each hop's recent-set + thread wakeups, then wait for drain.
    constexpr std::size_t WARM = 1000;
    for (std::size_t i = 0; i < WARM; ++i) publish();
    while (recv.load(std::memory_order_relaxed) < WARM) std::this_thread::yield();

    // Throughput: blast MSGS, wait for the tail to drain the whole H-hop pipeline.
    constexpr std::size_t MSGS = 50000;
    recv.store(0, std::memory_order_relaxed);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) publish();
    while (recv.load(std::memory_order_relaxed) < MSGS) std::this_thread::yield();
    const double secs = (now_ns() - t0) / 1e9;
    const double pub_s = MSGS / secs;
    const double deliv_s = pub_s;  // fan=1: one tail delivery per publish
    const double mb_s = deliv_s * static_cast<double>(S) / 1e6;

    // Latency: one frame at a time, end-to-end across all H hops (empty pipeline
    // between samples, so each sample is the pure H-hop traversal).
    Latency lat;
    for (std::size_t i = 0; i < 5000; ++i) {
        const std::uint64_t before = recv.load(std::memory_order_relaxed);
        const auto start = now_ns();
        publish();
        while (recv.load(std::memory_order_relaxed) == before) std::this_thread::yield();
        lat.add(now_ns() - start);
    }

    const std::string mode = "routers-h" + std::to_string(H);
    emit("libtracer", mode.c_str(), S, 1, H, pub_s, deliv_s, mb_s, lat.summarize());

    for (auto& ch : chans) ch->shutdown();  // join recv threads before teardown (UAF-safe)
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
    vertex_t* v = *g.register_vertex(path, role_t::STREAM, {}, st);
    std::atomic<std::uint64_t> recv{0};
    (void)g.subscribe(path, [&](const view_t&) { recv.fetch_add(1, std::memory_order_relaxed); });

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

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "grid") {
        run_grid();
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
    for (std::size_t S : kSizes) run_loopback(S);
    run_mixed();
    // n-cores (parallel-dispatch) axis: thread counts clamped to the host CPU.
    const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    for (std::size_t T : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}})
        if (T <= hw) run_inproc_mt(T);
    // ep-type (endpoint-dispatch-class) axis: lean / lean-cached / stream.
    run_eptype();
    // n-routers (bridge-hop) axis: end-to-end cost across H bridge/ROUTER hops.
    for (std::size_t H : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}})
        run_routers(H);
    return 0;
}
