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
using tracer::graph::Graph;
using tracer::graph::Path;
using tracer::graph::Role;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

tracer::PeerId peer_of(std::uint8_t fill) {
    tracer::PeerId p{};
    p.fill(static_cast<std::byte>(fill));
    return p;
}

// A VALUE TLV (01 00 <len> <payload>) as raw bytes.
std::vector<std::byte> value_tlv(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> payload;
    for (std::uint8_t b : bytes) payload.push_back(std::byte{b});
    tracer::Tlv t{.type = tracer::Type::Value, .payload = payload};
    return tracer::encode(t);
}

tracer::View owned_view(std::span<const std::byte> bytes) {
    tracer::SegmentPtr seg = tracer::mem::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tracer::View::over(std::move(seg));
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
    const tracer::RouterMeta meta{.origin = peer_of(0x42), .ts = 0x0102030405060708ull, .hop = 3};
    const auto frame = tracer::router_wrap(data, meta);

    // It decodes as a structured ROUTER TLV.
    const auto decoded = tracer::decode(frame);
    check(decoded.has_value() && decoded->type == tracer::Type::Router,
          "wrapped frame decodes to a ROUTER TLV");
    check(decoded.has_value() && decoded->opt.pl, "ROUTER is structured (PL=1)");

    // unwrap round-trips the metadata and recovers the exact data TLV bytes.
    const auto un = tracer::router_unwrap(frame);
    check(un.has_value(), "router_unwrap succeeds");
    check(un.has_value() && un->meta == meta, "metadata (origin, ts, hop) round-trips");
    check(un.has_value() && un->data.size() == data.size() &&
              std::memcmp(un->data.data(), data.data(), data.size()) == 0,
          "wrapped data TLV bytes recovered verbatim");

    // A non-ROUTER frame is rejected.
    check(!tracer::router_unwrap(data).has_value(), "a bare VALUE is not a ROUTER (rejected)");
}

void test_two_node_delivery() {
    std::printf("Two-node delivery over the loopback wire:\n");
    tracer::LoopbackChannel channel;
    Graph node_a;
    Graph node_b;
    tracer::Bridge bridge_a(node_a, channel.a(), peer_of(0xA1));
    tracer::Bridge bridge_b(node_b, channel.b(), peer_of(0xB2));

    (void)node_a.register_vertex(*Path::parse("/sensor/temp"), Role::StoredValue);
    (void)node_b.register_vertex(*Path::parse("/remote/temp"), Role::StoredValue);
    bridge_b.set_mount(*Path::parse("/remote/temp"));

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    (void)node_b.subscribe(*Path::parse("/remote/temp"), [&got](const tracer::View& v) {
        const auto b = v.bytes();
        got.set_value(std::vector<std::byte>(b.begin(), b.end()));
    });

    check(bridge_a.export_vertex(*Path::parse("/sensor/temp")).has_value(),
          "node A exports /sensor/temp");

    const auto payload = value_tlv({0x2A});
    (void)node_a.write(*Path::parse("/sensor/temp"), owned_view(payload));

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
    tracer::LoopbackChannel channel;
    Graph node_a;
    Graph node_b;
    tracer::Bridge bridge_a(node_a, channel.a(), peer_of(0xA1));
    tracer::Bridge bridge_b(node_b, channel.b(), peer_of(0xB2));
    (void)node_b.register_vertex(*Path::parse("/remote/temp"), Role::StoredValue);
    bridge_b.set_mount(*Path::parse("/remote/temp"));

    // Inject the identical ROUTER frame twice (same origin+ts => same dedup key).
    const auto frame =
        tracer::router_wrap(value_tlv({0x07}), {.origin = peer_of(0xCC), .ts = 999, .hop = 0});
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
    tracer::LoopbackChannel channel;
    Graph node_a;
    Graph node_b;
    tracer::Bridge bridge_a(node_a, channel.a(), peer_of(0xA1));
    tracer::Bridge bridge_b(node_b, channel.b(), peer_of(0xB2));

    // Both re-forward and both have dedup OFF: only hop_count can stop the loop.
    bridge_a.set_recent_set_capacity(0);
    bridge_b.set_recent_set_capacity(0);
    bridge_a.set_reforward(true);
    bridge_b.set_reforward(true);

    // Inject one frame at hop 0; it bounces A<->B incrementing hop until MAX_HOPS.
    const auto frame =
        tracer::router_wrap(value_tlv({0x01}), {.origin = peer_of(0xEE), .ts = 7, .hop = 0});
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
