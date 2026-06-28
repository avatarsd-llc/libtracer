/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #55 (increment 2) — REAL-bus smoke test for transport_can over Linux SocketCAN.
 * It drives two transport_can instances over an actual `vcan0` virtual CAN device
 * via socketcan_link_t (the genuine PF_CAN / SOCK_RAW path) and asserts a byte-exact
 * frame round-trips each way.
 *
 * This needs the kernel `vcan` module + a `vcan0` link, which an unprivileged
 * container cannot load — so the test SELF-SKIPS (clean exit 0) when the socket
 * cannot bind (no vcan / non-Linux). The existing required CI gates therefore never
 * depend on kernel CAN; the dedicated can-vcan-e2e workflow sets vcan0 up so the
 * socket path is genuinely exercised. CAN_RAW sockets do not receive their own
 * sent frames (CAN_RAW_RECV_OWN_MSGS defaults off) but other sockets on the same
 * interface do, so A→B and B→A are cleanly separable without self-delivery.
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "libtracer/transport_can.hpp"
#include "libtracer/view_can.hpp"

namespace {

using namespace std::chrono_literals;

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

class sink_t {
   public:
    void on(std::span<const std::byte> f) {
        const std::lock_guard lock(m_);
        last_.assign(f.begin(), f.end());
        ++count_;
        cv_.notify_all();
    }
    bool wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, budget, [&] { return count_ >= n; });
    }
    std::vector<std::byte> last() {
        const std::lock_guard lock(m_);
        return last_;
    }

   private:
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::byte> last_;
    std::size_t count_ = 0;
};

std::vector<std::byte> payload(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::byte>((i * 5 + seed) & 0xFF);
    return v;
}

bool equal_bytes(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace

int main() {
    std::printf("transport_can REAL vcan0 smoke test:\n");

    // Probe the bus; self-skip cleanly if vcan0 is unavailable.
    auto probe = std::make_unique<tr::net::socketcan_link_t>("vcan0");
    if (!probe->ok()) {
        std::printf(
            "  [SKIP] vcan0 unavailable (no kernel CAN here) — covered by can-vcan-e2e CI\n");
        return 0;
    }
    probe.reset();

    auto link_a = std::make_unique<tr::net::socketcan_link_t>("vcan0");
    auto link_b = std::make_unique<tr::net::socketcan_link_t>("vcan0");
    check(link_a->ok() && link_b->ok(), "two CAN_RAW sockets bound to vcan0");

    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 1, tr::view::can_frame_mode_t::CLASSIC, "a/p"});
    tr::net::transport_can tx_b(std::move(link_b),
                                {0, 2, tr::view::can_frame_mode_t::CLASSIC, "b/q"});

    sink_t sink_a, sink_b;
    tx_a.set_receiver([&](std::span<const std::byte> f) { sink_a.on(f); });
    tx_b.set_receiver([&](std::span<const std::byte> f) { sink_b.on(f); });

    // A -> B (a 20-byte payload spans 3 classic CAN data frames).
    const std::vector<std::byte> a2b = payload(20, 0x11);
    tx_a.send(a2b);
    const bool got_b = sink_b.wait_for_count(1, 3s);
    check(got_b, "B received A's frame over vcan0");
    if (got_b) check(equal_bytes(sink_b.last(), a2b), "A->B bytes byte-exact over the real bus");

    // B -> A.
    const std::vector<std::byte> b2a = payload(14, 0x44);
    tx_b.send(b2a);
    const bool got_a = sink_a.wait_for_count(1, 3s);
    check(got_a, "A received B's frame over vcan0");
    if (got_a) check(equal_bytes(sink_a.last(), b2a), "B->A bytes byte-exact over the real bus");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
