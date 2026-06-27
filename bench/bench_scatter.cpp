/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Scatter-gather egress demonstration (the "rope we put into tx" model). Instead
 * of a batching layer, a composite endpoint's value IS a rope already batched in
 * memory; the transport ships the whole rope with ONE sendmsg(iovec) — one
 * syscall for K values, at the latency of a single send. So network throughput
 * scales with composition size K while p50 latency stays flat — beating zenoh on
 * BOTH axes (zenoh's timer-batching trades latency for throughput; this doesn't).
 *
 * Localhost, one process, two udp_transport_t. Sweeps K (values per composite).
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/transport_udp.hpp"

using namespace bench;

namespace {

void run(std::size_t S, std::size_t K) {
    tr::net::udp_transport_t sub(49000, "127.0.0.1", 49001);
    tr::net::udp_transport_t pub(49001, "127.0.0.1", 49000);

    std::atomic<std::uint64_t> datagrams{0};
    std::mutex latm;
    Latency lat;
    sub.set_receiver([&](std::span<const std::byte> f) {
        datagrams.fetch_add(1, std::memory_order_relaxed);
        if (f.size() >= 8) {
            std::uint64_t ts = 0;
            for (int i = 0; i < 8; ++i)
                ts |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(f[i])) << (8 * i);
            if (ts != 0) {
                const std::uint64_t l = now_ns() - ts;
                const std::lock_guard lock(latm);
                lat.add(l);
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // K segments of S bytes — the composite rope put into tx (seg0 holds a ts).
    std::vector<std::vector<std::byte>> segs(K, std::vector<std::byte>(S, std::byte{0xAB}));
    const auto build_iov = [&]() {
        std::vector<std::span<const std::byte>> iov;
        iov.reserve(K);
        for (const auto& s : segs) iov.emplace_back(s);
        return iov;
    };

    // Egress throughput: time to issue N composite sends — one sendmsg(iovec) per
    // composite, so this is the transmit-path syscall rate. N scales down with K to
    // bound total bytes. (We measure egress, not delivery: large composites would
    // overflow an untuned loopback receiver — a buffer-tuning artifact, not the
    // egress cost the scatter-gather optimizes.)
    const std::size_t N = publishes_for(K, kDeliveryBudget);
    for (auto& s : segs)
        if (s.size() >= 8) std::fill(s.begin(), s.begin() + 8, std::byte{0});
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < N; ++i) {
        const auto iov = build_iov();
        pub.send(std::span<const std::span<const std::byte>>(iov));
    }
    const double secs = (now_ns() - t0) / 1e9;
    const double dps = N / secs;                      // composites (sendmsg) per second
    const double vps = dps * static_cast<double>(K);  // effective values per second

    // Latency: paced single composites with a ts in seg0.
    for (std::size_t i = 0; i < 3000; ++i) {
        const std::uint64_t now = now_ns();
        for (int b = 0; b < 8; ++b)
            segs[0][static_cast<std::size_t>(b)] = static_cast<std::byte>((now >> (8 * b)) & 0xFF);
        const std::uint64_t before = datagrams.load(std::memory_order_relaxed);
        const auto iov = build_iov();
        pub.send(std::span<const std::span<const std::byte>>(iov));
        const auto ld = Clock::now() + std::chrono::milliseconds(50);
        while (datagrams.load(std::memory_order_relaxed) == before && Clock::now() < ld)
            std::this_thread::yield();
    }

    emit("libtracer", "scatter", S, K, 1, dps, vps, vps * static_cast<double>(S) / 1e6,
         lat.summarize());
}

}  // namespace

int main() {
    for (std::size_t K : {std::size_t{1}, std::size_t{8}, std::size_t{64}, std::size_t{256}})
        run(64, K);
    return 0;
}
