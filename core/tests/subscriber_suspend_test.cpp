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
 *   3. The published array carries the ACTIVE edges: a cleared slot leaves it, a suspended
 *      one keeps its entry (clear), so a toggle republishes nothing.
 *   4. A toggle draws nothing from any source, so it succeeds with the source exhausted. A
 *      resume after a REFUSED republish rebuilds the stale array itself: with the source still
 *      exhausted it answers BACKPRESSURE and changes nothing, and the RETRY succeeds once the
 *      source has room, with no other mutation in between.
 *   5. The liveness flip finds its entry by slot — the published array no longer mirrors the
 *      slot table — observed as a suspend silencing exactly its own edge.
 *   6. The RFC-0005 counts are of DELIVERING edges: with 0 live and M suspended edges the
 *      vertex has no subscribers, its descendants do not bubble, and clearing a suspended
 *      edge does not uncount it a second time.
 *   7. Toggling races a writer cleanly (the TSan legs run this): a writer thread against
 *      suspend, resume and unsubscribe/re-subscribe on the same and neighbouring slots, and the
 *      counts come out exact.
 *   8. A remote (routed) slot toggles through the same per-slot primitive #2019's wire door
 *      will drive: one link goes silent and comes back, the other keeps receiving.
 */

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string_view>
#include <thread>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/tlv_emit.hpp"
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

/** @brief (3) + (5) The published array holds the ACTIVE edges: a cleared slot leaves it, a
 *         suspended one keeps a skipped entry, and the flip still finds the right entry once
 *         the array stops mirroring the slot table. */
void test_published_array_is_the_active_edges() {
    std::printf("the published array carries the active edges only:\n");
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/s/c"), role_t::STORED_VALUE);
    constexpr std::size_t kN = 16;
    std::vector<counter_t> sinks(kN);
    std::vector<subscription_t> subs;
    for (counter_t& s : sinks) subs.push_back(*g.subscribe(path_t("/s/c"), count, &s));
    check(published(src) == kN, "16 edges, 16 published entries");

    for (std::size_t i = 0; i < kN; i += 2) (void)g.set_suspended(subs[i], true);
    check(published(src) == kN, "suspending 8 republishes nothing: their entries stay, clear");

    // Clear the odd edges one at a time; the survivors keep receiving across each republish,
    // and the suspended ones stay silent through it (each republish rebuilds their entry clear).
    for (std::size_t i = 1; i < kN; i += 2) {
        (void)g.unsubscribe(subs[i]);
        (void)g.write(src, byte_value(static_cast<std::uint8_t>(i)));
        for (std::size_t j = i + 2; j < kN; j += 2)
            check(sinks[j].seen == static_cast<int>((i + 1) / 2),
                  "a still-live edge keeps receiving after a neighbour's unsubscribe");
    }
    check(published(src) == kN / 2, "the cleared 8 left the array; the suspended 8 remain");
    for (std::size_t i = 0; i < kN; i += 2)
        check(sinks[i].seen == 0, "no suspended edge received anything");

    for (std::size_t i = 0; i < kN; i += 2) (void)g.set_suspended(subs[i], false);
    check(published(src) == kN / 2, "resuming them republishes nothing either");
    (void)g.write(src, byte_value(0x44));
    for (std::size_t i = 0; i < kN; i += 2)
        check(sinks[i].seen == 1 && sinks[i].last == 0x44, "each resumed edge takes the write");
}

/** @brief (4) A toggle draws nothing; a resume after a refused republish rebuilds the array,
 *         answering BACKPRESSURE, unchanged, only while the source has no room. */
void test_toggle_allocates_nothing() {
    std::printf("a toggle allocates nothing; a resume over a stale array rebuilds it:\n");
    gate_source_t gate;
    graph_t g{gate};
    const vertex_handle_t src = g.register_vertex(path_t("/s/d"), role_t::STORED_VALUE);
    counter_t lo, a, hi, x;
    const auto slo = g.subscribe(path_t("/s/d"), count, &lo);
    const auto sa = g.subscribe(path_t("/s/d"), count, &a);
    const auto shi = g.subscribe(path_t("/s/d"), count, &hi);
    const auto sx = g.subscribe(path_t("/s/d"), count, &x);
    check(slo && sa && shi && sx, "four edges admitted");

    gate.arm(0);  // every draw refused (the toggles only: a write draws its value's block)
    check(g.set_suspended(*sa, true).has_value(), "a suspend with no room succeeds");
    gate.disarm();
    (void)g.write(src, byte_value(0x55));
    check(a.seen == 0 && lo.seen == 1 && hi.seen == 1,
          "... and silences exactly its own edge (the flip found its entry by slot)");
    gate.arm(0);
    check(g.set_suspended(*sa, false).has_value(), "a resume with no room succeeds too");
    check(g.set_suspended(*sa, true).has_value(), "... as does the next suspend");

    check(g.unsubscribe(*sx).has_value(), "an unsubscribe with no room still takes effect");
    const auto r = g.set_suspended(*sa, false);
    check(!r && r.error() == status_t::BACKPRESSURE,
          "after that refused republish, a resume with no room answers BACKPRESSURE");
    gate.disarm();
    check(g.is_suspended(*sa).value_or(false) && g.own_subs(src) == 2,
          "... and the edge is still suspended, its speculative count given back");
    (void)g.write(src, byte_value(0x66));
    check(a.seen == 0 && x.seen == 1, "... still receives nothing, and the cleared edge neither");

    // The source has room again and NOTHING else on the vertex changed: the retry itself
    // rebuilds the stale array (the selector's case: switch edges under memory pressure).
    check(g.set_suspended(*sa, false).has_value(), "the retry, once the source has room, succeeds");
    check(g.own_subs(src) == 3 && published(src) == 3,
          "... counts the edge, and rebuilt the array without the cleared edge");
    (void)g.write(src, byte_value(0x77));
    check(a.seen == 1 && a.last == 0x77 && x.seen == 1 && lo.seen == 3,
          "... and the edge delivers again, the cleared one still does not");
    gate.arm(0);
    check(g.set_suspended(*sa, true).has_value() && g.set_suspended(*sa, false).has_value(),
          "... and with the array current again, a switch draws nothing");
    gate.disarm();
}

/** @brief A sink for the graph's remote-delivery hook: deliveries per link name. */
struct link_counts_t {
    int a = 0; /**< @brief Deliveries routed over link "a". */
    int b = 0; /**< @brief Deliveries routed over link "b". */
};

/** @brief A SUBSCRIBER{ PATH @p marker } TLV, as a peer's SUBSCRIBE carries it. */
tr::view::view_t subscriber_tlv(std::string_view marker) {
    std::vector<std::byte> path;
    (void)tr::wire::emit_path_segment(path, marker);
    std::vector<std::byte> body;
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{}, path);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::SUBSCRIBER, tr::wire::opt_t{.pl = true}, body);
    return make_value(out);
}

/** @brief A return route naming @p link. */
tr::view::view_t route_to(std::string_view link) {
    std::vector<std::byte> path;
    (void)tr::wire::emit_path_segment(path, link);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, path);
    return make_value(out);
}

/** @brief (8) A remote (routed) slot toggles through the same primitive (#2019's layer). */
void test_remote_slot_toggles() {
    std::printf("a remote (routed) slot suspends and resumes in place:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/s/r"), role_t::STORED_VALUE);
    link_counts_t seen;
    const tr::testing::remote_sink_guard_t sink(
        g, [&](const tr::graph::remote_delivery_t& d, const value_t&) {
            (d.link == tr::testing::test_link("a") ? seen.a : seen.b) += 1;
        });
    check(g.subscribe_wire(v, subscriber_tlv("pa"), route_to("a"), "a", {}, {}, {},
                           tr::testing::test_link("a"))
                  .has_value() &&
              g.subscribe_wire(v, subscriber_tlv("pb"), route_to("b"), "b", {}, {}, {},
                               tr::testing::test_link("b"))
                  .has_value(),
          "two routed edges, slots 0 (link a) and 1 (link b)");
    tr::graph::vertex_t& raw_v = *std::bit_cast<tr::graph::vertex_t*>(v);
    using edge_suspend_t = tr::graph::vertex_t::edge_suspend_t;

    check(raw_v.set_edge_suspended(0, true) == edge_suspend_t::CHANGED, "suspend slot 0");
    check(raw_v.edge_suspended(0).value_or(false) && published(v) == 2,
          "... it reads back suspended and keeps its entry");
    (void)g.write(v, byte_value(0x21));
    check(seen.a == 0 && seen.b == 1, "... link a receives nothing, link b still does");

    check(raw_v.set_edge_suspended(0, false) == edge_suspend_t::CHANGED, "resume slot 0");
    (void)g.write(v, byte_value(0x22));
    check(seen.a == 1 && seen.b == 2, "... and both links receive the next write, no replay");
    check(raw_v.set_edge_suspended(0, false) == edge_suspend_t::UNCHANGED &&
              raw_v.set_edge_suspended(2, true) == edge_suspend_t::NOT_FOUND,
          "a no-change toggle is UNCHANGED; an empty slot is NOT_FOUND");
}

/** @brief The raw vertex behind @p h, for the RFC-0005 counters. */
tr::graph::vertex_t& raw(vertex_handle_t h) { return *std::bit_cast<tr::graph::vertex_t*>(h); }

/** @brief (6) 0 live + M suspended is a vertex with no subscribers, to every reader. */
void test_counts_are_delivering_edges() {
    std::printf("0 live + M suspended edges count as no subscribers:\n");
    graph_t g;
    const vertex_handle_t top = g.register_vertex(path_t("/s/e"), role_t::STORED_VALUE);
    const vertex_handle_t kid = g.register_vertex(path_t("/s/e/k"), role_t::STORED_VALUE);
    constexpr std::size_t kM = 4;
    std::vector<counter_t> sinks(kM);
    std::vector<subscription_t> subs;
    for (counter_t& c : sinks) subs.push_back(*g.subscribe(path_t("/s/e"), count, &c));
    check(g.own_subs(top) == kM && raw(kid).listeners_above() == kM, "4 edges counted");

    for (const subscription_t& s : subs) (void)g.set_suspended(s, true);
    check(g.own_subs(top) == 0, "all suspended: own_subs is 0");
    check(!g.has_subscribers(top), "... has_subscribers(top) is false");
    check(raw(kid).listeners_above() == 0 && !g.has_subscribers(kid),
          "... and the descendant no longer bubbles");
    const vertex_handle_t late = g.register_vertex(path_t("/s/e/late"), role_t::STORED_VALUE);
    check(raw(late).listeners_above() == 0, "a descendant made while suspended inherits 0");
    (void)g.write(top, byte_value(1));
    (void)g.write(kid, byte_value(2));
    for (const counter_t& c : sinks) check(c.seen == 0, "no suspended edge received anything");

    check(g.set_suspended(subs[0], true).has_value() && g.own_subs(top) == 0,
          "a no-change suspend does not uncount twice");
    check(g.unsubscribe(subs[1]).has_value() && g.own_subs(top) == 0 &&
              raw(kid).listeners_above() == 0,
          "clearing a suspended edge does not uncount it again (no underflow)");

    check(g.set_suspended(subs[2], false).has_value(), "resume one");
    check(g.own_subs(top) == 1 && raw(kid).listeners_above() == 1 &&
              raw(late).listeners_above() == 1 && g.has_subscribers(top),
          "... counts exactly it, on the vertex and every descendant");
    check(g.set_suspended(subs[2], false).has_value() && g.own_subs(top) == 1,
          "a no-change resume gives its speculative count back");
    (void)g.write(kid, byte_value(3));
    check(sinks[2].seen == 1 && sinks[2].last == 3, "... and the descendant's write bubbles to it");
    check(g.unsubscribe(subs[2]).has_value() && g.unsubscribe(subs[3]).has_value() &&
              g.unsubscribe(subs[0]).has_value(),
          "clear the rest");
    check(g.own_subs(top) == 0 && raw(kid).listeners_above() == 0, "every count back to 0");
}

/** @brief (7) A writer thread against toggles and churn; the counts come out exact. */
void test_toggle_races_a_writer() {
    std::printf("toggling races a writer cleanly:\n");
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/s/f"), role_t::STORED_VALUE);
    counter_t a, b;  // a: toggled; b: churned beside it; the third edge is always live
    const auto sa = g.subscribe(path_t("/s/f"), count, &a);
    auto sb = g.subscribe(path_t("/s/f"), count, &b);
    std::atomic<int> c_seen{0};
    const auto sc = g.subscribe(
        path_t("/s/f"),
        [](void* ctx, const value_t&) {
            static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
        },
        &c_seen);
    check(sa && sb && sc, "three edges admitted");

    std::atomic<bool> stop{false};
    std::atomic<int> writes{0};
    std::thread writer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            (void)g.write(src, byte_value(7));
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });
    constexpr int kIters = 2000;
    bool ok = true;
    for (int i = 0; i < kIters; ++i) {
        ok = g.set_suspended(*sa, true).has_value() && ok;
        ok = g.set_suspended(*sa, false).has_value() && ok;
        if (i % 4 == 0) {
            ok = g.unsubscribe(*sb).has_value() && ok;
            sb = g.subscribe(path_t("/s/f"), count, &b);
            ok = sb.has_value() && ok;
        }
    }
    stop.store(true, std::memory_order_release);
    writer.join();
    check(ok, "every toggle and churn step answered OK");
    check(g.own_subs(src) == 3, "the count is exact afterwards: three delivering edges");
    check(writes.load() == 0 || c_seen.load() == writes.load(),
          "the untouched neighbour received every write");
    (void)g.set_suspended(*sa, true);
    check(g.own_subs(src) == 2, "a final suspend uncounts exactly one");
}

}  // namespace

int main() {
    test_suspend_and_resume_deliver_correctly();
    test_slot_survives();
    test_published_array_is_the_active_edges();
    test_toggle_allocates_nothing();
    test_counts_are_delivering_edges();
    test_toggle_races_a_writer();
    test_remote_slot_toggles();
    return tr::testing::summary("subscriber_suspend");
}
