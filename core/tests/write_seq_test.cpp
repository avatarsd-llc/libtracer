/**
 * @file
 * @brief The 32-bit write sequence (#1621, RFC-0028 D6): its width, the wrap, and `await`
 *        waking on every role that publishes.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `vertex_t::write_seq_` is `std::atomic<std::uint32_t>` on every target. Its only consumer is
 * `await`, which tests `current != seq0`, an equality test, so the counter wrapping at 2^32 is
 * not an event. This test pins:
 *
 *  - the WIDTH: `write_seq_t` is 4 bytes and a lock-free atomic (no libatomic call on rv32);
 *  - the WRAP: a snapshot one bump before the wrap sees the change, and a real vertex's
 *    `wait_for_change` fed that snapshot returns at once;
 *  - the WAKE on all three shapes a publish can take, each of which bumps the sequence: a
 *    STORED_VALUE vertex, a HANDLER vertex (stores nothing; its `on_write` consumes), and a
 *    `retention_t::NONE` value vertex (stores nothing). The last two are why the sequence
 *    cannot be replaced by the published value's identity.
 *
 * The CI TSan matrix runs this binary under both `lkv_slot_t` bindings.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "libtracer/tracer.hpp"
#include "libtracer/vertex.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

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
static_assert(std::atomic<write_seq_t>::is_always_lock_free,
              "a lock-free 32-bit atomic: no libatomic call per publish on rv32");

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

/** @brief The await predicate on a REAL vertex, fed a snapshot one bump before the wrap. */
void test_wait_across_wrap() {
    std::printf("write sequence: wait_for_change across the wrap:\n");
    tr::graph::vertex_t v{role_t::STORED_VALUE, {}, {}};
    const write_seq_t now = v.current_seq();
    // A fresh vertex starts at 0, so a snapshot one bump behind it is spelled 0xFFFFFFFF.
    const write_seq_t before = static_cast<write_seq_t>(now - 1u);
    check(now == 0 && before == 0xFFFFFFFFu, "the snapshot straddles the wrap");
    check(v.wait_for_change(before, 0ms),
          "a snapshot one bump behind, across the wrap, is a change");
    check(!v.wait_for_change(now, 5ms), "the current sequence is not a change (timeout)");
    v.note_write();
    check(static_cast<write_seq_t>(v.current_seq() - now) == 1u, "one bump moves it by one");
    check(v.wait_for_change(now, 0ms), "and the old snapshot now sees the change");
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
    test_wait_across_wrap();
    test_await_wakes_every_role();
    return tr::testing::summary("write_seq");
}
