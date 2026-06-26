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
using tr::graph::path_t;
using tr::graph::role_t;

tr::view::view_t value_u32_tlv(std::uint32_t v) {
    std::array<std::byte, 4> payload{};
    for (int i = 0; i < 4; ++i)
        payload[static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    const auto bytes = tr::wire::encode(t);
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

std::uint32_t as_u32(const tr::view::view_t& v) {
    const auto tlv = tr::wire::view_as_tlv(v);
    std::uint32_t r = 0;
    if (tlv) {
        const auto p = tlv->payload;
        for (std::size_t i = 0; i < p.size() && i < 4; ++i)
            r |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
    }
    return r;
}

tr::net::peer_id_t peer_of(std::uint8_t fill) {
    tr::net::peer_id_t p{};
    p.fill(static_cast<std::byte>(fill));
    return p;
}

}  // namespace

int main() {
    tr::net::loopback_channel_t channel;
    tr::graph::graph_t node_a;
    tr::graph::graph_t node_b;
    tr::net::bridge_t bridge_a(node_a, channel.a(), peer_of(0xA1));
    tr::net::bridge_t bridge_b(node_b, channel.b(), peer_of(0xB2));

    (void)node_a.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    (void)node_b.register_vertex(*path_t::parse("/remote/temp"), role_t::STORED_VALUE);
    bridge_b.set_mount(*path_t::parse("/remote/temp"));

    std::promise<std::uint32_t> got;
    auto fut = got.get_future();
    (void)node_b.subscribe(*path_t::parse("/remote/temp"),
                           [&got](const tr::view::view_t& v) { got.set_value(as_u32(v)); });
    (void)bridge_a.export_vertex(*path_t::parse("/sensor/temp"));

    std::printf("node A: write /sensor/temp = 23  (peer A1 -> ROUTER -> wire -> peer B2)\n");
    (void)node_a.write(*path_t::parse("/sensor/temp"), value_u32_tlv(23));

    if (fut.wait_for(2s) == std::future_status::ready) {
        std::printf("node B: /remote/temp received %u over the loopback wire\n", fut.get());
    } else {
        std::printf("node B: (timed out)\n");
    }

    channel.shutdown();  // join receive threads before teardown
    return 0;
}
