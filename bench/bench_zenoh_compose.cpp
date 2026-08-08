/**
 * @file
 * @brief The Zenoh arm of the composition-throughput comparison — TWO PROCESSES over real
 *        loopback UDP, K puts per K values, deliveries counted by the SUBSCRIBER.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Zenoh has no composite send, so the same K values that libtracer ships in one datagram are
 * K separate `put`s here. That is the comparison, and it is only fair if this side is a REAL
 * publisher talking to a REAL subscriber in another process: the subscriber listens on a
 * configured UDP endpoint, the publisher connects to it, and multicast scouting is disabled so
 * the pair talks over that socket and nothing else. The withdrawn comparison's Zenoh side had
 * no peer at all and never reached the wire — run_compose.sh's syscall audit exists so that
 * cannot recur silently.
 *
 * Same record framing, same subscriber-side counter and same RESULT rows as the libtracer arm
 * (bench_compose.hpp), so "a value" means the same thing on both sides.
 *
 *   bench_zenoh_compose sub <port> <K> <value_bytes> <window_ms>
 *   bench_zenoh_compose pub <port> <K> <value_bytes> <groups> <latency_groups>
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
#include "zenoh.hxx"

using namespace std::chrono_literals;
using bench::compose::phase_t;
using zenoh::Bytes;
using zenoh::Config;
using zenoh::KeyExpr;
using zenoh::Sample;
using zenoh::Session;

namespace {

/** @brief Peer-mode config pinned to ONE loopback UDP endpoint, scouting off. */
Config make_config(bool listen, std::uint16_t port) {
    Config c = Config::create_default();
    const std::string ep = "[\"udp/127.0.0.1:" + std::to_string(port) + "\"]";
    c.insert_json5(listen ? "listen/endpoints" : "connect/endpoints", ep);
    c.insert_json5("mode", "\"peer\"");
    c.insert_json5("scouting/multicast/enabled", "false");
    return c;
}

/** @brief The delivery-COUNTING subscriber process. */
int run_sub(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
    const auto width = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
    const auto value_bytes = static_cast<std::size_t>(std::strtoul(argv[4], nullptr, 10));
    const auto window_ms = static_cast<std::uint64_t>(std::strtoull(argv[5], nullptr, 10));
    const bool audit = argc > 6 && std::string_view(argv[6]) == "audit";

    bench::compose::sub_counter_t counter("zenoh", "compose-udp", width, value_bytes);
    Session session = Session::open(make_config(true, port));
    auto sub = session.declare_subscriber(
        KeyExpr("bench/compose"),
        [&](const Sample& s) {
            const std::vector<std::uint8_t> v = s.get_payload().as_vector();
            (void)counter.on_message(
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.data()), v.size()));
        },
        zenoh::closures::none);

    std::printf("SUB_READY\n");
    std::fflush(stdout);

    const auto deadline = bench::Clock::now() + std::chrono::milliseconds(window_ms);
    while (!counter.done() && bench::Clock::now() < deadline) std::this_thread::sleep_for(2ms);
    std::this_thread::sleep_for(50ms);
    return audit ? counter.finish_audit() : counter.finish();
}

/** @brief The publisher process — K puts per group, because there is no composite send. */
int run_pub(int argc, char** argv) {
    (void)argc;
    const auto port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
    const auto width = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
    const auto value_bytes = static_cast<std::size_t>(std::strtoul(argv[4], nullptr, 10));
    const auto groups = static_cast<std::uint64_t>(std::strtoull(argv[5], nullptr, 10));
    const auto lat_groups = static_cast<std::uint64_t>(std::strtoull(argv[6], nullptr, 10));

    Session session = Session::open(make_config(false, port));
    auto pub = session.declare_publisher(KeyExpr("bench/compose"));
    std::this_thread::sleep_for(700ms);  // let the UDP session establish

    // Built once, exactly as on the libtracer arm — the records and their `uint8_t` staging
    // buffers both, so neither timed loop allocates.
    const auto stage = [](const std::vector<std::vector<std::byte>>& recs) {
        std::vector<std::vector<std::uint8_t>> out;
        out.reserve(recs.size());
        for (const std::vector<std::byte>& r : recs)
            out.emplace_back(reinterpret_cast<const std::uint8_t*>(r.data()),
                             reinterpret_cast<const std::uint8_t*>(r.data()) + r.size());
        return out;
    };
    std::vector<std::vector<std::uint8_t>> lat_u8 =
        stage(bench::compose::make_group(width, value_bytes, phase_t::LATENCY));
    const std::vector<std::vector<std::uint8_t>> thru_u8 =
        stage(bench::compose::make_group(width, value_bytes, phase_t::THROUGHPUT));
    const std::vector<std::vector<std::uint8_t>> eop_u8 =
        stage(bench::compose::make_group(width, value_bytes, phase_t::END_OF_POINT));

    for (std::uint64_t i = 0; i < lat_groups; ++i) {
        bench::compose::put_le(std::as_writable_bytes(std::span<std::uint8_t>(lat_u8[0])),
                               bench::compose::kOffTs, bench::now_ns(), 8);
        for (std::size_t j = 0; j < width; ++j) pub.put(Bytes(lat_u8[j]));
        const auto until = bench::Clock::now() + std::chrono::nanoseconds(bench::compose::kPaceNs);
        while (bench::Clock::now() < until) {
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(bench::compose::kDrainMs));

    const std::uint64_t a = bench::now_ns();
    for (std::uint64_t i = 0; i < groups; ++i) {
        for (std::size_t j = 0; j < width; ++j) pub.put(Bytes(thru_u8[j]));
        if ((i & 63) == 0) std::this_thread::yield();
    }
    const std::uint64_t b = bench::now_ns();

    std::this_thread::sleep_for(200ms);
    for (int i = 0; i < 8; ++i) {
        pub.put(Bytes(eop_u8[0]));
        std::this_thread::sleep_for(20ms);
    }

    const double span = static_cast<double>(b - a) / 1e9;
    std::printf("PUB_SENT\tzenoh\t%zu\t%llu\t%llu\t%.0f\n", width,
                static_cast<unsigned long long>(groups),
                static_cast<unsigned long long>(groups * width),
                span > 0 ? static_cast<double>(groups) / span : 0.0);
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    zenoh::init_log_from_env_or("error");
    if (argc >= 6 && std::string_view(argv[1]) == "sub") return run_sub(argc, argv);
    if (argc >= 7 && std::string_view(argv[1]) == "pub") return run_pub(argc, argv);
    std::fprintf(stderr,
                 "usage: bench_zenoh_compose sub <port> <K> <value_bytes> <window_ms>\n"
                 "       bench_zenoh_compose pub <port> <K> <value_bytes> <groups> "
                 "<latency_groups>\n");
    return 2;
}
