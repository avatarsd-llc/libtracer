/**
 * @file
 * @brief A bracket and an edge pin opened from a `thread_local` destructor that runs after the
 *        thread gave its cells back (#2123): no free gets through.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Thread A builds a `thread_local` before its first bracket and its first pin, so that object
 * is destroyed after the thread-exit releases of both cells. Its destructor opens a bracket
 * and takes a pin, as an app-level `thread_local` handle that publishes or sends on its way
 * out would. While A is inside, thread B claims the freed cells and goes through a bracket of
 * its own, and the test retires a pair. A thread that announced on its old cell would now be
 * invisible (B's offline store overwrote it), and the pair would be freed under A. The record
 * keeps the cells at "none" after their release instead: A's bracket counts into
 * `overflow_online`, so the scan answers "not quiescent", and its pin takes the stripe-mutex
 * fallback rather than a cell B owns.
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <semaphore>
#include <thread>

#include "libtracer/edge_pin.hpp"
#include "libtracer/qsbr.hpp"
#include "test_support.hpp"

namespace {

namespace q = tr::graph::detail_qsbr;
namespace ep = tr::graph::detail_ep;
using tr::testing::check;

std::binary_semaphore g_a_inside{0};
std::binary_semaphore g_retired{0};
std::atomic<bool> g_released{false};
std::atomic<bool> g_released_under_a{true};
std::atomic<bool> g_pin_fell_back{false};
std::atomic<bool> g_timed_out{false};
std::atomic<std::uint32_t> g_a_cell{0}, g_a_pin{0}, g_b_cell{0}, g_b_pin{0};

// Generous: a wait that runs out is a failed check, not a hung test run.
constexpr auto kWait = std::chrono::seconds(30);

/** @brief A record field that names a cell: neither "not yet claimed" (0) nor "none". */
bool owned(std::uint32_t cell) { return cell != 0 && cell != q::kNoCell; }

void release(void*) { g_released.store(true, std::memory_order_release); }

/** @brief Opens a bracket and a pin from its destructor, after the cells went back. */
struct late_t {
    bool armed = false;
    ~late_t() {
        if (!armed) return;
        q::enter();
        {
            const ep::pin_t pin;
            g_pin_fell_back.store(!pin.valid(), std::memory_order_relaxed);
        }
        g_a_inside.release();
        if (!g_retired.try_acquire_for(kWait)) g_timed_out.store(true, std::memory_order_relaxed);
        g_released_under_a.store(g_released.load(std::memory_order_acquire),
                                 std::memory_order_relaxed);
        if (q::leave()) q::drain();
    }
};

void a_late_bracket_blocks_every_free() {
    std::printf("a bracket and a pin opened after the thread gave its cells back\n");
    static int object = 0;
    std::thread a([] {
        static thread_local late_t late;  // built before the cells are claimed: destroyed after
        late.armed = true;
        q::enter();  // claims the QSBR cell
        (void)q::leave();
        { const ep::pin_t pin; }  // claims the edge-pin cell
        g_a_cell.store(q::rec().cell, std::memory_order_relaxed);
        g_a_pin.store(q::rec().pin, std::memory_order_relaxed);
    });
    if (!g_a_inside.try_acquire_for(kWait)) {
        check(false, "thread A reached its late bracket");
        g_retired.release();
        a.join();
        return;
    }
    std::thread b([] {
        q::enter();  // claims the lowest free cell: the one A gave back
        (void)q::leave();
        { const ep::pin_t pin; }
        g_b_cell.store(q::rec().cell, std::memory_order_relaxed);
        g_b_pin.store(q::rec().pin, std::memory_order_relaxed);
    });
    b.join();
    // The case under test is B owning the very cells A gave back; without that the late
    // bracket would have nothing to collide with and the checks below would pass vacuously.
    check(owned(g_a_cell.load()) && g_b_cell.load() == g_a_cell.load(),
          "B claimed the QSBR cell A gave back");
    check(owned(g_a_pin.load()) && g_b_pin.load() == g_a_pin.load(),
          "B claimed the edge-pin cell A gave back");
    (void)q::retire(&object, release);
    check(!g_released.load(std::memory_order_acquire),
          "a pair retired while the late bracket is open is not freed");
    g_retired.release();
    a.join();
    check(!g_timed_out.load(), "the late bracket saw the retire within the wait bound");
    check(!g_released_under_a.load(), "nothing was freed while the late bracket was open");
    check(g_pin_fell_back.load(), "the late pin takes the fallback, not a cell another owns");
    q::drain();
    check(g_released.load(std::memory_order_acquire),
          "once the late bracket closed, the pair is freed");
}

}  // namespace

int main() {
    a_late_bracket_blocks_every_free();
    return tr::testing::summary("qsbr_exit_order");
}
