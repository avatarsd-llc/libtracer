/**
 * @file
 * @brief #1713 — a STREAM write takes its vertex stripe lock ONCE and makes NO heap allocation
 *        in the common case: one locked admit-and-take into a stack-first buffer.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A STREAM write used to pay two stripe-lock sections — the receiver ring's admission, then a
 * separate drain — and a heap `std::vector` per write for the drain's snapshot (two
 * allocate/free pairs on a `-fno-exceptions` target, where the nothrow reserve probes first).
 * The write path now admits the entry and takes the unflushed window under the SAME lock hold,
 * into a buffer whose first `vertex_t::ring_take_t::kInline` slots live on the writer's stack.
 *
 * @section instrument The instruments, and what makes them non-vacuous
 *
 *  - **Stripe-lock sections.** The test links with `-Wl,--wrap=pthread_mutex_lock` (the
 *    `--wrap=send` precedent the httpd suites use) and counts acquisitions of ONE mutex: the
 *    stripe the STREAM vertex rides. Other locks are not the claim. The stripe is LEARNED
 *    from a positive control: `graph_t::ring_reserved_bytes` is a one-section verb, so it must
 *    make exactly one acquisition, and that acquisition names the stripe. A standard library whose
 * `std::mutex::lock` is not inlined (libc++) never reaches the wrap, and the lock half then reports
 * SKIP rather than passing on a dead counter. Under ThreadSanitizer `__real_pthread_mutex_lock` is
 *    TSan's own interceptor, so the lock model it checks is unchanged.
 *  - **Heap allocations.** Global `operator new` is replaced on the `batch_compose_alloc_test`
 *    precedent, armed only around the measured writes. The graph's value blocks and the ring's
 *    reservations come from INJECTED sources that are malloc-backed, so the one allocation a
 *    publish legitimately costs (RFC-0028 §5.1) is counted at its own seam and never hides in,
 *    or pads, the heap count. Positive control: an armed `new` must count. The warm-up runs
 *    past one hazard retire batch: under `hazard_slot_t` the LKV publish draws its reclamation
 *    node from the global heap until the thread's first scan stocks its free list (the #873
 *    carve-out in `lkv_slot.hpp`), which is a one-time per-thread cost, not the write's.
 *
 * The concurrent arm is the TSan half: four writers on one STREAM vertex, every value
 * delivered exactly once — the fused take must neither lose nor duplicate an entry.
 */

#include <pthread.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set on this thread. */
thread_local std::size_t g_allocs = 0;
/** @brief Arms @ref g_allocs (per thread, so the concurrent arm's workers never count). */
thread_local bool g_arm = false;

/** @brief The counted allocation — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) ++g_allocs;
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation — `aligned_alloc` only when genuinely over-aligned. */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) ++g_allocs;
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

/** @brief The one mutex whose acquisitions are counted (null ⇒ count nothing). */
std::atomic<pthread_mutex_t*> g_watch{nullptr};
/** @brief Acquisitions of @ref g_watch on this thread. */
thread_local std::size_t g_locks = 0;
/** @brief Acquisitions of ANY mutex on this thread (the control's learning count). */
thread_local std::size_t g_any = 0;
/** @brief The mutex this thread acquired last (the control learns the stripe from it). */
thread_local pthread_mutex_t* g_last = nullptr;

}  // namespace

extern "C" {
/** @brief The real `pthread_mutex_lock` (`-Wl,--wrap`), TSan's interceptor under TSan. */
int __real_pthread_mutex_lock(pthread_mutex_t* m);  // NOLINT(bugprone-reserved-identifier)

/** @brief Count an acquisition of the watched stripe mutex, then take it for real. */
int __wrap_pthread_mutex_lock(pthread_mutex_t* m) {  // NOLINT(bugprone-reserved-identifier)
    if (m == g_watch.load(std::memory_order_relaxed)) ++g_locks;
    ++g_any;
    g_last = m;
    return __real_pthread_mutex_lock(m);
}
}

/** @brief Counted replacement of the global allocation form. */
void* operator new(std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
/** @brief Counted replacement of the global array allocation form. */
void* operator new[](std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
/** @brief Counted replacement of the nothrow form. */
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
/** @brief Counted replacement of the nothrow array form. */
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
/** @brief Counted replacement of the aligned form. */
void* operator new(std::size_t n, std::align_val_t a) {
    void* const p = counted_aligned(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
/** @brief Counted replacement of the aligned array form. */
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
/** @brief Counted replacement of the aligned nothrow form. */
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
/** @brief Counted replacement of the aligned nothrow array form. */
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
/** @brief Matching release (malloc-backed). */
void operator delete(void* p) noexcept { std::free(p); }
/** @brief Matching release (malloc-backed). */
void operator delete[](void* p) noexcept { std::free(p); }
/** @brief Matching release (malloc-backed). */
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
/** @brief Matching release (malloc-backed). */
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
/** @brief Matching release (malloc-backed). */
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
/** @brief Matching release (malloc-backed). */
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
/** @brief Matching release (malloc-backed). */
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
/** @brief Matching release (malloc-backed). */
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
/** @brief Matching release (malloc-backed). */
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
/** @brief Matching release (malloc-backed). */
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;

/**
 * @brief A malloc-backed source counting its serves — so its blocks never touch the counted
 *        global `operator new`, and "how many blocks did this seam serve" is a direct read.
 */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Named for the census, like every other source. */
    counting_source_t() noexcept : block_source_t("test-counting") {}
    /** @brief Serve from `aligned_alloc`; count the serve. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        const std::size_t a = align < alignof(std::max_align_t) ? alignof(std::max_align_t) : align;
        const std::size_t rounded = ((bytes == 0 ? 1 : bytes) + a - 1) / a * a;
        void* const p = std::aligned_alloc(a, rounded);
        if (p != nullptr) served.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    /** @brief Sized reclaim matching @ref try_alloc. */
    void release(void* p, std::size_t /*bytes*/, std::size_t /*align*/) noexcept override {
        std::free(p);
    }
    std::atomic<std::size_t> served{0}; /**< @brief Blocks served so far. */
};

/** @brief How many writes each measured arm makes. */
constexpr std::size_t kWrites = 8;

/** @brief A fixed-capacity, allocation-free delivery log — the callback must not allocate. */
struct seen_t {
    std::array<std::uint8_t, 64> v{}; /**< @brief First byte of each delivered value, in order. */
    std::size_t n = 0;                /**< @brief Entries delivered. */
};

/** @brief The recording callback: append the delivered value's first byte (no allocation). */
void record_cb(void* ctx, const tr::graph::value_t& value) {
    auto* s = static_cast<seen_t*>(ctx);
    if (s->n < s->v.size()) s->v[s->n] = std::to_integer<std::uint8_t>(value.only().bytes()[0]);
    ++s->n;
}

/**
 * @brief LEARN the stripe mutex @p v rides, and prove the lock instrument is live.
 *
 * `graph_t::ring_reserved_bytes` is a one-section verb: it takes @p v's stripe and no other
 * mutex. So the one acquisition it makes IS the stripe, which is how the test names it without
 * a door to the vertex's address. The learned mutex is then the only one counted.
 * @return false when the wrap is never reached (the standard library's mutex is out of line).
 */
bool lock_instrument_live(graph_t& g, vertex_handle_t v) {
    g_watch.store(nullptr, std::memory_order_relaxed);
    g_any = 0;
    g_last = nullptr;
    (void)g.ring_reserved_bytes(v);
    if (g_any == 0) {
        std::printf("  SKIP lock half: the wrap is not reached by this standard library\n");
        return false;
    }
    check(g_any == 1, "control: ring_reserved_bytes is exactly one mutex section");
    g_watch.store(g_last, std::memory_order_relaxed);
    return g_any == 1;
}

/** @brief Zero this thread's count of the watched stripe's acquisitions. */
void watch_stripe() { g_locks = 0; }

/** @brief The common case: a steady-state STREAM write is ONE stripe section and ZERO heap. */
void test_stream_write_one_lock_no_heap() {
    std::printf("STREAM write — one stripe section, no heap allocation (#1713):\n");
    counting_source_t values;  // MUST outlive the graph: its blocks are released into it
    counting_source_t ring;
    graph_t g(values);
    auto v = g.register_vertex(path_t("/s/one"), role_t::STREAM);
    check(
        g.set_policy(v, {.retention = tr::graph::retention_t::N, .depth = 4, .ring_source = &ring})
            .has_value(),
        "the receiver declares depth 4 against its own ring source");
    seen_t seen;
    check(g.subscribe(path_t("/s/one"), record_cb, &seen).has_value(), "one subscriber");

    // Warm-up: the first append creates the ring state and fills it to its depth, after which
    // every admission is funded by the retiring entry's reservation (carried, not re-asked).
    // It runs past TWO hazard retire batches (`kRetireBatch` is `kHazardReaderSlots`): under
    // `hazard_slot_t` each LKV publish displaces a reclamation node, and the thread's free list
    // is stocked only by its first scan — until then a publish allocates its node on the global
    // heap ON PURPOSE (the #873 carve-out). A no-op under the other slot bindings.
    constexpr std::size_t kWarmup = 2 * tr::graph::kHazardReaderSlots + 4;
    bool warm_ok = true;
    for (std::size_t i = 0; i < kWarmup; ++i)
        warm_ok = g.write(v, make_value({static_cast<std::uint8_t>(i)})).has_value() && warm_ok;
    check(warm_ok, "every warm-up write succeeds");
    check(seen.n == kWarmup, "warm-up delivered every write once");
    seen = seen_t{};  // the measured log starts empty, so its fixed capacity covers kWrites

    // The payloads are minted BEFORE arming: `make_value` heap-allocates the test's segment,
    // which is the caller's cost, not the write's.
    std::vector<tr::view::view_t> payloads;
    payloads.reserve(kWrites);
    for (std::size_t i = 0; i < kWrites; ++i)
        payloads.push_back(make_value({static_cast<std::uint8_t>(0x40 + i)}));

    {
        g_arm = true;
        g_allocs = 0;
        void* const probe = ::operator new(1);  // positive control: the counter is live
        const std::size_t control = g_allocs;
        ::operator delete(probe);
        g_arm = false;
        check(control == 1, "control: an armed global new is counted");
    }
    const bool locks = lock_instrument_live(g, v);

    const std::size_t values_before = values.served.load();
    const std::size_t ring_before = ring.served.load();
    const std::size_t seen_before = seen.n;
    std::size_t max_locks = 0;
    std::size_t min_locks = SIZE_MAX;
    std::size_t heap = 0;
    for (tr::view::view_t& p : payloads) {
        watch_stripe();
        g_allocs = 0;
        g_arm = true;
        const bool ok = g.write(v, std::move(p)).has_value();
        g_arm = false;
        heap += g_allocs;
        max_locks = std::max(max_locks, g_locks);
        min_locks = std::min(min_locks, g_locks);
        check(ok, "the measured write succeeds");
    }
    g_watch.store(nullptr, std::memory_order_relaxed);

    check(seen.n - seen_before == kWrites, "every measured write delivered exactly once");
    bool in_order = true;
    for (std::size_t i = 0; i < kWrites; ++i)
        in_order = in_order && seen.v[seen_before + i] == 0x40 + i;
    check(in_order, "... in write order");
    check(heap == 0, "NO global heap allocation on the steady-state STREAM write");
    check(values.served.load() - values_before == kWrites,
          "exactly ONE value block per write, from the graph's injected source");
    check(ring.served.load() == ring_before,
          "the ring's reservations are carried over — zero source calls in steady state");
    if (locks)
        check(min_locks == 1 && max_locks == 1,
              "the STREAM write takes its stripe lock exactly ONCE (admit and take fused)");
    (void)g.propagate(v);
    check(seen.n - seen_before == kWrites, "a covering sweep re-delivers nothing");
}

/**
 * @brief A backlog wider than the stack slots spills, still in one section, still in order,
 *        and still exactly once — the stack-first buffer's overflow arm.
 */
void test_backlog_spills_in_order() {
    std::printf("STREAM write over a backlog — the take spills, in order, once:\n");
    counting_source_t ring;
    graph_t g;
    auto v = g.register_vertex(path_t("/s/backlog"), role_t::STREAM);
    (void)g.set_policy(v,
                       {.retention = tr::graph::retention_t::N, .depth = 16, .ring_source = &ring});
    seen_t seen;
    (void)g.subscribe(path_t("/s/backlog"), record_cb, &seen);

    constexpr std::size_t kBacklog = tr::graph::vertex_t::ring_take_t::kInline + 2;
    for (std::size_t i = 0; i < kBacklog; ++i)
        check(g.assign(v, make_value({static_cast<std::uint8_t>(0x10 + i)})).has_value(),
              "assign queues without delivering");
    check(seen.n == 0, "assign delivered nothing");

    const bool locks = lock_instrument_live(g, v);
    watch_stripe();
    check(g.write(v, make_value({0x7F})).has_value(), "the write over the backlog succeeds");
    const std::size_t n_locks = g_locks;
    g_watch.store(nullptr, std::memory_order_relaxed);

    check(seen.n == kBacklog + 1, "the write delivered the whole unflushed window");
    bool in_order = true;
    for (std::size_t i = 0; i < kBacklog; ++i) in_order = in_order && seen.v[i] == 0x10 + i;
    check(in_order && seen.v[kBacklog] == 0x7F, "... oldest first, the write's own entry last");
    if (locks) check(n_locks == 1, "and the spill still took ONE stripe section");
    (void)g.propagate(v);
    check(seen.n == kBacklog + 1, "a covering sweep re-delivers nothing");
}

/** @brief Per-writer delivery tally for the concurrent arm. */
struct tally_t {
    static constexpr std::size_t kThreads = 4;      /**< @brief Concurrent writers. */
    static constexpr std::size_t kPerThread = 2000; /**< @brief Writes per writer. */
    /** @brief Deliveries per (writer, index) — each must end at exactly 1. */
    std::array<std::atomic<std::uint8_t>, kThreads * kPerThread> hits{};
};

/** @brief Concurrent-arm callback: the 3-byte big-endian payload is the entry's global id
 *         (`writer * kPerThread + index`); count one delivery against it. */
void tally_cb(void* ctx, const tr::graph::value_t& value) {
    auto* t = static_cast<tally_t*>(ctx);
    const auto b = value.only().bytes();
    const std::size_t id = (std::to_integer<std::size_t>(b[0]) << 16) |
                           (std::to_integer<std::size_t>(b[1]) << 8) |
                           std::to_integer<std::size_t>(b[2]);
    if (id < t->hits.size()) t->hits[id].fetch_add(1, std::memory_order_relaxed);
}

/** @brief Four writers on one STREAM vertex: every value is delivered exactly once (TSan). */
void test_concurrent_writers_exactly_once() {
    std::printf("STREAM write — concurrent writers, every entry delivered exactly once:\n");
    graph_t g;
    auto v = g.register_vertex(path_t("/s/mt"), role_t::STREAM);
    (void)g.set_policy(v, {.retention = tr::graph::retention_t::N, .depth = 64});
    auto t = std::make_unique<tally_t>();
    (void)g.subscribe(path_t("/s/mt"), tally_cb, t.get());

    std::vector<std::thread> writers;
    std::atomic<std::size_t> failed{0};
    for (std::size_t w = 0; w < tally_t::kThreads; ++w) {
        writers.emplace_back([&g, v, w, &failed] {
            for (std::size_t i = 0; i < tally_t::kPerThread; ++i) {
                const std::size_t id = w * tally_t::kPerThread + i;
                if (!g.write(v, make_value({static_cast<std::uint8_t>(id >> 16),
                                            static_cast<std::uint8_t>(id >> 8),
                                            static_cast<std::uint8_t>(id)}))
                         .has_value())
                    failed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& th : writers) th.join();
    (void)g.propagate(v);  // a trailing sweep must find nothing left to deliver

    std::size_t once = 0;
    std::size_t other = 0;
    for (const auto& h : t->hits) (h.load() == 1 ? once : other) += 1;
    check(failed.load() == 0, "every concurrent write succeeded");
    check(once == t->hits.size() && other == 0,
          "every entry was delivered EXACTLY once — none lost, none duplicated");
    std::vector<tr::graph::value_ref_t> rest;
    std::uint64_t gaps = 1;
    check(g.drain_unflushed(v, rest, &gaps).has_value() && rest.empty() && gaps == 0,
          "nothing was left unflushed, and no shed point was recorded");
}

}  // namespace

int main() {
    std::printf("#1713 — single-lock STREAM admission into a stack-first buffer\n\n");
    test_stream_write_one_lock_no_heap();
    test_backlog_spills_in_order();
    test_concurrent_writers_exactly_once();
    return tr::testing::summary("stream_admit_one_lock");
}
