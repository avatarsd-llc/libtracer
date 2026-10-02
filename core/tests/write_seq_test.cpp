/**
 * @file
 * @brief The 32-bit write sequence (#1621, RFC-0028 D6): its width, the wrap, and `await`
 *        waking on every role that publishes.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `vertex_t::write_seq_` is a 32-bit `rmw_counter_t` on every target. Its only consumer is
 * `await`, which tests `current != seq0`, an equality test, so the counter wrapping at 2^32 is
 * not an event. This test pins:
 *
 *  - the WIDTH: `write_seq_t` is 4 bytes, and a host build bumps it with one hardware RMW;
 *  - BOTH `rmw_counter_t` bindings count every bump exactly under concurrent bumpers: the
 *    native `fetch_add`, and the guarded load + store a core with no atomic RMW (ESP32-C3,
 *    Cortex-M0) compiles to, driven here on the host by naming that binding;
 *  - the WRAP: a test-only door presets a real vertex's atomic to 0xFFFFFFFE, two bumps wrap it
 *    through 0, and `wait_for_change` (blocked or not) and `graph_t::await` see every bump;
 *  - the WAKE on all three shapes a publish can take, each of which bumps the sequence: a
 *    STORED_VALUE vertex, a HANDLER vertex (stores nothing; its `on_write` consumes), and a
 *    `retention_t::NONE` value vertex (stores nothing). The last two are why the sequence
 *    cannot be replaced by the published value's identity.
 *
 * The CI TSan matrix runs this binary under both `lkv_slot_t` bindings, and the test build links
 * it a second time as `write_seq_guarded_test`, over the guarded write-sequence binding (#1715).
 */

#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "libtracer/guard.hpp"
#include "libtracer/guard_mutex.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/vertex.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace tr::graph {

/** @brief The test-only door `vertex.hpp` declares (#1621): presets the write sequence. */
struct vertex_seq_test_door_t {
    /** @brief Store @p seq into @p v's write sequence (seq_cst, like the bump). */
    static void preset(vertex_t& v, write_seq_t seq) { v.write_seq_.preset(seq); }
};

}  // namespace tr::graph

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::retention_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::graph::write_seq_t;
using tr::testing::check;
using tr::testing::make_value;

static_assert(sizeof(write_seq_t) == 4, "the write sequence is 32-bit on every target (#1621)");
static_assert(tr::graph::write_seq_counter_t::is_native != tr::graph::kForceGuardedRmw,
              "a host build bumps the write sequence with one hardware RMW, never a guard — "
              "unless the build forces the guarded binding (the write_seq_guarded leg, #1715)");

/** @brief The guarded binding, named on a host that has atomic RMW so CI can drive it. */
using guarded_counter_t = tr::rmw_counter_t<write_seq_t, tr::mutex_guard_t, false>;
/** @brief The native binding, named explicitly so the pair below reads side by side. */
using native_counter_t = tr::rmw_counter_t<write_seq_t, tr::mutex_guard_t, true>;
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
    c.preset(0xFFFFFFFEu);
    std::vector<std::thread> bumpers;
    for (unsigned t = 0; t < kThreads; ++t) {
        bumpers.emplace_back([&c] {
            for (unsigned i = 0; i < kBumps; ++i) c.bump();
        });
    }
    for (auto& b : bumpers) b.join();
    const write_seq_t want = static_cast<write_seq_t>(0xFFFFFFFEu + kThreads * kBumps);
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

/** @brief The equality compare across the wrap, and the one documented alias. */
void test_wrap_arithmetic() {
    std::printf("write sequence: equality compare across 2^32:\n");
    constexpr write_seq_t kMax = 0xFFFFFFFFu;
    static_assert(static_cast<write_seq_t>(kMax + 1u) == 0u, "the bump wraps to zero");
    static_assert(static_cast<write_seq_t>(kMax + 1u) != kMax,
                  "one bump across the wrap is a change");

    // Every snapshot within 4096 of the wrap, every distance below 4096: the value read back
    // differs from the snapshot iff at least one bump landed.
    bool all = true;
    for (write_seq_t back = 0; back < 4096; ++back) {
        const write_seq_t seq0 = static_cast<write_seq_t>(0u - back);
        for (write_seq_t d = 0; d < 4096; d += 7) {
            const write_seq_t now = static_cast<write_seq_t>(seq0 + d);
            all = all && ((now != seq0) == (d != 0));
        }
    }
    check(all, "changed iff at least one bump, for snapshots up to 4096 before the wrap");
    // The documented alias: exactly 2^32 bumps read as none. Stated so a change to it is seen.
    check(static_cast<write_seq_t>(123u + 0x100000000ull) == 123u,
          "exactly 2^32 bumps alias to 'no change' (49 days at 1 kHz; bounded by the timeout)");
}

/**
 * @brief The await predicate on a REAL vertex whose atomic is driven through 2^32 -> 0.
 *
 * The test door presets `write_seq_` to 0xFFFFFFFE; two `note_write()` bumps then really wrap
 * the counter (0xFFFFFFFE -> 0xFFFFFFFF -> 0), and every snapshot must see each bump.
 */
void test_wait_across_wrap() {
    std::printf("write sequence: wait_for_change while the atomic wraps through 0:\n");
    tr::graph::vertex_t v{role_t::STORED_VALUE, {}, {}};
    tr::graph::vertex_seq_test_door_t::preset(v, 0xFFFFFFFEu);
    const write_seq_t s0 = v.current_seq();
    check(s0 == 0xFFFFFFFEu, "the door presets the sequence two bumps before the wrap");
    check(!v.wait_for_change(s0, 5ms), "no bump yet: the preset snapshot times out");

    v.note_write();
    const write_seq_t s1 = v.current_seq();
    check(s1 == 0xFFFFFFFFu, "first bump: 0xFFFFFFFE -> 0xFFFFFFFF");
    check(v.wait_for_change(s0, 0ms), "the first bump wakes the preset snapshot");
    check(!v.wait_for_change(s1, 5ms), "and 0xFFFFFFFF itself is not a change yet");

    // A BLOCKED waiter across the wrap: the snapshot is 0xFFFFFFFF, taken on this thread before
    // the waiter starts (level-triggered, as in vertex_test), and the bump lands it on 0.
    std::atomic<bool> woke{false};
    std::thread waiter([&] {
        if (v.wait_for_change(s1, 5s)) woke.store(true);
    });
    std::this_thread::sleep_for(10ms);  // widens the blocked case; not load-bearing
    v.note_write();
    waiter.join();
    check(v.current_seq() == 0u, "second bump: 0xFFFFFFFF -> 0, the atomic really wrapped");
    check(woke.load(), "the bump across the wrap wakes a blocked waiter");
    check(v.wait_for_change(s1, 0ms) && v.wait_for_change(s0, 0ms),
          "both pre-wrap snapshots see the change after the wrap");
    check(!v.wait_for_change(0u, 5ms), "the post-wrap value is not a change to itself");
}

/**
 * @brief `graph_t::await` returns across a sequence preset to the wrap.
 *
 * The vertex's sequence is preset to 0xFFFFFFFF, so the first publish lands it on 0. The
 * writer delays its first write so the awaiter normally snapshots 0xFFFFFFFF first, then
 * re-arms until the awaiter is out (#1418); the wrap-exact assertions are the
 * `wait_for_change` ones above, which is the predicate `await` runs.
 */
void test_graph_await_across_wrap() {
    std::printf("write sequence: graph_t::await across the wrap:\n");
    graph_t g;
    const vertex_handle_t h = g.register_vertex(path_t("/seq/wrap"), role_t::STORED_VALUE);
    tr::graph::vertex_t* v = std::bit_cast<tr::graph::vertex_t*>(h);
    tr::graph::vertex_seq_test_door_t::preset(*v, 0xFFFFFFFFu);
    std::atomic<bool> awaited{false};
    std::thread writer([&] {
        std::this_thread::sleep_for(20ms);
        const auto deadline = std::chrono::steady_clock::now() + 30s;  // backstop
        while (!awaited.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)g.write(h, make_value({0x77}));
            std::this_thread::sleep_for(2ms);
        }
    });
    const auto r = g.await(h, 5s);
    awaited.store(true, std::memory_order_release);
    writer.join();
    check(r.has_value() && std::to_integer<int>((*r)->only().bytes()[0]) == 0x77,
          "await returns the value published across the wrap");
    check(v->current_seq() < 0x80000000u, "the sequence wrapped through 0 (small again)");
}

/**
 * @brief `await` on @p v, while a concurrent writer publishes to it.
 *
 * Re-arms from the writer until the awaiter is out: `await` snapshots the sequence on entry,
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
    auto r = g.await(v, 5s);
    awaited.store(true, std::memory_order_release);
    writer.join();
    return r;
}

/** @brief `await` wakes on every role a publish can take: stored, HANDLER, retention NONE. */
void test_await_wakes_every_role() {
    std::printf("await wakes on STORED_VALUE, HANDLER and retention NONE:\n");
    graph_t g;

    const vertex_handle_t stored = g.register_vertex(path_t("/seq/stored"), role_t::STORED_VALUE);
    const auto rs = await_with_writer(g, stored);
    check(rs.has_value() && std::to_integer<int>((*rs)->only().bytes()[0]) == 0x5A,
          "STORED_VALUE: woken, and the awaited value is served");

    // A HANDLER stores nothing: its on_write consumes the value and only the sequence moves.
    tr::graph::handlers_t h;
    auto on_write = [](const tr::graph::value_t&,
                       const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> { return {}; };
    auto on_read = [] { return make_value({0x42}); };
    h.on_write = tr::graph::thunk(on_write);
    h.on_read = tr::graph::thunk(on_read);
    const vertex_handle_t handler =
        g.register_vertex(path_t("/seq/handler"), role_t::HANDLER, std::move(h));
    const auto rh = await_with_writer(g, handler);
    check(rh.has_value() && std::to_integer<int>((*rh)->only().bytes()[0]) == 0x42,
          "HANDLER: woken by a write that stored nothing, served from on_read");

    // A NONE vertex retains nothing either: the wake answers NOT_FOUND, never TIMEOUT.
    const vertex_handle_t none = g.register_vertex(path_t("/seq/none"), role_t::STORED_VALUE);
    check(g.set_policy(none, {.retention = retention_t::NONE}).has_value(), "a NONE vertex");
    const auto rn = await_with_writer(g, none);
    check(!rn.has_value() && rn.error() == status_t::NOT_FOUND,
          "retention NONE: woken (NOT_FOUND, not TIMEOUT) by a write that retained nothing");
}

}  // namespace

int main() {
    test_wrap_arithmetic();
    test_both_bindings_count_exactly();
    test_wait_across_wrap();
    test_graph_await_across_wrap();
    test_await_wakes_every_role();
    return tr::testing::summary("write_seq");
}
