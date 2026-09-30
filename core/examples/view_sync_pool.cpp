/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a backend at a SHARED seam must be thread-safe (ADR-0060 §2).
 *
 * A segment self-routes its reclaim on whatever thread drops the last reference — typically a
 * subscriber or a transport receive thread, concurrent with a writer's `alloc`. So any
 * `mem_backend_t` injected at a shared seam (a `graph_t`'s value backend, a router's flat, a
 * transport vertex's rx backend) must tolerate that. `tr::mem::synchronized_pool_t` is the
 * bounded answer: one `pool_t` whose O(1) free-list ops run inside a critical section, with
 * the MECHANISM as a compile-time guard — because only the target knows its concurrency
 * model. Since RFC-0028 slice 10 the guard is the SAME trait the last-known-value slot uses
 * (`tr::graph::reader_guard_t`), so `synchronized_pool_t<>` binds the build's one guard: the
 * host `mutex_guard_t` (a short bounded spin, then a nap — never a pure spin, so it is safe on
 * every scheduler), or an ESP-IDF build's interrupt-masked `critical_guard_t`.
 *
 * Runs under ctest as `example_view_sync_pool`; returns non-zero on any failed check.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief Slots per thread in the churn below — enough to interleave, small enough to be quick. */
constexpr int kRounds = 2000;

/** @brief The pool exercise: two threads race one free list through the synchronized pool. */
bool run_sync_pool() {
    alignas(std::max_align_t) std::array<std::byte, 4096> slab{};
    tr::mem::synchronized_pool_t<> pool{slab, 32};  // the build's one guard
    std::printf("synchronized_pool_t<%s> over a %zu-byte slab: %zu slots, two threads\n",
                tr::graph::reader_guard_t::name, slab.size(), pool.capacity());

    std::atomic<int> served{0};
    const auto churn = [&pool, &served] {
        for (int i = 0; i < kRounds; ++i) {
            // alloc on this thread, drop on this thread — but the two threads race for the
            // same free list, which is exactly what the guard protects.
            tr::view::segment_ptr_t seg = tr::view::segment_alloc(pool, 8);
            if (seg) served.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread a(churn);
    std::thread b(churn);
    a.join();
    b.join();

    // The free list has no `available()` counter through the synchronized facade, so the
    // honest check is to drain it: every slot must still be reachable afterwards.
    std::vector<tr::view::segment_ptr_t> drained;
    while (auto seg = tr::view::segment_alloc(pool, 8)) drained.push_back(std::move(seg));

    bool ok = true;
    std::printf("%d of %d allocations served; %zu/%zu slots reachable afterwards\n", served.load(),
                2 * kRounds, drained.size(), pool.capacity());
    check(ok, served.load() == 2 * kRounds, "every request was served — the slab never leaked");
    check(ok, drained.size() == pool.capacity(),
          "and the free list is whole: no slot was lost to a race");
    check(ok, tr::mem::synchronized_pool_t<>::is_isr_safe == tr::graph::reader_guard_t::is_isr_safe,
          "the pool forwards its guard's guarantees rather than inventing them");
    return ok;
}

}  // namespace

int main() {
    const bool ok = run_sync_pool();
    std::printf("RESULT %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
