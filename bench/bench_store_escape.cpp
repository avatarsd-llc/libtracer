/**
 * @file
 * @brief What ESCAPES to the process heap under each ADR-0079 store composition — the number
 *        that says whether a "bounded node" is actually bounded (#941, #873).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The companion to `bench_store_sweep.cpp`, composing the SAME node
 * (`bench/store_sweep_node.hpp`) under the same four arms, untimed, with the global
 * `operator new` / `delete` override set from `bench_conn_ram.cpp` counting every byte the
 * node takes from the process heap. It is a separate binary on purpose: that override costs a
 * relaxed atomic load per allocation, on arms that allocate at different rates, so it cannot
 * live in a TU whose job is to compare their latency.
 *
 * @section windows Two windows, and why both are reported
 *
 *  * `build`  — the graph, the router and every lane constructed. This is where `vertex_t`
 *    placement lands, and it is why the escape figure will NOT be zero in any arm: vertices are
 *    still `std::make_unique<vertex_t>` on the global heap (ADR-0079 **Stage 2**, #843, gated
 *    on #1285, not landed). The injected SLABS are allocated before the window opens and are
 *    deliberately not counted — they are the deployer's declared budget, and counting them
 *    would report the most bounded arm as the most expensive one on the page.
 *  * `steady` — @ref kOpsPerLane workload iterations per lane, after the build window closed.
 *    This is the one that answers "is the hot path bounded": what a peer can still drive onto
 *    the process heap once the node is up.
 *
 * @section trust How much each column is worth
 *
 * `bench/ram_census_pins.json` records a transport high-water moving **66 % across runs and
 * ~41 KB within one run**, which is why the high-water columns are excluded from
 * `ram_census.py`'s pinned rows. The same caution applies here and is not optional: the peak
 * columns below are reported best-of-rounds with their full spread by
 * `bench/collate_store_sweep.py`, they carry their own null band, and they are **never gated**.
 * The LIVE-delta columns are far steadier, and the store-side `used()` high-water in
 * `bench_store_sweep hwm` is the deterministic one. Read them in that order of trust.
 *
 * @section tags Output
 *
 *     RESULT_STORE_ESCAPE round tag arm window live_delta peak_delta allocs frees
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if __has_include(<malloc.h>)
#include <malloc.h>
#define BENCH_HAS_USABLE_SIZE 1
#endif

// --- the counting allocator override (all variants) ------------------------------------------
//
// Verbatim in shape from bench_conn_ram.cpp:96-167, which is the in-tree instrument for this
// question. Every variant must be overridden: `heap_alloc` uses the aligned-nothrow form and
// the STL uses the plain one, so covering only one leaves half the escapes invisible.

namespace {

/** @brief Live usable-size balance while armed — the process heap the node holds. */
std::atomic<long long> g_live{0};
/** @brief High-water mark of @ref g_live — catches TRANSIENT per-frame buffers. */
std::atomic<long long> g_peak{0};
/** @brief Blocks allocated / freed while armed. */
std::atomic<long long> g_allocs{0};
std::atomic<long long> g_frees{0};
/** @brief Counting is on. */
std::atomic<bool> g_armed{false};

/** @brief Raise @ref g_peak to @p now if @p now is higher. */
void bump_peak(long long now) {
    long long seen = g_peak.load(std::memory_order_relaxed);
    while (now > seen && !g_peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
}

/** @brief `malloc` plus the accounting, when armed. */
void* counted_alloc(std::size_t size) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (g_armed.load(std::memory_order_relaxed) && p != nullptr) {
        g_allocs.fetch_add(1, std::memory_order_relaxed);
#ifdef BENCH_HAS_USABLE_SIZE
        const long long now = g_live.fetch_add(static_cast<long long>(malloc_usable_size(p)),
                                               std::memory_order_relaxed) +
                              static_cast<long long>(malloc_usable_size(p));
        bump_peak(now);
#endif
    }
    return p;
}

/** @brief `free` plus the accounting, when armed. */
void counted_free(void* p) {
    if (p == nullptr) return;
    if (g_armed.load(std::memory_order_relaxed)) {
        g_frees.fetch_add(1, std::memory_order_relaxed);
#ifdef BENCH_HAS_USABLE_SIZE
        g_live.fetch_sub(static_cast<long long>(malloc_usable_size(p)), std::memory_order_relaxed);
#endif
    }
    std::free(p);
}

}  // namespace

void* operator new(std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new(std::size_t size, std::align_val_t) { return operator new(size); }
void* operator new(std::size_t size, std::align_val_t, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new[](std::size_t size, std::align_val_t) { return operator new(size); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }

// --- harness ---------------------------------------------------------------------------------

#include "store_sweep_node.hpp"

namespace {

using bench_store::arm_t;
using bench_store::counting_source_t;
using bench_store::kArms;
using bench_store::name_of;
using bench_store::node_t;

/** @brief Lanes the census composes — one, so the figure is per-lane and comparable to `hwm`. */
constexpr std::size_t kLanes = 1;

/** @brief Workload iterations per lane inside the `steady` window. */
constexpr std::size_t kOpsPerLane = 256;

/** @brief Elements the disjointness / escape canaries push through a `block_array_t`. */
constexpr std::size_t kCanaryElems = 64;

/** @brief Current live balance. */
[[nodiscard]] long long live() { return g_live.load(std::memory_order_relaxed); }
/** @brief Re-seat the high-water at the current balance. */
void reset_peak() { g_peak.store(live(), std::memory_order_relaxed); }
/** @brief The high-water since the last @ref reset_peak. */
[[nodiscard]] long long peak() { return g_peak.load(std::memory_order_relaxed); }

/** @brief `RESULT_STORE_ESCAPE` — one arm's one window. */
void emit_escape(int round, const char* tag, const char* arm, const char* window, long long live_d,
                 long long peak_d, long long allocs, long long frees) {
    std::printf("RESULT_STORE_ESCAPE\t%d\t%s\t%s\t%s\t%lld\t%lld\t%lld\t%lld\n", round, tag, arm,
                window, live_d, peak_d, allocs, frees);
    std::fflush(stdout);
}

/** @brief Arm the process-heap counter — `node_t`'s post-stores hook. */
void arm_counter(void*) {
    g_live = 0;
    g_allocs = 0;
    g_frees = 0;
    reset_peak();
    g_armed = true;
}

/**
 * @brief Census one arm: the build window, then the steady-state window.
 *
 * The counter arms through `node_t`'s post-stores hook, so every slab and pool this arm
 * declares is already allocated and OUTSIDE the window. That is the honest split: the slab is
 * the deployer's stated budget, and counting it as node heap would report the most bounded arm
 * as the most expensive one. What the `build` column then holds is what the node reached for
 * BEYOND its slab while standing up — which, with vertex placement still on the global heap
 * (ADR-0079 Stage 2 / #843), is dominated by `vertex_t`.
 */
void census_arm(int round, const char* tag, arm_t arm) {
    long long build_live = 0;
    long long build_peak = 0;
    long long build_allocs = 0;
    long long build_frees = 0;
    {
        node_t n(arm, kLanes, /*count_channels=*/false, &arm_counter, nullptr);
        build_live = live();
        build_peak = peak();
        build_allocs = g_allocs.load();
        build_frees = g_frees.load();

        const long long s0 = live();
        reset_peak();
        const long long a0 = g_allocs.load();
        const long long f0 = g_frees.load();
        for (std::size_t k = 0; k < kOpsPerLane; ++k) n.step_full(n.lane(0));
        const long long s1 = live();
        const long long p1 = peak();
        const long long a1 = g_allocs.load();
        const long long f1 = g_frees.load();
        g_armed = false;

        emit_escape(round, tag, name_of(arm), "build", build_live, build_peak, build_allocs,
                    build_frees);
        emit_escape(round, tag, name_of(arm), "steady", s1 - s0, p1 - s0, a1 - a0, f1 - f0);
        if (!n.faults("escape", kLanes)) {
            std::fprintf(stderr, "FAULT %s: escape census point is not trustworthy\n",
                         name_of(arm));
        }
    }
    g_armed = false;
}

/**
 * @brief The escape-side non-vacuity gate — the half `bench_store_sweep calibrate` cannot run.
 *
 *  1. **The escape canary.** A `block_array_t` over `tr::mem::heap_source()` MUST be seen to
 *     escape. If it reads zero, the `operator new` override is blind — elided, LTO'd away, or a
 *     different allocator linked in — and every "escape" figure on the page is worthless.
 *     `bench_failable_census.cpp::census_canary_heap_escape` is the same row at a different
 *     seam.
 *  2. **Disjointness.** The identical array over a @ref bench_store::counting_source_t with NO
 *     inner store must show the seam serving and NOTHING escaping. That decorator serves from
 *     `std::aligned_alloc`, never `::operator new`, for exactly this reason — the pre-#1402
 *     defect made the two columns identical and no row could report the zero-escape result.
 *  3. **The null arm.** Two balance snapshots with nothing between them must differ by exactly
 *     0 (`bench_conn_ram.cpp`'s rule): the instrument must not allocate inside its own window.
 *  4. **H-baseline escapes.** The real workload on the shipping composition must show a
 *     non-zero steady-state escape. A zero here would mean the workload is not reaching the
 *     heap at all, which would make every arm's "bounded" claim free.
 *
 * @return 0 when every check passed.
 */
[[nodiscard]] int run_calibrate() {
    int failures = 0;
    const auto check = [&failures](const char* what, bool ok, const char* detail) {
        std::printf("CALIBRATE\t%s\t%s\t%s\n", what, ok ? "OK" : "FAIL", detail);
        if (!ok) ++failures;
    };
    char buf[128];

    // (1) The escape canary.
    {
        g_allocs = 0;
        g_live = 0;
        g_armed = true;
        {
            tr::mem::block_array_t<std::uint64_t> a(tr::mem::heap_source());
            for (std::size_t i = 0; i < kCanaryElems; ++i) {
                std::uint64_t* slot = a.push_slot();
                if (slot == nullptr) break;
                *slot = i;
            }
            asm volatile("" : : "r"(a.data()) : "memory");
        }
        g_armed = false;
        const long long escaped = g_allocs.load();
        std::snprintf(buf, sizeof(buf), "heap_allocs=%lld", escaped);
        check("escape_canary_override_is_live", escaped > 0, buf);
    }

    // (2) Disjointness: the counting decorator serves, and nothing escapes.
    {
        counting_source_t src;  // no inner store => std::aligned_alloc, NOT ::operator new
        g_allocs = 0;
        g_live = 0;
        g_armed = true;
        {
            tr::mem::block_array_t<std::uint64_t> a(src);
            for (std::size_t i = 0; i < kCanaryElems; ++i) {
                std::uint64_t* slot = a.push_slot();
                if (slot == nullptr) break;
                *slot = i;
            }
            asm volatile("" : : "r"(a.data()) : "memory");
        }
        g_armed = false;
        const long long escaped = g_allocs.load();
        const std::size_t served = src.blocks.load();
        std::snprintf(buf, sizeof(buf), "seam_blocks=%zu heap_allocs=%lld", served, escaped);
        check("disjointness_seam_serves_and_nothing_escapes", served > 0 && escaped == 0, buf);
    }

    // (3) The null arm: nothing happens, and the instrument says so.
    {
        g_armed = true;
        const long long a = live();
        const long long b = live();
        g_armed = false;
        std::snprintf(buf, sizeof(buf), "delta=%lld", b - a);
        check("null_arm_reads_zero", b - a == 0, buf);
    }

    // (4) The shipping composition must be seen to reach the process heap.
    {
        node_t n(arm_t::H_BASELINE, kLanes, /*count_channels=*/false);
        g_allocs = 0;
        g_live = 0;
        reset_peak();
        g_armed = true;
        for (std::size_t k = 0; k < 16; ++k) n.step_full(n.lane(0));
        g_armed = false;
        const long long escaped = g_allocs.load();
        std::snprintf(buf, sizeof(buf), "heap_allocs=%lld", escaped);
        check("h_baseline_steady_escape_is_nonzero", escaped > 0, buf);
    }

    std::printf("CALIBRATE\ttotal_failures\t%d\t-\n", failures);
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? std::string_view(argv[1]) : std::string_view("escape");
    int round0 = 0;
    const char* tag = "A";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--round0=", 0) == 0) {
            round0 = std::atoi(a.c_str() + 9);
        } else if (a.rfind("--tag=", 0) == 0) {
            tag = argv[i] + 6;
        }
    }

    if (mode == "calibrate") return run_calibrate();
    if (mode != "escape") {
        std::fprintf(stderr, "usage: %s escape|calibrate [--round0=N] [--tag=X]\n", argv[0]);
        return 2;
    }

    std::printf("# RESULT_STORE_ESCAPE round tag arm window live_delta peak_delta allocs frees\n");
    constexpr std::size_t kNArms = sizeof(kArms) / sizeof(kArms[0]);
    for (std::size_t j = 0; j < kNArms; ++j) {
        // Same rotation rule as the timed harness: never exhaust one arm before the next.
        census_arm(round0, tag, kArms[(static_cast<std::size_t>(round0) + j) % kNArms]);
    }
    return 0;
}
