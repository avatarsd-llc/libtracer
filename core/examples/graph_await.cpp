/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the readiness plane: `await` blocks until the next store here.
 *
 * `await` is the third of the three data calls (`CONTEXT.md` §read / write / await). It is
 * single-shot and lives wholly in the state plane: it observes assigns AT ITS OWN VERTEX and
 * is not subtree-scoped, so a write to a descendant does not wake it
 * (`docs/reference/02-graph-model.md` §Assign, propagate). A deadline that expires answers
 * `TIMEOUT`, which is why a consumer can tell a quiet vertex from a delivered value.
 *
 * Runs under ctest as `example_graph_await`; returns non-zero on any failed check.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>

#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;

/** @brief An owned one-segment view over @p text. */
tr::view::view_t value_of(std::string_view text) {
    return *tr::view::over_bytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief This application's clock, in milliseconds: the `now` it hands the graph. */
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

/**
 * @brief The application's side of an await's deadline: pass `now` to expire_awaits until
 *        @p done is set. libtracer reads no clock, so this loop is what ends a wait that is due.
 */
void drive_until(tr::graph::graph_t& g, const std::atomic<bool>& done) {
    while (!done.load(std::memory_order_acquire)) {
        (void)g.expire_awaits(now_ms());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

int main() {
    tr::graph::graph_t g;
    bool ok = true;

    const auto temp = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    const auto child = g.register_vertex(path_t("/sensor/temp/raw"), role_t::STORED_VALUE);

    // A waiter parks in await(); the writer wakes it with the value it stored. Its deadline,
    // 2 s on this app's clock, bounds a write that lands before it parks: that costs a
    // TIMEOUT, never a hang.
    bool woke = false;
    std::atomic<bool> answered{false};
    std::thread waiter([&] {
        const std::uint64_t now = now_ms();
        const auto r = g.await(temp, now, now + 2000);
        woke = r && (*r)->only().bytes().size() == 5;
        answered.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(50ms);  // let the waiter park before the write lands

    (void)g.write(child, value_of("noise"));  // a DESCENDANT write — not this vertex
    (void)g.write(temp, value_of("21.5C"));
    drive_until(g, answered);
    waiter.join();
    check(ok, woke, "await returns the value assigned at its own vertex");

    // await is not subtree-scoped, and the deadline is honoured: nothing writes /sensor/temp
    // again, so this one expires rather than picking up the descendant write above.
    tr::graph::result_t<tr::graph::value_ref_t> quiet;
    std::atomic<bool> done{false};
    std::thread late([&] {
        const std::uint64_t now = now_ms();
        quiet = g.await(temp, now, now + 20);
        done.store(true, std::memory_order_release);
    });
    drive_until(g, done);
    late.join();
    check(ok, !quiet && quiet.error() == status_t::TIMEOUT, "an expired deadline answers TIMEOUT");
    std::printf("await woke once and timed out once, as expected\n");
    return ok ? 0 : 1;
}
