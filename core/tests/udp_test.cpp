// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// M5 UDP transport tests: raw frame delivery over a real localhost UDP socket,
// and an end-to-end two-node exchange through the full Graph + Bridge + ROUTER
// stack over UDP. Built under TSan (the recv thread + receiver handoff) and
// ASan+UBSan. Uses fixed loopback ports; SO_REUSEADDR is set on the sockets.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tracer::graph::Graph;
using tracer::graph::Path;
using tracer::graph::Role;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

tracer::PeerId peer_of(std::uint8_t f) {
    tracer::PeerId p{};
    p.fill(static_cast<std::byte>(f));
    return p;
}
std::vector<std::byte> value_tlv(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> payload;
    for (std::uint8_t b : bytes) payload.push_back(std::byte{b});
    tracer::Tlv t{.type = tracer::Type::Value, .payload = payload};
    return tracer::encode(t);
}
tracer::View owned_view(std::span<const std::byte> bytes) {
    tracer::SegmentPtr seg = tracer::mem::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tracer::View::over(std::move(seg));
}

void test_raw_frame() {
    std::printf("UDP transport — raw frame over localhost:\n");
    tracer::UdpTransport a(47100, "127.0.0.1", 47101);
    tracer::UdpTransport b(47101, "127.0.0.1", 47100);
    check(a.ok() && b.ok(), "both UDP sockets bound");

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    b.set_receiver([&](std::span<const std::byte> f) {
        got.set_value(std::vector<std::byte>(f.begin(), f.end()));
    });

    const std::array<std::byte, 5> frame{std::byte{0x09}, std::byte{0xAB}, std::byte{0xCD},
                                         std::byte{0xEF}, std::byte{0x42}};
    a.send(frame);

    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "frame received on the peer socket");
    if (arrived) {
        const auto r = fut.get();
        check(r.size() == frame.size() && std::memcmp(r.data(), frame.data(), frame.size()) == 0,
              "received bytes are identical");
    }
}

void test_two_nodes_over_udp() {
    std::printf("Two nodes over UDP — full Graph+Bridge+ROUTER stack:\n");
    Graph node_a, node_b;
    tracer::UdpTransport ta(47102, "127.0.0.1", 47103);
    tracer::UdpTransport tb(47103, "127.0.0.1", 47102);
    tracer::Bridge ba(node_a, ta, peer_of(0xA1));
    tracer::Bridge bb(node_b, tb, peer_of(0xB2));

    (void)node_a.register_vertex(*Path::parse("/sensor/temp"), Role::StoredValue);
    (void)node_b.register_vertex(*Path::parse("/remote/temp"), Role::StoredValue);
    bb.set_mount(*Path::parse("/remote/temp"));

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    (void)node_b.subscribe(*Path::parse("/remote/temp"), [&got](const tracer::View& v) {
        const auto b = v.bytes();
        got.set_value(std::vector<std::byte>(b.begin(), b.end()));
    });
    check(ba.export_vertex(*Path::parse("/sensor/temp")).has_value(), "node A exports over UDP");

    const auto payload = value_tlv({0x2A, 0x2B});
    auto* va = node_a.find(Path::parse("/sensor/temp")->key());
    (void)node_a.write(va, owned_view(payload));

    const bool arrived = fut.wait_for(3s) == std::future_status::ready;
    check(arrived, "node B receives the value over real UDP");
    if (arrived) {
        const auto r = fut.get();
        check(r.size() == payload.size() && std::memcmp(r.data(), payload.data(), r.size()) == 0,
              "delivered TLV bytes match across the wire");
    }
}

}  // namespace

int main() {
    test_raw_frame();
    test_two_nodes_over_udp();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
