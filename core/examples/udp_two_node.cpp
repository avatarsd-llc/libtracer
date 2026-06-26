// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// Two libtracer nodes talking over a real localhost UDP socket — the M5 path,
// the same program as two_node_loopback.cpp with the in-process loopback_channel_t
// swapped for udp_transport_t (the "one-line swap" the getting-started guide promises;
// everything above the transport_t seam — bridge, router, graph — is unchanged).
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
using tr::graph::path_t;
using tr::graph::role_t;

constexpr std::uint16_t kPortA = 47200;
constexpr std::uint16_t kPortB = 47201;

tr::view::view_t value_u32_tlv(std::uint32_t v) {
    std::array<std::byte, 4> payload{};
    for (int i = 0; i < 4; ++i)
        payload[static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    tr::tlv_t t{.type = tr::type_t::VALUE, .payload = payload};
    const auto bytes = tr::encode(t);
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

std::uint32_t as_u32(const tr::view::view_t& v) {
    const auto tlv = tr::view::view_as_tlv(v);
    std::uint32_t r = 0;
    if (tlv) {
        const auto p = tlv->payload;
        for (std::size_t i = 0; i < p.size() && i < 4; ++i)
            r |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
    }
    return r;
}

tr::peer_id_t peer_of(std::uint8_t fill) {
    tr::peer_id_t p{};
    p.fill(static_cast<std::byte>(fill));
    return p;
}

}  // namespace

int main() {
    // The only change from two_node_loopback.cpp: a real UDP socket per node.
    tr::udp_transport_t udp_a(kPortA, "127.0.0.1", kPortB);
    tr::udp_transport_t udp_b(kPortB, "127.0.0.1", kPortA);
    if (!udp_a.ok() || !udp_b.ok()) {
        std::printf("could not bind localhost UDP ports %u/%u\n", kPortA, kPortB);
        return 0;  // a busy port is an environment issue, not a node failure
    }

    tr::graph::graph_t node_a;
    tr::graph::graph_t node_b;
    tr::bridge_t bridge_a(node_a, udp_a, peer_of(0xA1));
    tr::bridge_t bridge_b(node_b, udp_b, peer_of(0xB2));

    (void)node_a.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    (void)node_b.register_vertex(*path_t::parse("/remote/temp"), role_t::STORED_VALUE);
    bridge_b.set_mount(*path_t::parse("/remote/temp"));

    std::promise<std::uint32_t> got;
    auto fut = got.get_future();
    (void)node_b.subscribe(*path_t::parse("/remote/temp"),
                           [&got](const tr::view::view_t& v) { got.set_value(as_u32(v)); });
    (void)bridge_a.export_vertex(*path_t::parse("/sensor/temp"));

    std::this_thread::sleep_for(50ms);  // let both receive threads settle on their sockets

    std::printf("node A: write /sensor/temp = 23  (peer A1 -> ROUTER -> UDP :%u -> peer B2)\n",
                kPortB);
    (void)node_a.write(*path_t::parse("/sensor/temp"), value_u32_tlv(23));

    if (fut.wait_for(3s) == std::future_status::ready) {
        std::printf("node B: /remote/temp received %u over real localhost UDP\n", fut.get());
        return 0;
    }
    std::printf("node B: (timed out)\n");
    return 1;
}
