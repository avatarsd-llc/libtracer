/**
 * @file
 * @brief Does the TCP server's single poll thread saturate before the host does?
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_transports` measures ONE connection across two processes — real sockets, real kernel,
 * valid one-way latency. What nothing measures is **concurrency**: how aggregate throughput moves
 * as the number of simultaneous peers rises.
 *
 * That gap matters because of a deliberate design choice whose cost has never been priced.
 * `slot_server_t::run` (`core/src/posix_endpoint.cpp`, the poll loop the tcp
 * and ws servers have shared since #871) is a **single thread** that rebuilds a
 * `pollfd` vector over every live session and calls `::poll` on all of them. Its own comment names
 * the reasoning — *"per-peer thread (the MCU-shaped choice, #362)"* — and for a 16 KB single-core
 * MCU it is plainly right: one thread, one stack, no per-peer scheduling.
 *
 * **The host runs that same choice.** On a 24-core machine every peer's bytes are demultiplexed by
 * one thread doing an O(peers) descriptor scan per wakeup. This project's design center is
 * latency-first across a **dual** target (16 KB MCU *and* 128 GB host), so "the MCU-shaped choice,
 * unmeasured on the host" is exactly the kind of gap worth an instrument.
 *
 * @section what What this measures
 *
 * One `transport_tcp_server`, N concurrent dialers each on its own thread sending fixed-size
 * frames as fast as they are accepted, for a fixed window. Reports **aggregate frames/s** and
 * **per-peer frames/s** as N sweeps.
 *
 * The shape of the answer is the point:
 *
 *  - If aggregate throughput **rises with N**, the server is not the bottleneck and a
 *    multi-threaded reactor would buy little.
 *  - If aggregate throughput **plateaus** while per-peer throughput falls as 1/N, the single poll
 *    thread is saturated and the plateau is the ceiling any multi-threaded io_context (Asio or
 *    otherwise) would have to beat. That plateau is the number, and there is no point discussing
 *    a rewrite without it.
 *
 * @section honest What this is NOT
 *
 * Loopback, one host, one process — so it prices the **server's demultiplex**, not a NIC, not a
 * scheduler under real load, and not two machines. Sender threads and the server thread contend
 * for the same 24 cores, so at high N the senders themselves become a term; the per-peer column is
 * there to make that visible rather than to hide it.
 *
 * It reports; it does not gate. There is no pass/fail threshold, because the right threshold is
 * exactly what is unknown.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/transport_tcp.hpp"

namespace {

using namespace std::chrono_literals;

/** @brief Peer counts swept. Ends at 32 on a 24-core host so the last point is oversubscribed. */
constexpr std::size_t kPeerCounts[] = {1, 2, 4, 8, 16, 32};

/** @brief Payload bytes per frame — small, so the measurement is per-FRAME demultiplex cost. */
constexpr std::size_t kFrameBytes = 64;

/** @brief Seconds each sweep point runs. Override with LIBTRACER_BENCH_SECONDS. */
[[nodiscard]] double window_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return 1.0;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : 1.0;
}

/**
 * @brief Counts inbound frames on the server, without serialising the poll thread.
 *
 * A `relaxed` atomic increment and nothing else: a mutex here would measure the sink rather than
 * the server, and copying the payload would measure the allocator. The receiver runs ON the poll
 * thread, so anything it does is charged directly to the term under test.
 */
struct counting_sink_t {
    std::atomic<std::uint64_t> frames{0};

    /** @brief The peer-named receiver callable (bound by address). */
    void operator()(std::string_view, std::span<const std::byte>) {
        frames.fetch_add(1, std::memory_order_relaxed);
    }
};

/** @brief One sweep point. */
struct point_t {
    std::size_t peers = 0;
    std::uint64_t frames = 0;
    double seconds = 0.0;
    std::uint64_t sent = 0;
};

/** @brief Run one sweep point: @p peers dialers hammering one server for the window. */
[[nodiscard]] point_t run_point(std::size_t peers, double seconds) {
    counting_sink_t sink;
    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/0,
                                         /*peer_named=*/true);
    if (!server.ok() || server.bus() == nullptr) return point_t{peers, 0, 0.0, 0};
    server.bus()->set_peer_receiver(sink);
    const std::uint16_t port = server.local_port();

    // Dialers are constructed BEFORE the timed window and given time to complete their
    // handshakes, so the measurement prices steady-state demultiplex and not N connects.
    std::vector<std::unique_ptr<tr::net::tcp_transport_t>> peers_v;
    peers_v.reserve(peers);
    for (std::size_t i = 0; i < peers; ++i)
        peers_v.push_back(std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", port));
    std::this_thread::sleep_for(300ms);

    const std::vector<std::byte> payload(kFrameBytes, std::byte{0x5A});
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> sent{0};

    std::vector<std::thread> senders;
    senders.reserve(peers);
    for (std::size_t i = 0; i < peers; ++i) {
        senders.emplace_back([&, i] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            std::uint64_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                peers_v[i]->send(std::span<const std::byte>(payload));
                ++n;
            }
            sent.fetch_add(n, std::memory_order_relaxed);
        });
    }

    // Zero the counter at the LAST moment before the window: the handshake settle above may have
    // carried stray frames, and attributing those to the window would inflate every point.
    sink.frames.store(0, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true, std::memory_order_relaxed);
    const auto t1 = std::chrono::steady_clock::now();

    for (std::thread& t : senders) t.join();

    // Read the counter AFTER joining but WITHOUT draining: frames still in flight are simply not
    // counted. That biases the result DOWN, which is the safe direction for a ceiling claim.
    point_t p;
    p.peers = peers;
    p.frames = sink.frames.load(std::memory_order_relaxed);
    p.seconds = std::chrono::duration<double>(t1 - t0).count();
    p.sent = sent.load(std::memory_order_relaxed);
    return p;
}

}  // namespace

int main() {
    const double seconds = window_seconds();
    std::printf(
        "TCP server fan-in — one transport_tcp_server, N concurrent dialers, %zu B frames\n"
        "server model: ONE poll thread, O(peers) pollfd scan per wakeup (transport_tcp.cpp run())\n"
        "hardware threads: %u\n\n",
        kFrameBytes, std::thread::hardware_concurrency());

    std::vector<point_t> points;
    for (const std::size_t n : kPeerCounts) points.push_back(run_point(n, seconds));

    std::printf("%-8s %-16s %-16s %-14s %s\n", "peers", "aggregate f/s", "per-peer f/s",
                "delivered", "sent");
    double best = 0.0;
    for (const point_t& p : points) {
        const double agg = p.seconds > 0.0 ? static_cast<double>(p.frames) / p.seconds : 0.0;
        best = agg > best ? agg : best;
        std::printf("%-8zu %-16.0f %-16.0f %-14llu %llu\n", p.peers, agg,
                    p.peers ? agg / static_cast<double>(p.peers) : 0.0,
                    static_cast<unsigned long long>(p.frames),
                    static_cast<unsigned long long>(p.sent));
        std::printf("RESULT\tlibtracer\ttcp-fanin-%zu\t%zu\t1\t%zu\t%.0f\t%.0f\t0.0\t0\t0\t0\n",
                    p.peers, kFrameBytes, p.peers, agg, agg);
    }

    const double one =
        points.empty()
            ? 0.0
            : (points[0].seconds > 0.0 ? static_cast<double>(points[0].frames) / points[0].seconds
                                       : 0.0);

    // The verdict is computed, not left to the reader: doubling the peer count at the top of the
    // sweep either buys throughput or it does not, and that single ratio is the whole finding.
    double last = 0.0;
    double prev = 0.0;
    if (points.size() >= 2) {
        const point_t& a = points[points.size() - 2];
        const point_t& b = points[points.size() - 1];
        prev = a.seconds > 0.0 ? static_cast<double>(a.frames) / a.seconds : 0.0;
        last = b.seconds > 0.0 ? static_cast<double>(b.frames) / b.seconds : 0.0;
    }
    const double top_gain = prev > 0.0 ? (last / prev - 1.0) * 100.0 : 0.0;

    std::printf(
        "\nSUMMARY peak aggregate %.0f frames/s; a single peer reaches %.0f (%.1fx across the "
        "sweep).\n",
        best, one, one > 0.0 ? best / one : 0.0);
    std::printf("        Doubling %zu -> %zu peers changed aggregate throughput by %+.1f%%.\n",
                kPeerCounts[std::size(kPeerCounts) - 2], kPeerCounts[std::size(kPeerCounts) - 1],
                top_gain);
    if (top_gain < 5.0) {
        std::printf(
            "        VERDICT SATURATED. The single poll thread is the limit, and %.0f frames/s\n"
            "        is the ceiling a multi-threaded io_context would have to beat. Per-peer\n"
            "        throughput falling while aggregate holds flat is the signature.\n",
            best);
    } else {
        std::printf(
            "        VERDICT NOT SATURATED at this sweep width — aggregate is still rising, so\n"
            "        the poll thread is not yet the limit. Widen the sweep before concluding.\n");
    }
    std::printf(
        "\n        CAVEATS, because the plateau is the load-bearing claim:\n"
        "        - Loopback, one process: this prices the SERVER's demultiplex, not a NIC.\n"
        "        - %u hardware threads against %zu sender threads plus the poll thread at the\n"
        "          top point, so senders are oversubscribed there and contribute their own term.\n"
        "        - `delivered` is read WITHOUT draining, so in-flight frames go uncounted. That\n"
        "          biases delivered DOWN, which is the safe direction for a ceiling claim but\n"
        "          means the delivered-vs-sent gap is NOT clean evidence on its own. The\n"
        "          plateau is the evidence; the gap is only suggestive.\n",
        std::thread::hardware_concurrency(), kPeerCounts[std::size(kPeerCounts) - 1]);
    return 0;
}
