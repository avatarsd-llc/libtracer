// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Zenoh network bench (two processes, real UDP). The subscriber listens on a UDP
// endpoint; the publisher connects to it; multicast scouting is disabled so they
// talk only over the configured socket. Same phase protocol / payload as
// bench_libtracer_net.cpp (see bench_net.hpp), same RESULT format.
//
//   bench_zenoh_net pub <sub_listen_port>
//   bench_zenoh_net sub <sub_listen_port>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_net.hpp"
#include "zenoh.hxx"

using namespace zenoh;
using namespace bench;
using namespace std::chrono_literals;

namespace {

Config make_config(std::string_view proto, bool listen, std::uint16_t port) {
    Config c = Config::create_default();
    const std::string ep =
        "[\"" + std::string(proto) + "/127.0.0.1:" + std::to_string(port) + "\"]";
    c.insert_json5(listen ? "listen/endpoints" : "connect/endpoints", ep);
    c.insert_json5("mode", "\"peer\"");
    c.insert_json5("scouting/multicast/enabled", "false");
    return c;
}

void run_sub(std::string_view proto, std::uint16_t port) {
    auto session = Session::open(make_config(proto, true, port));
    net::SubState state("zenoh", "net-" + std::string(proto));
    std::atomic<std::uint64_t> last_recv{0};
    auto sub = session.declare_subscriber(
        KeyExpr("bench/net"),
        [&](const Sample& s) {
            const auto v = s.get_payload().as_vector();
            state.on_payload(
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.data()), v.size()));
            last_recv.store(now_ns(), std::memory_order_relaxed);
        },
        closures::none);

    while (last_recv.load(std::memory_order_relaxed) == 0) std::this_thread::sleep_for(10ms);
    for (;;) {
        std::this_thread::sleep_for(200ms);
        if (now_ns() - last_recv.load(std::memory_order_relaxed) > 3'000'000'000ULL) break;
    }
}

void run_pub(std::string_view proto, std::uint16_t port) {
    auto session = Session::open(make_config(proto, false, port));
    auto pub = session.declare_publisher(KeyExpr("bench/net"));
    std::this_thread::sleep_for(700ms);  // let the UDP session establish

    std::vector<std::uint8_t> payload;
    const auto send = [&](std::size_t S, net::Phase ph) {
        net::make_payload(payload, S, ph);
        pub.put(Bytes(payload));
    };
    for (std::size_t S : net::kSizes) {
        for (std::size_t i = 0; i < net::kLatencyMsgs; ++i) {
            send(S, net::kLatency);
            const auto until = Clock::now() + std::chrono::nanoseconds(net::kPaceNs);
            while (Clock::now() < until) {
            }
        }
        for (std::size_t i = 0; i < net::kThroughputMsgs; ++i) send(S, net::kThroughput);
        for (int i = 0; i < 8; ++i) send(16, net::kEof);
        std::this_thread::sleep_for(std::chrono::milliseconds(net::kDrainMs));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <pub|sub> <udp|tcp|ws> <port>\n", argv[0]);
        return 2;
    }
    init_log_from_env_or("error");
    const std::string_view role = argv[1];
    const std::string_view proto = argv[2];
    const auto port = static_cast<std::uint16_t>(std::strtoul(argv[3], nullptr, 10));
    if (role == "pub")
        run_pub(proto, port);
    else if (role == "sub")
        run_sub(proto, port);
    else
        return 2;
    return 0;
}
