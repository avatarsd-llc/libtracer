/**
 * @file
 * @brief #1294 — the peer-receiver seam carries an opaque per-peer HANDLE, not a name string.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Four properties, each of which a consumer of the seam is entitled to build on:
 *
 *  1. The handle is an 8-byte POD with a reserved-zero generation, so "no peer" is
 *     expressible and every handle the seam hands down is valid — no null arm (ruling 3).
 *  2. `bus_link_t::peer_name` is the ONE bridge back to the routing plane's name, and it is
 *     a pure function of the handle's index — the same `p<slot>` the accept edge stamps, and
 *     the same string `enumerate_peers` lists, resolved with no lock.
 *  3. A frame's handle round-trips: the tag delivery carries is the tag the arrival notifier
 *     minted, and it resolves to the name the peer is addressable under.
 *  4. The generation is what makes the handle a SESSION identity rather than a slot one: a
 *     recycled slot comes back at the same index and a DIFFERENT generation, so a handle
 *     minted against the departed session never matches its successor.
 *
 * The `slot_server_t` plane (here: `transport_tcp_server`) is the vehicle because it is the
 * positional kind — the one where slot reuse can confuse identity at all. The announce-census
 * arm (`transport_can`, one constant generation) is covered by `transport_can_test`'s
 * peer-named rope delivery, which resolves its `n<node>` name back through this same seam.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_tcp.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::bus_link_t;
using tr::net::peer_handle_t;
using tr::net::tcp_transport_t;
using tr::testing::check;

/** @brief Records what the peer-named seam handed down, handle and all. */
struct handle_sink_t {
    std::mutex m;                       /**< @brief Guards the two vectors. */
    std::condition_variable cv;         /**< @brief Signalled on every delivery. */
    std::vector<peer_handle_t> handles; /**< @brief One entry per delivered frame. */

    /** @brief The peer-named receiver callable (bound by address). */
    void operator()(peer_handle_t peer, std::span<const std::byte>) {
        {
            const std::lock_guard lock(m);
            handles.push_back(peer);
        }
        cv.notify_all();
    }
    /** @brief True once at least @p n frames arrived before @p timeout. */
    bool wait_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return handles.size() >= n; });
    }
    /** @brief The handle the @p i-th delivery carried. */
    peer_handle_t at(std::size_t i) {
        const std::lock_guard lock(m);
        return handles[i];
    }
};

/** @brief What the arrival/departure notifiers observed — the mint and the retire. */
struct lifecycle_probe_t {
    std::mutex m;                      /**< @brief Guards the two vectors. */
    std::condition_variable cv;        /**< @brief Signalled on every event. */
    std::vector<peer_handle_t> up;     /**< @brief Handles minted at arrival. */
    std::vector<std::string> up_names; /**< @brief The names they arrived under. */
    std::vector<peer_handle_t> down;   /**< @brief Handles retired at departure. */

    /** @brief Record one arrival. */
    void note_up(peer_handle_t h, std::string_view name) {
        {
            const std::lock_guard lock(m);
            up.push_back(h);
            up_names.emplace_back(name);
        }
        cv.notify_all();
    }
    /** @brief Record one departure. */
    void note_down(peer_handle_t h) {
        {
            const std::lock_guard lock(m);
            down.push_back(h);
        }
        cv.notify_all();
    }
    /** @brief True once @p n arrivals AND @p d departures landed before @p timeout. */
    bool wait_for(std::size_t n, std::size_t d, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return up.size() >= n && down.size() >= d; });
    }
};

/** @brief The `{fn, ctx}` arrival notifier — the seam's own shape, not the sugar. */
void on_peer_up(void* ctx, peer_handle_t handle, std::string_view peer) {
    static_cast<lifecycle_probe_t*>(ctx)->note_up(handle, peer);
}

/** @brief The `{fn, ctx}` departure notifier. */
void on_peer_down(void* ctx, peer_handle_t handle, std::string_view) {
    static_cast<lifecycle_probe_t*>(ctx)->note_down(handle);
}

/** @brief Resolve @p h through @p bus into an owned string (the scratch is ours). */
std::string name_of(bus_link_t& bus, peer_handle_t h) {
    std::array<char, tr::net::kPeerNameChars> scratch{};
    return std::string(bus.peer_name(h, scratch));
}

/** @brief Property 1 — the POD contract, checked without any transport at all. */
void test_handle_pod_contract() {
    std::printf("#1294 — the handle's POD contract:\n");
    check(sizeof(peer_handle_t) == 8, "the per-frame handle is 8 bytes");
    check(!peer_handle_t{}.valid(), "a default-constructed handle names no peer");
    check(!peer_handle_t{7, 0}.valid(), "generation 0 is reserved for 'no peer'");
    check(tr::net::kSolePeerHandle.valid(),
          "the constant a link with no per-peer identity mints IS valid (no null arm)");
    check(peer_handle_t{3, 5} == peer_handle_t{3, 5}, "handles compare by identity");
    check(!(peer_handle_t{3, 5} == peer_handle_t{3, 6}),
          "the generation is part of that identity — a recycled slot is a DIFFERENT peer");
    check(peer_handle_t{3, 5}.bits() != peer_handle_t{5, 3}.bits(),
          "bits() does not fold the two fields into each other");
}

/** @brief Properties 2–4 — the whole seam over a real multi-peer listener. */
void test_seam_over_slot_server() {
    std::printf("#1294 — the handle across the slot_server_t seam:\n");

    // Sink and probe BEFORE the transport (the destruction-order idiom every transport test
    // in this tree follows: the poll thread must not outlive what it delivers into).
    handle_sink_t sink;
    lifecycle_probe_t probe;

    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/0,
                                         /*peer_named=*/true);
    check(server.ok(), "listen socket bound");
    bus_link_t* const bus = server.bus();
    check(bus != nullptr, "a peer_named server exposes the bus facet");
    if (bus == nullptr) return;
    bus->set_peer_up_notifier(&on_peer_up, &probe);
    bus->set_peer_down_notifier(&on_peer_down, &probe);
    bus->set_peer_receiver(sink);

    // --- property 2: resolution is a pure function of the index, live peer or not ---
    check(name_of(*bus, peer_handle_t{3, 7}) == "p3",
          "peer_name is the accept edge's own p<slot> formatting");
    check(name_of(*bus, peer_handle_t{}).empty(), "an invalid handle names no peer");

    const std::uint16_t port = server.local_port();

    // --- property 3: the delivered tag IS the minted tag, and it names the peer ---
    peer_handle_t first_handle{};
    std::string first_name;
    {
        tcp_transport_t dialer("127.0.0.1", port);
        check(dialer.ok(), "the dialer connected");
        const std::vector<std::byte> frame(4, std::byte{0x5A});
        dialer.send(frame);
        check(sink.wait_count(1, 2000ms), "the frame reached the peer-named sink");
        check(probe.wait_for(1, 0, 2000ms), "the arrival notifier fired for that session");
        first_handle = sink.at(0);
        check(first_handle.valid(), "the seam never hands down an absent handle");
        {
            const std::lock_guard lock(probe.m);
            check(!probe.up.empty() && probe.up[0] == first_handle,
                  "the handle a frame carries is the one the ARRIVAL minted");
        }
        first_name = name_of(*bus, first_handle);
        check(!first_name.empty(), "the delivered handle resolves to a name");
        // ...and that name is the one the peer is ADDRESSABLE under, which is the whole
        // reason the routing plane still wants a name at all.
        bool listed = false;
        bus->enumerate_peers([&](std::string_view p) { listed = listed || p == first_name; });
        check(listed, "the resolved name is the one enumerate_peers lists");
        check(bus->peer_link(first_name) != nullptr, "and the one peer_link resolves");
        {
            const std::lock_guard lock(probe.m);
            check(!probe.up_names.empty() && probe.up_names[0] == first_name,
                  "the arrival announced that same name beside the handle");
        }
    }  // the dialer hangs up here

    // --- property 4: a recycled slot is a NEW session, and the handle says so ---
    check(probe.wait_for(1, 1, 2000ms), "the departure notifier fired");
    {
        const std::lock_guard lock(probe.m);
        check(!probe.down.empty() && probe.down[0] == first_handle,
              "the retired handle is the one that was minted (a whole session's identity)");
    }

    tcp_transport_t successor("127.0.0.1", port);
    check(successor.ok(), "a second dialer connected onto the freed slot");
    const std::vector<std::byte> frame(4, std::byte{0xA5});
    successor.send(frame);
    check(sink.wait_count(2, 2000ms), "the successor's frame reached the sink");
    const peer_handle_t second_handle = sink.at(1);
    check(second_handle.valid(), "the successor's handle is valid too");
    check(name_of(*bus, second_handle) == first_name,
          "it inherits the slot's NAME — names are positional and always were");
    check(!(second_handle == first_handle),
          "but NOT the departed session's HANDLE — the generation moved (#1294 / #1013)");
    check(second_handle.index == first_handle.index,
          "same slot index, so the difference is entirely the generation");
}

}  // namespace

int main() {
    test_handle_pod_contract();
    test_seam_over_slot_server();
    std::printf("peer_handle_seam_test: OK\n");
    return 0;
}
