/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * M5 UDP transport tests: raw frame delivery over a real localhost UDP socket,
 * and an end-to-end two-node FWD delivery through graph_t + fwd_router_t over UDP
 * (the explicit-source-routed net plane, ADR-0040 — no bridge_t/ROUTER). Built
 * under TSan (the recv thread + receiver handoff) and ASan+UBSan. Uses fixed
 * loopback ports; SO_REUSEADDR is set on the sockets.
 */

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

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

std::vector<std::byte> value_tlv(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> payload;
    for (std::uint8_t b : bytes) payload.push_back(std::byte{b});
    tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    return tr::wire::encode(t);
}

void test_raw_frame() {
    std::printf("UDP transport — raw frame over localhost:\n");
    tr::net::udp_transport_t a(47100, "127.0.0.1", 47101);
    tr::net::udp_transport_t b(47101, "127.0.0.1", 47100);
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

// Build FWD{ op=WRITE, dst=<segs...>, src=<empty PATH>, payload=<VALUE> } — a remote
// write routed by explicit source route (RFC-0004 §D, ADR-0040).
std::vector<std::byte> fwd_write(std::initializer_list<std::string_view> dst,
                                 std::span<const std::byte> payload_value_tlv) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::detail::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                         std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst_segs;
    for (std::string_view s : dst) tr::detail::emit_name(dst_segs, s);
    tr::detail::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{.pl = true}, dst_segs);
    tr::detail::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{.pl = true},
                         std::span<const std::byte>{});  // src: empty, grows per hop
    body.insert(body.end(), payload_value_tlv.begin(), payload_value_tlv.end());
    std::vector<std::byte> frame;
    tr::detail::emit_tlv(frame, tr::wire::type_t::FWD, tr::wire::opt_t{.pl = true}, body);
    return frame;
}

void test_two_nodes_over_udp() {
    std::printf("Two nodes over UDP — FWD delivery through fwd_router_t (ADR-0040):\n");
    // Declaration order matters: the transports are declared AFTER the routers so they
    // destruct FIRST — ~udp_transport_t joins its recv thread, so no inbound frame can
    // reach a router's child_registry_t after the router is gone (ASan use-after-free).
    graph_t node_a, node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    tr::net::udp_transport_t ta(47102, "127.0.0.1", 47103);
    tr::net::udp_transport_t tb(47103, "127.0.0.1", 47102);

    // B holds the target vertex and a subscriber; A knows the link to B as "b".
    (void)node_b.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    router_a.add_child("b", ta);  // A routes a `dst` starting with "b" out over UDP to B
    router_b.add_child("a", tb);  // B's name for the inbound link (src accumulation)

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    (void)node_b.subscribe(*path_t::parse("/sensor/temp"), [&got](const tr::view::view_t& v) {
        const auto b = v.bytes();
        got.set_value(std::vector<std::byte>(b.begin(), b.end()));
    });

    // A client FWD{WRITE dst=/b/sensor/temp} handed to A's router: A strips "b" and
    // forwards /sensor/temp over real UDP to B, whose terminus writes it locally.
    const auto payload = value_tlv({0x2A, 0x2B});
    const auto frame = fwd_write({"b", "sensor", "temp"}, payload);
    router_a.on_frame("client", frame);

    const bool arrived = fut.wait_for(3s) == std::future_status::ready;
    check(arrived, "node B receives the FWD-delivered value over real UDP");
    if (arrived) {
        const auto r = fut.get();
        check(r.size() == payload.size() && std::memcmp(r.data(), payload.data(), r.size()) == 0,
              "delivered TLV bytes match across the wire (explicit source route)");
    }
}

void test_scatter_gather() {
    std::printf("UDP transport — scatter-gather send (rope -> one datagram, no flatten):\n");
    tr::net::udp_transport_t a(47104, "127.0.0.1", 47105);
    tr::net::udp_transport_t b(47105, "127.0.0.1", 47104);
    check(a.ok() && b.ok(), "both UDP sockets bound");

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    b.set_receiver([&](std::span<const std::byte> f) {
        got.set_value(std::vector<std::byte>(f.begin(), f.end()));
    });

    // A 3-segment rope (the "rope we put into tx"), sent via one sendmsg(iovec).
    const std::array<std::byte, 2> s0{std::byte{0x01}, std::byte{0x02}};
    const std::array<std::byte, 3> s1{std::byte{0x03}, std::byte{0x04}, std::byte{0x05}};
    const std::array<std::byte, 1> s2{std::byte{0x06}};
    const std::array<std::span<const std::byte>, 3> iov{std::span<const std::byte>(s0),
                                                        std::span<const std::byte>(s1),
                                                        std::span<const std::byte>(s2)};
    a.send(std::span<const std::span<const std::byte>>(iov));

    const std::array<std::byte, 6> expect{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                          std::byte{0x04}, std::byte{0x05}, std::byte{0x06}};
    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "scatter-gather frame received");
    if (arrived) {
        const auto r = fut.get();
        check(r.size() == 6 && std::memcmp(r.data(), expect.data(), 6) == 0,
              "gathered segments arrive concatenated as one datagram");
    }
}

}  // namespace

int main() {
    test_raw_frame();
    test_two_nodes_over_udp();
    test_scatter_gather();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
