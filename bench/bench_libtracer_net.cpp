// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// libtracer network bench (two processes, real UDP). The full stack over the
// kernel: graph write -> bridge ROUTER-wrap -> UdpTransport sendto -> [UDP] ->
// peer UdpTransport recv -> bridge unwrap/dedup -> graph write -> subscriber.
//
//   bench_libtracer_net pub <my_port> <peer_port>
//   bench_libtracer_net sub <my_port> <peer_port>
//
// Note the hot path crafts NO strings: the path is parsed ONCE to resolve a
// Vertex* handle, which is reused on every send (and the bridge's mount is a
// resolved Vertex* too) — "encode the path once, reuse the handle".

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_net.hpp"
#include "libtracer/tracer.hpp"

using namespace bench;
using namespace std::chrono_literals;
using tracer::graph::Graph;
using tracer::graph::Path;
using tracer::graph::Role;

namespace {

tracer::PeerId peer_of(std::uint8_t f) {
    tracer::PeerId p{};
    p.fill(static_cast<std::byte>(f));
    return p;
}

void run_pub(std::uint16_t my_port, std::uint16_t peer_port) {
    Graph g;
    tracer::UdpTransport transport(my_port, "127.0.0.1", peer_port);
    tracer::Bridge bridge(g, transport, peer_of(0xA1));
    const auto path = Path::parse("/data");  // parsed ONCE
    (void)g.register_vertex(*path, Role::StoredValue);
    (void)bridge.export_vertex(*path);
    tracer::graph::Vertex* va = g.find(path->key());  // the handle, resolved once

    std::vector<std::uint8_t> payload;
    const auto send = [&](std::size_t S, net::Phase ph) {
        net::make_payload(payload, S, ph);
        const std::span<const std::byte> pb(reinterpret_cast<const std::byte*>(payload.data()),
                                            payload.size());
        tracer::Tlv t{.type = tracer::Type::Value, .payload = pb};
        const auto bytes = tracer::encode(t);
        tracer::SegmentPtr seg = tracer::mem::heap_alloc(bytes.size());
        std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
        (void)g.write(va, tracer::View::over(std::move(seg)));  // hot path: handle, no string
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the sub bind
    for (std::size_t S : net::kSizes) {
        for (std::size_t i = 0; i < net::kLatencyMsgs; ++i) {
            send(S, net::kLatency);
            const auto until = Clock::now() + std::chrono::nanoseconds(net::kPaceNs);
            while (Clock::now() < until) {
            }
        }
        for (std::size_t i = 0; i < net::kThroughputMsgs; ++i) send(S, net::kThroughput);
        for (int i = 0; i < 8; ++i) send(16, net::kEof);  // redundant EOF (UDP may drop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void run_sub(std::uint16_t my_port, std::uint16_t peer_port) {
    Graph g;
    tracer::UdpTransport transport(my_port, "127.0.0.1", peer_port);
    tracer::Bridge bridge(g, transport, peer_of(0xB2));
    (void)g.register_vertex(*Path::parse("/in"), Role::StoredValue);
    bridge.set_mount(*Path::parse("/in"));
    bridge.set_recent_set_capacity(0);  // no dedup; measure the data path

    net::SubState state("libtracer");
    std::atomic<std::uint64_t> last_recv{0};
    (void)g.subscribe(*Path::parse("/in"), [&](const tracer::View& v) {
        const auto tlv = tracer::view_as_tlv(v);
        if (tlv) state.on_payload(tlv->payload);
        last_recv.store(now_ns(), std::memory_order_relaxed);
    });

    // Run until idle (no frame for 3 s after the first arrives).
    while (last_recv.load(std::memory_order_relaxed) == 0) std::this_thread::sleep_for(10ms);
    for (;;) {
        std::this_thread::sleep_for(200ms);
        if (now_ns() - last_recv.load(std::memory_order_relaxed) > 3'000'000'000ULL) break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <pub|sub> <my_port> <peer_port>\n", argv[0]);
        return 2;
    }
    const std::string_view role = argv[1];
    const auto my_port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
    const auto peer_port = static_cast<std::uint16_t>(std::strtoul(argv[3], nullptr, 10));
    if (role == "pub")
        run_pub(my_port, peer_port);
    else if (role == "sub")
        run_sub(my_port, peer_port);
    else
        return 2;
    return 0;
}
