/**
 * @file
 * @brief #1618 — no LKV slot policy may livelock a higher-priority reader against a
 *        lower-priority writer on one CPU.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The hazard is the one a priority-preemptive single-core RTOS has, reproduced on the host: two
 * `SCHED_FIFO` threads pinned to ONE CPU, a low-priority writer publishing in a loop and a
 * high-priority reader woken by a timer. The timer preempts the writer wherever it is — and a
 * slot whose `store` holds a lock bit that `load` SPINS on hands the reader a CPU it never gives
 * back, because `sched_yield` only yields to equal or higher priority. The writer that holds the
 * bit never runs again, and on a chip that is a task-watchdog reset.
 *
 * A watchdog on a DIFFERENT CPU measures the reader's progress. If it stalls past
 * @ref kStallLimit the watchdog calls it a livelock, demotes the reader to `SCHED_OTHER` so the
 * writer can finish its window (otherwise the test itself would never end), and fails.
 *
 * Needs `CAP_SYS_NICE` (real-time scheduling) and two CPUs. Without them it cannot build the
 * scenario at all, and it says so with exit code 77, which ctest reports as SKIPPED — not as a
 * pass. The build-time half of #1618, which runs everywhere, is `spin_slot_guard`.
 */

#include <pthread.h>
#include <sched.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "libtracer/lkv_slot.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::value_t;
using tr::testing::check;

/** @brief Reader wake-ups per policy: enough that a slot which CAN be caught mid-window is. */
constexpr std::size_t kReads = 20000;

/** @brief The reader's sleep between reads — short, so the writer is preempted often. */
constexpr long kReadPeriodNs = 20'000;

/** @brief How long the reader may make no progress before the watchdog calls it a livelock. */
constexpr auto kStallLimit = std::chrono::milliseconds(1000);

/** @brief The writer's `SCHED_FIFO` priority — the task that holds the slot's window. */
constexpr int kWriterPriority = 10;

/** @brief The reader's `SCHED_FIFO` priority — above the writer, as an ISR-fed task would be. */
constexpr int kReaderPriority = 20;

/**
 * @brief A host stand-in for the single-core critical section: one process-wide mutex.
 *
 * What the RTOS guard provides is "the reader never waits by burning the CPU the holder needs".
 * An interrupt mask gets there by making the window unpreemptable; a futex-backed mutex gets
 * there by putting the waiter to sleep, which hands the CPU back to the holder. Either is what
 * this test asks of a policy, and only the second exists in a user-space host process.
 */
struct test_mutex_guard_t {
    static constexpr bool is_isr_safe = false;        /**< @brief A host mutex. */
    static constexpr bool is_nonblocking = false;     /**< @brief The waiter sleeps. */
    static constexpr bool may_spin = false;           /**< @brief Never a pure spin. */
    static constexpr const char* name = "test_mutex"; /**< @brief Diagnostic name. */

    /** @brief Enter the one process-wide section. */
    void lock() noexcept { m_.lock(); }
    /** @brief Leave it. */
    void unlock() noexcept { m_.unlock(); }
    /** @brief The `tr::guard` lookup: one mutex every slot in this binary shares. */
    static test_mutex_guard_t& for_address(const void*) noexcept {
        static test_mutex_guard_t g;
        return g;
    }

   private:
    std::mutex m_; /**< @brief The shared mutex. */
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

/**
 * @brief Run the writer/reader pair on one CPU for one policy.
 * @return `true` iff the reader never stalled past @ref kStallLimit.
 */
/** @brief A link-less value from the heap: the cheapest thing a slot can publish. */
[[nodiscard]] value_t* make_empty() {
    value_t* v = value_t::make(std::span<const tr::view::view_t>{}, tr::mem::heap_source());
    if (v == nullptr) std::abort();
    return v;
}

template <typename slot_t>
[[nodiscard]] bool no_livelock(const char* name, int work, int watch) {
    std::printf("%s — high-priority reader vs low-priority writer on CPU %d:\n", name, work);
    slot_t slot;
    (void)slot.store(make_empty());

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> progress{0};
    std::atomic<bool> setup_ok{true};
    std::atomic<std::size_t> empty{0};
    std::atomic<std::size_t> writes{0};

    std::thread writer([&] {
        if (!pin_to(work) || !go_fifo(kWriterPriority)) setup_ok.store(false);
        while (!stop.load(std::memory_order_relaxed)) {
            (void)slot.store(make_empty());
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    pthread_t reader_handle{};
    std::atomic<bool> reader_ready{false};
    std::thread reader([&] {
        reader_handle = pthread_self();
        if (!pin_to(work) || !go_fifo(kReaderPriority)) setup_ok.store(false);
        reader_ready.store(true, std::memory_order_release);
        const timespec period{0, kReadPeriodNs};
        for (std::size_t i = 0; i < kReads; ++i) {
            clock_nanosleep(CLOCK_MONOTONIC, 0, &period, nullptr);
            if (!slot.load()) empty.fetch_add(1, std::memory_order_relaxed);
            progress.fetch_add(1, std::memory_order_release);
        }
    });

    // The watchdog: this thread, on the other CPU, so a livelock on `work` cannot starve it.
    (void)pin_to(watch);
    while (!reader_ready.load(std::memory_order_acquire)) std::this_thread::yield();
    bool stalled = false;
    std::size_t seen = progress.load(std::memory_order_acquire);
    auto last_move = std::chrono::steady_clock::now();
    while (seen < kReads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const std::size_t now = progress.load(std::memory_order_acquire);
        if (now != seen) {
            seen = now;
            last_move = std::chrono::steady_clock::now();
            continue;
        }
        if (std::chrono::steady_clock::now() - last_move > kStallLimit) {
            // Livelock. Demote the reader so the writer can leave its window and the test can
            // end; the verdict is already in.
            stalled = true;
            sched_param sp{};
            (void)pthread_setschedparam(reader_handle, SCHED_OTHER, &sp);
            break;
        }
    }
    reader.join();
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    std::printf("    %zu reads, %zu writes%s\n", progress.load(), writes.load(),
                stalled ? " — READER LIVELOCKED" : "");
    check(setup_ok.load(), "both threads got their CPU and their real-time priority");
    check(!stalled, "the reader never spun on a window the preempted writer could not leave");
    check(empty.load() == 0, "and never read an empty slot while a value was published");
    return !stalled;
}

}  // namespace

/** @brief Run the scenario on every shipped slot policy, or skip where it cannot be built. */
int main() {
    std::printf("LKV slot priority inversion (#1618)\n\n");
    int work = -1;
    int watch = -1;
    if (!two_cpus(work, watch) || !rt_available()) {
        std::printf("SKIP: needs two CPUs and CAP_SYS_NICE for SCHED_FIFO\n");
        return 77;
    }

    (void)no_livelock<tr::graph::basic_single_writer_slot_t<test_mutex_guard_t>>(
        "single_writer_slot_t", work, watch);
    (void)no_livelock<tr::graph::hazard_slot_t>("hazard_slot_t", work, watch);

    return tr::testing::summary("lkv_slot_inversion");
}
