/**
 * @file
 * @brief #882 — a shape-flipping rebind must never let a forward route a BUS link
 *        point-to-point.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `child_registry_t::add`'s rebind path publishes a slot's SHAPE and its LINK. The forward
 * mount descent (`resolve_mount_by`) and the bound-egress reader both read those two facts,
 * and a reader that pairs a STALE shape with a FRESH link takes the point-to-point branch
 * over a link whose `send()` BROADCASTS — one directed request drawing N replies, the #409
 * misroute the descent's own rejected-hit branch exists to prevent.
 *
 * Two guards, both driven by the same storm: one control thread rebinds a single mount name
 * back and forth between a point-to-point link and a bus link (what a reconnect onto a
 * differently-configured endpoint does), while a reader thread hammers the same slot.
 *
 *   1. @ref shape_snapshot_is_coherent — the REGISTRY-level guard. Every sample of a slot's
 *      egress must agree with the link it names: a link that exposes `bus()` must read
 *      multi-peer, and one that does not must not. This is the misroute CONDITION, sampled
 *      directly, so it fires without needing a frame to be in flight at the wrong instant.
 *
 *   2. @ref forward_never_broadcasts — the ROUTER-level guard, the consequence. A forward
 *      thread routes a DIRECTED `dst` through the same name for the whole storm. The bus
 *      transport's own `send()` is the broadcast egress; it must be called ZERO times,
 *      whichever shape the slot happens to be carrying.
 *
 * **What each one actually bites, measured rather than assumed.** Guard 1 is the #882 guard:
 * against the pre-fix two-field publication it reports thousands of misroutable samples per
 * run, and it reports them against the "load the link first" reorder too — which is why that
 * reorder was not enough and the fold shipped. Guard 2 did **not** reproduce the race in
 * 30000 shape-flipping rebinds (0 broadcasts pre-fix): its reader touches the slot once per
 * frame rather than in a tight loop, so it is almost never standing between the two reads at
 * the instant a rebind lands. It is kept because it bites a DIFFERENT mutation — dropping the
 * shape bit on decode makes it fire in the tens of thousands — so it guards that the fold
 * actually carries the shape, end to end, through a real forward. Neither guard should be
 * read as proving more than that: a PASS on a concurrency guard is evidence in proportion to
 * the interleavings the run happened to hit, so both print their own storm counters.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::net::child_registry_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

/** @brief A point-to-point endpoint — no `bus()`, so a directed `dst` egresses over it. */
struct p2p_link_t : transport_t {
    std::atomic<std::size_t> sends{0}; /**< @brief Directed frames handed to this endpoint. */
    void send(std::span<const std::byte>) override {
        sends.fetch_add(1, std::memory_order_relaxed);
    }
    void send(std::span<const std::span<const std::byte>>) override {
        sends.fetch_add(1, std::memory_order_relaxed);
    }
};

/**
 * @brief A multi-peer endpoint whose own `send()` is the BROADCAST leg.
 *
 * A real bus adapter fans an untagged `send()` out to every open peer, so any count in
 * @ref broadcasts is one request that went to N peers (ADR-0073 §3 / RFC-0020).
 */
struct bus_link_t : transport_t, tr::net::bus_link_t {
    std::vector<std::pair<std::string, p2p_link_t*>> peers;
    std::atomic<std::size_t> broadcasts{0}; /**< @brief Fan-out sends — must stay zero. */
    void send(std::span<const std::byte>) override {
        broadcasts.fetch_add(1, std::memory_order_relaxed);
    }
    void send(std::span<const std::span<const std::byte>>) override {
        broadcasts.fetch_add(1, std::memory_order_relaxed);
    }
    tr::net::bus_link_t* bus() override { return this; }
    transport_t* peer_link(std::string_view name) override {
        for (auto& [n, l] : peers) {
            if (n == name) return l;
        }
        return nullptr;
    }
    void enumerate_peers(const tr::net::bus_link_t::peer_visitor_t& visit) const override {
        for (const auto& [n, l] : peers) visit(n);
    }
};

std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief How long each storm runs, in rebinds. Not a bound on anything shipped. */
constexpr int kRebinds = 60000;

/**
 * @brief GUARD 1 — a slot's egress snapshot must never disagree with itself.
 *
 * The reader samples the slot exactly as the forward descent does and asks the link it just
 * read whether it is a bus. A sample pairing a bus link with "not multi-peer" IS the #882
 * misroute: the descent would return that link as a directed egress and `send()` it.
 */
void shape_snapshot_is_coherent() {
    std::printf("shape-flip storm: the egress snapshot is coherent\n");
    graph_t g;
    fwd_router_t router(g);
    p2p_link_t p2p;
    bus_link_t bus;
    p2p_link_t peer;
    bus.peers.emplace_back("leaf", &peer);
    (void)router.add_child("m/a", p2p);

    const child_registry_t::child_t* const slot = router.registry().entry_by_name("m/a");
    check(slot != nullptr, "the mount slot is addressable");
    if (slot == nullptr) return;

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> incoherent{0};
    std::atomic<std::uint64_t> misroutable{0};

    std::thread reader([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        std::uint64_t n = 0;
        std::uint64_t bad = 0;
        std::uint64_t hot = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const child_registry_t::egress_t eg = slot->egress();
            ++n;
            if (eg.link == nullptr) continue;
            const bool really_a_bus = eg.link->bus() != nullptr;
            if (really_a_bus != eg.multi_peer) {
                ++bad;
                // The DANGEROUS half: a bus link the descent would treat as directed.
                if (really_a_bus && !eg.multi_peer) ++hot;
            }
        }
        samples.store(n, std::memory_order_relaxed);
        incoherent.store(bad, std::memory_order_relaxed);
        misroutable.store(hot, std::memory_order_relaxed);
    });

    go.store(true, std::memory_order_release);
    for (int i = 0; i < kRebinds; ++i) {
        if ((i & 1) == 0) {
            (void)router.add_child("m/a", bus);
        } else {
            (void)router.add_child("m/a", p2p);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    std::printf("    %llu rebinds, %llu samples, %llu incoherent (%llu misroutable)\n",
                static_cast<unsigned long long>(kRebinds),
                static_cast<unsigned long long>(samples.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(incoherent.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(misroutable.load(std::memory_order_relaxed)));
    // The instrument control: a reader that never sampled proves nothing.
    check(samples.load(std::memory_order_relaxed) > 0, "the reader sampled the slot at all");
    check(misroutable.load(std::memory_order_relaxed) == 0,
          "no sample pairs a BUS link with a point-to-point shape (#882)");
    check(incoherent.load(std::memory_order_relaxed) == 0,
          "no sample pairs a link with the other shape at all");
}

/**
 * @brief GUARD 2 — the consequence: no forward ever reaches the bus transport's `send()`.
 *
 * `dst = /m/a/leaf` is a directed address. Against the point-to-point shape it egresses over
 * the p2p link; against the bus shape it resolves the PEER `leaf` and egresses over that
 * peer's directed endpoint. Neither answer is the bus link itself — so a single broadcast
 * count is one request fanned out to every open peer.
 */
void forward_never_broadcasts() {
    std::printf("shape-flip storm: a directed dst never reaches the broadcast send\n");
    graph_t g;
    fwd_router_t router(g);
    p2p_link_t inbound;
    p2p_link_t p2p;
    bus_link_t bus;
    p2p_link_t peer;
    bus.peers.emplace_back("leaf", &peer);
    (void)router.add_child("in", inbound);
    (void)router.add_child("m/a", p2p);

    const std::vector<std::byte> frame =
        tr::testing::b_fwd(fwd_op_t::READ, b_path({"m", "a", "leaf"}), b_path({"reply-ep"}));

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> forwards{0};

    std::thread forwarder([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        std::uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            router.on_frame("in", frame);
            ++n;
        }
        forwards.store(n, std::memory_order_relaxed);
    });

    go.store(true, std::memory_order_release);
    for (int i = 0; i < kRebinds; ++i) {
        if ((i & 1) == 0) {
            (void)router.add_child("m/a", bus);
        } else {
            (void)router.add_child("m/a", p2p);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    forwarder.join();

    const std::size_t fan = bus.broadcasts.load(std::memory_order_relaxed);
    std::printf("    %llu rebinds, %llu forwards, p2p=%llu peer=%llu broadcast=%llu\n",
                static_cast<unsigned long long>(kRebinds),
                static_cast<unsigned long long>(forwards.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p2p.sends.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(peer.sends.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(fan));
    // Instrument control: a storm that forwarded nothing, or one where no forward ever
    // egressed, would report zero broadcasts for the wrong reason.
    check(forwards.load(std::memory_order_relaxed) > 0, "the forwarder ran at all");
    check(
        p2p.sends.load(std::memory_order_relaxed) + peer.sends.load(std::memory_order_relaxed) > 0,
        "and forwards did egress over a DIRECTED endpoint");
    check(fan == 0, "the bus transport's broadcasting send() was never reached (#882/#409)");
}

}  // namespace

int main() {
    std::printf("mount-descent shape flip (#882):\n");
    shape_snapshot_is_coherent();
    forward_never_broadcasts();
    return tr::testing::summary("mount_shape_flip");
}
