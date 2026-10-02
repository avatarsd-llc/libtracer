/**
 * @file
 * @brief The fused guarded publish (#1715): on the guarded write-sequence binding, a publish
 *        opens ONE guard section for the LKV swap and the sequence bump together.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Linked against `libtracer_guarded_seq` (see `tests/guarded_seq/libtracer/config_override.hpp`):
 * the required core built with `guard_t` = a section-counting `tr::mutex_guard_t` and
 * `kForceGuardedRmw = true`, so `vertex_t::write_seq_` is the guarded `rmw_counter_t` a core
 * with no atomic RMW compiles to. This test pins:
 *
 *  - the BINDING: the sequence counter is guarded and the slot publishes under the same guard,
 *    so `vertex_t` selects the fused publish;
 *  - the COUNT: `graph_t::write` to a STORED_VALUE vertex opens exactly one section, and a
 *    HANDLER write (no slot store) opens exactly one — against two for the slot store and the
 *    counter bump taken separately, measured on the same guard;
 *  - EXACTNESS: concurrent writers to one vertex leave the sequence at exactly the number of
 *    publishes, with the HANDLER-style bump interleaved (both bump sites take one guard);
 *  - the PUBLICATION: a reader that sees a new sequence also sees the value published with it.
 */

#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#include "libtracer/guard.hpp"
#include "libtracer/lkv_slot.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/vertex.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::counting_guard_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::graph::vertex_t;
using tr::graph::write_seq_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief A stored value holding @p bytes, as the vertex's own `store` takes it. */
[[nodiscard]] tr::graph::value_ref_t make_ref(std::initializer_list<std::uint8_t> bytes) {
    const tr::view::view_t link = make_value(bytes);
    tr::graph::value_t* v = tr::graph::value_t::make(std::span<const tr::view::view_t>(&link, 1),
                                                     tr::mem::heap_source());
    if (v == nullptr) std::abort();  // the test's own allocation — cannot decline
    return tr::graph::value_ref_t::adopt(v);
}

static_assert(std::is_same_v<tr::graph::guard_t, counting_guard_t>,
              "this test links the library built over the counting guard");
static_assert(!tr::graph::write_seq_counter_t::is_native,
              "kForceGuardedRmw selects the guarded binding on a host");
static_assert(tr::graph::publishes_under<tr::graph::lkv_slot_t, tr::graph::guard_t>,
              "the default slot publishes under the bound guard, so the publish fuses");

/** @brief Sections the calling thread opens while running @p f. */
template <class F>
std::size_t sections_in(F&& f) {
    const std::size_t before = counting_guard_t::sections;
    f();
    return counting_guard_t::sections - before;
}

/**
 * @brief One publish, one section: graph write to a STORED_VALUE vertex and to a HANDLER.
 *
 * The reference arm drives the same slot type and counter type separately — a slot store, then
 * a bump anchored at the slot — which is the two-section shape the fusion removes.
 */
void test_one_section_per_publish() {
    std::printf("fused publish: one guard section per publish:\n");
    graph_t g;
    const vertex_handle_t stored = g.register_vertex(path_t("/fused/stored"), role_t::STORED_VALUE);
    (void)g.write(stored, make_value({0x01}));  // first write: any lazy setup happens here

    const std::size_t n = sections_in(
        [&] { check(g.write(stored, make_value({0x02})).has_value(), "write accepted"); });
    std::printf("  graph_t::write, STORED_VALUE: %zu section(s)\n", n);
    check(n == 1, "a STORED_VALUE publish opens exactly one guard section");

    tr::graph::handlers_t h;
    auto on_write = [](const tr::graph::value_t&,
                       const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> { return {}; };
    h.on_write = tr::graph::thunk(on_write);
    const vertex_handle_t handler =
        g.register_vertex(path_t("/fused/handler"), role_t::HANDLER, std::move(h));
    (void)g.write(handler, make_value({0x01}));
    const std::size_t nh = sections_in([&] { (void)g.write(handler, make_value({0x02})); });
    std::printf("  graph_t::write, HANDLER: %zu section(s)\n", nh);
    check(nh == 1, "a HANDLER publish (bump only, no slot store) opens exactly one section");

    // Reference: the separated shape, on the same guard and types.
    tr::graph::lkv_slot_t slot{};
    tr::graph::write_seq_counter_t seq{};
    const auto value = make_ref({0x03});
    const std::size_t separate = sections_in([&] {
        auto* v = const_cast<tr::graph::value_t*>(value.get());
        v->retain();
        (void)slot.store(v);
        seq.bump(&slot);
    });
    const std::size_t fused = sections_in([&] {
        auto* v = const_cast<tr::graph::value_t*>(value.get());
        v->retain();
        (void)slot.store(v, [&]() noexcept { seq.bump_in_section(); });
    });
    std::printf("  slot store + bump: separated %zu, fused %zu\n", separate, fused);
    check(separate == 2 && fused == 1, "fusing takes the publish from two sections to one");
    check(seq.load() == 2u, "both shapes bumped the counter once");
    slot.clear();
}

/**
 * @brief Writers on one vertex, with HANDLER-style bumps interleaved, count exactly.
 *
 * The fused bump runs inside the slot's section; `note_write` bumps under the guard anchored at
 * the same slot. Were the two anchored differently they would not exclude each other and a
 * guarded load + store would lose updates. An exact total is the property.
 */
void test_concurrent_publishes_count_exactly() {
    std::printf("fused publish: concurrent publishes count exactly:\n");
    constexpr int kThreads = 4;
    constexpr int kEach = 20000;
    graph_t g;
    const vertex_handle_t h = g.register_vertex(path_t("/fused/race"), role_t::STORED_VALUE);
    vertex_t* v = std::bit_cast<vertex_t*>(h);
    const write_seq_t s0 = v->current_seq();
    std::atomic<int> declined{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&g, &declined, h, v, t] {
            for (int i = 0; i < kEach; ++i) {
                if ((i & 3) == 0) {
                    v->note_write();
                } else {
                    if (!g.write(h, make_value({static_cast<std::uint8_t>(t)})).has_value()) {
                        declined.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    check(declined.load() == 0, "every racing write is accepted");
    const auto want = static_cast<write_seq_t>(s0 + kThreads * kEach);
    check(v->current_seq() == want, "every publish bumped the sequence exactly once");
}

/**
 * @brief A reader that observes a new sequence reads the value published with it.
 *
 * The writer publishes values 1..N in order; the reader snapshots the sequence and then the
 * stored value. Because the bump lands inside the swap's section, a sequence of `s0 + k`
 * implies the slot already holds the k-th value or a later one — never an earlier one.
 */
void test_sequence_implies_value() {
    std::printf("fused publish: a seen sequence implies its value:\n");
    constexpr int kWrites = 20000;
    graph_t g;
    const vertex_handle_t h = g.register_vertex(path_t("/fused/order"), role_t::STORED_VALUE);
    vertex_t* v = std::bit_cast<vertex_t*>(h);
    const write_seq_t s0 = v->current_seq();
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            const auto k = static_cast<int>(static_cast<write_seq_t>(v->current_seq() - s0));
            if (k == 0) continue;
            const auto r = v->read_stored();
            if (!r) {
                ok.store(false);
                continue;
            }
            const auto b = r->only().bytes();
            const int got = std::to_integer<int>(b[0]) | (std::to_integer<int>(b[1]) << 8);
            if (got < k) ok.store(false);  // kWrites < 2^16: the 16-bit tag never wraps
        }
    });
    for (int i = 1; i <= kWrites; ++i) {
        (void)g.write(h, make_value({static_cast<std::uint8_t>(i & 0xFF),
                                     static_cast<std::uint8_t>((i >> 8) & 0xFF)}));
    }
    done.store(true, std::memory_order_release);
    reader.join();
    check(ok.load(), "no reader saw a sequence ahead of the value published with it");
}

}  // namespace

int main() {
    test_one_section_per_publish();
    test_concurrent_publishes_count_exactly();
    test_sequence_implies_value();
    return tr::testing::summary("fused_publish");
}
