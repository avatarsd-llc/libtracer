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
 * the MECHANISM as a compile-time policy — because only the target knows its concurrency
 * model. `tr::mem::sync_pool_t` is the multi-core-host spelling, `spin_sync_t`.
 *
 * THIS EXAMPLE IS BUILD-CONFIGURATION-DEPENDENT, and it is the only one in the wire/view set
 * that is. A single-core priority-preemptive target sets `tr::mem::kSpinWaitSafe = false` in
 * `libtracer/config_override.hpp` — there a spinner that outranks the lock holder never
 * yields the CPU the holder needs, so binding `spin_sync_t` is a hang, and
 * `synchronized_pool_t` refuses the instantiation. An example a binding does not apply to
 * must SKIP AT RUN TIME, never fail to compile, so the body lives in a template: in a
 * non-template function the discarded `if constexpr` branch is still instantiated, and
 * declaring the pool there would trip the assert anyway. Line 1 of a run says which arm it
 * took.
 *
 * Runs under ctest as `example_view_sync_pool`; returns non-zero on any failed check. Where
 * `kSpinWaitSafe` is false it prints a skip line and passes.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <optional>
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

/**
 * @brief The pool exercise — a TEMPLATE so the discarded branch is never instantiated.
 *
 * Two things are load-bearing about this signature, and both are easy to get wrong. First,
 * `if constexpr` alone is not enough: in a NON-template function the discarded branch is still
 * fully instantiated, so declaring the pool inside a discarded branch of `main` would trip the
 * assert regardless. Second, being a template is not enough EITHER — a template's
 * NON-dependent constructs are checked at definition time, so spelling the concrete
 * `tr::mem::sync_pool_t` here would trip it too. The type has to depend on a template
 * parameter, which is what @p Sync is for.
 *
 * @tparam Enabled `tr::mem::kSpinWaitSafe`, i.e. whether this target may bind a spin-waiting
 *         critical section at all.
 * @tparam Sync The synchronization policy to bind. Defaulted rather than hard-coded purely so
 *         that `synchronized_pool_t<Sync>` below is a DEPENDENT type and is therefore left
 *         uninstantiated when the branch is discarded.
 * @return True when every expectation held (and trivially true on the skip arm).
 */
template <bool Enabled, class Sync = tr::mem::spin_sync_t>
bool run_sync_pool() {
    if constexpr (Enabled) {
        alignas(std::max_align_t) std::array<std::byte, 4096> slab{};
        tr::mem::synchronized_pool_t<Sync> pool{slab, 32};  // == tr::mem::sync_pool_t
        std::printf("sync_pool_t over a %zu-byte slab: %zu slots, two threads\n", slab.size(),
                    pool.capacity());

        std::atomic<int> served{0};
        const auto churn = [&pool, &served] {
            for (int i = 0; i < kRounds; ++i) {
                // alloc on this thread, drop on this thread — but the two threads race for
                // the same free list, which is exactly what the policy guards.
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
        std::printf("%d of %d allocations served; %zu/%zu slots reachable afterwards\n",
                    served.load(), 2 * kRounds, drained.size(), pool.capacity());
        check(ok, served.load() == 2 * kRounds, "every request was served — the slab never leaked");
        check(ok, drained.size() == pool.capacity(),
              "and the free list is whole: no slot was lost to a race");
        check(ok, !tr::mem::synchronized_pool_t<Sync>::is_isr_safe,
              "a spinlock is not an ISR critical section — the policy says so in a constant");
        return ok;
    } else {
        std::printf(
            "skipped: this build sets tr::mem::kSpinWaitSafe = false — sync_pool_t spin-waits "
            "and this target must bind an interrupt-disable policy instead\n");
        return true;
    }
}

}  // namespace

int main() {
    std::printf("tr::mem::kSpinWaitSafe bound by this build: %d\n",
                static_cast<int>(tr::mem::kSpinWaitSafe));
    const bool ok = run_sync_pool<tr::mem::kSpinWaitSafe>();
    std::printf("RESULT %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
