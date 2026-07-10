/**
 * @file
 * @brief #383 — can_tx_pool_t ownership/backpressure host suite.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The pool is the host-testable half of the TWAI TX-lifetime fix: frames
 * handed to an asynchronous CAN driver must live in link-owned slots until the
 * driver's tx-done callback returns them. This suite pins:
 *
 *   - the ownership protocol: acquire hands out distinct slots, exhaustion is
 *     a clean nullptr (never a reused live slot), release makes a slot
 *     reacquirable;
 *   - the cross-thread contract (TSan-meaningful): a serialized acquirer vs a
 *     concurrent releaser standing in for the tx-done ISR — a slot is never
 *     handed out while its previous owner still holds it;
 *   - the #383 FULL policy semantics the twai_link_t write path builds on the
 *     pool: a BACKPRESSURED producer (bounded wait for a free slot) pushes a
 *     burst deeper than the pool through a slow consumer with NO loss and in
 *     order — the deterministic 7th-of-8 continuation-frame drop cannot
 *     happen — while a zero-wait producer drops COUNTED, never silently
 *     (delivered + dropped accounts for every frame).
 *
 * The FreeRTOS glue itself (counting semaphore + tx-done release) is thin and
 * build-gated by the esp-idf workflow; the on-hardware evidence lives on #383.
 */

#include "libtracer/can_tx_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A stand-in for twai_link_t's tx_slot_t: payload + an ownership probe. */
struct slot_t {
    std::uint32_t seq = 0;      /**< @brief The "frame" the producer wrote. */
    std::atomic<int> owners{0}; /**< @brief Test probe: live owners, must stay 0/1. */
};

// ---------------------------------------------------------------------------
// Exhaustion + reuse: the basic ownership protocol.
// ---------------------------------------------------------------------------
void test_exhaustion_and_reuse() {
    std::printf("exhaustion + reuse:\n");
    tr::net::can_tx_pool_t<slot_t> pool(4);
    check(pool.capacity() == 4, "capacity() reports the constructed size");

    std::set<slot_t*> seen;
    std::vector<slot_t*> held;
    for (int i = 0; i < 4; ++i) {
        slot_t* s = pool.try_acquire();
        check(s != nullptr, "acquire succeeds while a slot is free");
        seen.insert(s);
        held.push_back(s);
    }
    check(seen.size() == 4, "the four handed-out slots are distinct");
    check(pool.in_flight() == 4, "in_flight() == capacity when exhausted");
    check(pool.try_acquire() == nullptr, "exhausted pool yields nullptr, not a live slot");

    pool.release(held[2]);
    check(pool.in_flight() == 3, "release drops the in-flight count");
    slot_t* again = pool.try_acquire();
    check(again == held[2], "the released slot is the one handed out again");
    for (slot_t* s : held) pool.release(s);
    check(pool.in_flight() == 0, "full drain returns the pool to empty");

    tr::net::can_tx_pool_t<slot_t> tiny(0);
    check(tiny.capacity() == 1, "capacity 0 is clamped to one slot");
}

// ---------------------------------------------------------------------------
// Cross-thread ownership invariant: serialized acquirer vs ISR-like releaser.
// ---------------------------------------------------------------------------
void test_cross_thread_ownership() {
    std::printf("cross-thread ownership (acquirer vs modeled tx-done ISR):\n");
    constexpr std::size_t kCapacity = 8;
    constexpr std::uint32_t kFrames = 20000;

    tr::net::can_tx_pool_t<slot_t> pool(kCapacity);
    std::mutex m;
    std::deque<slot_t*> in_driver;  // models the driver's queued frame pointers
    std::atomic<bool> done{false};
    std::atomic<int> double_owner{0};
    std::atomic<std::uint32_t> released{0};

    // The "tx-done ISR": pops a queued frame and returns its slot to the pool.
    std::thread isr([&] {
        while (true) {
            slot_t* s = nullptr;
            {
                const std::lock_guard lock(m);
                if (!in_driver.empty()) {
                    s = in_driver.front();
                    in_driver.pop_front();
                }
            }
            if (s == nullptr) {
                if (done.load(std::memory_order_acquire)) break;
                std::this_thread::yield();
                continue;
            }
            if (s->owners.fetch_sub(1, std::memory_order_relaxed) != 1)
                double_owner.fetch_add(1, std::memory_order_relaxed);
            pool.release(s);
            released.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // The (externally serialized) writer: acquire, stamp, hand to the driver.
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        slot_t* s = nullptr;
        while ((s = pool.try_acquire()) == nullptr) std::this_thread::yield();
        if (s->owners.fetch_add(1, std::memory_order_relaxed) != 0)
            double_owner.fetch_add(1, std::memory_order_relaxed);
        s->seq = i;
        {
            const std::lock_guard lock(m);
            in_driver.push_back(s);
        }
    }
    done.store(true, std::memory_order_release);
    isr.join();

    check(double_owner.load() == 0, "no slot was ever handed out while still owned");
    check(released.load() == kFrames, "every acquired slot came back through release");
    check(pool.in_flight() == 0, "nothing left in flight after the run");
}

// ---------------------------------------------------------------------------
// The #383 FULL policy: bounded backpressure loses nothing; zero-wait drops
// are counted, never silent.
// ---------------------------------------------------------------------------

/** @brief One producer→consumer burst through the pool; returns (delivered, dropped). */
struct burst_result_t {
    std::vector<std::uint32_t> delivered; /**< @brief Frames the consumer saw, in order. */
    std::uint32_t dropped = 0;            /**< @brief Frames the producer dropped (counted). */
};

burst_result_t run_burst(bool backpressure, std::size_t capacity, std::uint32_t frames,
                         std::chrono::microseconds drain_interval) {
    tr::net::can_tx_pool_t<slot_t> pool(capacity);
    std::mutex m;
    std::deque<slot_t*> in_driver;
    std::atomic<bool> done{false};
    burst_result_t out;

    std::thread consumer([&] {
        while (true) {
            slot_t* s = nullptr;
            {
                const std::lock_guard lock(m);
                if (!in_driver.empty()) {
                    s = in_driver.front();
                    in_driver.pop_front();
                }
            }
            if (s == nullptr) {
                if (done.load(std::memory_order_acquire)) break;
                std::this_thread::yield();
                continue;
            }
            out.delivered.push_back(s->seq);
            pool.release(s);
            std::this_thread::sleep_for(drain_interval);  // the rate-limited wire
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (std::uint32_t i = 0; i < frames; ++i) {
        slot_t* s = pool.try_acquire();
        // The backpressured producer models write_raw's bounded semaphore
        // wait; the zero-wait producer models the old best-effort behavior.
        while (backpressure && s == nullptr && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            s = pool.try_acquire();
        }
        if (s == nullptr) {
            ++out.dropped;
            continue;
        }
        s->seq = i;
        const std::lock_guard lock(m);
        in_driver.push_back(s);
    }
    done.store(true, std::memory_order_release);
    consumer.join();
    return out;
}

void test_full_policy() {
    std::printf("FULL policy — backpressure vs counted drop:\n");
    // A burst 8x deeper than the pool through a slow consumer: the shape of
    // the fw repro (8 continuation frames vs tx_queue_depth) scaled up.
    constexpr std::size_t kCapacity = 4;
    constexpr std::uint32_t kFrames = 32;

    const burst_result_t bp = run_burst(true, kCapacity, kFrames, 200us);
    check(bp.dropped == 0, "backpressure: a burst deeper than the pool drops NOTHING");
    check(bp.delivered.size() == kFrames, "backpressure: every frame was delivered");
    bool in_order = true;
    for (std::uint32_t i = 0; i < bp.delivered.size(); ++i)
        if (bp.delivered[i] != i) in_order = false;
    check(in_order, "backpressure: delivery kept the burst's order (reassembly-safe)");

    const burst_result_t drop = run_burst(false, kCapacity, kFrames, 2000us);
    check(drop.dropped > 0, "zero-wait: the saturated pool forces drops");
    check(drop.delivered.size() + drop.dropped == kFrames,
          "zero-wait: delivered + dropped accounts for EVERY frame (no silent loss)");
}

}  // namespace

int main() {
    std::printf("can_tx_pool host suite (#383):\n");
    test_exhaustion_and_reuse();
    test_cross_thread_ownership();
    test_full_policy();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
