// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Zenoh side of the NETWORK transport comparison: two zenoh sessions in one process,
// linked over a chosen protocol (tcp / udp / ws) on the loopback path — one listens,
// the other connects; publish on the connecting session, subscribe on the listening
// one, so each sample crosses the real kernel link (the fair analogue to libtracer's
// bench_transports). Same measurement (throughput + one-way p50/p99 latency per payload
// size) and the same mode-tagged RESULT line (`net-<proto>`), so render_compare.py plots
// the two engines on shared per-transport axes.
//
//   bench_zenoh_transports            # sweeps tcp, udp, ws
//   bench_zenoh_transports tcp udp    # only the named protocols
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "bench_net.hpp"
#include "zenoh.hxx"

using namespace zenoh;
using namespace bench;
using namespace std::chrono_literals;

namespace {

Config link_config(std::string_view proto, bool listen, std::uint16_t port) {
    Config c = Config::create_default();
    const std::string ep =
        "[\"" + std::string(proto) + "/127.0.0.1:" + std::to_string(port) + "\"]";
    c.insert_json5(listen ? "listen/endpoints" : "connect/endpoints", ep);
    c.insert_json5("mode", "\"peer\"");
    c.insert_json5("scouting/multicast/enabled", "false");
    return c;
}

// Returns false (with a stderr note) if the link never comes up — e.g. a protocol the
// prebuilt zenoh-c wasn't compiled with. The comparison then just omits that transport.
bool run_proto(std::string_view proto, std::uint16_t port) {
    Session sub_session = Session::open(link_config(proto, true, port));
    Session pub_session = Session::open(link_config(proto, false, port));

    std::atomic<std::uint64_t> recv{0};
    auto sub = sub_session.declare_subscriber(
        KeyExpr("bench/net"), [&](const Sample&) { recv.fetch_add(1, std::memory_order_relaxed); },
        closures::none);
    auto pub = pub_session.declare_publisher(KeyExpr("bench/net"));
    std::this_thread::sleep_for(800ms);  // let the link + routing establish

    for (std::size_t S : net::kSizes) {
        std::vector<std::uint8_t> payload(S, 0xAB);
        for (int i = 0; i < 200; ++i) pub.put(Bytes(payload));  // warmup
        std::this_thread::sleep_for(50ms);

        const std::size_t MSGS = S >= 4096 ? 20000 : 100000;
        recv.store(0);
        const auto t0 = now_ns();
        for (std::size_t i = 0; i < MSGS; ++i) pub.put(Bytes(payload));
        const auto deadline = Clock::now() + 5s;
        while (recv.load(std::memory_order_relaxed) < MSGS * 9 / 10 && Clock::now() < deadline)
            std::this_thread::yield();
        const std::uint64_t got = recv.load(std::memory_order_relaxed);
        const double secs = (now_ns() - t0) / 1e9;
        const double rate = secs > 0 ? got / secs : 0;
        if (got == 0) {
            std::fprintf(stderr, "[zenoh-%.*s] no delivery (protocol unsupported?) — skipped\n",
                         static_cast<int>(proto.size()), proto.data());
            return false;
        }

        // Drain in-flight throughput leftovers so each latency probe is clean.
        for (std::uint64_t prev = recv.load();;) {
            std::this_thread::sleep_for(30ms);
            const std::uint64_t cur = recv.load();
            if (cur == prev) break;
            prev = cur;
        }
        Latency lat;
        for (int i = 0; i < 3000; ++i) {
            const std::uint64_t before = recv.load(std::memory_order_relaxed);
            const auto sent = now_ns();
            pub.put(Bytes(payload));
            const auto ld = Clock::now() + 200ms;
            while (recv.load(std::memory_order_relaxed) == before && Clock::now() < ld)
                std::this_thread::yield();
            if (recv.load(std::memory_order_relaxed) > before) lat.add(now_ns() - sent);
        }
        const std::string mode = "net-" + std::string(proto);
        emit("zenoh", mode.c_str(), S, 1, 1, rate, rate, rate * static_cast<double>(S) / 1e6,
             lat.summarize());
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    init_log_from_env_or("error");
    std::vector<std::string> protos;
    for (int i = 1; i < argc; ++i) protos.emplace_back(argv[i]);
    if (protos.empty()) protos = {"tcp", "udp", "ws"};
    std::uint16_t port = 48330;
    for (const std::string& p : protos) run_proto(p, port++);
    return 0;
}
