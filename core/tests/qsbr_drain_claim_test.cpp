/**
 * @file
 * @brief The shared retired table under concurrent drainers: a pair's release hook runs only
 *        once no reader that could still name it is inside its bracket (`qsbr.hpp`,
 *        `drain_ripe`).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Several readers each open a bracket, load the published object, hold it briefly, check that
 * its hook has not run, and drain at their quiescent point, as an outermost dispatch exit
 * does. One writer swaps the published object and retires the old one, so slots are emptied
 * and refilled while other drainers walk the table. Every drainer must judge the pair it
 * actually claims. The objects come from a pool that is never freed, so a hook that ran early
 * is seen as a flag, not as a sanitizer report.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "libtracer/qsbr.hpp"
#include "test_support.hpp"

namespace {

namespace q = tr::graph::detail_qsbr;
using tr::testing::check;

struct object_t {
    std::atomic<bool> released{false};
};

void release(void* ctx) {
    static_cast<object_t*>(ctx)->released.store(true, std::memory_order_release);
}

/** @brief Readers holding the published object while slots churn never see its hook run. */
void hook_runs_after_every_reader() {
    constexpr std::size_t kPool = std::size_t{1} << 21;
    const auto pool = std::make_unique<object_t[]>(kPool);
    std::atomic<object_t*> current{&pool[0]};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> early{0};
    std::atomic<std::uint64_t> retired{0};

    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const unsigned readers = std::min(4u, std::max(2u, hw - 1));
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < readers; ++t) {
        ts.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                q::enter();
                const object_t* const p = current.load(std::memory_order_acquire);
                for (int k = 0; k < 64; ++k) std::atomic_signal_fence(std::memory_order_seq_cst);
                if (p->released.load(std::memory_order_acquire))
                    early.fetch_add(1, std::memory_order_relaxed);
                if (q::leave()) q::drain();
            }
        });
    }
    std::thread writer([&] {
        for (std::size_t i = 1; i < kPool && !stop.load(std::memory_order_relaxed); ++i) {
            object_t* const old = current.exchange(&pool[i], std::memory_order_acq_rel);
            (void)q::retire(old, release);
            retired.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < deadline &&
           early.load(std::memory_order_relaxed) == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (auto& t : ts) t.join();
    // Run what is still parked, so the table is empty for whatever runs after this test.
    q::drain();

    std::printf("  %u readers, %llu retired, %llu hooks run under a reader\n", readers,
                static_cast<unsigned long long>(retired.load()),
                static_cast<unsigned long long>(early.load()));
    check(early.load() == 0,
          "no release hook runs while a reader that loaded its object is inside");
    check(retired.load() > 0, "the writer retired pairs");
}

}  // namespace

int main() {
    hook_runs_after_every_reader();
    return tr::testing::summary("qsbr_drain_claim");
}
