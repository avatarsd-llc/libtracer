/**
 * @file
 * @brief #2310 — a peer's lane lock never livelocks a higher-priority sender against a
 *        lower-priority holder on one CPU (the #1618 hazard, for the egress lanes).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two `SCHED_FIFO` threads are pinned to ONE CPU. The low-priority holder takes peer A's lane
 * lock (its lane draw is parked inside the source) and stays inside its step until the
 * high-priority sender has started. The sender then pushes into the same peer and needs the
 * same lock. A lock whose waiter spins and yields never lets the holder run again, because
 * `sched_yield` only yields to equal or higher priority; a waiter that sleeps on the role byte
 * hands the CPU back, the holder finishes its step, and its unlock wakes the sender.
 *
 * A watchdog on a DIFFERENT CPU measures the sender's progress. Past @ref kStallLimit it calls
 * the run a livelock, demotes the sender to `SCHED_OTHER` so the holder can finish (otherwise
 * the test itself would never end), and fails.
 *
 * Needs `CAP_SYS_NICE` (real-time scheduling) and two CPUs. Without them the scenario cannot
 * be built, and the test says so with exit code 77, which ctest reports as SKIPPED.
 */

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

#include "libtracer/egress.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::delivery_lane_t;
using tr::net::egress_frame_t;
using tr::net::egress_peer_t;
using tr::net::egress_step_t;
using tr::net::egress_t;
using tr::testing::check;

/** @brief How long the sender may make no progress before the watchdog calls a livelock. */
constexpr auto kStallLimit = 1000ms;

/** @brief The holder's `SCHED_FIFO` priority. */
constexpr int kHolderPriority = 10;

/** @brief The sender's `SCHED_FIFO` priority: above the holder. */
constexpr int kSenderPriority = 20;

thread_local bool t_gated = false; /**< @brief This thread's draws go through the gate. */
thread_local int t_draws = 0;      /**< @brief Draws this thread made through the gate. */

/**
 * @brief A heap source that holds the gated thread inside its SECOND draw (the lane, drawn
 *        under the lane lock; the first is the frame) until the sender has started.
 *
 * The hold yields rather than blocks: on the shared CPU it only runs when the higher-priority
 * sender has given that CPU up.
 */
class gated_source_t final : public tr::mem::block_source_t {
   public:
    gated_source_t() noexcept : block_source_t("egress_inversion_test") {}
    void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (t_gated && ++t_draws == 2) {
            in_step.store(true, std::memory_order_release);
            while (!sender_started.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }
    std::atomic<bool> in_step{false};        /**< @brief The holder is inside its step. */
    std::atomic<bool> sender_started{false}; /**< @brief The sender is about to push. */
};

/** @brief Pin the calling thread to @p cpu. */
[[nodiscard]] bool pin_to(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

/** @brief Give the calling thread `SCHED_FIFO` at @p prio. */
[[nodiscard]] bool go_fifo(int prio) {
    sched_param sp{};
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
}

/** @brief Whether this process may use real-time scheduling at all. */
[[nodiscard]] bool rt_available() {
    bool ok = false;
    std::thread probe([&] { ok = go_fifo(1); });
    probe.join();
    return ok;
}

/** @brief Two distinct CPUs this process may run on, or `false` when it has fewer. */
[[nodiscard]] bool two_cpus(int& work, int& watch) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return false;
    work = watch = -1;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (!CPU_ISSET(c, &set)) continue;
        if (work < 0) {
            work = c;
        } else {
            watch = c;
            break;
        }
    }
    return watch >= 0;
}

}  // namespace

int main() {
    int work = -1;
    int watch = -1;
    if (!rt_available() || !two_cpus(work, watch)) {
        std::printf("egress_inversion: SKIPPED (needs SCHED_FIFO and two CPUs)\n");
        return 77;
    }
    std::printf("a higher-priority sender vs a preempted lane-lock holder on CPU %d:\n", work);
    if (!pin_to(watch)) {
        std::printf("egress_inversion: SKIPPED (cannot pin the watchdog)\n");
        return 77;
    }

    gated_source_t src;
    egress_t eg(src);
    egress_peer_t a;
    eg.open(a);
    int vertex = 0;
    const delivery_lane_t lane{.key = &vertex, .depth = 4, .reliable = false};
    const std::vector<std::byte> own(8, std::byte{0x5A});
    const std::span<const std::byte> one[1] = {own};
    const auto none = [](const egress_frame_t&, std::size_t) {
        return egress_step_t{0, true, false};
    };
    const auto push = [&] {
        tr::net::egress_once_t once([&] { return egress_frame_t::make(src, one); });
        (void)eg.send(a, lane, once, [] { return egress_step_t{0, true, false}; }, none, [] {});
    };
    push();  // the socket holds the first frame on the wire, so every later send queues

    std::atomic<bool> setup_ok{true};
    std::atomic<bool> sent{false};
    std::thread holder([&] {
        if (!pin_to(work) || !go_fifo(kHolderPriority)) setup_ok.store(false);
        t_gated = true;
        push();  // stays inside its lane draw, A's lane lock held, until the sender starts
    });
    while (!src.in_step.load(std::memory_order_acquire)) std::this_thread::yield();

    std::thread sender([&] {
        if (!pin_to(work) || !go_fifo(kSenderPriority)) setup_ok.store(false);
        src.sender_started.store(true, std::memory_order_release);
        push();  // needs A's lane lock: it must give the CPU back to the holder
        sent.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + kStallLimit;
    while (!sent.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    const bool livelocked = !sent.load(std::memory_order_acquire);
    if (livelocked) {
        // Demote the sender so the holder can run, finish its step and let the test end.
        sched_param sp{};
        (void)pthread_setschedparam(sender.native_handle(), SCHED_OTHER, &sp);
    }
    holder.join();
    sender.join();
    eg.close(a);

    if (!setup_ok.load()) {
        std::printf("egress_inversion: SKIPPED (could not set up the real-time pair)\n");
        return 77;
    }
    check(!livelocked, "the higher-priority sender got the lane lock: the holder ran again");
    return tr::testing::summary("egress_inversion");
}
