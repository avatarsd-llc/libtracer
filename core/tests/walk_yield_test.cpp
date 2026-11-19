/**
 * @file
 * @brief A subtree walk yields to a thread waiting to take the map lock exclusively (#2846).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The proof uses no clock and no sleep. The `subject_lookup` hook runs once per node of a
 * composed read, under the walk's shared map hold, so it can park a walk there. The test then
 * starts a writer (a vertex registration), which blocks on that hold, and waits until the
 * graph counts the writer as waiting. Released, every parked walk answers `BACKPRESSURE` once
 * the node it is visiting is done, instead of finishing, so the writer runs after one node
 * visit per walk, however many walks overlap. A retried walk succeeds once no writer waits.
 *
 * The `:children` listing and `for_each_vertex` take no hook, so they are checked from a third
 * thread while a parked walk holds the lock and the writer waits: both yield at once. That arm
 * relies on the host's reader/writer lock admitting a new reader while a writer waits, which
 * glibc's default lock does.
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "graph_children_test_door.hpp"
#include "graph_sinks.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_children_test_door_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;

using tr::testing::check;

/** @brief The hook call, counted per thread, that a walk parks on. Calls 1 and 2 are the
 *         root's gates in `read` and `read_subtree_folded`, before the walk's hold; call 3 is
 *         the first child, inside it. */
constexpr int kParkCall = 3;

/** @brief Where walks park, and the gate that releases them. */
struct park_t {
    std::atomic<bool> armed{false}; /**< @brief Park walks on @ref kParkCall. */
    std::atomic<int> parked{0};     /**< @brief Walks parked right now. */
    std::atomic<bool> open{false};  /**< @brief Releases every parked walk. */
};

/** @brief This thread's hook calls since it last reset them. */
thread_local int t_calls = 0;

/** @brief The `subject_lookup` hook: names every caller "u", and parks an armed walk. */
std::expected<void, tr::wire::err_t> lookup(void* ctx, std::string_view, tr::mem::bytes_t& out,
                                            std::uint64_t&) {
    auto& p = *static_cast<park_t*>(ctx);
    if (p.armed.load() && ++t_calls == kParkCall) {
        p.parked.fetch_add(1);
        while (!p.open.load()) std::this_thread::yield();
        p.parked.fetch_sub(1);
    }
    return tr::testing::subject_into(out, "u");
}

/** @brief Spin until @p cond holds. Each wait ends on an event the test caused, never a clock. */
template <class Cond>
void wait_for(Cond cond) {
    while (!cond()) std::this_thread::yield();
}

/** @brief A graph with `/p` and eight children, `/p/c3` with a child of its own. */
struct fixture_t {
    graph_t g;         /**< @brief The graph. */
    park_t park;       /**< @brief The hook's parking state. */
    vertex_handle_t p; /**< @brief The walked branch. */

    fixture_t() : p(g.register_vertex(path_t("/p"), role_t::STORED_VALUE)) {
        for (const char* k :
             {"/p/c0", "/p/c1", "/p/c2", "/p/c3", "/p/c4", "/p/c5", "/p/c6", "/p/c7", "/p/c3/g"})
            (void)g.register_vertex(path_t(k), role_t::STORED_VALUE);
        g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {&lookup, &park});
    }

    /** @brief Writers the graph counts as waiting for the map lock. */
    [[nodiscard]] std::uint32_t waiting() const {
        return graph_children_test_door_t::map_writers_waiting(g);
    }
};

/**
 * @brief @p walks composed reads park inside their hold; a writer waits; released, every walk
 *        answers BACKPRESSURE, the writer completes, and a retry succeeds.
 */
void test_walks_yield(int walks) {
    std::printf("%d parked walk(s) yield to one waiting writer:\n", walks);
    fixture_t f;
    f.park.armed = true;
    std::vector<std::optional<status_t>> got(static_cast<std::size_t>(walks));
    std::vector<std::thread> readers;
    for (int i = 0; i < walks; ++i) {
        readers.emplace_back([&f, &got, i] {
            t_calls = 0;
            const auto r = f.g.read(f.p, "peer");
            if (!r) got[static_cast<std::size_t>(i)] = r.error();
        });
    }
    wait_for([&] { return f.park.parked.load() == walks; });

    std::atomic<bool> wrote{false};
    std::thread writer([&] {
        check(f.g.try_register_vertex(path_t("/q"), role_t::STORED_VALUE).has_value(),
              "the writer's registration lands");
        wrote = true;
    });
    // The writer is counted while it waits and uncounted once it holds the lock, so a count of
    // one with `wrote` still false is exactly "blocked behind the parked walks".
    // `wrote` ends the wait too, so a walk parked outside its hold (the hook has no vertex to
    // name, so the park is by call count) fails here instead of hanging.
    wait_for([&] { return f.waiting() == 1 || wrote.load(); });
    check(!wrote.load(), "the writer is blocked behind the parked walks");

    if (walks == 1) {
        // The two hookless walks, from a third thread, while the writer waits.
        std::thread third([&] {
            const auto folded = f.g.read_children_folded(f.p);
            check(!folded && folded.error() == status_t::BACKPRESSURE,
                  "`:children` (folded) yields: BACKPRESSURE");
            const auto flat = tr::testing::read_children_materialized(f.g, f.p);
            check(!flat && flat.error() == status_t::BACKPRESSURE,
                  "`:children` (materialized) yields: BACKPRESSURE");
            int visited = 0;
            check(!f.g.for_each_vertex([&visited](auto, vertex_handle_t) { ++visited; }),
                  "for_each_vertex yields: false");
            check(visited == 0, "a yielded for_each_vertex visits nothing");
        });
        third.join();
    }

    f.park.open = true;
    for (std::thread& t : readers) t.join();
    writer.join();
    for (const std::optional<status_t>& s : got)
        check(s == status_t::BACKPRESSURE,
              "a walk that saw the waiting writer answers BACKPRESSURE");
    check(wrote.load(), "the writer completed after the walks yielded");
    check(f.waiting() == 0, "no writer is counted once it has the lock");

    // Retried with no writer waiting, every walk completes.
    f.park.armed = false;
    check(f.g.read(f.p, "peer").has_value(), "a retried composed read completes");
    check(f.g.read_children_folded(f.p).has_value(), "a retried `:children` completes");
    int visited = 0;
    check(f.g.for_each_vertex([&visited](auto, vertex_handle_t) { ++visited; }),
          "a retried for_each_vertex completes");
    check(visited >= 11, "and visits every vertex, the writer's included");
}

}  // namespace

int main() {
    test_walks_yield(1);
    test_walks_yield(2);
    test_walks_yield(4);
    return tr::testing::summary("walk_yield");
}
