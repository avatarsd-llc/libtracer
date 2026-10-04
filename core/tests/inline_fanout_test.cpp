/**
 * @file
 * @brief `kInlineFanout` (#1708): a publish delivers to every subscriber below, at and above
 *        the inline snapshot width, and the overflow path is counted as it was.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Built twice: as `inline_fanout_test` against the test build's `libtracer` (the host default,
 * 8) and as `inline_fanout_narrow_test` against `libtracer_narrow_fanout` (the ESP chip value,
 * 2). `EXPECT_INLINE_FANOUT` names the width each binary must see, so a fragment that failed to
 * reach the library would fail here rather than quietly test the default twice.
 *
 * For each width the test publishes to vertices whose fan-out sits one below the boundary, on
 * it, one past it and well past it. Every subscriber must receive every publish exactly once,
 * and `delivery_drops().fan_out_truncated` must stay 0: the overflow vector reserves, so
 * nothing is shed. A last case re-publishes from inside a wide fan-out's callback, which takes
 * the nested-overflow path (a fresh local vector, because the thread's reusable one is busy).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "libtracer/config.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/vertex.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::graph::vertex_t;
using tr::testing::check;
using tr::testing::make_value;

static_assert(vertex_t::kInlineFanout == tr::graph::kInlineFanout,
              "the vertex mirrors the build's trait");
static_assert(tr::graph::kInlineFanout == EXPECT_INLINE_FANOUT,
              "this binary links the library built at the width it was told to expect");

constexpr std::size_t kCap = tr::graph::kInlineFanout;

/** @brief One subscriber: counts the deliveries it received. */
struct counter_t {
    std::uint64_t got = 0; /**< @brief Deliveries received. */
    /** @brief The delivery callback. */
    void operator()(const tr::graph::value_t&) { ++got; }
};

/** @brief Publish @p rounds times to a vertex with @p fan subscribers; each must get every one. */
void check_fan(graph_t& g, std::size_t fan, const char* label) {
    const std::string name = "/fan/n" + std::to_string(fan);
    const auto p = path_t::parse(name);
    check(p.has_value(), "the test path parses");
    const vertex_handle_t h = g.register_vertex(*p, role_t::STORED_VALUE);
    std::vector<std::unique_ptr<counter_t>> subs;
    for (std::size_t i = 0; i < fan; ++i) {
        subs.push_back(std::make_unique<counter_t>());
        check(g.subscribe(*p, *subs.back()).has_value(), "a subscriber is admitted");
    }
    constexpr int kRounds = 3;
    for (int r = 0; r < kRounds; ++r) (void)g.write(h, make_value({static_cast<std::uint8_t>(r)}));
    bool all = true;
    for (const auto& s : subs) all = all && s->got == kRounds;
    std::printf("  fan-out %zu (%s, inline width %zu)\n", fan, label, kCap);
    check(all, "every subscriber received every publish exactly once");
}

/** @brief Fan-outs one below, at, one past and well past the inline width. */
void test_around_the_boundary() {
    std::printf("inline fan-out width %zu:\n", kCap);
    graph_t g;
    if (kCap > 1) check_fan(g, kCap - 1, "below");
    check_fan(g, kCap, "at");
    check_fan(g, kCap + 1, "one past: overflow vector");
    check_fan(g, 2 * kCap + 3, "well past: overflow vector");
    check(g.delivery_drops().fan_out_truncated == 0,
          "the overflow path reserved: no delivery was truncated");
}

/**
 * @brief A wide fan-out whose callback publishes another wide fan-out.
 *
 * The outer publish holds the thread's reusable overflow vector, so the inner one takes the
 * nested path (a fresh local vector). Both must deliver to every subscriber.
 */
void test_nested_wide_fan_out() {
    std::printf("nested wide fan-out:\n");
    graph_t g;
    const std::size_t wide = kCap + 2;
    const auto outer = path_t::parse("/nest/outer");
    const auto inner = path_t::parse("/nest/inner");
    check(outer.has_value() && inner.has_value(), "the test paths parse");
    const vertex_handle_t ho = g.register_vertex(*outer, role_t::STORED_VALUE);
    const vertex_handle_t hi = g.register_vertex(*inner, role_t::STORED_VALUE);
    std::vector<std::unique_ptr<counter_t>> inner_subs;
    for (std::size_t i = 0; i < wide; ++i) {
        inner_subs.push_back(std::make_unique<counter_t>());
        check(g.subscribe(*inner, *inner_subs.back()).has_value(), "an inner subscriber");
    }
    std::uint64_t outer_got = 0;
    bool republished = false;
    auto first = [&](const tr::graph::value_t&) {
        ++outer_got;
        if (!republished) {
            republished = true;
            (void)g.write(hi, make_value({0x11}));  // nested: the outer holds the TLS vector
        }
    };
    auto rest = [&](const tr::graph::value_t&) { ++outer_got; };
    check(g.subscribe(*outer, first).has_value(), "the re-publishing outer subscriber");
    for (std::size_t i = 1; i < wide; ++i)
        check(g.subscribe(*outer, rest).has_value(), "an outer subscriber");
    (void)g.write(ho, make_value({0x22}));
    bool all = true;
    for (const auto& s : inner_subs) all = all && s->got == 1;
    check(outer_got == wide, "the outer wide fan-out reached every subscriber");
    check(all, "the nested wide fan-out reached every subscriber");
    check(g.delivery_drops().fan_out_truncated == 0, "nothing was truncated");
}

}  // namespace

int main() {
    test_around_the_boundary();
    test_nested_wide_fan_out();
    return tr::testing::summary("inline_fanout");
}
