// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// libtracer NETWORK transport comparison: send framed bytes a()->b() over each real
// point-to-point transport (UDP / TCP / WebSocket) across the loopback kernel path,
// measuring throughput and one-way latency per payload size. Two endpoints in one
// process (each its own socket) — the frames still cross the kernel, but send and
// receive share one steady clock so one-way latency (recv_ns - send_ns) is exact and
// there is no cross-process coordination. Emits the same mode-tagged RESULT line as
// the in-process bench (mode = `net-udp` / `net-tcp` / `net-ws`), so render_compare.py
// plots it against the Zenoh transport numbers (bench_zenoh_transports) on shared axes.
//
// (This is the successor to the retired two-process bench_libtracer_net — ADR-0040 moved
// the net plane to explicit-source-routed FWD; here we measure the transport seam itself,
// the fair analogue to Zenoh's per-link session.)
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
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

// Distinct base ports per transport so repeated runs on one host don't collide.
struct udp_pair {
    tr::net::udp_transport_t a_{48300, "127.0.0.1", 48301};
    tr::net::udp_transport_t b_{48301, "127.0.0.1", 48300};
    [[nodiscard]] bool ok() const { return a_.ok() && b_.ok(); }
    [[nodiscard]] transport_t& a() { return a_; }
    [[nodiscard]] transport_t& b() { return b_; }
    static constexpr const char* mode = "net-udp";
    static constexpr bool lossy = true;  // UDP is best-effort; drops shrink the count
};

struct tcp_pair {
    tr::net::tcp_transport_t listener_{48311};             // binds first
    tr::net::tcp_transport_t dialer_{"127.0.0.1", 48311};  // connects in the ctor
    [[nodiscard]] bool ok() const { return listener_.ok() && dialer_.ok(); }
    [[nodiscard]] transport_t& a() { return dialer_; }
    [[nodiscard]] transport_t& b() { return listener_; }
    static constexpr const char* mode = "net-tcp";
    static constexpr bool lossy = false;
};

struct ws_pair {
    tr::net::transport_ws_server server_{48321};
    tr::net::transport_ws_client client_{"127.0.0.1", 48321};  // handshake in the ctor
    [[nodiscard]] bool ok() const { return server_.ok() && client_.ok(); }
    [[nodiscard]] transport_t& a() { return client_; }
    [[nodiscard]] transport_t& b() { return server_; }
    static constexpr const char* mode = "net-ws";
    static constexpr bool lossy = false;
};

// Throughput + one-way p50/p99 latency for one transport at one payload size.
template <class Pair>
void run_size(Pair& pair, std::size_t S) {
    std::atomic<std::uint64_t> recv{0};
    pair.b().set_receiver(
        [&](std::span<const std::byte>) { recv.fetch_add(1, std::memory_order_relaxed); });

    std::vector<std::byte> payload(S, std::byte{0xAB});
    for (int i = 0; i < 200; ++i) pair.a().send(payload);  // warmup + let TCP/WS settle
    std::this_thread::sleep_for(50ms);

    // --- throughput: blast, then count what arrived over the elapsed window ---
    const std::size_t MSGS = S >= 4096 ? 20000 : 100000;
    recv.store(0);
    const auto t0 = now_ns();
    for (std::size_t i = 0; i < MSGS; ++i) pair.a().send(payload);
    const auto deadline = Clock::now() + 5s;
    const std::uint64_t want = pair.lossy ? MSGS * 9 / 10 : MSGS;  // tolerate UDP drops
    while (recv.load(std::memory_order_relaxed) < want && Clock::now() < deadline)
        std::this_thread::yield();
    const std::uint64_t got = recv.load(std::memory_order_relaxed);
    const double secs = (now_ns() - t0) / 1e9;
    const double rate = secs > 0 ? got / secs : 0;

    // Drain in-flight throughput leftovers so each latency probe measures one clean
    // send->receive, not a straggler from the blast above.
    for (std::uint64_t prev = recv.load();;) {
        std::this_thread::sleep_for(30ms);
        const std::uint64_t cur = recv.load();
        if (cur == prev) break;
        prev = cur;
    }
    // --- one-way latency: send one frame, spin until b's callback observes it ---
    Latency lat;
    for (int i = 0; i < 3000; ++i) {
        const std::uint64_t before = recv.load(std::memory_order_relaxed);
        const auto sent = now_ns();
        pair.a().send(payload);
        const auto ld = Clock::now() + 200ms;
        while (recv.load(std::memory_order_relaxed) == before && Clock::now() < ld)
            std::this_thread::yield();
        if (recv.load(std::memory_order_relaxed) > before) lat.add(now_ns() - sent);
    }
    emit("libtracer", Pair::mode, S, 1, 1, rate, rate, rate * static_cast<double>(S) / 1e6,
         lat.summarize());
}

template <class Pair>
void run_transport() {
    Pair pair;
    if (!pair.ok()) {
        std::fprintf(stderr, "[transports] %s endpoints did not come up — skipped\n", Pair::mode);
        return;
    }
    for (std::size_t S : net::kSizes) run_size(pair, S);
}

}  // namespace

int main() {
    run_transport<udp_pair>();
    run_transport<tcp_pair>();
    run_transport<ws_pair>();
    return 0;
}
