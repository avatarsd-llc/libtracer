/**
 * @file
 * @brief Delivery order across concurrent writers to one vertex (#2423, reference/15 O3).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A write delivers on the writer's own thread, after its store and outside any lock that a
 * second writer's store or delivery would wait on: `graph_t::write_impl` publishes the value
 * through `store_value` and only then calls `deliver_vertex`, and `graph_t::fan_out` releases
 * its edge pin before it dispatches. Nothing joins one writer's store to its own delivery, so a
 * second writer's later store can reach a subscriber first.
 *
 * So the claim reference/15 O3 can make is per writer: one writer's assigns reach each
 * subscriber in that writer's order, because that writer delivers them one after another on one
 * thread. Across two writers, a subscriber may see a later assign before an earlier one.
 *
 * This file shows both halves, deterministically. Nothing sleeps and nothing reads a clock:
 * writer 1 is held inside its own delivery by a promise that writer 2 completes, so the
 * interleaving is forced, not hoped for.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief The value writer 1 assigns, first. */
constexpr std::uint8_t kFirst = 0xA1;
/** @brief The value writer 2 assigns, second. */
constexpr std::uint8_t kSecond = 0xB2;

/**
 * @brief What one callback subscriber received, in arrival order.
 *
 * Two writer threads append to it, so a mutex guards it; the mutex is the test's, not the
 * graph's, and is never held across a graph call.
 */
struct record_t {
    std::mutex lock;                /**< @brief Guards @ref seen. */
    std::vector<std::uint8_t> seen; /**< @brief First byte of each delivered value. */

    /** @brief Append @p b under the lock. */
    void push(std::uint8_t b) {
        const std::lock_guard<std::mutex> hold(lock);
        seen.push_back(b);
    }
};

/** @brief The first byte of a delivered single-link value. */
std::uint8_t first_byte(const tr::graph::value_t& v) {
    return std::to_integer<std::uint8_t>(v.only().bytes()[0]);
}

/** @brief Index of @p b in @p seen, or `seen.size()` when absent. */
std::size_t index_of(const std::vector<std::uint8_t>& seen, std::uint8_t b) {
    std::size_t i = 0;
    while (i < seen.size() && seen[i] != b) ++i;
    return i;
}

/**
 * @brief One writer's writes reach every subscriber in that writer's order.
 */
void test_one_writer_keeps_its_order() {
    std::printf("one writer: each subscriber sees its writes in write order:\n");
    graph_t g;
    const auto vh = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    record_t s1;
    record_t s2;
    auto on_s1 = [&s1](const tr::graph::value_t& v) { s1.push(first_byte(v)); };
    auto on_s2 = [&s2](const tr::graph::value_t& v) { s2.push(first_byte(v)); };
    check(g.subscribe(path_t("/v"), on_s1).has_value(), "subscribe s1");
    check(g.subscribe(path_t("/v"), on_s2).has_value(), "subscribe s2");

    check(g.write(vh, make_value({kFirst})).has_value(), "write first");
    check(g.write(vh, make_value({kSecond})).has_value(), "write second");

    const std::vector<std::uint8_t> want{kFirst, kSecond};
    check(s1.seen == want, "s1 received first, then second");
    check(s2.seen == want, "s2 received first, then second");
}

/**
 * @brief Two writers: a later assign's delivery overtakes an earlier one's.
 *
 * Writer 1 writes `kFirst`. Its store lands, and its delivery starts. The first subscriber it
 * reaches parks writer 1 inside that callback until writer 2 has finished. Writer 2 then
 * writes `kSecond`: its store lands after writer 1's (the vertex's value afterwards is
 * `kSecond`), and its delivery reaches both subscribers while writer 1 is still parked. When
 * writer 1 resumes, it delivers `kFirst` to the subscriber it had not reached yet. That
 * subscriber has received the later assign before the earlier one.
 *
 * Which subscriber parks writer 1 depends only on the edge snapshot order, so the test claims
 * the park with an atomic flag instead of assuming one.
 */
void test_two_writers_can_reorder() {
    std::printf("two writers: a later assign can reach a subscriber first:\n");
    graph_t g;
    const auto vh = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);

    std::promise<void> w1_parked;    // writer 1 has stored and is inside its delivery
    std::promise<void> w2_finished;  // writer 2's write returned, deliveries included
    std::shared_future<void> release = w2_finished.get_future().share();
    std::atomic<bool> parked{false};

    record_t s1;
    record_t s2;
    // Each callback records the value first, then, on writer 1's value only and only once,
    // parks writer 1 until writer 2 is done. Writer 2's callbacks never wait.
    auto make_sink = [&](record_t& rec) {
        return [&rec, &parked, &w1_parked, release](const tr::graph::value_t& v) {
            const std::uint8_t b = first_byte(v);
            rec.push(b);
            if (b == kFirst && !parked.exchange(true)) {
                w1_parked.set_value();
                release.wait();
            }
        };
    };
    auto on_s1 = make_sink(s1);
    auto on_s2 = make_sink(s2);
    check(g.subscribe(path_t("/v"), on_s1).has_value(), "subscribe s1");
    check(g.subscribe(path_t("/v"), on_s2).has_value(), "subscribe s2");

    std::future<void> parked_now = w1_parked.get_future();
    bool w1_ok = false;
    std::thread writer1(
        [&g, vh, &w1_ok] { w1_ok = g.write(vh, make_value({kFirst})).has_value(); });
    parked_now.wait();  // writer 1's store has landed; its delivery is under way

    // Writer 2, on this thread: assigned after writer 1's store, delivered in full now.
    check(g.write(vh, make_value({kSecond})).has_value(), "writer 2's write");
    w2_finished.set_value();
    writer1.join();
    check(w1_ok, "writer 1's write");

    // Assign order is first, then second: the vertex keeps the later one.
    const auto now = g.read(vh);
    check(now.has_value() && first_byte(**now) == kSecond,
          "the vertex holds writer 2's value: it was assigned after writer 1's");

    // Each subscriber got both values once.
    check(s1.seen.size() == 2 && s2.seen.size() == 2, "each subscriber received both values");
    // The subscriber writer 1 had not reached yet received them in reverse assign order.
    record_t& late = index_of(s1.seen, kFirst) == 0 ? s2 : s1;
    check(index_of(late.seen, kSecond) < index_of(late.seen, kFirst),
          "a subscriber received the later assign before the earlier one");
}

}  // namespace

int main() {
    test_one_writer_keeps_its_order();
    test_two_writers_can_reorder();
    return tr::testing::summary("writer_order_race");
}
