/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * libtracer network bench (two processes, real UDP). The full stack over the
 * kernel: graph write -> bridge ROUTER-wrap -> udp_transport_t sendto -> [UDP] ->
 * peer udp_transport_t recv -> bridge unwrap/dedup -> graph write -> subscriber.
 *
 *   bench_libtracer_net pub <my_port> <peer_port>
 *   bench_libtracer_net sub <my_port> <peer_port>
 *
 * The hot path crafts NO strings: the path is parsed ONCE to resolve a vertex_t*
 * handle, reused on every send (the bridge's mount is a resolved vertex_t* too).
 */
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_net.hpp"
#include "libtracer/tracer.hpp"

using namespace bench;
using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_t;

namespace {

tr::net::peer_id_t peer_of(std::uint8_t f) {
    tr::net::peer_id_t p{};
    p.fill(static_cast<std::byte>(f));
    return p;
}

void run_pub(std::uint16_t my_port, std::uint16_t peer_port) {
    graph_t g;
    tr::net::udp_transport_t transport(my_port, "127.0.0.1", peer_port);
    tr::net::bridge_t bridge(g, transport, peer_of(0xA1));
    const auto path = path_t::parse("/data");  // parsed ONCE
    (void)g.register_vertex(*path, role_t::STORED_VALUE);
    (void)bridge.export_vertex(*path);
    vertex_t* va = g.find(path->key());  // the handle, resolved once

    std::vector<std::uint8_t> payload;
    const auto send = [&](std::size_t S, net::Phase ph) {
        net::make_payload(payload, S, ph);
        const std::span<const std::byte> pb(reinterpret_cast<const std::byte*>(payload.data()),
                                            payload.size());
        tr::wire::tlv_t t{};
        t.type = tr::wire::type_t::VALUE;
        t.payload = pb;
        const auto bytes = tr::wire::encode(t);
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
        std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
        (void)g.write(va, tr::view::view_t::over(std::move(seg)));  // hot path: handle, no string
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
    graph_t g;
    tr::net::udp_transport_t transport(my_port, "127.0.0.1", peer_port);
    tr::net::bridge_t bridge(g, transport, peer_of(0xB2));
    (void)g.register_vertex(*path_t::parse("/in"), role_t::STORED_VALUE);
    bridge.set_mount(*path_t::parse("/in"));
    bridge.set_recent_set_capacity(0);  // no dedup; measure the data path

    net::SubState state("libtracer");
    std::atomic<std::uint64_t> last_recv{0};
    (void)g.subscribe(*path_t::parse("/in"), [&](const tr::view::view_t& v) {
        const auto tlv = tr::wire::view_as_tlv(v);
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
