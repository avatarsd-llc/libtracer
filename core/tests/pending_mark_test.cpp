/**
 * @file
 * @brief #1712 — the per-vertex pending-mark hint: an eager write to an UNMARKED vertex pays
 *        no key render, no allocation and no graph-wide sweep lock, however many other
 *        vertices hold an `assign` mark (RFC-0008 §B, ADR-0057).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every observed eager write ends in `graph_t::clear_pending`, which retires the vertex's own
 * IF_NEWER mark. Its only fast path used to be graph-wide — "is ANY mark pending?" — so one
 * outstanding `assign` anywhere put a key render (one heap block), and the sweep lock every
 * marker and every covering sweep also takes, on every observed write in the whole graph. The
 * hint is a spare `vertex_t` flag bit (+0 B) raised with the mark and dropped with it; a write
 * whose vertex has it down returns before the render.
 *
 * @section instrument What makes the allocation claim non-vacuous
 *
 * The lock itself is not observable from the graph API, but the key render sits in front of
 * it on the same path, and it is one heap block. So the instrument is a global `operator new`
 * counter (the `handler_write_alloc_test` precedent: every allocating and deallocating form
 * replaced), and the assertion is a DIFFERENCE between two arms that run the same writes to
 * the same vertex: with nothing marked, and with another vertex marked. Before #1712 the
 * second arm costs one more block per write (the render) — measured, not assumed: the test
 * was run red against the pre-#1712 gate. The control that the mark really was pending for
 * the whole counted window is a covering sweep that must then deliver it.
 *
 * @section race The mark/clear race (TSan)
 *
 * The hint has three writers — a marker raising it under the sweep lock, an eager write
 * dropping it under the same lock, and a covering sweep dropping it AFTER the lock, on another
 * thread — and the eager write reads it with no lock at all. The race test runs all of them at once
 * on shared leaves; the tsan CI lane runs this suite, so a plain-memory access on any of those
 * paths is a report. It also checks the property the hint must never break: after the threads
 * quiesce and one covering sweep runs, every leaf's CURRENT value has been delivered. A hint
 * that wrongly retired a mark would lose exactly that value.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. Atomic because the race
 *         test below allocates from several threads at once (unarmed, but still read). */
std::atomic<std::size_t> g_allocs{0};
/** @brief Arms @ref g_allocs. */
std::atomic<bool> g_arm{false};

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation — `aligned_alloc` only for a genuinely OVER-aligned
 *         request (which `free` accepts), `malloc` for a fundamental one. */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

}  // namespace

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
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief Writes per counted arm; the settle pass takes the one-offs out of the window. */
constexpr std::size_t kWrites = 200;
/** @brief The band the two arms must agree within, in blocks per write. The pre-#1712 gap is
 *         a whole block per write; the band absorbs a deferred reclamation slot's amortized
 *         retire batches, which both arms feed identically. */
constexpr double kBand = 0.5;

/** @brief Allocations per eager write to @p v, over @ref kWrites writes of one prebuilt value
 *         (copying a `view_t` is a refcount bump, so the value itself allocates nothing). */
[[nodiscard]] double per_write_allocs(graph_t& g, vertex_handle_t v) {
    const tr::view::view_t value = make_value({0x5a, 0x5b, 0x5c, 0x5d});
    for (std::size_t i = 0; i < 50; ++i) (void)g.write(v, value);  // settle the one-offs
    g_allocs.store(0, std::memory_order_relaxed);
    g_arm.store(true, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kWrites; ++i) (void)g.write(v, value);
    g_arm.store(false, std::memory_order_relaxed);
    return static_cast<double>(g_allocs.load(std::memory_order_relaxed)) / kWrites;
}

/**
 * @brief A pending mark on one vertex costs an eager write to ANOTHER vertex nothing — and
 *        once the mark is retired, by either retirer, it costs the marked vertex nothing.
 */
void test_unmarked_write_skips_the_sweep_path() {
    std::printf("unmarked eager write vs a pending mark elsewhere (#1712):\n");
    graph_t g;
    auto p = g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    auto a = g.register_vertex(path_t("/p/a"), role_t::STORED_VALUE);
    auto b = g.register_vertex(path_t("/q/b"), role_t::STORED_VALUE);
    auto c = g.register_vertex(path_t("/q/c"), role_t::STORED_VALUE);
    std::atomic<int> hits_a{0};
    auto on_a = [&hits_a](const tr::graph::value_t&) {
        hits_a.fetch_add(1, std::memory_order_relaxed);
    };
    auto on_any = [](const tr::graph::value_t&) {};
    // Every vertex is observed: an unobserved one never reaches the mark path at all, and the
    // claim is about the observed write the old graph-wide gate taxed.
    (void)g.subscribe(path_t("/p/a"), on_a);
    (void)g.subscribe(path_t("/q/b"), on_any);
    (void)g.subscribe(path_t("/q/c"), on_any);

    const double b_idle = per_write_allocs(g, b);
    check(b_idle > 0.0, "an armed stored write allocates (the counter is live)");

    // Mark /p/a and leave it pending across the whole counted window.
    const int a_before = hits_a.load(std::memory_order_relaxed);
    check(g.assign(a, make_value({0x01})).has_value(), "assign /p/a is admitted");
    const double b_marked = per_write_allocs(g, b);
    std::printf("  /q/b: %.3f blocks/write idle, %.3f with /p/a marked\n", b_idle, b_marked);
    check(b_marked - b_idle < kBand,
          "a write to an unmarked vertex renders no key while another vertex is marked");
    (void)g.propagate(p);
    check(hits_a.load(std::memory_order_relaxed) == a_before + 1,
          "the mark on /p/a was pending for the whole counted window (the sweep delivers it)");

    // The SWEEP's post-lock drop: /p/a's mark went with the propagate above. Keep the graph
    // non-empty (mark /q/c) so only the per-vertex hint can take /p/a off the slow path.
    const double a_idle = per_write_allocs(g, a);  // /p/a: nothing marked anywhere
    check(g.assign(c, make_value({0x02})).has_value(), "assign /q/c is admitted");
    check(g.assign(a, make_value({0x03})).has_value(), "assign /p/a is admitted again");
    (void)g.propagate(p);  // drains /p/a only — /q/c stays marked
    const double a_swept = per_write_allocs(g, a);
    std::printf("  /p/a: %.3f blocks/write idle, %.3f after a sweep drained it\n", a_idle, a_swept);
    check(a_swept - a_idle < kBand, "a covering sweep drops the hint with the mark it drains");

    // The EAGER WRITE's drop: one write retires /p/a's mark (the slow path, once), and every
    // later write to it is back on the fast path while /q/c is still marked.
    check(g.assign(a, make_value({0x04})).has_value(), "assign /p/a is admitted a third time");
    (void)g.write(a, make_value({0x05}));  // retires the mark, drops the hint
    const double a_written = per_write_allocs(g, a);
    std::printf("  /p/a: %.3f blocks/write after an eager write retired its mark\n", a_written);
    check(a_written - a_idle < kBand, "an eager write drops the hint with the mark it retires");
}

/**
 * @brief A mark landing between a sweep's drain and its hint drop keeps its hint, so the eager
 *        write that follows retires it and the next sweep does not re-deliver.
 *
 * The window is driven deterministically: the sweep delivers `/s/a` before `/s/b` (key order),
 * and `/s/a`'s callback re-marks `/s/b` — AFTER the drain took `/s/b`'s first mark, BEFORE
 * `/s/b` is delivered. A drop that does not re-check the set clears the re-marked hint; the
 * write to `/s/b` then skips its retire and the closing sweep delivers that write's value a
 * second time.
 */
void test_remark_inside_the_sweep_window() {
    std::printf("re-mark inside a sweep's drain-to-drop window (#1712):\n");
    graph_t g;
    auto s = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    auto a = g.register_vertex(path_t("/s/a"), role_t::STORED_VALUE);
    auto b = g.register_vertex(path_t("/s/b"), role_t::STORED_VALUE);
    int hits_b = 0;
    bool armed = true;
    auto on_a = [&g, b, &armed](const tr::graph::value_t&) {
        if (!armed) return;
        armed = false;
        (void)g.assign(b, make_value({0x22}));  // re-marks /s/b inside the window
    };
    auto on_b = [&hits_b](const tr::graph::value_t&) { ++hits_b; };
    (void)g.subscribe(path_t("/s/a"), on_a);
    (void)g.subscribe(path_t("/s/b"), on_b);

    (void)g.assign(a, make_value({0x11}));
    (void)g.assign(b, make_value({0x21}));
    (void)g.propagate(s);
    check(!armed, "the earlier vertex's callback ran inside the sweep (the window was driven)");
    check(hits_b == 1, "the sweep delivers /s/b once, with its current value");
    (void)g.write(b, make_value({0x23}));
    check(hits_b == 2, "the eager write delivers its own value");
    (void)g.propagate(s);
    check(hits_b == 2, "the closing sweep does not re-deliver what the write delivered");
}

/** @brief Leaves the race runs on — several, so the sweep has more than one key to drain. */
constexpr std::size_t kLeaves = 4;
/** @brief Operations per racing thread. */
constexpr std::uint16_t kOps = 4000;

/** @brief Every value one leaf's subscriber has seen, keyed by the 16-bit tag it carries. */
struct seen_t {
    std::mutex m;                                          /**< @brief Guards @ref tags. */
    std::vector<bool> tags = std::vector<bool>(1U << 16U); /**< @brief Delivered tags. */
};

/** @brief A leaf's subscriber: records every tag delivered to it in @ref seen. */
struct seen_sink_t {
    seen_t* seen = nullptr; /**< @brief Where the tags go. */

    /** @brief Record one delivery. */
    void operator()(const tr::graph::value_t& v) const;
};

/** @brief A value carrying the 16-bit @p tag — unique per operation, so delivery is traceable. */
[[nodiscard]] tr::view::view_t tagged(std::uint16_t tag) {
    return make_value({static_cast<std::uint8_t>(tag >> 8U), static_cast<std::uint8_t>(tag)});
}

/** @brief The tag @p v carries. */
[[nodiscard]] std::uint16_t tag_of(const tr::graph::value_t& v) {
    const auto bytes = v.only().bytes();
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(bytes[0]) << 8U) |
                                      std::to_integer<unsigned>(bytes[1]));
}

void seen_sink_t::operator()(const tr::graph::value_t& v) const {
    const std::lock_guard lock(seen->m);
    seen->tags[tag_of(v)] = true;
}

/**
 * @brief Markers, eager writers and covering sweeps on the same leaves at once: no data race
 *        on the hint, and no value lost to it.
 */
void test_mark_clear_race() {
    std::printf("mark / eager clear / sweep clear race (#1712):\n");
    graph_t g;
    auto r = g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    std::vector<vertex_handle_t> leaves;
    leaves.reserve(kLeaves);
    std::array<seen_t, kLeaves> seen;
    // The callable form binds by ADDRESS, so the sinks live as long as the graph's edges.
    std::array<seen_sink_t, kLeaves> sinks{};
    for (std::size_t i = 0; i < kLeaves; ++i) {
        const std::string spelling = "/r/x" + std::to_string(i);
        leaves.push_back(g.register_vertex(path_t(spelling), role_t::STORED_VALUE));
        sinks[i].seen = &seen[i];
        (void)g.subscribe(path_t(spelling), sinks[i]);
    }

    // One PUBLISHER per leaf, alternating assign (raises the hint) and eager write (drops it
    // under the sweep lock) — one writer per vertex, the calling plane's contract, so the
    // write sequence and the LKV agree on what "current" is. The sweeper drops hints AFTER
    // the sweep lock, on its own thread: that is the cross-thread mark/clear race. Every tag
    // is unique, so a seen tag names the one operation that published it.
    std::vector<std::thread> publishers;
    publishers.reserve(kLeaves);
    for (std::size_t leaf = 0; leaf < kLeaves; ++leaf) {
        publishers.emplace_back([&g, &leaves, leaf] {
            for (std::uint16_t i = 1; i <= kOps; ++i) {
                const tr::view::view_t v = tagged(i);
                if ((i & 1U) != 0)
                    (void)g.assign(leaves[leaf], v);
                else
                    (void)g.write(leaves[leaf], v);
            }
        });
    }
    std::thread sweeper([&g, r] {
        for (std::uint16_t i = 0; i < kOps; ++i) (void)g.propagate(r);
    });
    for (std::thread& t : publishers) t.join();
    sweeper.join();

    // Quiesced. One covering sweep delivers whatever marks survived; after it, every leaf's
    // current value must have reached its subscriber at least once.
    (void)g.propagate(r);
    bool all = true;
    for (std::size_t i = 0; i < kLeaves; ++i) {
        auto cur = g.read(leaves[i]);
        if (!cur.has_value()) {
            all = false;
            continue;
        }
        const std::uint16_t tag = tag_of(**cur);
        const std::lock_guard lock(seen[i].m);
        all = all && seen[i].tags[tag];
    }
    check(all,
          "after quiescence and one covering sweep, every leaf's current value was "
          "delivered (no mark lost to the hint)");
}

}  // namespace

int main() {
    test_unmarked_write_skips_the_sweep_path();
    test_remark_inside_the_sweep_window();
    test_mark_clear_race();
    return tr::testing::summary("pending_mark_test");
}
