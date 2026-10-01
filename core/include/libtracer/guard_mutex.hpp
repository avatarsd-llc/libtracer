/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * guard_mutex — the host's critical-section guard, the hosted half of the guard vocabulary.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "libtracer/guard.hpp"

/**
 * @file
 * @brief `tr::mutex_guard_t`, the host's address-striped guard — the HOSTED half of the guard
 *        vocabulary, apart from `%guard.hpp` because its contender sleeps (`<thread>`).
 *
 * A config override fragment must never include this header: `%config.hpp` includes the
 * fragment, and a fragment that pulled a libtracer header back in would be an include cycle.
 * `%config.hpp` forward-declares the type instead, which is all a `using guard_t = ...;`
 * binding needs.
 */

namespace tr {

/**
 * @brief The host guard: a one-word lock, and a table of them striped by address.
 *
 * On a single-core RTOS the guard is an interrupt-masked critical section (the ESP-IDF
 * component binds `tr::esp::critical_guard_t`), which makes the window unpreemptable. A host
 * process cannot mask interrupts, so this is the host's spelling of the same promise: a
 * contender that finds the window held gives the CPU back to whoever holds it, instead of
 * spinning until the holder is scheduled again.
 *
 * ## The shape of the lock, and why it is not `std::mutex`
 *
 * Taking it is one read-modify-write (`exchange`), releasing it is one plain release store.
 * The first cut of the slot's guard was a `std::mutex` per stripe, and the blocking perf gate
 * refused it (#1628): `inproc-target-stored/64/8/1` lost 13 % of its deliveries per second,
 * ~11 ns per guarded section on the gate's runner, one section per delivery. A parking mutex
 * cannot be cheaper than two read-modify-writes: its unlock has to publish the release AND then
 * look for a sleeper, and that store-then-load pair needs a full fence or an RMW or it loses a
 * wakeup.
 *
 * This lock has no wakeup to lose, so its unlock needs no fence. A contender re-reads the flag
 * @ref kSpinsBeforeNap times, then SLEEPS for @ref kNap and looks again, for as long as it
 * takes. The holder's window is a pointer swap, a handle copy or a free-list edit — a handful of
 * instructions — so a contender only ever reaches the nap when the holder was descheduled inside
 * the window, and the nap is exactly what lets a descheduled holder run: on one CPU under
 * priority preemption a `sched_yield` spinner never lets a lower-priority holder back (the #1618
 * hang); a sleeper does. `lkv_slot_inversion` checks that on the host with `SCHED_FIFO`. That
 * is why @ref may_spin is `false`: the wait is bounded by the holder's window, never by the
 * spinner's priority.
 *
 * ## Striped, by address
 *
 * A lock per LKV slot would put bytes in every vertex, and `sizeof(vertex_t)` is ratcheted, so
 * the slot takes @ref for_address — a static table of padded locks costing `kStripes * 64`
 * bytes once; two vertices share a lock only when their addresses hash to the same stripe. A
 * pool owns its own instance instead.
 */
struct alignas(64) mutex_guard_t {
    /** @brief Stripes in the process-wide table @ref for_address draws from. */
    static constexpr std::size_t kStripes = 64;

    /** @brief Re-reads of a held flag before a contender sleeps; covers a cross-core release. */
    static constexpr unsigned kSpinsBeforeNap = 128;

    /**
     * @brief How long a contender sleeps between looks once it has spun out.
     *
     * Only a holder descheduled inside its few-instruction window makes anyone sleep, so this
     * bounds the extra latency of that rare case, not the common one. Bounded from below by
     * what one `nanosleep` costs anyway.
     */
    static constexpr std::chrono::microseconds kNap{20};

    static constexpr bool is_isr_safe = false; /**< @brief A host thread lock, never an ISR's. */
    /** @brief A contender that spins out SLEEPS — an OS wait (#928). */
    static constexpr bool is_nonblocking = false;
    /** @brief The wait naps rather than spins, so it is safe on any target (see the class). */
    static constexpr bool may_spin = false;
    static constexpr const char* name = "mutex_guard"; /**< @brief Census name. */

    mutex_guard_t() noexcept = default;
    mutex_guard_t(const mutex_guard_t&) = delete;
    mutex_guard_t& operator=(const mutex_guard_t&) = delete;

    /** @brief Take the lock: one RMW, and the out-of-line wait only when it was held. */
    void lock() noexcept {
        if (!taken_.exchange(true, std::memory_order_acquire)) [[likely]]
            return;
        wait_for_window();
    }
    /** @brief Give the lock back: a release store, and nobody to notify (see the class). */
    void unlock() noexcept { taken_.store(false, std::memory_order_release); }

    /** @brief The stripe the address @p at hashes to: a Fibonacci hash of the address. */
    static mutex_guard_t& for_address(const void* at) noexcept {
        static mutex_guard_t table[kStripes];
        auto a = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(at));
        a ^= a >> 17;
        a *= 0x9E3779B97F4A7C15ull;
        return table[(a >> 58) % kStripes];
    }

   private:
    // The flag must be a lock-free atomic, or the "one RMW" above is a libatomic lock. That is
    // asserted in `vertex.hpp` beside the BINDING, not here: this header is included by every
    // consumer of a vertex, esp32c3 (rv32imc, no atomics at all) included, and a class-scope
    // assertion fires there even though that target binds an interrupt-masked guard and never
    // instantiates this one (#1628, the C3 legs).

    /**
     * @brief The contended path: re-read, then nap, until an `exchange` finds the flag clear.
     *
     * Out of line so the uncontended @ref lock stays one RMW and a branch at every call site.
     * Reads before the RMW so contenders do not bounce the line between them while they wait.
     */
    [[gnu::noinline]] void wait_for_window() noexcept {
        for (;;) {
            for (unsigned spins = 0; taken_.load(std::memory_order_relaxed); ++spins) {
                if (spins >= kSpinsBeforeNap) {
                    std::this_thread::sleep_for(kNap);
                    spins = 0;
                }
            }
            if (!taken_.exchange(true, std::memory_order_acquire)) return;
        }
    }

    std::atomic<bool> taken_{false}; /**< @brief Whether some thread is inside the window. */
};

}  // namespace tr
