// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// M4 transport + bridge tests: ROUTER wrap/unwrap (golden), two-node delivery
// over the in-process loopback, recent-set dedup, and hop_count cycle
// termination (ADR-0014). The loopback runs receive threads, so this is built
// under TSan (cross-thread frame handoff + recent-set) and ASan+UBSan.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

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

tr::peer_id_t peer_of(std::uint8_t fill) {
    tr::peer_id_t p{};
    p.fill(static_cast<std::byte>(fill));
    return p;
}

// A VALUE TLV (01 00 <len> <payload>) as raw bytes.
std::vector<std::byte> value_tlv(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> payload;
    for (std::uint8_t b : bytes) payload.push_back(std::byte{b});
    tr::tlv_t t{.type = tr::type_t::VALUE, .payload = payload};
    return tr::encode(t);
}

tr::view::view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

// Spin until `cond` or the deadline; returns whether it became true.
template <class Fn>
bool wait_until(Fn cond, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return cond();
}

void test_router_golden() {
    std::printf("ROUTER wrap/unwrap (golden):\n");
    const auto data = value_tlv({0xAB, 0xCD});
    const tr::router_meta_t meta{.origin = peer_of(0x42), .ts = 0x0102030405060708ull, .hop = 3};
    const auto frame = tr::router_wrap(data, meta);

    // It decodes as a structured ROUTER TLV.
    const auto decoded = tr::decode(frame);
    check(decoded.has_value() && decoded->type == tr::type_t::ROUTER,
          "wrapped frame decodes to a ROUTER TLV");
    check(decoded.has_value() && decoded->opt.pl, "ROUTER is structured (PL=1)");

    // unwrap round-trips the metadata and recovers the exact data TLV bytes.
    const auto un = tr::router_unwrap(frame);
    check(un.has_value(), "router_unwrap succeeds");
    check(un.has_value() && un->meta == meta, "metadata (origin, ts, hop) round-trips");
    check(un.has_value() && un->data.size() == data.size() &&
              std::memcmp(un->data.data(), data.data(), data.size()) == 0,
          "wrapped data TLV bytes recovered verbatim");

    // A non-ROUTER frame is rejected.
    check(!tr::router_unwrap(data).has_value(), "a bare VALUE is not a ROUTER (rejected)");
}

void test_two_node_delivery() {
    std::printf("Two-node delivery over the loopback wire:\n");
    tr::loopback_channel_t channel;
    graph_t node_a;
    graph_t node_b;
    tr::bridge_t bridge_a(node_a, channel.a(), peer_of(0xA1));
    tr::bridge_t bridge_b(node_b, channel.b(), peer_of(0xB2));

    (void)node_a.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    (void)node_b.register_vertex(*path_t::parse("/remote/temp"), role_t::STORED_VALUE);
    bridge_b.set_mount(*path_t::parse("/remote/temp"));

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    (void)node_b.subscribe(*path_t::parse("/remote/temp"), [&got](const tr::view::view_t& v) {
        const auto b = v.bytes();
        got.set_value(std::vector<std::byte>(b.begin(), b.end()));
    });

    check(bridge_a.export_vertex(*path_t::parse("/sensor/temp")).has_value(),
          "node A exports /sensor/temp");

    const auto payload = value_tlv({0x2A});
    (void)node_a.write(*path_t::parse("/sensor/temp"), owned_view(payload));

    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "node B's subscriber receives the bridged write");
    if (arrived) {
        const auto received = fut.get();
        check(received.size() == payload.size() &&
                  std::memcmp(received.data(), payload.data(), payload.size()) == 0,
              "delivered bytes equal the published data TLV (wire round-trip)");
    }
    check(bridge_b.delivered() == 1, "exactly one delivery");
    channel.shutdown();
}

void test_dedup() {
    std::printf("Recent-set dedup (same frame twice => one delivery):\n");
    tr::loopback_channel_t channel;
    graph_t node_a;
    graph_t node_b;
    tr::bridge_t bridge_a(node_a, channel.a(), peer_of(0xA1));
    tr::bridge_t bridge_b(node_b, channel.b(), peer_of(0xB2));
    (void)node_b.register_vertex(*path_t::parse("/remote/temp"), role_t::STORED_VALUE);
    bridge_b.set_mount(*path_t::parse("/remote/temp"));

    // Inject the identical ROUTER frame twice (same origin+ts => same dedup key).
    const auto frame =
        tr::router_wrap(value_tlv({0x07}), {.origin = peer_of(0xCC), .ts = 999, .hop = 0});
    channel.a().send(frame);
    channel.a().send(frame);

    const bool done =
        wait_until([&] { return bridge_b.delivered() + bridge_b.deduped() == 2; }, 2s);
    check(done, "both frames processed");
    check(bridge_b.delivered() == 1, "first frame delivered");
    check(bridge_b.deduped() == 1, "duplicate frame dropped by the recent-set");
    channel.shutdown();
}

void test_cycle_termination() {
    std::printf("Cycle termination by hop_count (recent-set disabled):\n");
    tr::loopback_channel_t channel;
    graph_t node_a;
    graph_t node_b;
    tr::bridge_t bridge_a(node_a, channel.a(), peer_of(0xA1));
    tr::bridge_t bridge_b(node_b, channel.b(), peer_of(0xB2));

    // Both re-forward and both have dedup OFF: only hop_count can stop the loop.
    bridge_a.set_recent_set_capacity(0);
    bridge_b.set_recent_set_capacity(0);
    bridge_a.set_reforward(true);
    bridge_b.set_reforward(true);

    // Inject one frame at hop 0; it bounces A<->B incrementing hop until MAX_HOPS.
    const auto frame =
        tr::router_wrap(value_tlv({0x01}), {.origin = peer_of(0xEE), .ts = 7, .hop = 0});
    channel.a().send(frame);  // -> node B

    const bool terminated =
        wait_until([&] { return bridge_a.hop_dropped() + bridge_b.hop_dropped() >= 1; }, 2s);
    check(terminated, "the cycle terminates (a frame hits MAX_HOPS and is dropped)");
    std::this_thread::sleep_for(20ms);  // let any tail settle
    channel.shutdown();
    const std::uint64_t drops = bridge_a.hop_dropped() + bridge_b.hop_dropped();
    check(drops == 1, "exactly one terminal drop (bounded, not an infinite loop)");
}

}  // namespace

int main() {
    test_router_golden();
    test_two_node_delivery();
    test_dedup();
    test_cycle_termination();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
