/**
 * @file
 * @brief libtracer's arm of the composition-throughput comparison — TWO PROCESSES over real
 *        loopback UDP, one `sendmsg(iovec)` per K-value group, deliveries counted by the
 *        SUBSCRIBER.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The publisher builds the K records of one group and the iovec over them ONCE, then the
 * timed loop is nothing but `transport_t::send(iov)` — which `udp_transport_t` lowers to a
 * single `sendmsg` with a K-entry iovec. That is the property under test: one syscall, one
 * datagram, K values. See bench_compose.hpp for the record framing, the claim, and the list of
 * what this harness does NOT measure.
 *
 *   bench_compose_net sub <port> <K> <value_bytes> <window_ms>
 *   bench_compose_net pub <port> <K> <value_bytes> <groups> <latency_groups>
 *
 * The subscriber exits non-zero, and emits no `RESULT` row at all, when it observed no values
 * or any malformed record. run_compose.sh treats that as a failed point.
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "bench_compose.hpp"
#include "libtracer/transport_udp.hpp"

using namespace std::chrono_literals;
using bench::compose::phase_t;

namespace {

/** @brief The delivery-COUNTING subscriber process. */
int run_sub(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
    const auto width = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
    const auto value_bytes = static_cast<std::size_t>(std::strtoul(argv[4], nullptr, 10));
    const auto window_ms = static_cast<std::uint64_t>(std::strtoull(argv[5], nullptr, 10));
    const bool audit = argc > 6 && std::string_view(argv[6]) == "audit";

    // The counter and the receiver lambda outlive the transport: the slot binds the callable
    // by address, and ~udp_transport_t joins the recv thread.
    bench::compose::sub_counter_t counter("libtracer", "compose-udp", width, value_bytes);
    std::atomic<std::uint64_t> last_recv{0};
    auto rx = [&](std::span<const std::byte> d) {
        (void)counter.on_message(d);
        last_recv.store(bench::now_ns(), std::memory_order_relaxed);
    };
    tr::net::udp_transport_t t(port, "127.0.0.1", static_cast<std::uint16_t>(port + 1));
    if (!t.ok()) {
        std::fprintf(stderr, "compose sub: bind failed on %u\n", port);
        return 1;
    }
    t.set_receiver(rx);

    std::printf("SUB_READY\n");
    std::fflush(stdout);

    const auto t0 = bench::Clock::now();
    const auto deadline = t0 + std::chrono::milliseconds(window_ms);
    while (!counter.done() && bench::Clock::now() < deadline) std::this_thread::sleep_for(2ms);
    // Let a straggling datagram land before finalizing; the publisher repeats its
    // END_OF_POINT marker, so a lost one costs this pause and not the point.
    std::this_thread::sleep_for(50ms);
    return audit ? counter.finish_audit() : counter.finish();
}

/** @brief The publisher process. Its rate is NOT the measurement; only its send count is. */
int run_pub(int argc, char** argv) {
    (void)argc;
    const auto port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
    const auto width = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
    const auto value_bytes = static_cast<std::size_t>(std::strtoul(argv[4], nullptr, 10));
    const auto groups = static_cast<std::uint64_t>(std::strtoull(argv[5], nullptr, 10));
    const auto lat_groups = static_cast<std::uint64_t>(std::strtoull(argv[6], nullptr, 10));

    tr::net::udp_transport_t t(static_cast<std::uint16_t>(port + 1), "127.0.0.1", port);
    if (!t.ok()) {
        std::fprintf(stderr, "compose pub: bind failed on %u\n", port + 1);
        return 1;
    }
    std::this_thread::sleep_for(200ms);  // let the subscriber's recv thread settle

    // BUILT ONCE, OUTSIDE EVERY TIMED LOOP. The composite endpoint's value is supposed to be a
    // rope that already exists; charging its construction to the send is what made the
    // withdrawn bench understate libtracer by 33-58%.
    std::vector<std::vector<std::byte>> lat_recs =
        bench::compose::make_group(width, value_bytes, phase_t::LATENCY);
    std::vector<std::vector<std::byte>> thru_recs =
        bench::compose::make_group(width, value_bytes, phase_t::THROUGHPUT);
    const auto gather = [](const std::vector<std::vector<std::byte>>& recs) {
        std::vector<std::span<const std::byte>> iov;
        iov.reserve(recs.size());
        for (const std::vector<std::byte>& r : recs) iov.emplace_back(r);
        return iov;
    };
    const std::vector<std::span<const std::byte>> lat_iov = gather(lat_recs);
    const std::vector<std::span<const std::byte>> thru_iov = gather(thru_recs);

    // Paced groups: one 8-byte timestamp store per group, then one sendmsg.
    for (std::uint64_t i = 0; i < lat_groups; ++i) {
        bench::compose::stamp(lat_recs[0], bench::now_ns());
        t.send(std::span<const std::span<const std::byte>>(lat_iov));
        const auto until = bench::Clock::now() + std::chrono::nanoseconds(bench::compose::kPaceNs);
        while (bench::Clock::now() < until) {
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(bench::compose::kDrainMs));

    // The blast. Nothing in this loop but the send.
    const std::uint64_t a = bench::now_ns();
    for (std::uint64_t i = 0; i < groups; ++i) {
        t.send(std::span<const std::span<const std::byte>>(thru_iov));
        // A pure blast overruns the loopback receive buffer and starts measuring the kernel's
        // drop policy. One yield every 64 groups keeps the receiver's thread scheduled; it is
        // identical on both engines' arms.
        if ((i & 63) == 0) std::this_thread::yield();
    }
    const std::uint64_t b = bench::now_ns();

    // Repeat the marker: it is a datagram like any other and may be dropped.
    std::vector<std::vector<std::byte>> eop =
        bench::compose::make_group(width, value_bytes, phase_t::END_OF_POINT);
    std::this_thread::sleep_for(200ms);
    for (int i = 0; i < 8; ++i) {
        t.send(std::span<const std::byte>(eop[0]));
        std::this_thread::sleep_for(20ms);
    }

    const double span = static_cast<double>(b - a) / 1e9;
    std::printf("PUB_SENT\tlibtracer\t%zu\t%llu\t%llu\t%.0f\n", width,
                static_cast<unsigned long long>(groups),
                static_cast<unsigned long long>(groups * width),
                span > 0 ? static_cast<double>(groups) / span : 0.0);
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 6 && std::string_view(argv[1]) == "sub") return run_sub(argc, argv);
    if (argc >= 7 && std::string_view(argv[1]) == "pub") return run_pub(argc, argv);
    std::fprintf(stderr,
                 "usage: bench_compose_net sub <port> <K> <value_bytes> <window_ms>\n"
                 "       bench_compose_net pub <port> <K> <value_bytes> <groups> "
                 "<latency_groups>\n");
    return 2;
}
