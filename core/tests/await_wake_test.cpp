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
 *  - BOTH `rmw_counter_t` bindings count every bump exactly under concurrent bumpers: the
 *    native `fetch_add`, and the guarded load + store a core with no atomic RMW (ESP32-C3,
 *    Cortex-M0) compiles to, driven here on the host by naming that binding, across the wrap.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

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

}  // namespace

int main() {
    test_both_bindings_count_exactly();
    test_await_wakes_every_role();
    return tr::testing::summary("await_wake");
}
