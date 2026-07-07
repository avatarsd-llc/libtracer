// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// libtracer NETWORK transport bench — TWO PROCESSES over a real loopback socket, one
// transport each (UDP / TCP / WebSocket). A separate publisher and subscriber process
// cross the kernel, exactly like the Zenoh two-process bench (bench_zenoh_net) — same
// topology, same bench_net.hpp phase protocol (payload self-identifies its phase and
// carries a CLOCK_MONOTONIC send-timestamp, so one-way latency is valid across the two
// processes on one host), same RESULT line (`net-<proto>`). run_net.sh orchestrates the
// pub/sub pair per protocol for both engines, so render_compare.py plots them on shared
// absolute axes.
//
//   bench_transports sub <udp|tcp|ws> <port>
//   bench_transports pub <udp|tcp|ws> <port>
//
// (Successor to the retired two-process bench_libtracer_net — ADR-0040 moved the net
// plane to explicit-source-routed FWD; this measures the transport seam itself.)
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "bench_net.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"

using namespace std::chrono_literals;
using tr::net::transport_t;
using namespace bench;

namespace {

// The subscriber binds/listens on `port`; the publisher's own UDP bind is port+1.
std::unique_ptr<transport_t> make_endpoint(std::string_view proto, bool sub, std::uint16_t port) {
    if (proto == "udp")
        return sub ? std::make_unique<tr::net::udp_transport_t>(port, "127.0.0.1", port + 1)
                   : std::make_unique<tr::net::udp_transport_t>(port + 1, "127.0.0.1", port);
    if (proto == "tcp")
        return sub ? std::make_unique<tr::net::tcp_transport_t>(port)
                   : std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", port);
    if (proto == "ws")
        return sub ? std::unique_ptr<transport_t>(new tr::net::transport_ws_server(port))
                   : std::unique_ptr<transport_t>(
                         new tr::net::transport_ws_client("127.0.0.1", port));
    return nullptr;
}

void run_sub(std::string_view proto, std::uint16_t port) {
    auto ep = make_endpoint(proto, true, port);
    if (!ep) return;
    net::SubState state("libtracer", "net-" + std::string(proto));
    std::atomic<std::uint64_t> last_recv{0};
    ep->set_receiver([&](std::span<const std::byte> f) {
        state.on_payload(f);
        last_recv.store(now_ns(), std::memory_order_relaxed);
    });
    // Wait for the first frame, then finish once the stream goes quiet (the publisher
    // has sent its EOF markers per size and stopped).
    while (last_recv.load(std::memory_order_relaxed) == 0) std::this_thread::sleep_for(10ms);
    for (;;) {
        std::this_thread::sleep_for(200ms);
        if (now_ns() - last_recv.load(std::memory_order_relaxed) > 3'000'000'000ULL) break;
    }
}

void run_pub(std::string_view proto, std::uint16_t port) {
    auto ep = make_endpoint(proto, false, port);
    if (!ep) return;
    std::this_thread::sleep_for(400ms);  // let the connection/link establish

    std::vector<std::uint8_t> payload;
    const auto send = [&](std::size_t S, net::Phase ph) {
        net::make_payload(payload, S, ph);
        ep->send(std::as_bytes(std::span<const std::uint8_t>(payload)));
    };
    for (std::size_t S : net::kSizes) {
        for (std::size_t i = 0; i < net::kLatencyMsgs; ++i) {
            send(S, net::kLatency);
            const auto until = Clock::now() + std::chrono::nanoseconds(net::kPaceNs);
            while (Clock::now() < until) { /* pace so latency sends don't queue */
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
