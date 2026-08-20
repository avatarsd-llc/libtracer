/**
 * @file
 * @brief #1442 / #1448 — NOBODY deep-copies a subscription's cold half any more. A republish
 *        shares it (so an admission's allocation count is flat in the edges already on the
 *        vertex) and so does the dispatch snapshot (so a WRITE's allocation count is flat in
 *        the remote fan-out).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #635 made the dispatch side of a vertex an immutable published array, so a control-plane
 * mutation publishes a WHOLE new array: appending the k-th subscriber rebuilds k+1 entries and
 * retires k. That walk is inherent and this test does not object to it. What it pins is the
 * per-entry CONSTANT: a REMOTE entry's cold half used to be deep-copied on every republish —
 * one `operator new` for the record plus a `std::string` copy each for the link and the caller
 * — and `scan_retired_edges` freed every one of them again on the next pass. Since the cold
 * half is written once at admission and never again, each of those copies reproduced something
 * byte-identical to the record it was retiring. PR #1441 measured it at ~940 instructions per
 * pre-existing edge against a ~158 inherent floor.
 *
 * @section instrument What makes this non-vacuous
 *
 * The instrument is a global `operator new` COUNTER, and the assertion is a SHAPE, not a
 * budget: the number of allocations one admission makes must not grow with the number of edges
 * already on the vertex. Restore the deep copy and section (a) reddens by a wide margin — at
 * 44 pre-existing edges the old code makes ~3 allocations per pre-existing edge (the record and
 * two over-SSO strings) where this asserts a flat constant. Both link names are deliberately
 * pushed past the small-string buffer for exactly that reason: with SSO-sized names two thirds
 * of the old cost would never reach the counter and the test would under-report the thing it
 * exists to catch.
 *
 * Section (b) is the lifetime half. Sharing is only correct because the record outlives every
 * holder, so a slot RECLAIM (`clear_edge` moves an inert shell in, dropping the slot's
 * reference) must leave the surviving edges' records — and any still-published array naming a
 * reclaimed one — intact. It is written as a delivery assertion on the CONTENT of the cold half
 * (link, caller, route bytes) rather than a counter, because a premature free's most likely
 * shape is a delivery carrying the right count and the wrong bytes. The ASan/TSan legs read
 * this arm as the use-after-free arm.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. */
std::size_t g_allocs = 0;
bool g_arm = false;

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) ++g_allocs;
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation — `aligned_alloc` only for a genuinely OVER-aligned
 *         request (which `free` accepts), `malloc` for a fundamental one. */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) ++g_allocs;
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

}  // namespace

// Every allocating and deallocating form is replaced (the `folded_read_backend_test`
// precedent): the published edge array is built with `::operator new(bytes, std::nothrow)`, so
// a hole in the set would make the very allocation under test INVISIBLE to the counter, and a
// missing delete form is an `alloc-dealloc-mismatch` under ASan.
void* operator new(std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    void* const p = counted_aligned(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::remote_delivery_t;
using tr::graph::role_t;
using tr::testing::check;
using tr::testing::make_value;
using tr::view::rope_t;
using tr::view::view_t;

/** @brief A link NAME long enough that copying it is a real heap allocation, never SSO. */
[[nodiscard]] std::string long_link(std::size_t i) {
    std::string s = "lnk" + std::to_string(i);
    s.append(64 - s.size(), 'x');  // 64 B ⇒ well past libstdc++'s 15-byte SSO buffer
    return s;
}

/** @brief A minimal well-formed SUBSCRIBER record and a minimal PATH return route. */
[[nodiscard]] view_t subscriber_tlv() { return make_value({0x04, 0x40, 0x00, 0x00}); }
[[nodiscard]] view_t route_tlv() { return make_value({0x06, 0x00, 0x00, 0x00}); }

/**
 * @brief (a) An admission's allocation count does not grow with the edge count.
 *
 * `kEdges` remote subscribers land on ONE vertex over distinct long-named links. Every
 * admission's global-`new` count is recorded, and the assertion compares a LATE admission
 * against an EARLY one. Both indices are chosen off a power of two so neither is the admission
 * that grows the slot vector — that growth is a genuine one-off allocation and comparing across
 * it would make the test read as a failure for the wrong reason.
 */
void test_republish_allocation_is_flat() {
    std::printf("an admission's allocation count is flat in the edges already present:\n");
    constexpr std::size_t kEdges = 48;
    constexpr std::size_t kEarly = 5;  // 5 pre-existing edges; capacity 8, no vector growth
    constexpr std::size_t kLate = 45;  // 45 pre-existing edges; capacity 64, no vector growth
    graph_t g;
    const path_t src = *path_t::parse("/t/share/src");
    const auto v = g.register_vertex(src, role_t::STORED_VALUE);

    std::vector<std::size_t> per_admission(kEdges, 0);
    for (std::size_t i = 0; i < kEdges; ++i) {
        const view_t sub = subscriber_tlv();  // built OUTSIDE the armed window
        const view_t route = route_tlv();
        std::string link = long_link(i);
        g_allocs = 0;
        g_arm = true;
        const auto r = g.subscribe_wire(v, sub, route, std::move(link));
        g_arm = false;
        per_admission[i] = g_allocs;
        check(r.has_value(), "every remote subscribe is admitted");
        if (!r) return;
    }

    const std::size_t early = per_admission[kEarly];
    const std::size_t late = per_admission[kLate];
    std::printf("    admission #%zu: %zu allocations; admission #%zu: %zu allocations\n", kEarly,
                early, kLate, late);
    check(early > 0, "the counter is live (an armed admission allocates SOMETHING)");
    // Slack of 2, not 0: the admission itself legitimately allocates (the new edge's own cold
    // half and its two strings, the fresh array), and one of those is size-dependent, so an
    // allocator packaging difference could add a bucket. The defect this guards against is
    // THREE allocations per pre-existing edge — 120 of them between these two points.
    check(late <= early + 2,
          "a republish over 45 pre-existing edges costs no more than over 5 — the cold half is "
          "SHARED, not deep-copied");

    // The whole series, not just two points: nothing may trend upward across the sweep. The
    // allowance covers the slot-vector doublings, which are one allocation each and land on
    // exactly the power-of-two admissions.
    std::size_t worst = 0;
    for (std::size_t i = 2; i < kEdges; ++i)
        worst = worst > per_admission[i] ? worst : per_admission[i];
    check(worst <= early + 8,
          "no admission in the sweep spikes beyond the slot-vector growth allowance");
}

/**
 * @brief (b) Reclaiming one slot leaves every OTHER edge's shared cold half intact.
 *
 * Three remote edges, each with its own link and caller. The middle one is unsubscribed —
 * `clear_edge` moves an inert shell into the slot, which drops that slot's reference to its
 * record while a published array may still name it — and the surviving two must then deliver
 * with their own link, caller and route bytes unchanged. A record freed one holder too early
 * shows up here as wrong BYTES, which is why this asserts content and not a delivery count.
 */
void test_reclaim_leaves_the_survivors_intact() {
    std::printf("reclaiming one slot leaves the other edges' shared cold halves intact:\n");
    graph_t g;
    const path_t src = *path_t::parse("/t/share/live");
    const auto v = g.register_vertex(src, role_t::STORED_VALUE);

    std::vector<std::string> seen_links;
    std::vector<std::string> seen_callers;
    std::size_t empty_routes = 0;
    const tr::testing::remote_sink_guard_t sink(g, [&](const remote_delivery_t& d, const rope_t&) {
        seen_links.emplace_back(d.link);
        seen_callers.emplace_back(d.caller);
        if (d.return_route.bytes().empty()) ++empty_routes;
    });

    for (std::size_t i = 0; i < 3; ++i)
        check(g.subscribe_wire(v, subscriber_tlv(), route_tlv(), long_link(i), view_t{},
                               "caller" + std::to_string(i))
                  .has_value(),
              "three remote subscribers are admitted");

    check(g.write(src, make_value({0x30, 0x00, 0x00, 0x00})).has_value(), "the first write lands");
    check(seen_links.size() == 3, "all three remote edges were delivered to");

    // Slot 1 is cleared through the wire door — the empty-STATUS eviction sentinel on
    // `:subscribers[1]`, which is `vertex_t::clear_edge` and therefore the in-place reclaim.
    const auto field = path_t::parse("/t/share/live:subscribers[1]");
    check(field.has_value(), "the indexed subscribers field parses");
    check(g.write(v, field->field(), make_value({0x09, 0x00, 0x00, 0x00})).has_value(),
          "the middle edge is unsubscribed");

    seen_links.clear();
    seen_callers.clear();
    empty_routes = 0;
    check(g.write(src, make_value({0x31, 0x00, 0x00, 0x00})).has_value(), "the second write lands");
    check(seen_links.size() == 2, "exactly the two survivors were delivered to");
    check(empty_routes == 0, "every surviving delivery still carries its stored return route");
    bool zero = false, two = false, one = false;
    for (std::size_t i = 0; i < seen_links.size(); ++i) {
        if (seen_links[i] == long_link(0) && seen_callers[i] == "caller0") zero = true;
        if (seen_links[i] == long_link(2) && seen_callers[i] == "caller2") two = true;
        if (seen_links[i] == long_link(1)) one = true;
    }
    check(zero && two, "each survivor's own link and caller bytes came through unchanged");
    check(!one, "the reclaimed edge delivered nothing");
}

/**
 * @brief (c) A DELIVERY's allocation count does not grow with the REMOTE fan-out (#1448).
 *
 * The second arm, and the hotter one: #1442/#1447 took the deep copy off the SUBSCRIBE path,
 * where it was paid once per admission, while `vertex_t::copy_published` kept paying it once
 * per remote edge **per write**. The dispatch snapshot now takes a refcount share of the same
 * immutable record, so a write's allocation count must be flat in the number of remote edges
 * it fans out to.
 *
 * The same instrument and the same shape-not-budget assertion as section (a), read on the
 * write instead of the subscribe. MEASURED ablation: put the two pre-#1448 `std::string`
 * copies back into `copy_entry` and the two compared points read **7 and 47** allocations
 * instead of 1 and 1, i.e. one per remote edge, and this section reddens with two failures
 * and exit 1. One rather than two because @ref long_link is deliberately past the SSO buffer
 * while `callerN` is not — which is also why the link is the field that must stay long: with
 * SSO-sized links the removed cost would never reach the counter at all.
 *
 * Each armed write is preceded by an UNARMED one at the same width. That is not hygiene, it
 * is what keeps the measurement about the per-edge term: the wide-fan-out overflow buffer is
 * a thread-local `std::vector` that keeps its capacity across publishes (`graph_t::fan_out`),
 * so the first write at a new width legitimately reallocates it exactly once, and counting
 * that growth would report a per-WIDTH cost as if it were a per-EDGE one.
 */
void test_delivery_allocation_is_flat() {
    std::printf("a delivery's allocation count is flat in the remote fan-out:\n");
    constexpr std::size_t kEdges = 48;
    constexpr std::size_t kEarly = 5;  // 5 remote edges — inside the inline snapshot
    constexpr std::size_t kLate = 45;  // 45 remote edges — through the overflow buffer
    graph_t g;
    const path_t src = *path_t::parse("/t/share/deliver");
    const auto v = g.register_vertex(src, role_t::STORED_VALUE);
    std::size_t delivered = 0;
    const tr::testing::remote_sink_guard_t sink(g, [&](const remote_delivery_t& d, const rope_t&) {
        // Touch the borrowed spellings: a snapshot that handed back a dangling view
        // rather than a held record is a read of freed bytes here, which is what the
        // ASan/TSan legs of this binary are for.
        delivered += d.link.size() + d.caller.size();
    });

    std::vector<std::size_t> per_write(kEdges, 0);
    for (std::size_t i = 0; i < kEdges; ++i) {
        check(g.subscribe_wire(v, subscriber_tlv(), route_tlv(), long_link(i), view_t{},
                               "caller" + std::to_string(i))
                  .has_value(),
              "every remote subscribe is admitted");
        const view_t warm = make_value({0x30, 0x00, 0x00, 0x00});
        const view_t armed = make_value({0x31, 0x00, 0x00, 0x00});
        check(g.write(src, warm).has_value(), "the un-armed settling write lands");
        g_allocs = 0;
        g_arm = true;
        const auto w = g.write(src, armed);
        g_arm = false;
        per_write[i] = g_allocs;
        check(w.has_value(), "the armed write lands");
        if (!w) return;
    }
    check(delivered > 0, "the sink actually read each delivery's link and caller bytes");

    const std::size_t early = per_write[kEarly];
    const std::size_t late = per_write[kLate];
    std::printf("    write at %zu remote edges: %zu allocations; at %zu edges: %zu allocations\n",
                kEarly + 1, early, kLate + 1, late);
    check(early > 0, "the counter is live (an armed write allocates SOMETHING — the LKV)");
    // Slack of 2 for the same reason section (a) takes it: the write's own store leg is
    // size-dependent and an allocator packaging difference could add a bucket. The defect
    // guarded against is one allocation per remote edge — 40 between these two points.
    check(late <= early + 2,
          "a write fanning out to 46 remote edges allocates no more than one to 6 — the "
          "dispatch snapshot SHARES the cold half instead of copying it");

    std::size_t worst = 0;
    for (std::size_t i = 0; i < kEdges; ++i) worst = worst > per_write[i] ? worst : per_write[i];
    check(worst <= early + 2, "and no write in the sweep spikes at all");
}

}  // namespace

int main() {
    std::printf("#1442 / #1448 shared subscription cold half\n\n");
    test_republish_allocation_is_flat();
    test_reclaim_leaves_the_survivors_intact();
    test_delivery_allocation_is_flat();
    return tr::testing::summary("edge_cold_half_share");
}
