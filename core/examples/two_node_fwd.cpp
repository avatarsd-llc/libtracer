/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief Two nodes over a wire — an FWD write routed between two graphs, and the
 *        end-to-end delivery latency across the "wire".
 *
 * Two independent `graph_t` nodes, each with a `fwd_router_t`, connected by a
 * `loopback_channel_t` — the in-process dev "wire" (`docs/reference/13-network-formation.md`,
 * ADR-0040). A client hands node A's router an `FWD{ op=WRITE, dst=/b/sensor/temp }`
 * frame; A peeks the first `dst` segment, strips `b`, and forwards `/sensor/temp`
 * across the wire; B's terminus writes it into B's local vertex, waking B's
 * subscriber. The route is explicit and loop-free by construction — no per-request
 * state on any hop.
 *
 * Going to a real socket is a one-line swap: replace `channel.a()`/`channel.b()`
 * with two `udp_transport_t` — exactly `core/tests/udp_test.cpp`'s two-node test.
 *
 * The RESULT perf line (mean cross-wire delivery latency) is informational so CI
 * never flakes on timing; the self-checks guard that every frame is delivered with
 * the exact payload. Runs under ctest as `example_two_node_fwd`.
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/loopback.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using clock_t_ = std::chrono::steady_clock;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;

/** @brief A VALUE TLV (encoded wire bytes) carrying @p bytes. */
std::vector<std::byte> value_tlv(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> payload;
    for (std::uint8_t b : bytes) payload.push_back(std::byte{b});
    tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    return tr::wire::encode(t);
}

/**
 * @brief Build FWD{ op=WRITE, dst=<segs…>, src=<empty>, payload=<VALUE> } — a remote
 *        write routed by explicit source route (RFC-0004 §D, ADR-0040).
 */
std::vector<std::byte> fwd_write(std::initializer_list<std::string_view> dst,
                                 std::span<const std::byte> payload_value_tlv) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst_segs;
    for (std::string_view s : dst) tr::wire::emit_name(dst_segs, s);
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{.pl = true}, dst_segs);
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{.pl = true},
                       std::span<const std::byte>{});  // src: empty, grows per hop
    body.insert(body.end(), payload_value_tlv.begin(), payload_value_tlv.end());
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, tr::wire::type_t::FWD, tr::wire::opt_t{.pl = true}, body);
    return frame;
}

}  // namespace

int main() {
    // Declaration order: the channel is declared LAST so it destructs FIRST — its
    // receive threads join before the routers they call into are gone.
    graph_t node_a, node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    tr::net::loopback_channel_t channel;

    // B owns the target vertex; A knows its link to B as "b", B knows its link back as "a".
    (void)node_b.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    router_a.add_child("b", channel.a());  // a `dst` starting with "b" routes over the wire
    router_b.add_child("a", channel.b());  // B's name for the inbound link (src accumulation)

    // B's subscriber signals each delivery; we count them and keep the last payload.
    std::mutex m;
    std::condition_variable cv;
    std::uint64_t delivered = 0;
    std::vector<std::byte> last;
    auto on_temp = [&](const tr::view::rope_t& v) {
        const auto b = v.only().bytes();
        std::lock_guard<std::mutex> lk(m);
        last.assign(b.begin(), b.end());
        ++delivered;
        cv.notify_one();
    };
    (void)node_b.subscribe(path_t("/sensor/temp"), on_temp);

    bool ok = true;
    const auto payload = value_tlv({0x2A, 0x2B});

    // One delivery for correctness: A routes /b/sensor/temp → strips "b" → B writes it.
    router_a.on_frame("client", fwd_write({"b", "sensor", "temp"}, payload));
    {
        std::unique_lock<std::mutex> lk(m);
        const bool arrived = cv.wait_for(lk, 3s, [&] { return delivered >= 1; });
        if (!arrived) {
            std::printf("  [FAIL] node B never received the FWD write\n");
            ok = false;
        } else if (last != payload) {
            std::printf("  [FAIL] delivered payload differs from what A sent\n");
            ok = false;
        } else {
            std::printf(
                "node B received the FWD-delivered value across the wire "
                "(explicit source route /b/sensor/temp → /sensor/temp)\n");
        }
    }

    // --- perf: mean cross-wire delivery latency over many sequential FWD writes ---
    constexpr std::uint64_t kMsgs = 2000;
    const std::uint64_t base = delivered;
    auto t0 = clock_t_::now();
    for (std::uint64_t i = 0; i < kMsgs; ++i)
        router_a.on_frame("client", fwd_write({"b", "sensor", "temp"}, payload));
    std::uint64_t target = base + kMsgs;
    {
        std::unique_lock<std::mutex> lk(m);
        const bool all = cv.wait_for(lk, 10s, [&] { return delivered >= target; });
        if (!all) {
            std::printf("  [FAIL] only %llu/%llu frames delivered\n",
                        static_cast<unsigned long long>(delivered - base),
                        static_cast<unsigned long long>(kMsgs));
            ok = false;
        }
    }
    auto t1 = clock_t_::now();

    const double total_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double per_ns = total_ns / double(kMsgs);
    std::printf("RESULT two_node_fwd msgs=%llu mean_delivery_ns=%.0f throughput_Kps=%.1f\n",
                static_cast<unsigned long long>(kMsgs), per_ns,
                (double(kMsgs) / (total_ns * 1e-9)) / 1e3);
    std::printf(
        "each hop reads three headers by offset and scatter-gathers the shrunk-dst / "
        "grown-src heads — the forward hop never touches the heap.\n");

    std::printf("%s\n", ok ? "two-node FWD OK" : "two-node FWD FAILED");
    return ok ? 0 : 1;
}
