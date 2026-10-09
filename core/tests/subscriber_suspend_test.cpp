/**
 * @file
 * @brief #1533: a subscription is suspended and resumed IN PLACE, and a suspended edge costs
 *        the fan-out nothing.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What is pinned, each with the half that keeps it from passing vacuously:
 *
 *   1. A suspended edge receives nothing, and its neighbours still do; a resumed edge receives
 *      the NEXT write and no replay of the value held while it was suspended — even when it
 *      asked for durability at join.
 *   2. The slot survives: its index stays held (a new subscribe does not reuse it), the same
 *      handle resumes and then unsubscribes it, and a cleared handle answers NOT_FOUND.
 *   3. The published edge array carries only delivering edges: suspending M of N leaves N - M
 *      entries for the copy loop to walk, the same as a vertex that never had the M.
 *   4. A resume whose republish is refused rolls back: the edge stays suspended, says so, and
 *      a retry with room succeeds.
 *   5. The liveness flip finds its entry by slot — the published array no longer mirrors the
 *      slot table — observed where the flip alone decides: a suspend whose republish is
 *      refused silences exactly its own edge.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::delivery_policy_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subscription_t;
using tr::graph::value_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief A one-byte write payload. */
tr::view::view_t byte_value(std::uint8_t b) {
    const std::byte one[1] = {std::byte{b}};
    return make_value(one);
}

/** @brief A counting sink: deliveries seen and the last byte delivered. */
struct counter_t {
    int seen = 0;          /**< @brief Deliveries received. */
    std::uint8_t last = 0; /**< @brief The last delivered byte. */
};

/** @brief The callback for @ref counter_t. */
void count(void* ctx, const value_t& v) {
    auto* c = static_cast<counter_t*>(ctx);
    ++c->seen;
    c->last = std::to_integer<std::uint8_t>(v.only().bytes()[0]);
}

/** @brief How many entries the vertex behind @p h has PUBLISHED for the fan-out to walk. */
std::size_t published(vertex_handle_t h) {
    return std::bit_cast<tr::graph::vertex_t*>(h)->published_edges();
}

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

   private:
    bool armed_ = false;    /**< @brief Refusing past the budget? */
    std::size_t allow_ = 0; /**< @brief Blocks left before refusing. */
};

/** @brief (1) Suspend silences one edge only; resume delivers the next write, no replay. */
void test_suspend_and_resume_deliver_correctly() {
    std::printf("suspend silences the edge; resume delivers the next write, no replay:\n");
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/s/a"), role_t::STORED_VALUE);
    counter_t a, b;
    const auto sa = g.subscribe(path_t("/s/a"), count, &a,
                                delivery_policy_t{delivery_policy_t::kDurabilityRequest});
    const auto sb = g.subscribe(path_t("/s/a"), count, &b);
    check(sa && sb, "two edges admitted");

    check(g.set_suspended(*sa, true).has_value(), "suspend answers OK");
    check(g.is_suspended(*sa).value_or(false), "... and the state reads back suspended");
    (void)g.write(src, byte_value(0x11));
    check(a.seen == 0, "a suspended edge receives nothing");
    check(b.seen == 1 && b.last == 0x11, "... while its neighbour still does (not a vertex mute)");

    check(g.set_suspended(*sa, false).has_value(), "resume answers OK");
    check(!g.is_suspended(*sa).value_or(true), "... and reads back live");
    check(a.seen == 0, "resume replays nothing, even for a durability subscriber");
    (void)g.write(src, byte_value(0x22));
    check(a.seen == 1 && a.last == 0x22, "the resumed edge receives the NEXT write");

    check(g.set_suspended(*sa, false).has_value() && a.seen == 1,
          "a no-change resume is OK and delivers nothing");
}

/** @brief (2) The suspended slot stays held, and the same handle drives it to the end. */
void test_slot_survives() {
    std::printf("the suspended slot keeps its index and its handle:\n");
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/s/b"), role_t::STORED_VALUE);
    counter_t a, c;
    const auto sa = g.subscribe(path_t("/s/b"), count, &a);
    check(sa && g.set_suspended(*sa, true).has_value(), "admitted, then suspended");
    const auto sc = g.subscribe(path_t("/s/b"), count, &c);
    check(sc && !(*sc == *sa), "a later subscribe takes a NEW slot — the suspended one is held");

    check(g.set_suspended(*sa, false).has_value(), "the original handle resumes it");
    (void)g.write(src, byte_value(0x33));
    check(a.seen == 1 && c.seen == 1, "both edges deliver");
    check(g.unsubscribe(*sa).has_value(), "... and the same handle unsubscribes it");
    check(g.set_suspended(*sa, true).error() == status_t::NOT_FOUND,
          "a cleared handle cannot be suspended (NOT_FOUND)");
    check(g.is_suspended(*sa).error() == status_t::NOT_FOUND, "... nor read (NOT_FOUND)");
    check(g.set_suspended(subscription_t{}, true).error() == status_t::NOT_FOUND,
          "an empty handle answers NOT_FOUND");
}

/** @brief (3) + (5) The published array holds delivering edges only, and the flip still finds
 *         the right entry once the array stops mirroring the slot table. */
void test_published_array_is_the_delivering_edges() {
    std::printf("the published array carries the delivering edges only:\n");
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/s/c"), role_t::STORED_VALUE);
    constexpr std::size_t kN = 16;
    std::vector<counter_t> sinks(kN);
    std::vector<subscription_t> subs;
    for (counter_t& s : sinks) subs.push_back(*g.subscribe(path_t("/s/c"), count, &s));
    check(published(src) == kN, "16 edges, 16 published entries");

    for (std::size_t i = 0; i < kN; i += 2) (void)g.set_suspended(subs[i], true);
    check(published(src) == kN / 2, "suspending 8 leaves 8 entries — none to skip");

    // Clear the odd edges one at a time; the survivors keep receiving across each republish.
    for (std::size_t i = 1; i < kN; i += 2) {
        (void)g.unsubscribe(subs[i]);
        (void)g.write(src, byte_value(static_cast<std::uint8_t>(i)));
        for (std::size_t j = i + 2; j < kN; j += 2)
            check(sinks[j].seen == static_cast<int>((i + 1) / 2),
                  "a still-live edge keeps receiving after a neighbour's unsubscribe");
    }
    check(published(src) == 0, "all live edges cleared, the rest suspended: nothing published");
    for (std::size_t i = 0; i < kN; i += 2)
        check(sinks[i].seen == 0, "no suspended edge received anything");

    for (std::size_t i = 0; i < kN; i += 2) (void)g.set_suspended(subs[i], false);
    check(published(src) == kN / 2, "resuming them republishes exactly those 8");
    (void)g.write(src, byte_value(0x44));
    for (std::size_t i = 0; i < kN; i += 2)
        check(sinks[i].seen == 1 && sinks[i].last == 0x44, "each resumed edge takes the write");
}

/** @brief (4) A refused resume leaves the edge suspended and says so; a retry succeeds. */
void test_refused_resume_rolls_back() {
    std::printf("a refused resume rolls back and can be retried:\n");
    gate_source_t gate;
    graph_t g{gate};
    const vertex_handle_t src = g.register_vertex(path_t("/s/d"), role_t::STORED_VALUE);
    counter_t lo, a, hi;
    const auto slo = g.subscribe(path_t("/s/d"), count, &lo);
    const auto sa = g.subscribe(path_t("/s/d"), count, &a);
    const auto shi = g.subscribe(path_t("/s/d"), count, &hi);
    check(slo && shi && sa && g.set_suspended(*sa, true).has_value(),
          "three edges admitted, the middle one suspended");

    gate.arm(0);
    const auto r = g.set_suspended(*sa, false);
    gate.disarm();
    check(!r && r.error() == status_t::BACKPRESSURE, "a resume with no room answers BACKPRESSURE");
    check(g.is_suspended(*sa).value_or(false), "... and the edge is still suspended");
    (void)g.write(src, byte_value(0x55));
    check(a.seen == 0, "... and still receives nothing");

    check(g.set_suspended(*sa, false).has_value(), "the retry with room succeeds");
    (void)g.write(src, byte_value(0x66));
    check(a.seen == 1 && a.last == 0x66, "... and the edge delivers again");

    gate.arm(0);
    check(g.set_suspended(*sa, true).has_value(), "a suspend with no room still succeeds");
    gate.disarm();
    (void)g.write(src, byte_value(0x77));
    check(a.seen == 1, "... and silences the edge at once (the liveness flip, no allocation)");
    // With the republish refused, the flip alone decides who is silenced: it must find the
    // middle edge's entry by its slot, not its neighbours'.
    check(lo.seen == 3 && hi.seen == 3 && lo.last == 0x77 && hi.last == 0x77,
          "... and ONLY that edge: the flip found its own entry by slot");
}

}  // namespace

int main() {
    test_suspend_and_resume_deliver_correctly();
    test_slot_survives();
    test_published_array_is_the_delivering_edges();
    test_refused_resume_rolls_back();
    return tr::testing::summary("subscriber_suspend");
}
