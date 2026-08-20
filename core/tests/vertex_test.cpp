/**
 * @file
 * @brief vertex_t verb-interface unit tests — a BARE vertex, no graph_t (the point of the verb
 *        seam: the vertex's storage/readiness/edge/ACL state is testable in isolation).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Covers: the #867 / #1300 visibility guard (the map-lock mutators AND the storage funnel are
 * graph_t's, enforced by `static_assert` because no runtime test can observe access control),
 * the note_write/read_stored readiness cursor, wait_for_change wake/timeout, snapshot_edges
 * under a concurrent add_edge storm (inline→heap crossover included), clear_edge, the edge slot
 * table, and set_acl/with_acl/with_aces.
 *
 * NOT here since #1300: the LKV publish, the STREAM ring keep-last trim, the RFC-0008 §E drain
 * cursor and the transient-local latch all need a stored value, and `vertex_t::store()` is
 * `graph_t`'s alone now. They live in `graph_test.cpp` — `test_assign_lkv_and_seq_bump`,
 * `test_stream`, `test_stream_drain_cursor`, `test_admission_door_uniformity` — driven through
 * `vertex_handle_t`, plus `qos_policy_test.cpp` §5.1/§5.2 for the per-subscriber latch ablation.
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
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/view.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::ace_t;
using tr::graph::delivery_policy_t;
using tr::graph::edge_latch_t;
using tr::graph::edge_snapshot_t;
using tr::graph::edge_view_t;
using tr::graph::path_key_t;
using tr::graph::role_t;
using tr::graph::subscriber_t;
using tr::graph::vertex_t;
using tr::view::rope_t;

using tr::graph::handlers_t;
using tr::testing::check;

// -- #867 ruling 2 (+ #1300): the graph-owned mutators, and the COMPILER enforces it ------
//
// Access checking happens during substitution, so an inaccessible member makes a
// requires-expression FALSE rather than ill-formed: these assertions observe the visibility
// itself, which no runtime test can. Reverting the `private:` hunk in `vertex.hpp` re-satisfies
// every concept below and turns this TU into four compile errors — that ablation IS the
// evidence, since a green suite says nothing about an encapsulation the compiler owns.

/** @brief True iff a caller outside `graph_t` can stamp a registration's identity onto a vertex. */
template <typename V>
concept fills_identity = requires(V& v) { v.fill(role_t::STORED_VALUE, handlers_t{}); };

/** @brief True iff a caller outside `graph_t` can flip a vertex back to a placeholder. */
template <typename V>
concept unregisters = requires(V& v) { v.mark_unregistered(); };

/** @brief True iff a caller outside `graph_t` can splice a node into the Composite child list. */
template <typename V>
concept adopts_child = requires(V& v, std::unique_ptr<V> c) { v.add_child(std::move(c)); };

/** @brief True iff a caller outside `graph_t` can publish a value into a vertex's LKV. */
template <typename V>
concept publishes_value = requires(V& v) { v.store(rope_t{}); };

static_assert(!fills_identity<vertex_t>,
              "#867: fill() is unique-map-lock state — only graph_t may stamp an identity");
static_assert(!unregisters<vertex_t>,
              "#867: mark_unregistered() is unique-map-lock state — only graph_t may retire");
static_assert(!adopts_child<vertex_t>,
              "#867: add_child() mutates the Composite tree — only graph_t may splice it");
static_assert(!publishes_value<vertex_t>,
              "#1300: store() is graph_t's storage funnel — only graph_t may publish an LKV");

/** @brief What deliberately STAYS public on a bare vertex: the read + verb surface the
 *         readiness/edge/ACL tests below drive with no graph_t in sight. */
static_assert(requires(vertex_t& v) {
    v.role();
    v.registered();
    v.has_registered_child();
    v.child_by_record(std::span<const std::byte>{});
});

/** @brief A single-link rope over a fresh owned heap segment holding one byte `b`. */
rope_t make_value(std::uint8_t b) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(1);
    seg->bytes[0] = std::byte{b};
    return rope_t{tr::view::view_t::over(std::move(seg))};
}

path_key_t key_of(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> k;
    for (std::uint8_t b : bytes) k.push_back(std::byte{b});
    return path_key_t{k};
}

/**
 * @brief The write-sequence arithmetic on a BARE vertex — the one property no graph seam can
 *        express, because `graph_t::await` snapshots the cursor internally.
 *
 * `store()` is `graph_t`'s alone since #1300, so what remains testable here is the readiness
 * half: `note_write()` is exactly "+1 per verb, publish nothing". The LKV facts this suite used
 * to assert on `store()` are restated through the handle in
 * `graph_test.cpp::test_assign_lkv_and_seq_bump`.
 */
void test_seq_cursor_and_empty_lkv() {
    std::printf("note_write / read_stored — the seq cursor moves, the LKV does not:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x01}), {}};
    check(v.read_stored() == nullptr, "a never-assigned vertex holds no LKV");
    const std::uint64_t seq0 = v.current_seq();
    v.note_write();
    check(v.current_seq() == seq0 + 1, "note_write bumps the write sequence exactly once");
    check(v.read_stored() == nullptr, "a seq bump publishes nothing");
    v.note_write();
    v.note_write();
    check(v.current_seq() == seq0 + 3, "three bumps land three apart — one per verb, no coalesce");
    check(v.read_stored() == nullptr, "still nothing published after three bumps");
}

void test_await_wake_and_timeout() {
    std::printf("wait_for_change — wake on a write bump, timeout when idle:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x03}), {}};
    check(!v.wait_for_change(v.current_seq(), 20ms), "no writer => timeout (returns false)");

    const std::uint64_t seq0 = v.current_seq();
    std::atomic<bool> woke{false};
    std::thread waiter([&] {
        if (v.wait_for_change(seq0, 2s)) woke.store(true);
    });
    std::this_thread::sleep_for(20ms);
    v.note_write();
    waiter.join();
    check(woke.load(), "a write bump wakes a blocked waiter (write_seq_ != seq0)");
    check(v.wait_for_change(seq0, 0ns), "a stale seq0 observes the change without blocking");
}

/**
 * @brief The edge slot table on a bare vertex: add / snapshot / clear, and the one latch fact a
 *        vertex with NO last-known-value can still state.
 *
 * The LATCHED half of RFC-0022 §3.A — a `durability_request` subscriber replayed with the
 * producer's current value, and the ablation next to it — needs an LKV, and since #1300 only
 * `graph_t` can publish one. It is asserted at graph level instead: `graph_test.cpp` §admission
 * door (callback door latches, default subscription does not, a durability_request on the SAME
 * vertex does) and `qos_policy_test.cpp` §5.1/§5.2 (the per-subscriber ablation, both edges
 * still live afterwards).
 */
void test_edges_snapshot_clear_latch() {
    std::printf("edges — add/snapshot/clear + the no-LKV latch floor:\n");
    // RFC-0022 §3.A: the latch predicate is the SUBSCRIBER's request, not a vertex flag.
    vertex_t v{role_t::STORED_VALUE, key_of({0x04}), {}};

    int hits = 0;
    // subscriber_t is move-only (#380 §3 cold-half unique_ptr): mint a fresh edge per add.
    const auto mk_edge = [&hits](std::uint16_t policy_bits = 0) {
        subscriber_t s;
        s.callback = [](void* ctx, const rope_t&) { ++*static_cast<int*>(ctx); };
        s.callback_ctx = &hits;
        s.policy.bits = policy_bits;
        return s;
    };
    edge_latch_t latch;
    check(v.add_edge(mk_edge(delivery_policy_t::kDurabilityRequest), &latch) == 0,
          "the first edge lands in slot 0");
    check(latch.value == nullptr, "no LKV yet => no latch");

    edge_latch_t latch2;
    check(v.add_edge(mk_edge(delivery_policy_t::kDurabilityRequest), &latch2) == 1,
          "the second edge lands in slot 1");
    edge_latch_t latch3;
    check(v.add_edge(mk_edge(), &latch3) == 2, "the third edge lands in slot 2");

    edge_snapshot_t buf;
    std::vector<edge_view_t> heap;
    vertex_t::snapshot_drops_t drops;
    check(v.snapshot_edges(buf, heap, drops) == 3 && heap.empty(),
          "3 active edges snapshot into the inline buffer (no heap)");
    check(!drops.any(), "an unpressured snapshot sheds nothing (#896)");
    // The dispatch view stands on its own: the value handed to a snapshotted edge is the
    // caller's rope, never something the vertex had to be holding.
    const rope_t dispatched = make_value(0x11);
    buf[0].callback(buf[0].callback_ctx, dispatched);
    check(hits == 1, "a snapshotted edge dispatches through its {fn, ctx} pair");

    check(v.clear_edge(0), "clearing an active slot reports true");
    check(!v.clear_edge(0), "re-clearing the same slot reports false");
    check(!v.clear_edge(99), "clearing an out-of-range slot reports false");
    check(v.snapshot_edges(buf, heap, drops) == 2, "a cleared edge vanishes from the snapshot");
    check(!drops.any(), "a cleared edge is not a DROPPED delivery — nothing shed");

    for (int i = 0; i < 11; ++i) (void)v.add_edge(mk_edge());
    const std::size_t n = v.snapshot_edges(buf, heap, drops);
    check(n == 13 && heap.size() == 13, "a >kInlineFanout subscriber list overflows to the heap");
    check(!drops.any(), "a heap-backed wide snapshot sheds nothing either");
}

void test_snapshot_under_concurrent_add() {
    std::printf("snapshot_edges under a concurrent add_edge storm:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x05}), {}};
    std::atomic<int> dummy{0};
    const auto mk_proto = [&dummy] {
        subscriber_t s;
        s.callback = [](void* ctx, const rope_t&) {
            static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
        };
        s.callback_ctx = &dummy;
        return s;
    };

    constexpr int kThreads = 4;
    constexpr int kPerThread = 64;
    std::atomic<bool> go{false};
    std::vector<std::thread> adders;
    adders.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        adders.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kPerThread; ++i) (void)v.add_edge(mk_proto());
        });
    }
    bool monotonic = true;
    std::size_t last = 0;
    go.store(true, std::memory_order_release);
    edge_snapshot_t buf;
    std::vector<edge_view_t> heap;
    vertex_t::snapshot_drops_t drops;
    for (int i = 0; i < 2000; ++i) {
        const std::size_t n = v.snapshot_edges(buf, heap, drops);
        if (n < last) monotonic = false;
        last = n;
    }
    for (std::thread& t : adders) t.join();
    check(monotonic, "concurrent snapshots see a monotonically growing active-edge count");
    check(v.snapshot_edges(buf, heap, drops) == kThreads * kPerThread,
          "every concurrently added edge is snapshotted after the storm");
}

/** @brief The presence bit @ref vertex_t::with_acl reports, lifted out of the callback. */
bool acl_present(vertex_t& v) {
    return v.with_acl([](bool present, const std::vector<ace_t>&) { return present; });
}

/** @brief How many ACEs @ref vertex_t::with_acl reports (0 for an absent ACL too). */
std::size_t acl_ace_count(vertex_t& v) {
    return v.with_acl([](bool, const std::vector<ace_t>& aces) { return aces.size(); });
}

void test_acl_verbs() {
    std::printf("ACL verbs — set_acl / with_acl / with_aces:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x06}), {}};
    check(!acl_present(v), "no :acl set => with_acl reports absent");
    check(v.with_aces([](const std::vector<ace_t>& aces) { return aces.empty(); }),
          "no :acl set => empty ACE list");

    ace_t ace;
    ace.access_mask = 0x3;
    v.set_acl({ace});
    check(acl_present(v) && acl_ace_count(v) == 1,
          "with_acl serves the presence bit and the stored ACE list together");
    check(v.with_aces([](const std::vector<ace_t>& aces) {
        return aces.size() == 1 && aces[0].access_mask == 0x3;
    }),
          "with_aces evaluates over the parsed ACE list under the lock");

    // #907: an empty store replaces the list, but the ACL stays PRESENT — the empty
    // container is a written policy that grants nothing, not the absence of one.
    v.set_acl({});
    check(acl_ace_count(v) == 0, "storing replaces — an empty store clears the ACE list");
    check(acl_present(v), "an empty store leaves the :acl PRESENT (an ACL granting nothing)");
}

void test_bookkeeping_counters() {
    std::printf("RFC-0005 listener bookkeeping counters:\n");
    vertex_t v{role_t::STORED_VALUE, key_of({0x07}), {}};
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
    test_seq_cursor_and_empty_lkv();
    test_await_wake_and_timeout();
    test_edges_snapshot_clear_latch();
    test_snapshot_under_concurrent_add();
    test_acl_verbs();
    test_bookkeeping_counters();
    return tr::testing::summary("vertex");
}
