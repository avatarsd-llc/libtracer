// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// Two libtracer nodes talking over a real localhost UDP socket — the M5 path,
// the same program as two_node_loopback.cpp with the in-process LoopbackChannel
// swapped for UdpTransport (the "one-line swap" the getting-started guide promises;
// everything above the Transport seam — bridge, router, graph — is unchanged).
// Node A publishes /sensor/temp; the bytes ROUTER-wrap, cross the kernel UDP stack,
// and land on node B's /remote/temp, where B's subscriber receives them.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>

#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tracer::graph::Path;
using tracer::graph::Role;

constexpr std::uint16_t kPortA = 47200;
constexpr std::uint16_t kPortB = 47201;

tracer::View value_u32_tlv(std::uint32_t v) {
    std::array<std::byte, 4> payload{};
    for (int i = 0; i < 4; ++i)
        payload[static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    tracer::Tlv t{.type = tracer::Type::Value, .payload = payload};
    const auto bytes = tracer::encode(t);
    tracer::SegmentPtr seg = tracer::mem::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tracer::View::over(std::move(seg));
}

std::uint32_t as_u32(const tracer::View& v) {
    const auto tlv = tracer::view_as_tlv(v);
    std::uint32_t r = 0;
    if (tlv) {
        const auto p = tlv->payload;
        for (std::size_t i = 0; i < p.size() && i < 4; ++i)
            r |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
    }
    return r;
}

tracer::PeerId peer_of(std::uint8_t fill) {
    tracer::PeerId p{};
    p.fill(static_cast<std::byte>(fill));
    return p;
}

}  // namespace

int main() {
    // The only change from two_node_loopback.cpp: a real UDP socket per node.
    tracer::UdpTransport udp_a(kPortA, "127.0.0.1", kPortB);
    tracer::UdpTransport udp_b(kPortB, "127.0.0.1", kPortA);
    if (!udp_a.ok() || !udp_b.ok()) {
        std::printf("could not bind localhost UDP ports %u/%u\n", kPortA, kPortB);
        return 0;  // a busy port is an environment issue, not a node failure
    }

    tracer::graph::Graph node_a;
    tracer::graph::Graph node_b;
    tracer::Bridge bridge_a(node_a, udp_a, peer_of(0xA1));
    tracer::Bridge bridge_b(node_b, udp_b, peer_of(0xB2));

    (void)node_a.register_vertex(*Path::parse("/sensor/temp"), Role::StoredValue);
    (void)node_b.register_vertex(*Path::parse("/remote/temp"), Role::StoredValue);
    bridge_b.set_mount(*Path::parse("/remote/temp"));

    std::promise<std::uint32_t> got;
    auto fut = got.get_future();
    (void)node_b.subscribe(*Path::parse("/remote/temp"),
                           [&got](const tracer::View& v) { got.set_value(as_u32(v)); });
    (void)bridge_a.export_vertex(*Path::parse("/sensor/temp"));

    std::this_thread::sleep_for(50ms);  // let both receive threads settle on their sockets

    std::printf("node A: write /sensor/temp = 23  (peer A1 -> ROUTER -> UDP :%u -> peer B2)\n",
                kPortB);
    (void)node_a.write(*Path::parse("/sensor/temp"), value_u32_tlv(23));

    if (fut.wait_for(3s) == std::future_status::ready) {
        std::printf("node B: /remote/temp received %u over real localhost UDP\n", fut.get());
        return 0;
    }
    std::printf("node B: (timed out)\n");
    return 1;
}
