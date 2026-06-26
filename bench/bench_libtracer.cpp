// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// libtracer side of the libtracer-vs-Zenoh comparison. Two in-process paths:
//   inproc   — graph pub/sub (M3): publisher write -> subscriber callback. The
//              zero-copy path (a refcount-bump View handoff; synchronous).
//   loopback — the M4 bridge: write -> ROUTER-wrap -> in-memory channel (cross
//              thread) -> unwrap/dedup -> deliver. Exercises encode/ROUTER/decode.
// Neither crosses a socket — see bench/README.md for the (important) caveats. The
// network path is M5; this harness will gain a "socket" mode when it lands.

#include <atomic>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/tracer.hpp"

using namespace bench;
using tracer::graph::Graph;
using tracer::graph::Path;
using tracer::graph::Role;

namespace {

// A VALUE TLV carrying a `payload` of S bytes (so the loopback path exercises the
// real encode/decode), as a fresh owned heap View per message.
std::vector<std::byte> value_tlv(std::size_t payload) {
    std::vector<std::byte> p(payload, std::byte{0xAB});
    tracer::Tlv t{.type = tracer::Type::Value, .payload = p};
    return tracer::encode(t);
}

tracer::View owned_view(std::span<const std::byte> bytes) {
    tracer::SegmentPtr seg = tracer::mem::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tracer::View::over(std::move(seg));
}

tracer::PeerId peer(std::uint8_t f) {
    tracer::PeerId p{};
    p.fill(static_cast<std::byte>(f));
    return p;
}

// --- inproc: synchronous graph pub/sub (the callback fires inside write) -------
void run_inproc(std::size_t S) {
    Graph g;
    auto* v = *g.register_vertex(*Path::parse("/bench/v"), Role::StoredValue);
    std::atomic<std::uint64_t> recv{0};
    (void)g.subscribe(*Path::parse("/bench/v"),
                      [&](const tracer::View&) { recv.fetch_add(1, std::memory_order_relaxed); });
    const auto tlv = value_tlv(S);

    const auto t0 = now_ns();
    for (std::size_t i = 0; i < kThroughputMsgs; ++i) (void)g.write(v, owned_view(tlv));
    const double secs = (now_ns() - t0) / 1e9;

    Latency lat;
    for (std::size_t i = 0; i < kLatencyMsgs; ++i) {
        const auto a = now_ns();
        (void)g.write(v, owned_view(tlv));  // subscriber callback runs inline
        lat.add(now_ns() - a);
    }
    emit("libtracer", "inproc", S, kThroughputMsgs / secs,
         kThroughputMsgs * static_cast<double>(S) / secs / 1e6, lat.summarize());
}

// --- loopback: two nodes + bridge; encode/ROUTER/decode + cross-thread handoff -
void run_loopback(std::size_t S) {
    tracer::LoopbackChannel ch;
    Graph a, b;
    tracer::Bridge ba(a, ch.a(), peer(0xA1));
    tracer::Bridge bb(b, ch.b(), peer(0xB2));
    auto* va = *a.register_vertex(*Path::parse("/bench/v"), Role::StoredValue);
    (void)b.register_vertex(*Path::parse("/bench/remote"), Role::StoredValue);
    bb.set_mount(*Path::parse("/bench/remote"));
    bb.set_recent_set_capacity(0);  // disable dedup: measure the data path, no ts-collision drops
    std::atomic<std::uint64_t> recv{0};
    (void)b.subscribe(*Path::parse("/bench/remote"),
                      [&](const tracer::View&) { recv.fetch_add(1, std::memory_order_relaxed); });
    (void)ba.export_vertex(*Path::parse("/bench/v"));
    const auto tlv = value_tlv(S);

    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < kThroughputMsgs; ++i) (void)a.write(va, owned_view(tlv));
    while (recv.load(std::memory_order_relaxed) < kThroughputMsgs) std::this_thread::yield();
    const double secs = (now_ns() - t0) / 1e9;

    Latency lat;
    for (std::size_t i = 0; i < kLatencyMsgs; ++i) {
        const std::uint64_t before = recv.load(std::memory_order_relaxed);
        const auto start = now_ns();
        (void)a.write(va, owned_view(tlv));
        while (recv.load(std::memory_order_relaxed) == before) std::this_thread::yield();
        lat.add(now_ns() - start);
    }
    emit("libtracer", "loopback", S, kThroughputMsgs / secs,
         kThroughputMsgs * static_cast<double>(S) / secs / 1e6, lat.summarize());
    ch.shutdown();
}

}  // namespace

int main() {
    for (std::size_t S : kSizes) run_inproc(S);
    for (std::size_t S : kSizes) run_loopback(S);
    return 0;
}
