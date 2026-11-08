/**
 * @file
 * @brief `await` waking on every role that publishes, and both `rmw_counter_t` bindings
 *        counting exactly (#1621, #2655).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A vertex carries no write sequence (#2655): a write wakes an awaiter by unlinking it. This
 * test pins:
 *
 *  - the WAKE on all three shapes a publish can take: a STORED_VALUE vertex, a HANDLER vertex
 *    (stores nothing; its `on_write` consumes), and a `retention_t::NONE` value vertex (stores
 *    nothing). The last two are why the wake cannot be keyed on the published value;
 *  - the ENTRY WINDOW, deterministically: a write that lands after `await` is entered and
 *    before it sleeps (here, from inside its own ACL gate) wakes it, with no second write and
 *    no expiry. `graph_t::await` arms before its gate, so the arm is where the wait begins;
 *  - BOTH `rmw_counter_t` bindings count every bump exactly under concurrent bumpers: the
 *    native `fetch_add`, and the guarded load + store a core with no atomic RMW (ESP32-C3,
 *    Cortex-M0) compiles to, driven here on the host by naming that binding, across the wrap.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <string_view>
#include <thread>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/guard.hpp"
#include "libtracer/guard_mutex.hpp"
#include "libtracer/tracer.hpp"
#include "test_await.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace tr {

/** @brief The test-only door `guard.hpp` declares (#1719): presets an `rmw_counter_t`. */
struct rmw_counter_test_door_t {
    /** @brief Store @p value into @p c outright (`seq_cst`, like the bump). */
    template <class T, class G, bool kNative>
    static void preset(rmw_counter_t<T, G, kNative>& c, T value) noexcept {
        c.preset(value);
    }
};

}  // namespace tr

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::retention_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::graph::vertex_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief The guarded binding, named on a host that has atomic RMW so CI can drive it. */
using guarded_counter_t = tr::rmw_counter_t<std::uint32_t, tr::mutex_guard_t, false>;
/** @brief The native binding, named explicitly so the pair below reads side by side. */
using native_counter_t = tr::rmw_counter_t<std::uint32_t, tr::mutex_guard_t, true>;
static_assert(!guarded_counter_t::is_native && native_counter_t::is_native);

/**
 * @brief Four threads bump one counter 100,000 times each; the count must come out exact.
 *
 * A lost update is what a guarded bump without its guard would produce (a preempted writer
 * stores a stale `n + 1`), so an exact total is the property, on either binding. Starting two
 * bumps short of the wrap also drives the guarded store's arithmetic through 0.
 */
template <class Counter>
void bump_concurrently(const char* binding) {
    constexpr unsigned kThreads = 4;
    constexpr unsigned kBumps = 100'000;
    Counter c;
    tr::rmw_counter_test_door_t::preset(c, std::uint32_t{0xFFFFFFFEu});
    std::vector<std::thread> bumpers;
    for (unsigned t = 0; t < kThreads; ++t) {
        bumpers.emplace_back([&c] {
            for (unsigned i = 0; i < kBumps; ++i) c.bump();
        });
    }
    for (auto& b : bumpers) b.join();
    const std::uint32_t want = static_cast<std::uint32_t>(0xFFFFFFFEu + kThreads * kBumps);
    std::printf("  %s binding: got %u, want %u\n", binding, static_cast<unsigned>(c.load()),
                static_cast<unsigned>(want));
    check(c.load() == want, "every bump counted exactly once, across the wrap");
}

/** @brief Both `rmw_counter_t` bindings lose no bump under contention. */
void test_both_bindings_count_exactly() {
    std::printf("rmw_counter_t: concurrent bumps on both bindings:\n");
    bump_concurrently<native_counter_t>("native");
    bump_concurrently<guarded_counter_t>("guarded");
}

/**
 * @brief `await` on @p v, while a concurrent writer publishes to it.
 *
 * Re-arms from the writer until the awaiter is out: `await` sees a write ordered after its arm,
 * so a single write that beats the awaiter into `await` would be missed (#1418).
 */
[[nodiscard]] tr::graph::result_t<tr::graph::value_ref_t> await_with_writer(graph_t& g,
                                                                            vertex_handle_t v) {
    std::atomic<bool> awaited{false};
    std::thread writer([&] {
        const auto deadline = std::chrono::steady_clock::now() + 30s;  // backstop
        while (!awaited.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)g.write(v, make_value({0x5A}));
            std::this_thread::sleep_for(2ms);
        }
    });
    auto r = tr::testing::await_within(g, v, 5s);
    awaited.store(true, std::memory_order_release);
    writer.join();
    return r;
}

/** @brief `await` wakes on every role a publish can take: stored, HANDLER, retention NONE. */
void test_await_wakes_every_role() {
    std::printf("await wakes on STORED_VALUE, HANDLER and retention NONE:\n");
    graph_t g;

    const vertex_handle_t stored = g.register_vertex(path_t("/wake/stored"), role_t::STORED_VALUE);
    const auto rs = await_with_writer(g, stored);
    check(rs.has_value() && std::to_integer<int>((*rs)->only().bytes()[0]) == 0x5A,
          "STORED_VALUE: woken, and the awaited value is served");

    // A HANDLER stores nothing: its on_write consumes the value and only the wake runs.
    tr::graph::handlers_t h;
    auto on_write = [](const tr::graph::value_t&,
                       const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> { return {}; };
    auto on_read = [] { return tr::graph::value_ref_t::composed(make_value({0x42})); };
    h.on_write = tr::graph::thunk(on_write);
    h.on_read = tr::graph::thunk(on_read);
    const vertex_handle_t handler =
        g.register_vertex(path_t("/wake/handler"), role_t::HANDLER, std::move(h));
    const auto rh = await_with_writer(g, handler);
    check(rh.has_value() && std::to_integer<int>((*rh)->only().bytes()[0]) == 0x42,
          "HANDLER: woken by a write that stored nothing, served from on_read");

    // A NONE vertex retains nothing either: the wake answers NOT_FOUND, never TIMEOUT.
    const vertex_handle_t none = g.register_vertex(path_t("/wake/none"), role_t::STORED_VALUE);
    check(g.set_policy(none, {.retention = retention_t::NONE}).has_value(), "a NONE vertex");
    const auto rn = await_with_writer(g, none);
    check(!rn.has_value() && rn.error() == status_t::NOT_FOUND,
          "retention NONE: woken (NOT_FOUND, not TIMEOUT) by a write that retained nothing");
}

/** @brief What the subject seam below needs: the graph, the vertex, and whether it wrote. */
struct gate_write_t {
    graph_t* g = nullptr;
    vertex_handle_t v;
    int writes = 0;
};

/**
 * @brief A subject seam that, the first time it runs, writes @ref gate_write_t::v from
 *        INSIDE the ACL gate, on the awaiting thread: after `await` was entered, before it
 *        sleeps. The write carries no caller, so it is trusted and never re-enters the seam.
 */
std::expected<void, tr::wire::err_t> write_in_gate(void* ctx, std::string_view caller,
                                                   tr::mem::bytes_t& out, std::uint64_t&) {
    auto& gw = *static_cast<gate_write_t*>(ctx);
    if (gw.writes++ == 0) (void)gw.g->write(gw.v, make_value({0x6B}));
    return tr::testing::subject_into(out, caller);
}

/**
 * @brief The entry window (#2655): a write between `await`'s entry and its sleep wakes it.
 *
 * No sleep, no second write, no clock in the assertion: the only write is the one the gate
 * makes, synchronously, on the awaiting thread. The 5 s deadline is a backstop that turns a
 * regression into a TIMEOUT failure instead of a hung suite.
 */
void test_write_in_the_entry_window_wakes() {
    std::printf("await: a write after entry and before the sleep wakes it:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/wake/window"), role_t::STORED_VALUE);
    gate_write_t gw{.g = &g, .v = v};
    g.set_hook(&tr::graph::graph_hooks_t::subject_lookup, {write_in_gate, &gw});
    const auto r = tr::testing::await_within(g, v, 5s, std::string_view{"app"});
    check(gw.writes >= 1, "the gate ran, and wrote, inside await");
    check(r.has_value() && std::to_integer<int>((*r)->only().bytes()[0]) == 0x6B,
          "await returned the value written in its entry window (no other write, no expiry)");
}

/** @brief The same window on a bare vertex: a write between the arm and the wait is seen. */
void test_write_between_arm_and_wait_is_seen() {
    std::printf("wait_for_change: a write between arm and wait returns at once:\n");
    vertex_t v{role_t::STORED_VALUE, {}, {}};
    graph_t g;  // owns the deadline: a regression expires (returns false) instead of hanging
    const std::uint64_t now = tr::testing::now_ns();
    tr::graph::await_deadline_t d{.owner = &g, .deadline = now + 5'000'000'000ull};
    tr::graph::await_waiter_t w{.ctx = &d};
    v.arm_waiter(w);
    v.wake_waiters();  // the write's wake, after the arm, before the wait
    const bool changed =
        tr::testing::with_expiry(g, d.deadline, [&] { return vertex_t::wait_for_change(w); });
    check(changed, "returns true without blocking: the write unlinked it");
    check(!vertex_t::disarm_waiter(w), "nothing left to disarm");
}

}  // namespace

int main() {
    test_both_bindings_count_exactly();
    test_await_wakes_every_role();
    test_write_in_the_entry_window_wakes();
    test_write_between_arm_and_wait_is_seen();
    return tr::testing::summary("await_wake");
}
