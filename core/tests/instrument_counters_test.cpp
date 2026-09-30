/**
 * @file
 * @brief #1664 — `graph_t`'s two instrumentation counters are a compile-time choice, lean by
 *        default, and closing them out changes what is COUNTED and nothing that is DONE.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `ancestor_walks()` (RFC-0005) and `target_canonical_resolves()` (#830) are relaxed 64-bit
 * `fetch_add`s on node-wide counters that only tests and benches read. `config_t` decides
 * whether they exist (`kInstrumentCounters`), and the shipped default is `false`.
 *
 * One executable serves both bindings, by `if constexpr`:
 *
 *  - **The default is lean.** `default_config_t::kInstrumentCounters` is `false`; a raw `-I`
 *    consumer (the footprint gate, a vendored drop, the ESP-IDF component) pays nothing.
 *  - **Instrumented** (the core test build's preset): both counters move exactly once per
 *    event — one per write with a listening ancestor, one per delivery down an unbound target
 *    edge.
 *  - **Lean** (a CI leg binding its own fragment): the same events leave both accessors at `0`.
 *
 * In BOTH arms the walk and the delivery still happen — the ancestor subscriber is notified and
 * the target receives the value. An instrument whose removal changed routing would not be an
 * instrument.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;

/** @brief Events each case drives — enough that an off-by-one cannot hide. */
constexpr std::uint64_t kEvents = 16;

static_assert(!tr::graph::default_config_t::kInstrumentCounters,
              "the shipped default must be the lean choice (#1664)");

/** @brief What a counter must read after @ref kEvents events, in THIS build. */
constexpr std::uint64_t expected_count() { return tr::graph::kInstrumentCounters ? kEvents : 0; }

/** @brief A one-byte STORED_VALUE write. */
[[nodiscard]] tr::view::view_t one_byte() {
    return tr::testing::make_value(std::vector<std::byte>{std::byte{0x2A}});
}

/** @brief The ancestor walk: counted iff instrumented, performed either way. */
void test_ancestor_walks() {
    graph_t g;
    (void)g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    const vertex_handle_t leaf = g.register_vertex(path_t("/a/b"), role_t::STORED_VALUE);
    std::uint64_t notified = 0;
    check(g.subscribe(
               path_t("/a"),
               [](void* ctx, const tr::graph::value_t&) { ++*static_cast<std::uint64_t*>(ctx); },
               &notified)
              .has_value(),
          "ancestor: subscribe at /a");
    for (std::uint64_t i = 0; i < kEvents; ++i) (void)g.write(leaf, one_byte());
    check(notified == kEvents, "ancestor: every leaf write still reached the /a subscriber");
    check(g.ancestor_walks() == expected_count(),
          "ancestor: ancestor_walks() counts iff kInstrumentCounters");
}

/** @brief The canonical fallback: an edge subscribed before its target exists never binds. */
void test_target_canonical_resolves() {
    graph_t g;
    const path_t src_path("/src");
    const path_t tpath("/t");
    const vertex_handle_t src = g.register_vertex(src_path, role_t::STORED_VALUE);
    // The target does not exist at admission, so no binding is minted and every delivery
    // takes the canonical `find_ptr` leg — the one counted event.
    check(g.subscribe(src_path, tpath).has_value(), "fallback: subscribe before the target");
    (void)g.register_vertex(tpath, role_t::STORED_VALUE);
    for (std::uint64_t i = 0; i < kEvents; ++i) (void)g.write(src, one_byte());
    check(g.read(tpath).has_value(), "fallback: the target still received the delivery");
    check(g.target_canonical_resolves() == expected_count(),
          "fallback: target_canonical_resolves() counts iff kInstrumentCounters");
}

}  // namespace

int main() {
    std::printf("#1664 instrumentation counters (kInstrumentCounters = %s)\n\n",
                tr::graph::kInstrumentCounters ? "true" : "false");
    test_ancestor_walks();
    test_target_canonical_resolves();
    return tr::testing::summary("instrument_counters");
}
