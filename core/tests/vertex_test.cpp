/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * vertex_t verb-interface unit tests — a BARE vertex, no graph_t (the point of the
 * verb seam: the vertex's storage/readiness/edge/ACL state is testable in isolation).
 * Covers: store/read_stored (LKV + seq bump), the STREAM ring keep-last trim and the
 * RFC-0008 §E drain cursor, wait_for_change wake/timeout, snapshot_edges under a
 * concurrent add_edge storm (inline→heap crossover included), clear_edge, the
 * transient-local add_edge latch, and set_acl/acl_bytes/with_aces.
 */

#include "libtracer/vertex.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/view.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::ace_t;
using tr::graph::edge_latch_t;
using tr::graph::edge_snapshot_t;
using tr::graph::edge_view_t;
using tr::graph::path_key_t;
using tr::graph::role_t;
using tr::graph::settings_t;
using tr::graph::subscriber_t;
using tr::graph::vertex_t;
using tr::view::rope_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// A single-link rope over a fresh owned heap segment holding one byte `b`.
rope_t make_value(std::uint8_t b) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(1);
    seg->bytes[0] = std::byte{b};
    return rope_t{tr::view::view_t::over(std::move(seg))};
}

std::uint8_t first_byte(const rope_t& r) {
    return std::to_integer<std::uint8_t>(r.only().bytes()[0]);
}

path_key_t key_of(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> k;
    for (std::uint8_t b : bytes) k.push_back(std::byte{b});
    return path_key_t{std::move(k)};
}

void test_store_and_lkv() {
    std::printf("store / read_stored — the LKV publish + seq bump:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x01}), settings_t{}, {}};
    check(v.read_stored() == nullptr, "a never-assigned vertex holds no LKV");
    const std::uint64_t seq0 = v.current_seq();
    v.store(make_value(0x42));
    check(v.read_stored() != nullptr && first_byte(*v.read_stored()) == 0x42,
          "store publishes the last-known-value");
    check(v.current_seq() == seq0 + 1, "store bumps the write sequence once");
    v.store(make_value(0x43));
    check(first_byte(*v.read_stored()) == 0x43, "a later store wins (last-writer-wins)");
    v.note_write();
    check(v.current_seq() == seq0 + 3, "note_write bumps the sequence without storing");
    check(first_byte(*v.read_stored()) == 0x43, "note_write leaves the LKV untouched");
}

void test_stream_ring_trim_and_drain() {
    std::printf("STREAM ring — keep-last trim + the RFC-0008 §E drain cursor:\n");
    settings_t st;
    st.history_keep_last = 3;
    vertex_t v{role_t::STREAM, key_of({0x02}), st, {}};
    for (std::uint8_t b = 1; b <= 5; ++b) v.store(make_value(b));
    const std::vector<rope_t> hist = v.history_snapshot();
    check(hist.size() == 3, "the ring keeps exactly history_keep_last entries");
    check(hist.size() == 3 && first_byte(hist[0]) == 3 && first_byte(hist[2]) == 5,
          "trim drops the oldest entries (ring holds 3,4,5)");

    std::vector<std::shared_ptr<const rope_t>> batch;
    check(v.drain_unflushed(batch) == 3, "5 unflushed appends drain to the 3 ring survivors");
    check(first_byte(*batch[0]) == 3 && first_byte(*batch[2]) == 5, "drain is in append order");
    check(v.drain_unflushed(batch) == 0, "a drained stream has nothing more to drain");

    v.store(make_value(6));
    check(v.drain_unflushed(batch) == 1 && first_byte(*batch[0]) == 6,
          "the next append drains alone (a queue, not a coalesce)");

    v.store(make_value(7));
    v.mark_flushed();
    check(v.drain_unflushed(batch) == 0, "mark_flushed advances the cursor without draining");
}

void test_await_wake_and_timeout() {
    std::printf("wait_for_change — wake on store, timeout when idle:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x03}), settings_t{}, {}};
    check(!v.wait_for_change(v.current_seq(), 20ms), "no writer => timeout (returns false)");

    const std::uint64_t seq0 = v.current_seq();
    std::atomic<bool> woke{false};
    std::thread waiter([&] {
        if (v.wait_for_change(seq0, 2s)) woke.store(true);
    });
    std::this_thread::sleep_for(20ms);
    v.store(make_value(0x99));
    waiter.join();
    check(woke.load(), "a store wakes a blocked waiter (write_seq_ != seq0)");
    check(v.wait_for_change(seq0, 0ns), "a stale seq0 observes the change without blocking");
}

void test_edges_snapshot_clear_latch() {
    std::printf("edges — add/snapshot/clear + the transient-local latch:\n");
    settings_t st;
    st.durability = 1;  // transient-local: add_edge latches once an LKV exists
    vertex_t v{role_t::STORED_VALUE, key_of({0x04}), st, {}};

    int hits = 0;
    subscriber_t s0;
    s0.callback = [](void* ctx, const rope_t&) { ++*static_cast<int*>(ctx); };
    s0.callback_ctx = &hits;
    edge_latch_t latch;
    check(v.add_edge(s0, &latch) == 0, "the first edge lands in slot 0");
    check(latch.value == nullptr, "no LKV yet => no latch");

    v.store(make_value(0x11));
    edge_latch_t latch2;
    check(v.add_edge(s0, &latch2) == 1, "the second edge lands in slot 1");
    check(latch2.value != nullptr && first_byte(*latch2.value) == 0x11,
          "transient-local + LKV => the latch snapshots the value");
    check(latch2.edge.callback != nullptr && latch2.edge.callback_ctx == &hits,
          "the latch carries the admitted edge's {fn, ctx} dispatch view");

    edge_snapshot_t buf;
    std::vector<edge_view_t> heap;
    check(v.snapshot_edges(buf, heap) == 2 && heap.empty(),
          "2 active edges snapshot into the inline buffer (no heap)");
    buf[0].callback(buf[0].callback_ctx, *v.read_stored());
    check(hits == 1, "a snapshotted edge dispatches through its {fn, ctx} pair");

    check(v.clear_edge(0), "clearing an active slot reports true");
    check(!v.clear_edge(0), "re-clearing the same slot reports false");
    check(!v.clear_edge(99), "clearing an out-of-range slot reports false");
    check(v.snapshot_edges(buf, heap) == 1, "a cleared edge vanishes from the snapshot");

    for (int i = 0; i < 12; ++i) (void)v.add_edge(s0);
    const std::size_t n = v.snapshot_edges(buf, heap);
    check(n == 13 && heap.size() == 13, "a >kInlineFanout subscriber list overflows to the heap");
}

void test_snapshot_under_concurrent_add() {
    std::printf("snapshot_edges under a concurrent add_edge storm:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x05}), settings_t{}, {}};
    std::atomic<int> dummy{0};
    subscriber_t proto;
    proto.callback = [](void* ctx, const rope_t&) {
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    };
    proto.callback_ctx = &dummy;

    constexpr int kThreads = 4;
    constexpr int kPerThread = 64;
    std::atomic<bool> go{false};
    std::vector<std::thread> adders;
    adders.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        adders.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kPerThread; ++i) (void)v.add_edge(proto);
        });
    }
    bool monotonic = true;
    std::size_t last = 0;
    go.store(true, std::memory_order_release);
    edge_snapshot_t buf;
    std::vector<edge_view_t> heap;
    for (int i = 0; i < 2000; ++i) {
        const std::size_t n = v.snapshot_edges(buf, heap);
        if (n < last) monotonic = false;
        last = n;
    }
    for (std::thread& t : adders) t.join();
    check(monotonic, "concurrent snapshots see a monotonically growing active-edge count");
    check(v.snapshot_edges(buf, heap) == kThreads * kPerThread,
          "every concurrently added edge is snapshotted after the storm");
}

void test_acl_verbs() {
    std::printf("ACL verbs — set_acl / acl_bytes / with_aces:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x06}), settings_t{}, {}};
    check(v.acl_bytes().empty(), "no :acl set => empty raw bytes");
    check(v.with_aces([](const std::vector<ace_t>& aces) { return aces.empty(); }),
          "no :acl set => empty ACE list");

    const std::array<std::byte, 3> raw{std::byte{0x0A}, std::byte{0x01}, std::byte{0xFF}};
    ace_t ace;
    ace.access_mask = 0x3;
    v.set_acl(raw, {ace});
    check(v.acl_bytes().size() == 3 && v.acl_bytes()[2] == std::byte{0xFF},
          "acl_bytes serves the stored raw bytes back verbatim");
    check(v.with_aces([](const std::vector<ace_t>& aces) {
        return aces.size() == 1 && aces[0].access_mask == 0x3;
    }),
          "with_aces evaluates over the parsed ACE list under the lock");

    v.set_acl({}, {});
    check(v.acl_bytes().empty(), "storing replaces — an empty store clears the ACL");
}

void test_bookkeeping_counters() {
    std::printf("RFC-0005 listener bookkeeping counters:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x07}), settings_t{}, {}};
    check(v.own_subs() == 0 && v.listeners_above() == 0, "counters start at zero");
    v.bump_own_subs(+1);
    v.bump_own_subs(+1);
    v.bump_own_subs(-1);
    check(v.own_subs() == 1, "own_subs tracks bump deltas");
    v.init_listeners_above(3);
    v.bump_listeners_above(-1);
    check(v.listeners_above() == 2, "listeners_above seeds then tracks deltas");
}

}  // namespace

int main() {
    test_store_and_lkv();
    test_stream_ring_trim_and_drain();
    test_await_wake_and_timeout();
    test_edges_snapshot_clear_latch();
    test_snapshot_under_concurrent_add();
    test_acl_verbs();
    test_bookkeeping_counters();
    if (g_failures != 0) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("all vertex_t verb tests passed\n");
    return 0;
}
