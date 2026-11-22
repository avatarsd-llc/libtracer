/**
 * @file
 * @brief libtracer NETWORK transport bench — TWO PROCESSES over a real loopback socket, one
 *        transport each (UDP / TCP / WebSocket).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A separate publisher and subscriber process
 * cross the kernel, exactly like the Zenoh two-process bench (bench_zenoh_net) — same
 * topology, same bench_net.hpp phase protocol (payload self-identifies its phase and
 * carries a CLOCK_MONOTONIC send-timestamp, so one-way latency is valid across the two
 * processes on one host), same RESULT line (`net-<proto>`). run_net.sh orchestrates the
 * pub/sub pair per protocol for both engines, so render_compare.py plots them on shared
 * absolute axes.
 *
 *   bench_transports sub <udp|tcp|ws> <port>
 *   bench_transports pub <udp|tcp|ws> <port>
 *
 * The publisher reports, per size on stderr, whether its link is still up and how many sends
 * it shed (`LINK net-<proto> size=<S> link_up=<0|1> dropped_tx=<n>`), and exits nonzero when
 * the link went down: a run whose later sizes reached nobody is a failed run, not a short one
 * (#2899).
 *
 * (Successor to the retired two-process bench_libtracer_net — ADR-0040 moved the net
 * plane to explicit-source-routed FWD; this measures the transport seam itself.)
 */
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
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"

using namespace std::chrono_literals;
using tr::net::transport_t;
using namespace bench;

namespace {

/** @brief A UDP link that admits any datagram, not only the one-MTU default (#2132). */
const tr::net::udp_config_t kWideUdp{.max_frame = tr::net::udp_transport_t::kMaxDatagram};

/** @brief The subscriber binds/listens on `port`; the publisher's own UDP bind is port+1.
 *         Owned through `poly_ptr_t`, the one owner of a link through its base (#2022). */
tr::mem::poly_ptr_t<transport_t> make_endpoint(std::string_view proto, bool sub,
                                               std::uint16_t port) {
    tr::mem::block_source_t& src = tr::mem::heap_source();
    if (proto == "udp")
        // The 8 KiB row is over the one-MTU default cap (#2132): admit every datagram size.
        return sub ? tr::mem::make_poly<tr::net::udp_transport_t>(src, port, "127.0.0.1", port + 1,
                                                                  kWideUdp)
                   : tr::mem::make_poly<tr::net::udp_transport_t>(src, port + 1, "127.0.0.1", port,
                                                                  kWideUdp);
    if (proto == "tcp")
        return sub ? tr::mem::make_poly<tr::net::tcp_transport_t>(src, port)
                   : tr::mem::make_poly<tr::net::tcp_transport_t>(src, "127.0.0.1", port);
    if (proto == "ws")
        return sub ? tr::mem::poly_ptr_t<transport_t>(
                         tr::mem::make_poly<tr::net::ws_server_transport_t>(src, port))
                   : tr::mem::poly_ptr_t<transport_t>(
                         tr::mem::make_poly<tr::net::ws_client_transport_t>(src, "127.0.0.1",
                                                                            port));
    return nullptr;
}

void run_sub(std::string_view proto, std::uint16_t port) {
    // Collector state + the named receiver lambda live BEFORE the endpoint:
    // the slot binds the callable by address, and the transport dtor joins its
    // recv thread.
    net::SubState state("libtracer", "net-" + std::string(proto));
    std::atomic<std::uint64_t> last_recv{0};
    auto rx = [&](std::span<const std::byte> f) {
        state.on_payload(f);
        last_recv.store(now_ns(), std::memory_order_relaxed);
    };
    auto ep = make_endpoint(proto, true, port);
    if (!ep) return;
    ep->set_receiver(rx);
    // Wait for the first frame, then finish once the stream goes quiet (the publisher
    // has sent its EOF markers per size and stopped).
    while (last_recv.load(std::memory_order_relaxed) == 0) std::this_thread::sleep_for(10ms);
    for (;;) {
        std::this_thread::sleep_for(200ms);
        // The receive thread may stamp a frame after this thread read the clock, so a stamp
        // newer than `now` is a live stream, not an unsigned wrap to "quiet for ages": that
        // wrap ended the subscriber mid-burst and reset the publisher's link (#2899).
        const std::uint64_t last = last_recv.load(std::memory_order_relaxed);
        if (now_ns() > last + 3'000'000'000ULL) break;
    }
    const tr::net::transport_drop_stats_t d = ep->drop_stats();
    std::fprintf(stderr, "LINK net-%.*s sub dropped_rx=%llu malformed_rx=%llu\n",
                 static_cast<int>(proto.size()), proto.data(),
                 static_cast<unsigned long long>(d.dropped_rx),
                 static_cast<unsigned long long>(d.malformed_rx));
}

/** @return 0 when every size ran with the link up; 1 when the link went down or never came. */
int run_pub(std::string_view proto, std::uint16_t port) {
    auto ep = make_endpoint(proto, false, port);
    if (!ep) return 1;
    ep->start_receiving();  // a DIAL link receives once armed (#2247)
    // Let the connection establish: up to 5 s, so a slow host is not a spurious failure, then
    // the settle the first size's latency probes always had.
    for (int i = 0; i < 500 && !ep->link_up(); ++i) std::this_thread::sleep_for(10ms);
    std::this_thread::sleep_for(400ms);
    if (!ep->link_up()) {
        std::fprintf(stderr, "LINK net-%.*s never came up\n", static_cast<int>(proto.size()),
                     proto.data());
        return 1;
    }

    std::vector<std::uint8_t> payload;
    const auto send = [&](std::size_t S, net::Phase ph) {
        net::make_payload(payload, S, ph);
        ep->send(std::as_bytes(std::span<const std::uint8_t>(payload)));
    };
    const std::size_t lat_msgs = net::latency_msgs();
    std::uint64_t dropped = 0;
    for (std::size_t S : net::sizes_for(proto)) {
        for (std::size_t i = 0; i < lat_msgs; ++i) {
            send(S, net::kLatency);
            const auto until = Clock::now() + std::chrono::nanoseconds(net::kPaceNs);
            while (Clock::now() < until) { /* pace so latency sends don't queue */
            }
        }
        for (std::size_t i = 0; i < net::throughput_msgs(S); ++i) send(S, net::kThroughput);
        for (int i = 0; i < 8; ++i) send(16, net::kEof);
        std::this_thread::sleep_for(std::chrono::milliseconds(net::kDrainMs));
        const std::uint64_t total = ep->drop_stats().dropped_tx;
        const bool up = ep->link_up();
        std::fprintf(stderr, "LINK net-%.*s size=%zu link_up=%d dropped_tx=%llu\n",
                     static_cast<int>(proto.size()), proto.data(), S, up ? 1 : 0,
                     static_cast<unsigned long long>(total - dropped));
        dropped = total;
        if (!up) return 1;  // every later size would reach nobody
    }
    return 0;
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
    if (role == "pub") return run_pub(proto, port);
    if (role == "sub")
        run_sub(proto, port);
    else
        return 2;
    return 0;
}
