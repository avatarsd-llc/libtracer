/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * park — sleep until an atomic word moves, without libstdc++'s shared waiter table.
 */
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

/**
 * @file
 * @brief `%tr::park_wait` and `%tr::park_wake`: `std::atomic::wait` and `notify_all` spelled
 *        over a small address-striped table of mutex + condition-variable pairs (#2507).
 *
 * `std::atomic::wait` is free on a futex host, but libstdc++ builds it on every target over
 * one shared waiter table, `std::__detail::__waiter_pool_base::_S_for::__w`, 2,048 B of
 * static RAM on esp32c6 the moment any translation unit instantiates a wait or a notify.
 * These two functions keep the same contract (block while the word still holds the value
 * the caller read, wake every sleeper on the word) at the cost of eight pairs: 64 B of
 * static RAM on ESP-IDF, where both the mutex and the condition variable are one word.
 *
 * Only contended or teardown paths reach either function; an uncontended caller never
 * touches the table. A word sharing a stripe with another costs a spurious wake and a
 * re-check, never a lost one. The table is its own, not the graph's vertex stripes, so a
 * caller holding a vertex stripe lock can still park and wake.
 */

namespace tr {

/** @brief Stripes in the park table; a collision is a spurious wake, never a missed one. */
inline constexpr std::size_t kParkStripes = 8;

/** @brief One stripe of the park table: the lock a sleeper re-checks its word under, and
 *         the condition variable it sleeps on. */
struct park_stripe_t {
    std::mutex m;               /**< @brief Orders a sleeper's re-check against a wake. */
    std::condition_variable cv; /**< @brief Where sleepers on this stripe's words wait. */
};

/** @brief The stripe @p word parks on. A guarded function-local static: the condition
 *         variable is never constant-initialized, and only a sleeper or a wake reaches it. */
inline park_stripe_t& park_stripe_of(const void* word) {
    static std::array<park_stripe_t, kParkStripes> stripes;
    auto h = reinterpret_cast<std::uintptr_t>(word);
    h ^= h >> 9;  // fold higher entropy into the allocation-aligned low bits
    return stripes[(h >> 4) % kParkStripes];
}

/**
 * @brief Block until @p word no longer holds @p old, the `std::atomic::wait` contract.
 *
 * Returns at once if the word already moved. The re-check runs under the stripe lock, and
 * @ref park_wake takes that lock after the store it announces, so a store landing between
 * the caller's read and this call is never slept through.
 */
template <class T>
void park_wait(const std::atomic<T>& word, T old, std::memory_order order) {
    park_stripe_t& s = park_stripe_of(&word);
    std::unique_lock<std::mutex> lock(s.m);
    s.cv.wait(lock, [&] { return word.load(order) != old; });
}

/**
 * @brief Wake every sleeper on @p word, the `notify_all` contract. Call it after the store
 *        the sleepers wait for.
 *
 * @note Reads and writes nothing at @p word: its address only picks the stripe. The owner of
 *       the word may therefore free it the moment the store lands, while this call is still
 *       running, which is what a teardown drain needs from its last sender's wake.
 */
template <class T>
void park_wake(const std::atomic<T>& word) {
    park_stripe_t& s = park_stripe_of(&word);
    { const std::lock_guard<std::mutex> lock(s.m); }
    s.cv.notify_all();
}

}  // namespace tr
