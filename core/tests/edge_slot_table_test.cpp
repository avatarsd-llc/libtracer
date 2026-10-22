/**
 * @file
 * @brief #1622 (L7): a vertex stores its subscriber table ONCE — the published edge array holds
 *        each edge's dispatch fields, and the slot table keeps only what no publisher reads.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What is pinned, each with the half that keeps it from passing vacuously:
 *
 *   1. The slot record is the cold remainder only: 32 B on LP64 and 16 B on ILP32, against the
 *      80 B and 44 B record it replaces, while the published entry the fan-out walks keeps its
 *      width (56 B and 32 B).
 *   2. A `:subscribers[N]` replace is all or nothing. At every point the table source can refuse
 *      it, either the write answers BACKPRESSURE and the vertex is exactly as it was (the old
 *      edge still delivers, the count and the read-back are unchanged), or it succeeds and the
 *      new edge delivers and the old one does not. A replace that answered success while
 *      delivering to neither edge is the state this forbids.
 *   3. A slot cleared under a refused compaction does not come back: its dead entry stays in the
 *      published array until the next successful republish, and reusing the slot delivers to
 *      the new occupant once and to the old one never.
 *   4. Retirement gives every routed edge's link hold back exactly once, suspended edges
 *      included, and none for an edge already cleared whose dead entry was still published.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::value_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief A source that serves @ref arm's budget of blocks and then refuses, by value. */
class gate_source_t final : public tr::mem::block_source_t {
   public:
    gate_source_t() noexcept : tr::mem::block_source_t("gate") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (armed_ && allow_ == 0) return nullptr;
        if (armed_) --allow_;
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t, std::size_t align) noexcept override {
        ::operator delete(p, std::align_val_t{align});
    }
    /** @brief Serve @p allow more blocks, then refuse. */
    void arm(std::size_t allow) noexcept {
        armed_ = true;
        allow_ = allow;
    }
    /** @brief Serve everything again. */
    void disarm() noexcept { armed_ = false; }
    /** @brief Was the budget used up — did the refusing arm run? */
    [[nodiscard]] bool exhausted() const noexcept { return armed_ && allow_ == 0; }

   private:
    bool armed_ = false;    /**< @brief Refusing past the budget? */
    std::size_t allow_ = 0; /**< @brief Blocks left before refusing. */
};

/** @brief A one-byte write payload. */
tr::view::view_t byte_value(std::uint8_t b) {
    const std::byte one[1] = {std::byte{b}};
    return make_value(one);
}

/** @brief A SUBSCRIBER{ PATH @p target } TLV — a local re-dispatch target. */
std::vector<std::byte> subscriber_tlv(std::string_view target) {
    std::vector<std::byte> path;
    (void)tr::wire::emit_path_segment(path, target);
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{}, path);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::SUBSCRIBER, tr::wire::opt_t{.pl = true}, body);
    return out;
}

/** @brief A return route naming @p link. */
tr::view::view_t route_to(std::string_view link) {
    std::vector<std::byte> path;
    (void)tr::wire::emit_path_segment(path, link);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, path);
    return make_value(out);
}

/** @brief The first byte stored at @p p, or -1 when it holds nothing. */
int stored_byte(graph_t& g, const char* p) {
    const auto r = g.read(path_t(p));
    if (!r || !*r || (*r)->only().bytes().empty()) return -1;
    return std::to_integer<int>((*r)->only().bytes()[0]);
}

/** @brief How many entries the vertex behind @p h has PUBLISHED for the fan-out to walk. */
std::size_t published(vertex_handle_t h) {
    return std::bit_cast<tr::graph::vertex_t*>(h)->published_edges();
}

/** @brief (1) The census: the slot record is the cold remainder, the hot entry is unchanged. */
void test_census() {
    std::printf("the slot table keeps only what no publisher reads:\n");
    constexpr std::size_t slot = sizeof(tr::graph::subscriber_slot_t);
    constexpr std::size_t entry = sizeof(tr::graph::pub_edge_t);
    std::printf("  subscriber_slot_t %zu B, pub_edge_t %zu B\n", slot, entry);
    if constexpr (sizeof(void*) == 8) {
        check(slot == 32, "a slot is 32 B on LP64 (it was an 80 B subscriber_t)");
        check(entry == 56, "a published entry stays 56 B on LP64");
    } else {
        check(slot == 16, "a slot is 16 B on ILP32 (it was a 44 B subscriber_t)");
        check(entry == 32, "a published entry stays 32 B on ILP32");
    }
}

/** @brief (2) A `:subscribers[0]` replace at every refusal point: all or nothing. */
void test_refused_replace_changes_nothing() {
    std::printf("a refused :subscribers[N] replace leaves the vertex as it was:\n");
    const std::vector<std::byte> old_sub = subscriber_tlv("t1");
    const std::vector<std::byte> new_sub = subscriber_tlv("t2");
    bool saw_refusal = false;
    bool saw_success = false;
    bool all_consistent = true;
    for (std::size_t allow = 0; allow < 64 && !saw_success; ++allow) {
        gate_source_t gate;
        graph_t g{gate};
        const vertex_handle_t v = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/t1"), role_t::STORED_VALUE);
        (void)g.register_vertex(path_t("/t2"), role_t::STORED_VALUE);
        const auto append = path_t::parse("/s:subscribers[]");
        const auto slot0 = path_t::parse("/s:subscribers[0]");
        if (!g.write(v, append->field(), make_value(old_sub)).has_value()) {
            all_consistent = false;
            break;
        }
        gate.arm(allow);
        const auto r = g.write(v, slot0->field(), make_value(new_sub));
        const bool refused_here = gate.exhausted();
        gate.disarm();
        (void)g.write(v, byte_value(static_cast<std::uint8_t>(0x40 + allow)));
        const int t1 = stored_byte(g, "/t1");
        const int t2 = stored_byte(g, "/t2");
        const int want = static_cast<int>(0x40 + allow);
        const auto back = g.read(path_t("/s:subscribers[0]"));
        if (r.has_value()) {
            saw_success = true;
            all_consistent &= t2 == want && t1 != want && g.own_subs(v) == 1;
        } else {
            if (!refused_here || r.error() != status_t::BACKPRESSURE) continue;
            saw_refusal = true;
            const bool old_back = back && *back && (*back)->only().bytes().size() == old_sub.size();
            all_consistent &= t1 == want && t2 != want && g.own_subs(v) == 1 && old_back;
        }
    }
    check(saw_refusal, "the sweep reached at least one refused replace");
    check(saw_success, "and ended on the budget that lets the replace land");
    check(all_consistent,
          "every refusal left the old edge delivering, and every success delivers to the new "
          "edge only");
}

/** @brief (3) A slot cleared under a refused compaction stays cleared through its reuse. */
void test_cleared_under_pressure_stays_cleared() {
    std::printf("a slot cleared under a refused compaction does not come back:\n");
    gate_source_t gate;
    graph_t g{gate};
    const vertex_handle_t v = g.register_vertex(path_t("/c"), role_t::STORED_VALUE);
    int a = 0, b = 0, c = 0;
    const auto bump = [](void* ctx, const value_t&) { ++*static_cast<int*>(ctx); };
    const auto sa = g.subscribe(path_t("/c"), bump, &a);
    const auto sb = g.subscribe(path_t("/c"), bump, &b);
    check(sa && sb && published(v) == 2, "two edges, two published entries");

    gate.arm(0);
    check(g.unsubscribe(*sa).has_value(), "an unsubscribe with no room still takes effect");
    gate.disarm();
    check(published(v) == 2, "... its dead entry waits for the next republish");
    (void)g.write(v, byte_value(1));
    check(a == 0 && b == 1, "... and the cleared edge receives nothing");

    const auto sc = g.subscribe(path_t("/c"), bump, &c);  // reuses slot 0
    check(sc.has_value() && published(v) == 2, "the reuse compacts: one entry per active slot");
    (void)g.write(v, byte_value(2));
    check(a == 0 && b == 2 && c == 1,
          "the new occupant receives once, the old one never, the neighbour throughout");
}

/** @brief A link-hold census: the net plane's view of how many edges hold each link. */
struct holds_t {
    std::map<std::uint32_t, int> held; /**< @brief Holds per link (its `test_link` slot). */
    bool negative = false;             /**< @brief Did any link get more releases than holds? */
};

/** @brief (4) Retirement gives each routed hold back once — suspended in, dead entries out. */
void test_retire_gives_each_hold_back_once() {
    std::printf("retirement gives every routed edge's link hold back exactly once:\n");
    gate_source_t gate;
    graph_t g{gate};
    holds_t holds;
    auto hooks = g.hooks();
    hooks.link_hold = {[](void* ctx, tr::graph::link_pair_t link, bool held) {
                           auto& h = *static_cast<holds_t*>(ctx);
                           int& n = h.held[link.conn];
                           n += held ? 1 : -1;
                           h.negative |= n < 0;
                       },
                       &holds};
    g.set_hooks(hooks);
    const vertex_handle_t v = g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    const std::vector<std::byte> sub = subscriber_tlv("peer");
    check(g.subscribe_wire(v, make_value(sub), route_to("a"), "a", {}, {}, {},
                           tr::testing::test_link("a"))
                  .has_value() &&
              g.subscribe_wire(v, make_value(sub), route_to("b"), "b", {}, {}, {},
                               tr::testing::test_link("b"))
                  .has_value() &&
              g.subscribe_wire(v, make_value(sub), route_to("c"), "c", {}, {}, {},
                               tr::testing::test_link("c"))
                  .has_value(),
          "three routed edges over links a, b and c");
    check(holds.held[tr::testing::test_link("a").conn] == 1 &&
              holds.held[tr::testing::test_link("b").conn] == 1 &&
              holds.held[tr::testing::test_link("c").conn] == 1,
          "each holds its link once");

    // Slot 0 cleared with the compaction refused: its dead entry stays published.
    const auto slot0 = path_t::parse("/r:subscribers[0]");
    std::vector<std::byte> evict;  // the empty-STATUS eviction sentinel
    tr::wire::emit_tlv(evict, tr::wire::type_t::STATUS, tr::wire::opt_t{},
                       std::span<const std::byte>{});
    gate.arm(0);
    check(g.write(v, slot0->field(), make_value(evict)).has_value(),
          "slot 0 is cleared with no room to compact");
    gate.disarm();
    check(published(v) == 3 && holds.held[tr::testing::test_link("a").conn] == 0,
          "... its entry lingers, its hold is back");

    // Slot 1 suspended: still an edge, still holding its link.
    tr::graph::vertex_t& raw = *std::bit_cast<tr::graph::vertex_t*>(v);
    check(raw.set_edge_suspended(1, true) == tr::graph::vertex_t::edge_suspend_t::CHANGED,
          "slot 1 is suspended");

    check(g.retire(v).has_value(), "the vertex retires");
    check(!holds.negative, "no link was released more often than it was held");
    check(holds.held[tr::testing::test_link("a").conn] == 0 &&
              holds.held[tr::testing::test_link("b").conn] == 0 &&
              holds.held[tr::testing::test_link("c").conn] == 0,
          "the suspended edge and the live one gave their holds back, the cleared one not again");
}

}  // namespace

int main() {
    test_census();
    test_refused_replace_changes_nothing();
    test_cleared_under_pressure_stays_cleared();
    test_retire_gives_each_hold_back_once();
    return tr::testing::summary("edge_slot_table");
}
