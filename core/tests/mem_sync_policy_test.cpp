/**
 * @file
 * @brief The `mem::synchronized_pool_t` SYNCHRONISATION-POLICY seam (#770, ADR-0060 §2):
 *        the critical section is a compile-time policy, and it is the thing that makes
 *        the pool safe at a receive seam.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Host threads stand in for the single-core MCU's priority preemption: what an
 * interrupt-disable critical section serialises on an ESP32-C6, a lock serialises here,
 * and the free-list invariant under test is the same either way. The ESP-IDF policy
 * (`tr::esp::portmux_sync_t`) cannot run on a host, so the seam — not that one adapter —
 * is what a host test can prove; the adapter is compile-gated by the IDF component build.
 *
 * Three checks, none of them needing a sanitizer to fire:
 *   1. POLICY ENGAGED — a counting policy proves every `alloc`/`destroy` really passes
 *      through the injected critical section (lock count == 2 × ops), so the seam is the
 *      mechanism and not decoration.
 *   2. NO DOUBLE HAND-OUT under contention — each thread stamps its slot with its own tag
 *      and reads it back; a slot handed to two live segments shows up as a mismatch.
 *   3. FREE-LIST INTACT afterwards — every slot is re-allocatable once the storm ends, so
 *      no slot was lost or duplicated into the list.
 *
 * THE ABLATION (#770): building this TU with `-DLIBTRACER_ABLATE_POOL_SYNC` swaps the
 * policy's `lock`/`unlock` for no-ops — the critical section, and nothing else, removed.
 * The run then goes RED without a sanitizer (measured: 3 threads × 20k iterations dies
 * on the mangled free list — SIGSEGV/exit 139 — before checks 2 and 3 even print). See
 * `core/tests/CMakeLists.txt` for the exact command; the ablation build is deliberately
 * NOT an `add_test` because "the race manifests" is a probabilistic pass condition.
 */
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_pool.hpp"
#include "libtracer/segment.hpp"

namespace {

using tr::mem::spin_sync_t;
using tr::mem::synchronized_pool_t;
using tr::view::segment_ptr_t;
using tr::view::segment_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
    }
}

/**
 * @brief A host sync policy that COUNTS its acquisitions — the instrument for check 1.
 *
 * Wraps the shipped host policy (@ref tr::mem::spin_sync_t) so the counted section is the
 * real one. Under `LIBTRACER_ABLATE_POOL_SYNC` the wrapped section is dropped and only
 * the counter remains: the pool is then a bare, unsynchronised `pool_t` behind the same
 * type, which is exactly the defect #770 reports at the receive seam.
 */
struct counting_sync_t {
    static constexpr bool is_isr_safe = false;              /**< @brief Spin => not ISR-safe. */
    static constexpr const char* name = "test_count_sync";  /**< @brief Backend name. */
    static inline std::atomic<std::size_t> acquisitions{0}; /**< @brief Lock count, all pools. */

    /** @brief Enter the critical section (ablated away when the guard is removed). */
    void lock() noexcept {
        acquisitions.fetch_add(1, std::memory_order_relaxed);
#ifndef LIBTRACER_ABLATE_POOL_SYNC
        inner_.lock();
#endif
    }
    /** @brief Leave the critical section. */
    void unlock() noexcept {
#ifndef LIBTRACER_ABLATE_POOL_SYNC
        inner_.unlock();
#endif
    }

   private:
    spin_sync_t inner_{};
};

static_assert(tr::mem::pool_sync_policy<spin_sync_t>, "the shipped host policy models the seam");
static_assert(tr::mem::pool_sync_policy<counting_sync_t>, "a user policy models the seam");
/** The ISR-safety trait is the POLICY's fact, forwarded by the pool (ADR-0047 §2). */
static_assert(synchronized_pool_t<spin_sync_t>::is_isr_safe == spin_sync_t::is_isr_safe,
              "the pool must publish its policy's ISR-safety, not its own guess");

using counted_pool_t = synchronized_pool_t<counting_sync_t>;

constexpr std::size_t kThreads = 3;
constexpr std::size_t kSlotPayload = 64;
constexpr std::size_t kIters = 20000;

/**
 * @brief Checks 1–3: N threads alloc / stamp / verify / release against one pool.
 *
 * The slab is sized so the pool holds a few more slots than there are threads: enough
 * that `alloc` usually succeeds, tight enough that the free list is genuinely hot.
 */
void contended_policy_seam() {
    std::vector<std::byte> slab((kThreads + 2) * (kSlotPayload + sizeof(segment_t) + 64));
    counted_pool_t pool(slab, kSlotPayload);
    check(pool.capacity() >= kThreads, "pool must hold at least one slot per thread");

    counting_sync_t::acquisitions.store(0, std::memory_order_relaxed);
    std::atomic<std::size_t> mismatches{0};
    std::atomic<std::size_t> ops{0};

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
                ops.fetch_add(1, std::memory_order_relaxed);
                // p drops here -> destroy through the policy's critical section.
            }
        });
    }
    for (auto& th : ts) th.join();

    // 1. The policy is the mechanism: one lock per alloc + one per destroy.
    check(counting_sync_t::acquisitions.load() == 2 * ops.load(),
          "policy seam: the injected critical section was not taken on every alloc/destroy");
    check(ops.load() > 0, "policy seam: no allocation succeeded");
    // 2. No slot was live in two segments at once.
    check(mismatches.load() == 0, "policy seam: a slot was handed to two live segments");

    // 3. The free list survived: every slot is handed out again, exactly once.
    std::vector<segment_ptr_t> held;
    std::vector<std::byte*> bases;
    for (std::size_t i = 0; i < pool.capacity(); ++i) {
        segment_t* raw = pool.alloc(kSlotPayload);
        check(raw != nullptr, "free-list integrity: a slot went missing after the storm");
        if (raw == nullptr) break;
        bases.push_back(raw->bytes.data());
        held.push_back(segment_ptr_t::adopt(raw));
    }
    check(pool.alloc(kSlotPayload) == nullptr,
          "free-list integrity: the pool handed out more slots than it owns");
    for (std::size_t i = 0; i < bases.size(); ++i)
        for (std::size_t j = i + 1; j < bases.size(); ++j)
            check(bases[i] != bases[j], "free-list integrity: one slot appeared twice in the list");
}

}  // namespace

int main() {
    contended_policy_seam();
#ifdef LIBTRACER_ABLATE_POOL_SYNC
    std::printf("mem_sync_policy_test: ABLATION build (critical section removed)\n");
#endif
    if (g_failures == 0) std::printf("mem_sync_policy_test: OK\n");
    return g_failures == 0 ? 0 : 1;
}
