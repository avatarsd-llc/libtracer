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
#include <barrier>
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

/**
 * @brief A drain that meets a pass in flight skips it and leaves its request behind
 *        (`qsbr.hpp` §qsbr_single_flight): the pair stays parked, the skip is counted in
 *        `in_flight`, and nothing is freed early.
 */
void a_skipped_pass_leaves_its_request() {
    if constexpr (!q::kMembarrier) {
        std::printf("  (single flight is not compiled in: the fenced build has no drain gate)\n");
        return;
    } else {
        auto& flight = q::in_flight;
        object_t x;
        q::enter();
        (void)q::retire(&x, release);                // this thread's own bracket holds it: parked
        flight.store(1, std::memory_order_relaxed);  // as if another pass were fencing now
        check(q::leave(), "the bracket closes");
        q::drain();
        check(!x.released.load() && flight.load() == 2,
              "a drain that meets a pass in flight skips it and records the skip");
        flight.store(0, std::memory_order_relaxed);  // that pass is over
        q::drain();
        check(x.released.load(), "with no pass in flight, the next drain frees the pair");
    }
}

/** @brief The forced window's state: its two pairs and the step the two threads are at. */
object_t g_first;
object_t g_second;
std::atomic<int> g_step{0};

/**
 * @brief `g_first`'s hook, run inside the drain pass that freed it, while that pass still holds
 *        the flight: it retires `g_second`, which the reader still holds, then lets the reader
 *        leave and drain, and waits until it has.
 */
void release_first_then_open_the_window(void* ctx) {
    release(ctx);
    (void)q::retire(&g_second, release);  // stamped after the pass in flight read the counter
    g_step.store(2, std::memory_order_release);
    while (g_step.load(std::memory_order_acquire) != 3) std::this_thread::yield();
}

/**
 * @brief The window a skip must not strand (`qsbr.hpp` §qsbr_single_flight), forced: a pass is
 *        in flight, a pair is retired after it read the counter, and the last reader holding
 *        that pair leaves and drains while that pass is still open. Under single flight that
 *        drain skips. No thread drains after it, so the pass in flight must free the pair.
 *
 * The pass in flight is this thread's own drain, held open by a release hook it runs. Without
 * the asymmetric fence there is no gate: the reader's drain frees the pair itself.
 */
void a_skip_inside_a_pass_is_served() {
    std::printf("a pair retired inside a pass, whose last reader's drain skips, still runs\n");
    g_step.store(0);
    std::thread reader([] {
        while (g_step.load(std::memory_order_acquire) != 1) std::this_thread::yield();
        q::enter();  // online at an epoch after g_first's, before g_second's
        g_step.store(4, std::memory_order_release);
        while (g_step.load(std::memory_order_acquire) != 2) std::this_thread::yield();
        if (q::leave())
            q::drain();  // the pass in flight is still open: under single flight, a skip
        g_step.store(3, std::memory_order_release);
    });
    q::enter();
    (void)q::retire(&g_first, release_first_then_open_the_window);  // held by this bracket
    g_step.store(1, std::memory_order_release);
    while (g_step.load(std::memory_order_acquire) != 4) std::this_thread::yield();
    check(q::leave(), "the bracket holding the first pair closes");
    q::drain();  // the pass that frees g_first, and inside its hook, the window
    reader.join();
    check(g_first.released.load(), "the first pair ran, in the pass");
    check(g_second.released.load(),
          "the pair retired inside the pass ran, though its last reader's drain met that pass");
    if constexpr (q::kMembarrier) check(q::in_flight.load() == 0, "and no pass is left in flight");
}

/**
 * @brief Drain passes that collide strand nothing (`qsbr.hpp` §qsbr_single_flight): once every
 *        reader has made its last quiescent point, with no drain after it, the pair those
 *        readers held has run.
 *
 * Each round the readers open a bracket, the test retires a pair they hold, and the readers
 * then all leave and drain at the same moment, so drain passes overlap. Under the asymmetric
 * fence only one of them fences; the others skip, and the one in flight must see what they
 * saw. The pair is retired before any of those passes reads the counter, which is the case a
 * skip must never defer. Nothing drains after that, so a skip that went unserved leaves the
 * pair parked. Without the asymmetric fence there is no gate and this checks the plain drain.
 */
void collided_passes_strand_nothing() {
    constexpr int kRounds = 3000;
    constexpr unsigned kReaders = 3;
    const auto pool = std::make_unique<object_t[]>(kRounds);
    std::barrier sync(kReaders + 1);
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < kReaders; ++t) {
        ts.emplace_back([&] {
            for (int round = 0; round < kRounds; ++round) {
                q::enter();
                sync.arrive_and_wait();  // online: the test retires a pair this bracket holds
                sync.arrive_and_wait();  // retired: every reader leaves and drains at once
                if (q::leave()) q::drain();
                sync.arrive_and_wait();  // drained: the test looks, then cleans up
                sync.arrive_and_wait();
            }
        });
    }
    const std::uint64_t drops_before = q::drops();
    int stranded = 0;
    for (int round = 0; round < kRounds; ++round) {
        sync.arrive_and_wait();
        (void)q::retire(&pool[round], release);
        sync.arrive_and_wait();
        sync.arrive_and_wait();
        stranded += !pool[round].released.load(std::memory_order_acquire);
        q::drain();  // so a stranded pair does not carry into the next round
        sync.arrive_and_wait();
    }
    for (auto& t : ts) t.join();
    std::printf("  %u readers x %d rounds (%s), %d pairs left parked after the last drain\n",
                kReaders, kRounds, q::kMembarrier ? "single-flight drain" : "no drain gate",
                stranded);
    check(stranded == 0, "no colliding drain pass leaves a ripe pair parked");
    check(q::drops() == drops_before, "and the table never filled");
}

}  // namespace

int main() {
    hook_runs_after_every_reader();
    a_skipped_pass_leaves_its_request();
    a_skip_inside_a_pass_is_served();
    collided_passes_strand_nothing();
    return tr::testing::summary("qsbr_drain_claim");
}
