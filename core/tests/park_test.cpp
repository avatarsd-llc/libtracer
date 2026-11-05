/**
 * @file
 * @brief #2510 — `tr::park_wait` / `tr::park_wake` (`libtracer/park.hpp`, #2507) on their own,
 *        not only through the egress and ESP-IDF WS client teardown paths that call them.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 *  1. A sleeper is woken by a store and a wake, and sees the stored value.
 *  2. Wake before wait: a store and its wake that land before the sleeper parks are never
 *     slept through; a word that already moved returns at once. Raced many times.
 *  3. A spurious wake (a wake with no store, or a wake for another word on the same stripe)
 *     sends the sleeper back to sleep; it returns only once its own word moves.
 *  4. Stripe collisions: several sleepers on two distinct words that share a stripe each
 *     wake on their own word's store, and none is lost.
 *  5. A wake after the waited-on object is gone: the last sender stores, the drainer frees
 *     the word, and the sender's wake (which reads only the address) still runs clean.
 *
 * The only clock is the bounded wait in `wait_flag` that keeps a lost wake from hanging the
 * suite; park itself takes none.
 */

#include "libtracer/park.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::testing::check;

/** @brief Wait up to a bound for @p flag to be set, so a lost wake fails instead of hanging. */
bool wait_flag(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

/** @brief Two words, in one array, that `park_stripe_of` puts on the same stripe. */
struct colliding_words_t {
    std::atomic<std::uint32_t> words[64]{};  /**< @brief Candidates for the pair. */
    std::atomic<std::uint32_t>* a = nullptr; /**< @brief The first word of the pair. */
    std::atomic<std::uint32_t>* b = nullptr; /**< @brief A distinct word on @ref a's stripe. */

    colliding_words_t() {
        a = &words[0];
        for (auto& w : words) {
            if (&w != a && &tr::detail::park_stripe_of(&w) == &tr::detail::park_stripe_of(a)) {
                b = &w;
                break;
            }
        }
    }
};

void woken_by_store_and_wake() {
    std::atomic<std::uint32_t> word{0};
    std::atomic<bool> done{false};
    std::uint32_t seen = 0;
    std::thread sleeper([&] {
        tr::park_wait(word, 0u, std::memory_order_acquire);
        seen = word.load(std::memory_order_relaxed);
        done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(20ms);  // let it park; correctness does not depend on it
    word.store(7, std::memory_order_release);
    tr::park_wake(word);
    check(wait_flag(done), "a store + wake wakes the sleeper");
    sleeper.join();
    check(seen == 7, "the sleeper sees the stored value");
}

void wake_before_wait() {
    std::atomic<std::uint32_t> moved{1};
    tr::park_wait(moved, 0u, std::memory_order_acquire);  // already moved: returns at once
    check(true, "a word that already moved returns at once");

    // The store + wake race the sleeper's park: whichever order they land in, it returns.
    constexpr int kRounds = 2000;
    int woke = 0;
    for (int i = 0; i < kRounds; ++i) {
        std::atomic<std::uint32_t> word{0};
        std::atomic<bool> done{false};
        std::thread sleeper([&] {
            tr::park_wait(word, 0u, std::memory_order_acquire);
            done.store(true, std::memory_order_release);
        });
        word.store(1, std::memory_order_release);
        tr::park_wake(word);
        if (!wait_flag(done)) {
            // Unstick the sleeper so the suite can report instead of hanging on join.
            word.store(2, std::memory_order_release);
            tr::park_wake(word);
        } else {
            ++woke;
        }
        sleeper.join();
    }
    check(woke == kRounds, "no wake is lost when the store and wake race the park");
}

void spurious_wake_rechecks() {
    colliding_words_t cw;
    check(cw.b != nullptr, "two words share a stripe");
    if (cw.b == nullptr) return;
    std::atomic<bool> done{false};
    std::thread sleeper([&] {
        tr::park_wait(*cw.a, 0u, std::memory_order_acquire);
        done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(20ms);
    // A wake with no store, and a store + wake for the other word on the same stripe: both
    // reach the sleeper's condition variable, and both must send it back to sleep.
    tr::park_wake(*cw.a);
    cw.b->store(1, std::memory_order_release);
    tr::park_wake(*cw.b);
    std::this_thread::sleep_for(20ms);
    check(!done.load(std::memory_order_acquire), "a spurious wake does not end the wait");
    cw.a->store(1, std::memory_order_release);
    tr::park_wake(*cw.a);
    check(wait_flag(done), "the sleeper's own store + wake ends it");
    sleeper.join();
}

void stripe_collisions() {
    colliding_words_t cw;
    check(cw.b != nullptr, "two words share a stripe");
    if (cw.b == nullptr) return;
    constexpr int kPerWord = 4;
    std::atomic<int> woke_a{0};
    std::atomic<int> woke_b{0};
    std::vector<std::thread> sleepers;
    for (int i = 0; i < kPerWord; ++i) {
        sleepers.emplace_back([&] {
            tr::park_wait(*cw.a, 0u, std::memory_order_acquire);
            woke_a.fetch_add(1, std::memory_order_acq_rel);
        });
        sleepers.emplace_back([&] {
            tr::park_wait(*cw.b, 0u, std::memory_order_acquire);
            woke_b.fetch_add(1, std::memory_order_acq_rel);
        });
    }
    std::this_thread::sleep_for(20ms);
    cw.a->store(1, std::memory_order_release);
    tr::park_wake(*cw.a);
    std::atomic<bool> a_all{false};
    std::thread watch_a([&] {
        while (woke_a.load(std::memory_order_acquire) < kPerWord) std::this_thread::yield();
        a_all.store(true, std::memory_order_release);
    });
    const bool a_ok = wait_flag(a_all);
    check(a_ok, "every sleeper on the first word wakes on its store");
    check(woke_b.load(std::memory_order_acquire) == 0, "no sleeper on the other word returns");
    cw.b->store(1, std::memory_order_release);
    tr::park_wake(*cw.b);
    if (!a_ok) {
        cw.a->store(2, std::memory_order_release);
        tr::park_wake(*cw.a);
    }
    for (auto& t : sleepers) t.join();
    watch_a.join();
    check(woke_b.load(std::memory_order_acquire) == kPerWord,
          "every sleeper on the second word wakes on its own store");
}

void wake_after_object_gone() {
    // The teardown drain: the drainer waits for the sender count to reach zero and frees the
    // object; the last sender's wake runs after its store, possibly after the free.
    constexpr int kRounds = 500;
    for (int i = 0; i < kRounds; ++i) {
        auto owned = std::make_unique<std::atomic<std::uint32_t>>(1);
        std::atomic<std::uint32_t>* word = owned.get();
        std::thread sender([word] {
            word->store(0, std::memory_order_release);
            tr::park_wake(*word);  // reads only the address
        });
        for (std::uint32_t n = word->load(std::memory_order_acquire); n != 0;
             n = word->load(std::memory_order_acquire)) {
            tr::park_wait(*word, n, std::memory_order_acquire);
        }
        owned.reset();
        sender.join();
    }
    check(true, "a wake after the word is freed runs clean");
}

}  // namespace

int main() {
    woken_by_store_and_wake();
    wake_before_wait();
    spurious_wake_rechecks();
    stripe_collisions();
    wake_after_object_gone();
    return tr::testing::summary("park");
}
