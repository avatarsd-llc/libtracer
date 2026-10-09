/**
 * @file
 * @brief `tr::seq_latch_t` under a tight publish-versus-read hammer (#2032): a reader never
 *        copies a record assembled from two publishes, and never goes back in time.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One writer publishes `{i, ~i}` into a two-word side as fast as it can; readers copy the pair
 * out as fast as they can. A copy whose words do not belong together is a TEAR; a copy older
 * than one the same reader already saw is a REGRESSION. Both must be zero.
 *
 * The control arm is what gives the hammer its teeth: the SAME two words, the same writer and
 * the same readers, but published with no latch at all — two relaxed stores, two relaxed
 * loads. On a host with two or more hardware threads it tears within the budget, so a latch
 * that let a reader through mid-publish would be caught here too. (ThreadSanitizer cannot see
 * a tear: every word is an atomic, so the race it would report does not exist. The control is
 * the proof that this test can.)
 */

#include "libtracer/seq_latch.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "test_support.hpp"

namespace {

using tr::testing::check;

/** @brief Publishes per arm. Large enough that an unlatched pair tears many times over. */
constexpr std::uint32_t kPublishes = 2'000'000;
/** @brief Concurrent readers per arm. */
constexpr int kReaders = 2;

/** @brief The record: two words that belong together only when `b == ~a`. */
struct pair_side_t {
    std::atomic<std::uint32_t> a{0};   /**< @brief The publish index. */
    std::atomic<std::uint32_t> b{~0U}; /**< @brief Its complement. */
};

/** @brief A plain copy of one record. */
struct pair_t {
    std::uint32_t a; /**< @brief The publish index read. */
    std::uint32_t b; /**< @brief The complement read beside it. */
};

/** @brief What the readers of one arm saw. */
struct tally_t {
    std::atomic<long> reads{0};       /**< @brief Copies made. */
    std::atomic<long> tears{0};       /**< @brief Copies whose words do not belong together. */
    std::atomic<long> regressions{0}; /**< @brief Copies older than the reader's previous one. */
};

/** @brief Run one arm: @p publish(i) by one writer, @p read() by `kReaders` readers. */
template <class Publish, class Read>
void hammer(Publish publish, Read read, tally_t& t) {
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r)
        readers.emplace_back([&] {
            std::uint32_t last = 0;
            long reads = 0;
            long tears = 0;
            long regressions = 0;
            while (!done.load(std::memory_order_acquire)) {
                const pair_t p = read();
                ++reads;
                if (p.b != ~p.a) ++tears;
                if (p.a < last) ++regressions;
                last = p.a;
            }
            t.reads.fetch_add(reads);
            t.tears.fetch_add(tears);
            t.regressions.fetch_add(regressions);
        });
    std::thread writer([&] {
        for (std::uint32_t i = 1; i <= kPublishes; ++i) publish(i);
        done.store(true, std::memory_order_release);
    });
    writer.join();
    for (std::thread& r : readers) r.join();
}

/** @brief The latch arm: no tear and no regression, however hard the writer pushes. */
void test_latch_never_tears() {
    std::printf("the latch under a tight publish/read hammer:\n");
    tr::seq_latch_t<pair_side_t> latch;
    tally_t t;
    hammer(
        [&](std::uint32_t i) {
            (void)latch.publish([i](pair_side_t& s) {
                s.a.store(i, std::memory_order_relaxed);
                s.b.store(~i, std::memory_order_relaxed);
                return true;
            });
        },
        [&] {
            return latch.read([](const pair_side_t& s) {
                return pair_t{s.a.load(std::memory_order_relaxed),
                              s.b.load(std::memory_order_relaxed)};
            });
        },
        t);
    std::printf("    (%ld reads over %u publishes)\n", t.reads.load(), kPublishes);
    check(t.tears.load() == 0, "no read ever copied words from two publishes");
    check(t.regressions.load() == 0, "no reader ever saw an older publish after a newer one");
}

/** @brief The control arm: the same pair with no latch tears, so the hammer can see a tear. */
void test_unlatched_pair_tears() {
    std::printf("control — the same pair published with no latch:\n");
    if (std::thread::hardware_concurrency() < 2) {
        std::printf("    (skipped: one hardware thread cannot interleave a copy)\n");
        return;
    }
    pair_side_t bare;
    tally_t t;
    hammer(
        [&](std::uint32_t i) {
            bare.a.store(i, std::memory_order_relaxed);
            bare.b.store(~i, std::memory_order_relaxed);
        },
        [&] {
            return pair_t{bare.a.load(std::memory_order_relaxed),
                          bare.b.load(std::memory_order_relaxed)};
        },
        t);
    std::printf("    (%ld reads, %ld torn)\n", t.reads.load(), t.tears.load());
    check(t.tears.load() > 0, "an unlatched pair tears under the same hammer");
}

/** @brief A publish whose fill refuses leaves the current record standing. */
void test_refused_publish_changes_nothing() {
    std::printf("a refused publish:\n");
    tr::seq_latch_t<pair_side_t> latch;
    (void)latch.publish([](pair_side_t& s) {
        s.a.store(7, std::memory_order_relaxed);
        s.b.store(~7U, std::memory_order_relaxed);
        return true;
    });
    const bool published = latch.publish([](pair_side_t& s) {
        s.a.store(9, std::memory_order_relaxed);  // half-written, then refused
        return false;
    });
    const pair_t p = latch.read([](const pair_side_t& s) {
        return pair_t{s.a.load(std::memory_order_relaxed), s.b.load(std::memory_order_relaxed)};
    });
    check(!published, "the refusal is reported");
    check(p.a == 7 && p.b == ~7U, "and readers still read the previous record, whole");
}

}  // namespace

int main() {
    test_latch_never_tears();
    test_unlatched_pair_tears();
    test_refused_publish_changes_nothing();
    return tr::testing::summary("seq_latch");
}
