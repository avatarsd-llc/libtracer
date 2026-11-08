/**
 * @file
 * @brief A suite's own clock for `graph_t::await` (#2579): the deadline the application runs.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * libtracer reads no clock, so a blocking `await` ends on a change, or when the application
 * passes a `now` past its deadline to `graph_t::expire_awaits`. A suite is that application
 * here: these helpers read the suite's steady clock and run a ticker thread that makes the
 * call at the deadline, so a check written against a time bound keeps that bound.
 *
 * `tr::testing` is a tests-only namespace — nothing under `core/src` or `core/include` may
 * name it.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include "libtracer/graph.hpp"

namespace tr::testing {

/** @brief The suite's clock, in nanoseconds: the `now` it hands the graph. */
inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

/**
 * @brief Run @p fn while a ticker passes the suite's clock to `g.expire_awaits`, from
 *        @p deadline on, until @p fn returns.
 *
 * The ticker repeats every millisecond past the deadline, because a wait that arms after
 * one call would otherwise miss it. @p now is what it passes; @ref end_every_await passes
 * `kNoDeadline`, which also ends a wait that has no deadline (the synchronous wire AWAIT).
 */
template <class F>
auto with_expiry(tr::graph::graph_t& g, std::uint64_t deadline, F&& fn,
                 std::uint64_t (*now)() = &now_ns) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::thread ticker([&] {
        std::unique_lock lk(m);
        const std::chrono::steady_clock::time_point at{std::chrono::nanoseconds(deadline)};
        cv.wait_until(lk, at, [&] { return done; });
        while (!done) {
            lk.unlock();
            (void)g.expire_awaits(now());
            lk.lock();
            cv.wait_for(lk, std::chrono::milliseconds(1), [&] { return done; });
        }
    });
    auto r = std::forward<F>(fn)();
    {
        const std::lock_guard lk(m);
        done = true;
    }
    cv.notify_one();
    ticker.join();
    return r;
}

/** @brief The `now` that ends every blocking await, deadline or not (the teardown value). */
inline std::uint64_t end_every_await() noexcept { return tr::graph::kNoDeadline; }

/**
 * @brief `g.await(target, now, now + within, extra...)` on the suite's clock: the wait ends
 *        on a change, or with `TIMEOUT` once @p within has passed.
 */
template <class T, class... A>
auto await_within(tr::graph::graph_t& g, const T& target, std::chrono::nanoseconds within,
                  const A&... extra) {
    const std::uint64_t now = now_ns();
    const std::uint64_t deadline = now + static_cast<std::uint64_t>(within.count());
    return with_expiry(g, deadline, [&] { return g.await(target, now, deadline, extra...); });
}

/**
 * @brief `v.wait_for_change(...)` with a deadline @p within from now on the suite's clock,
 *        owned by @p g (whose `expire_awaits` ends it).
 */
inline bool wait_for_change_within(tr::graph::graph_t& g, tr::graph::vertex_t& v,
                                   std::chrono::nanoseconds within) {
    tr::graph::await_deadline_t d{
        .owner = &g, .deadline = now_ns() + static_cast<std::uint64_t>(within.count())};
    return with_expiry(g, d.deadline, [&] { return v.wait_for_change(d); });
}

}  // namespace tr::testing
