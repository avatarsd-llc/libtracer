/**
 * @file
 * @brief ADR-0060 §2 — the thread-safe pool `value_backend_`: `mem::sync_pool_t` must
 *        serialize concurrent `alloc` (writer threads) with cross-thread `destroy`
 *        (a `segment` reclaimed on the reader/subscriber thread that drops the last ref).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two scenarios, both under ThreadSanitizer (core-ci.yml `tsan` job):
 *   1. CONTENDED alloc+destroy — N threads each alloc / write-own-pattern / verify /
 *      release in a loop. The own-pattern read-back proves no slot is handed to two live
 *      segments at once (the free-list corruption a missing lock would cause); TSan proves
 *      the free-list mutations race-free.
 *   2. CROSS-THREAD reclaim (the ADR-0060 §2 case) — a producer `alloc`s, a consumer
 *      `release`s, so `destroy` fires on a DIFFERENT thread than `alloc`. TSan proves the
 *      spinlock covers that cross-thread hand-off; a leak/undercount check proves every
 *      slot returns.
 */
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_pool.hpp"
#include "libtracer/segment.hpp"

namespace {

using tr::mem::sync_pool_t;
using tr::view::segment_ptr_t;
using tr::view::segment_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
    }
}

/** @brief Scenario 1: N threads hammer alloc+destroy; each verifies it owns its slot. */
void contended_alloc_destroy() {
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kSlotPayload = 64;
    constexpr std::size_t kIters = 20000;
    // Room for every thread to hold one slot at once (+ headroom) so alloc rarely
    // backpressures — the test is about correctness under concurrency, not exhaustion.
    std::vector<std::byte> slab(2 * kThreads * (kSlotPayload + sizeof(segment_t) + 64));
    sync_pool_t pool(slab, kSlotPayload);

    std::atomic<std::size_t> mismatches{0};
    std::atomic<std::size_t> allocs{0};
    std::vector<std::thread> ts;
    for (std::size_t t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            const auto tag = static_cast<std::byte>(0x11 * (t + 1));
            for (std::size_t i = 0; i < kIters; ++i) {
                segment_t* raw = pool.alloc(kSlotPayload);
                if (raw == nullptr) continue;  // transient exhaustion — legal backpressure
                segment_ptr_t p = segment_ptr_t::adopt(raw);
                std::span<std::byte> b = raw->bytes;
                for (auto& byte : b) byte = tag;  // stamp our slot
                for (auto byte : b)               // no other live segment shares it
                    if (byte != tag) mismatches.fetch_add(1, std::memory_order_relaxed);
                allocs.fetch_add(1, std::memory_order_relaxed);
                // p drops here -> release -> sync_pool_t::destroy (locked), on THIS thread.
            }
        });
    }
    for (auto& th : ts) th.join();
    check(mismatches.load() == 0, "scenario1: a slot was handed to two live segments");
    check(allocs.load() > 0, "scenario1: no allocations succeeded");
    check(pool.capacity() > 0, "scenario1: pool has slots");
}

/** @brief Scenario 2: alloc on producers, destroy on consumers (ADR-0060 §2 reclaim). */
void cross_thread_reclaim() {
    constexpr std::size_t kSlotPayload = 32;
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::size_t kPerProducer = 20000;
    std::vector<std::byte> slab(4096 * (kSlotPayload + sizeof(segment_t) + 64));
    sync_pool_t pool(slab, kSlotPayload);

    std::mutex qmu;  // guards the HAND-OFF queue only; the POOL guards itself.
    std::vector<segment_ptr_t> q;
    std::atomic<bool> producing{true};
    std::atomic<std::size_t> produced{0}, consumed{0};

    std::vector<std::thread> ts;
    for (std::size_t p = 0; p < kProducers; ++p) {
        ts.emplace_back([&] {
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                segment_t* raw = pool.alloc(kSlotPayload);
                if (raw == nullptr) {
                    std::this_thread::yield();
                    continue;
                }
                segment_ptr_t p2 = segment_ptr_t::adopt(raw);
                {
                    std::lock_guard<std::mutex> g(qmu);
                    q.push_back(std::move(p2));  // hand off; NOT released here
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::size_t c = 0; c < kConsumers; ++c) {
        ts.emplace_back([&] {
            for (;;) {
                segment_ptr_t got;
                {
                    std::lock_guard<std::mutex> g(qmu);
                    if (!q.empty()) {
                        got = std::move(q.back());
                        q.pop_back();
                    }
                }
                if (got) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    // got drops here -> destroy on THIS consumer thread (cross-thread).
                } else if (!producing.load(std::memory_order_acquire)) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (std::size_t p = 0; p < kProducers; ++p) ts[p].join();
    producing.store(false, std::memory_order_release);
    for (std::size_t c = kProducers; c < ts.size(); ++c) ts[c].join();
    {
        std::lock_guard<std::mutex> g(qmu);
        consumed.fetch_add(q.size(), std::memory_order_relaxed);
        q.clear();  // drain any stragglers (release on this thread)
    }
    check(produced.load() == consumed.load(), "scenario2: produced != consumed (leak/double)");
    check(produced.load() > 0, "scenario2: nothing produced");
}

}  // namespace

int main() {
    contended_alloc_destroy();
    cross_thread_reclaim();
    if (g_failures == 0) std::printf("mem_sync_pool_test: OK\n");
    return g_failures == 0 ? 0 : 1;
}
