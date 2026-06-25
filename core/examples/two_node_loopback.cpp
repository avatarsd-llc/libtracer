// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// Two libtracer nodes talking over the in-process loopback "wire" — the M4 path
// end to end, no sockets. Node A publishes /sensor/temp; its bridge ROUTER-wraps
// the value and sends it across; node B's bridge sheds the envelope (after dedup
// + hop_count checks) and writes the bare TLV to /remote/temp, where B's
// subscriber receives it. The bytes make a full encode -> ROUTER -> decode trip.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>

#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tracer::graph::Path;
using tracer::graph::Role;

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
    tracer::LoopbackChannel channel;
    tracer::graph::Graph node_a;
    tracer::graph::Graph node_b;
    tracer::Bridge bridge_a(node_a, channel.a(), peer_of(0xA1));
    tracer::Bridge bridge_b(node_b, channel.b(), peer_of(0xB2));

    (void)node_a.register_vertex(*Path::parse("/sensor/temp"), Role::StoredValue);
    (void)node_b.register_vertex(*Path::parse("/remote/temp"), Role::StoredValue);
    bridge_b.set_mount(*Path::parse("/remote/temp"));

    std::promise<std::uint32_t> got;
    auto fut = got.get_future();
    (void)node_b.subscribe(*Path::parse("/remote/temp"),
                           [&got](const tracer::View& v) { got.set_value(as_u32(v)); });
    (void)bridge_a.export_vertex(*Path::parse("/sensor/temp"));

    std::printf("node A: write /sensor/temp = 23  (peer A1 -> ROUTER -> wire -> peer B2)\n");
    (void)node_a.write(*Path::parse("/sensor/temp"), value_u32_tlv(23));

    if (fut.wait_for(2s) == std::future_status::ready) {
        std::printf("node B: /remote/temp received %u over the loopback wire\n", fut.get());
    } else {
        std::printf("node B: (timed out)\n");
    }

    channel.shutdown();  // join receive threads before teardown
    return 0;
}
