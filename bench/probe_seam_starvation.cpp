/**
 * @file
 * @brief The round-2 starvation probe: many long-lived readers against a retire storm,
 *        measuring the reclamation domain's live-block bound and its per-retire cost
 *        (#576, ADR-0072 erratum 2).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `hazard_domain_test` asserts the bound; this probe REPORTS the two numbers behind it, so
 * the claim can be re-derived against an older library rather than believed. It drives the
 * domain directly (not through `graph_t`) because the shape under test is the domain's:
 * more long-lived reader threads than a fixed announcement table would have held, each
 * taking a nested pair of guards continuously, while one thread retires 20 000 blocks.
 *
 * Against the erratum-1 head (a fixed 64-participant table) the 65th thread gets no
 * announcement word on EVERY guard, sets the per-domain stall, and no scan frees anything:
 *
 *     STARVE peak_live=20039 first_half_us=55.543 second_half_us=140.959   <- unbounded, O(parked)
 *     STARVE peak_live=80    first_half_us=3.530  second_half_us=2.475     <- this head
 *
 * Build it against the OLD library by pointing the include path and the link at that tree;
 * the `reclaim_due()`/`collect()` pair below is erratum 2's tenant contract, so an older
 * arm compiles it out (`-DLIBTRACER_PROBE_LEGACY_RETIRE=1`, where `retire` scanned inline).
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <thread>
#include <vector>

#include "libtracer/hazard_domain.hpp"
#include "libtracer/mem_heap.hpp"

namespace {

/** @brief Live count of @ref block_t — the instrument the probe reports. */
std::atomic<int> g_live{0};

/** @brief A tenant block carrying its retire record FIRST, as the domain requires. */
struct block_t {
    tr::mem::retire_link_t link;
    int payload = 0;
    block_t() noexcept { g_live.fetch_add(1, std::memory_order_relaxed); }
    ~block_t() { g_live.fetch_sub(1, std::memory_order_relaxed); }
};

/** @brief This domain's one reclaimer; the blocks come from the global heap. */
void reclaim_block(void* p, tr::mem::mem_backend_t&) noexcept { delete static_cast<block_t*>(p); }

/** @brief Reader threads, one past the 64-participant table this design replaced. */
constexpr std::size_t kReaders = 65;
/** @brief Retires driven from the control-plane thread. */
constexpr std::size_t kRetires = 20000;

}  // namespace

int main() {
    using tr::mem::hazard_domain_t;
    hazard_domain_t d{tr::mem::heap_backend(), &reclaim_block};

    std::vector<std::atomic<block_t*>> slots(kReaders);
    for (std::atomic<block_t*>& s : slots) s.store(new block_t{}, std::memory_order_release);

    std::atomic<std::size_t> running{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (std::size_t i = 0; i < kReaders; ++i) {
        readers.emplace_back([&, i] {
            running.fetch_add(1, std::memory_order_acq_rel);
            while (!stop.load(std::memory_order_relaxed)) {
                hazard_domain_t::guard_t g{d};
                (void)g.protect(slots[i]);
                // Nested, the way a user callback re-entering the graph is.
                hazard_domain_t::guard_t inner{d};
                (void)inner.protect(slots[(i + 1) % kReaders]);
                std::this_thread::yield();
            }
        });
    }
    while (running.load(std::memory_order_acquire) < kReaders) std::this_thread::yield();

    std::atomic<block_t*> slot{nullptr};
    int peak = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto half = t0;
    for (std::size_t i = 0; i < kRetires; ++i) {
        slot.store(new block_t{}, std::memory_order_release);
        block_t* old = slot.exchange(nullptr, std::memory_order_seq_cst);
        if (old != nullptr) d.retire(old->link);
#if !LIBTRACER_PROBE_LEGACY_RETIRE
        // Erratum 2's tenant contract: park under whatever lock you hold, reclaim where you
        // hold nothing. The legacy arm scanned inside `retire` and has neither call.
        if (d.reclaim_due()) d.collect();
#endif
        peak = std::max(peak, g_live.load());
        if (i + 1 == kRetires / 2) half = std::chrono::steady_clock::now();
    }
    const auto t1 = std::chrono::steady_clock::now();
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& t : readers) t.join();

    const double first = std::chrono::duration<double, std::micro>(half - t0).count();
    const double second = std::chrono::duration<double, std::micro>(t1 - half).count();
    std::printf(
        "STARVE readers=%zu retires=%zu peak_live=%d first_half_us=%.3f "
        "second_half_us=%.3f parked=%zu\n",
        kReaders, kRetires, peak, first / (kRetires / 2), second / (kRetires / 2),
        d.retired_count());
    return 0;
}
