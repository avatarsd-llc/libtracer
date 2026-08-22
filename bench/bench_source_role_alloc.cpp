/**
 * @file
 * @brief The HEAP half of the retention-role write bench (#1505): how many process-heap
 *        blocks one write costs at a retaining vertex versus a non-retaining one.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_source_role` times the two roles; this binary counts what each write ALLOCATES,
 * which is what turns the shed-on-OOM exposure from an argument into a number. A `HANDLER`
 * write builds a notify clone of the value before storing, and `rope_t::kInline` is 2 — so
 * once the written value carries more than two links, that clone grows a `std::vector<view_t>`
 * on **every write**. It is that allocation whose failure sheds the ENTIRE fan-out (counted at
 * own-subs width) while the write still returns success, so the number of allocations per
 * write at each link count IS the exposure: an arm that allocates nothing cannot shed.
 *
 * A SEPARATE binary from the timing half, on the `bench_store_escape` precedent and for the
 * same reason stated there: the override costs a relaxed atomic load per allocation, charged
 * to whichever arm allocates more — which here is the retaining arm under comparison. A timing
 * TU carrying this override would bias the result in favour of the side under suspicion.
 *
 * Output (its own tag — a verdict, not a charted series):
 *
 *     ROLE_ALLOC role links fan allocs_per_write frees_per_write
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

// --- the counting allocator override (all variants) ------------------------------------------
//
// Verbatim in shape from bench_store_escape.cpp, itself from bench_conn_ram.cpp: every variant
// must be overridden, because `heap_alloc` takes the aligned-nothrow form and the STL the plain
// one — covering only one leaves half the allocations invisible.

namespace {

/** @brief Blocks allocated / freed while armed. */
std::atomic<long long> g_allocs{0};
std::atomic<long long> g_frees{0};
/** @brief Counting is on. */
std::atomic<bool> g_armed{false};

/** @brief `malloc` plus the accounting, when armed. */
void* counted_alloc(std::size_t size) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (g_armed.load(std::memory_order_relaxed) && p != nullptr)
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    return p;
}

/** @brief `free` plus the accounting, when armed. */
void counted_free(void* p) {
    if (p == nullptr) return;
    if (g_armed.load(std::memory_order_relaxed)) g_frees.fetch_add(1, std::memory_order_relaxed);
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

#include "bench_source_role.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tracer.hpp"

namespace {

using bench_role::register_src;
using bench_role::value_fixture_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;

/** @brief Writes inside the measured window; large enough that any one-off amortizes to 0.00x. */
constexpr std::size_t kWrites = 1000;

/**
 * @brief Count the per-write allocations of one (role, links) point at fan-out 1.
 *
 * Fan 1 rather than 0 because the clone is what is being counted and a fan-out has to exist
 * for the delivery it feeds to be real; the clone itself is taken before the width is ever
 * consulted, so the count does not depend on the width.
 */
void run_point(bool handler, std::size_t links) {
    graph_t g;
    const path_t src = *path_t::parse("/bench/src");
    std::atomic<std::uint64_t> hits{0};
    const vertex_handle_t v = register_src(g, src, handler, hits);
    std::atomic<std::uint64_t> recv{0};
    auto cb = [&](const rope_t&) { recv.fetch_add(1, std::memory_order_relaxed); };
    (void)g.subscribe(src, cb);

    const value_fixture_t fx{links};
    for (std::size_t i = 0; i < 200; ++i) (void)g.write(v, fx.make());  // settle the one-offs
    if ((handler && hits.load() == 0) || recv.load() == 0) {
        std::fprintf(stderr, "SKIP role=%s links=%zu: the arm is not wired\n",
                     handler ? "handler" : "stored", links);
        return;
    }

    g_allocs.store(0, std::memory_order_relaxed);
    g_frees.store(0, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_seq_cst);
    for (std::size_t i = 0; i < kWrites; ++i) (void)g.write(v, fx.make());
    g_armed.store(false, std::memory_order_seq_cst);

    std::printf("ROLE_ALLOC\t%s\t%zu\t1\t%.3f\t%.3f\n", handler ? "handler" : "stored", links,
                static_cast<double>(g_allocs.load()) / kWrites,
                static_cast<double>(g_frees.load()) / kWrites);
    std::fflush(stdout);
}

}  // namespace

/**
 * @brief Run the census over a link ladder that straddles the rope's inline capacity.
 *
 * The capacity itself is deliberately not read off `rope_t` — it is private, and asserting a
 * number the instrument cannot see is how a bench outlives the constant it describes. The
 * ladder simply walks link counts until the per-write allocation count moves, so the knee is
 * MEASURED and the row above it is the exposure.
 */
int main() {
    for (const std::size_t links :
         {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}, std::size_t{8}}) {
        run_point(false, links);
        run_point(true, links);
    }
    return 0;
}
