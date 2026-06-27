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
        if (by_path) (void)g.write(paths[i % E], mk());
        else (void)g.write(verts[i % E], mk());
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
    if (csv) emit_csv("libtracer", S, F, E, pub_s, deliv_s, lat.summarize());
    else emit("libtracer", mode, S, F, E, pub_s, deliv_s, mb_s, lat.summarize());
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
    return 0;
}
