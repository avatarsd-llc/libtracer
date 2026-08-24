/**
 * @file
 * @brief RFC-0025 §4.1 delivery classes, HONOURED — what a class actually selects once
 *        Amendments 2, 3 and 4 have moved the machinery (#1204 phase 3).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `qos_policy_test.cpp` pins the DECODE: bits 6–7 carry the class, reserved bits above them
 * reach no honoured field. Nothing pinned what each class then DOES. This file does, and it
 * does it the way the amended RFC leaves the question rather than the way §4.1's original
 * prose implied.
 *
 * @section what_moved What the amendments moved
 *
 * §4.1 was written when the class was to be enforced by a per-subscriber buffer at the
 * producer's fan-out edge. Three rulings dismantled that:
 *
 *  - **Amendment 2** (§4.6.1) — *a producer never queues*. The ring belongs to the RECEIVING
 *    vertex, and the way a subscriber asks for depth is to **make its own target vertex a
 *    STREAM** (clause 2). The producer-side ring was deleted, not made optional.
 *  - **Amendment 3** (§4.1.2 clause 1) — accumulation is **graph state**, per source role:
 *    a plain value coalesces (RFC-0008 §B.2), a STREAM keeps its bounded since-flush list
 *    (§E). Per-`(vertex, subscriber)` fold buffers are REJECTED outright.
 *  - **Amendment 4** (§4.1.3) — batching is **user-orchestrated**: a batch is a value the
 *    application composes. `batch_count` / `batch_window_ns` are retired as graph and wire
 *    duties, so there is no graph-side trigger left for a class to arm.
 *
 * What survives is a single testable proposition: **the class is honoured by the ROLE of the
 * vertex on each side of the edge, plus the receiver's own declared pressure arm — never by a
 * bit consulted in the fan-out loop.** Every case below asserts one half of that, and each
 * carries its own ablation, because "N deliveries arrived" is a claim only if the same
 * harness can be made to show fewer.
 *
 * @section not_here What is deliberately NOT here
 *
 * The §4.4 pressure arms and the §4.5 gap signal AT the receiving ring are
 * `ring_pressure_test.cpp` (#1460 (a)/(c)); the composed-batch carriage is `batch_test.cpp`
 * and the three `stream/*` composition vectors; the FOLD emission mode is
 * `propagate_fold_test.cpp`. Nothing here re-times any of them.
 *
 * @par Callback lifetimes
 * `subscribe` binds a callback BY ADDRESS and dispatch reads it, so every log and lambda is
 * declared BEFORE the graph that points at it — reverse destruction tears the graph down
 * first. `unsubscribe` is not a barrier (ADR-0080 §Decision 4), so outliving the graph is the
 * only ordering that is actually safe (#1484 / #1489).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::delivery_class_t;
using tr::graph::delivery_policy_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief A packed policy carrying only @p cls in bits 6–7 — every other field default. */
[[nodiscard]] constexpr delivery_policy_t policy_of(delivery_class_t cls) {
    return delivery_policy_t{static_cast<std::uint16_t>(static_cast<std::uint16_t>(cls)
                                                        << delivery_policy_t::kDeliveryClassShift)};
}

/** @brief The first byte of each delivery, in arrival order — the whole observation. */
using trace_t = std::vector<std::uint8_t>;

/** @brief Append the first payload byte of @p v to @p out. */
void note(trace_t& out, const rope_t& v) {
    const std::span<const std::byte> bytes = v.only().bytes();
    out.push_back(bytes.empty() ? 0u : std::to_integer<std::uint8_t>(bytes[0]));
}

/** @brief Do the bytes of @p got run 1, 2, … @p n — every write, in write order? */
[[nodiscard]] bool is_run(const trace_t& got, std::size_t n) {
    if (got.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (got[i] != static_cast<std::uint8_t>(i + 1)) return false;
    return true;
}

/**
 * @brief §7 vector 2, `stream/class-immediate-order` — class 1 delivers EVERY write, in
 *        order, none conflated; and the ablation that makes that a claim.
 *
 * `write` is `assign` then deliver (RFC-0008 §D), so every write is its own delivery event
 * for every class. The class does not switch that on — which is the point: `IMMEDIATE` is
 * the shape the eager verb already has, and the honouring question is whether anything
 * *coalesces* it. Nothing does.
 *
 * The ablation is the coalescing verb pair on the SAME vertex and the SAME subscriber: k
 * `assign`s followed by one `propagate` is RFC-0008 §B.2 conflation, and it delivers ONCE
 * with the newest value. A harness that could not produce that difference would be asserting
 * nothing.
 */
void test_immediate_delivers_every_write_in_order() {
    std::printf("class 1 IMMEDIATE — every write, in order, none conflated:\n");
    trace_t eager;
    auto sink = [&eager](const rope_t& v) { note(eager, v); };

    graph_t g;
    const path_t p("/p/imm");
    const vertex_handle_t v = g.register_vertex(p, role_t::STORED_VALUE);
    const auto sub = g.subscribe(p, sink, policy_of(delivery_class_t::IMMEDIATE));
    check(sub.has_value(), "a class-1 subscription is admitted");

    for (std::uint8_t b = 1; b <= 5; ++b) (void)g.write(v, make_value({b}));
    check(is_run(eager, 5), "five writes => five deliveries, 1..5 in write order");

    // The ablation, same vertex, same edge: assign coalesces, propagate flushes once.
    eager.clear();
    for (std::uint8_t b = 6; b <= 10; ++b) (void)g.assign(v, make_value({b}));
    check(eager.empty(), "assign delivers NOTHING — the state half only (RFC-0008 §B)");
    (void)g.propagate(v);
    check(eager.size() == 1 && eager[0] == 10,
          "... and one covering propagate delivers ONCE, with the newest — the conflation "
          "this instrument can therefore tell apart from the run above");
}

/**
 * @brief §7 vector 4, `stream/class-stream-no-conflate` — a class-3 subscriber keeps every
 *        entry under a lagging consumer, BECAUSE ITS OWN TARGET IS A STREAM.
 *
 * This is Amendment 2 clause 2 made executable: depth is a property of the party that wants
 * depth, expressed where that party owns state. The two arms differ in exactly one thing —
 * the ROLE of the receiving vertex — and the class bits are identical in both, which is what
 * makes the arm the cause.
 */
void test_stream_class_no_conflate_at_a_stream_receiver() {
    std::printf(
        "class 3 STREAM — no conflation at a STREAM receiver; conflation at a plain one:\n");
    graph_t g;
    const path_t src("/p/str");
    const vertex_handle_t producer = g.register_vertex(src, role_t::STORED_VALUE);

    const path_t deep("/c/deep");
    const vertex_handle_t rx = g.register_vertex(deep, role_t::STREAM);
    g.set_history_depth(rx, 8);
    const path_t flat("/c/flat");
    const vertex_handle_t flat_rx = g.register_vertex(flat, role_t::STORED_VALUE);

    check(g.subscribe(src, deep, policy_of(delivery_class_t::STREAM)).has_value() &&
              g.subscribe(src, flat, policy_of(delivery_class_t::STREAM)).has_value(),
          "two class-3 edges off one producer, differing only in the RECEIVER's role");

    for (std::uint8_t b = 1; b <= 5; ++b) (void)g.write(producer, make_value({b}));

    std::vector<std::shared_ptr<const rope_t>> owed;
    const auto drained = g.drain_unflushed(rx, owed);
    check(drained.has_value() && *drained == 5, "the STREAM receiver owes all five, none lost");
    trace_t order;
    for (const std::shared_ptr<const rope_t>& sp : owed) note(order, *sp);
    check(is_run(order, 5), "... in write order — a queue, not a coalesce (RFC-0008 §E)");

    // The ablation: the same five deliveries into a plain receiver keep only the newest.
    const auto flat_history = g.history(flat_rx);
    check(!flat_history.has_value() && flat_history.error() == status_t::SCHEMA_NOT_FOUND,
          "the plain receiver has no ring to hold a history in (the role IS the difference)");
    const auto flat_now = g.read(flat_rx);
    check(flat_now.has_value(), "... it holds a value");
    trace_t last;
    note(last, **flat_now);
    check(last.size() == 1 && last[0] == 5,
          "... exactly one: the newest. Five deliveries, one retained — conflation, on the "
          "same class bits, because the receiver did not ask for depth");
}

/**
 * @brief §4.1.2 clause 2 — a flush emits the SNAPSHOT on a plain source and the FULL
 *        since-flush LIST on a STREAM source.
 *
 * The emission-by-role half of the retired `stream/class-batch-flush` vector (§7 item 3,
 * retired by Amendment 4 because there is no `batch_count` left to flush on). The trigger is
 * gone; the ROLE-dependent emission is not, and it is what `graph_t::propagate` does. The two
 * arms are each other's ablation: same verb, same number of assigns, different source role.
 */
void test_flush_emission_follows_the_source_role() {
    std::printf(
        "§4.1.2 clause 2 — flush emits snapshot on a plain source, the list on a STREAM:\n");
    trace_t plain_out;
    trace_t stream_out;
    auto plain_sink = [&plain_out](const rope_t& v) { note(plain_out, v); };
    auto stream_sink = [&stream_out](const rope_t& v) { note(stream_out, v); };

    graph_t g;
    const path_t plain("/s/plain");
    const vertex_handle_t pv = g.register_vertex(plain, role_t::STORED_VALUE);
    check(g.subscribe(plain, plain_sink).has_value(), "a subscriber on the plain source");

    const path_t stream("/s/stream");
    const vertex_handle_t sv = g.register_vertex(stream, role_t::STREAM);
    g.set_history_depth(sv, 8);
    check(g.subscribe(stream, stream_sink).has_value(), "a subscriber on the STREAM source");

    for (std::uint8_t b = 1; b <= 4; ++b) {
        (void)g.assign(pv, make_value({b}));
        (void)g.assign(sv, make_value({b}));
    }
    check(plain_out.empty() && stream_out.empty(), "no assign delivered anything");

    (void)g.propagate(pv);
    check(plain_out.size() == 1 && plain_out[0] == 4,
          "the plain source flushes the SNAPSHOT — one frame, the newest value");

    (void)g.propagate(sv);
    check(is_run(stream_out, 4),
          "the STREAM source flushes the FULL since-flush LIST — four frames, in order");

    // And the cursor advanced: a second flush with nothing appended emits nothing, so the
    // list above was the OWED set rather than the whole ring re-read.
    stream_out.clear();
    (void)g.propagate(sv);
    check(stream_out.empty(), "... and a re-flush with nothing appended emits nothing");
}

/**
 * @brief §7 vector 7, `stream/attach-forward` — a class-3 subscriber's first delivery is the
 *        first POST-ATTACH fan-out; the ring backfills nothing that predates the edge.
 *
 * §4.7 *Cold start*: *"the ring is drain machinery under pressure, **never history replay**:
 * no ring backfill, no epoch rewind, no 'seamless history' promise"*, and
 * `durability_request` (bit 5) keeps its ordinary meaning — the latched last value as the
 * join gift, with the stream class governing only deliveries **after** it.
 *
 * A host test, not a conformance vector: there are **no wire-observable bytes** here. The
 * proposition is about what does NOT arrive, and its instrument is a receiver's ring.
 *
 * The latch half of the vector is already pinned by `qos_policy_test.cpp`
 * (`test_policy_durability_is_per_subscriber`); this pins the attach-forward half, and uses
 * the latch as its ABLATION. The two edges below are admitted at the same instant, over the
 * same producer holding the same three pre-attach writes, with **identical class bits**,
 * differing in exactly one bit: bit 5. The requesting one receives a pre-attach value; the
 * plain one receives nothing until the next write. So "nothing arrived" is a claim about
 * attach-forward rather than about an inert harness — and what the latch delivers is ONE
 * value, the LKV, never the producer's history.
 */
void test_attach_forward_never_backfills_the_ring() {
    std::printf("§4.7 attach-forward — first delivery is the first post-attach fan-out:\n");
    graph_t g;
    const path_t src("/p/afwd");
    const vertex_handle_t producer = g.register_vertex(src, role_t::STORED_VALUE);

    // Three writes BEFORE any class-3 edge exists. The positive control that they happened.
    for (std::uint8_t b = 1; b <= 3; ++b) (void)g.write(producer, make_value({b}));
    const auto pre = g.read(producer);
    check(pre.has_value(), "the producer holds a value written before any subscriber attached");
    trace_t held;
    note(held, **pre);
    check(held.size() == 1 && held[0] == 3, "... the newest of the three pre-attach writes");

    const path_t forward("/c/forward");
    const vertex_handle_t rx = g.register_vertex(forward, role_t::STREAM);
    g.set_history_depth(rx, 8);
    const path_t latched("/c/latched");
    const vertex_handle_t rx_latched = g.register_vertex(latched, role_t::STREAM);
    g.set_history_depth(rx_latched, 8);

    constexpr delivery_policy_t kStream = policy_of(delivery_class_t::STREAM);
    constexpr delivery_policy_t kStreamDurable{
        static_cast<std::uint16_t>(kStream.bits | delivery_policy_t::kDurabilityRequest)};
    check(g.subscribe(src, forward, kStream).has_value() &&
              g.subscribe(src, latched, kStreamDurable).has_value(),
          "two class-3 edges admitted at the same instant, differing only in bit 5");

    std::vector<std::shared_ptr<const rope_t>> owed;
    const auto at_attach = g.drain_unflushed(rx, owed);
    check(at_attach.has_value() && *at_attach == 0 && owed.empty(),
          "the plain class-3 receiver owes NOTHING at attach — no backfill of the three "
          "writes that predate its edge (§4.7: the ring is not history replay)");

    // The ablation: the same producer, the same class, the same instant — bit 5 set.
    owed.clear();
    const auto joined = g.drain_unflushed(rx_latched, owed);
    check(joined.has_value() && *joined == 1,
          "... while the durability-requesting edge owes exactly ONE — so the harness CAN "
          "carry a pre-attach value, and the empty drain above is attach-forward, not inertia");
    trace_t gift;
    for (const std::shared_ptr<const rope_t>& sp : owed) note(gift, *sp);
    check(gift.size() == 1 && gift[0] == 3,
          "... and it is the LATCH — the LKV alone, never the producer's three-write history");

    // The first post-attach fan-out, and it is the FIRST delivery this edge ever sees.
    for (std::uint8_t b = 4; b <= 5; ++b) (void)g.write(producer, make_value({b}));
    owed.clear();
    const auto after = g.drain_unflushed(rx, owed);
    check(after.has_value() && *after == 2, "two post-attach writes => two deliveries");
    trace_t forward_trace;
    for (const std::shared_ptr<const rope_t>& sp : owed) note(forward_trace, *sp);
    check(forward_trace.size() == 2 && forward_trace[0] == 4 && forward_trace[1] == 5,
          "... and the FIRST of them is the first post-attach write (4), not the oldest "
          "value the producer ever held (1)");
}

/**
 * @brief The class bits reach the edge, read back, and select NOTHING in the fan-out loop.
 *
 * Stated as a test rather than a comment because it is the load-bearing consequence of the
 * three amendments and the thing most likely to drift: two edges on one producer whose
 * policies differ ONLY in bits 6–7 observe identical delivery sequences. The class is not a
 * per-edge switch, and `edge_view_t` — the always-inlined per-edge body of the wide fan-out
 * loop — must not grow a field to make it one (#1223 / #1250 measured 12 % for exactly that).
 *
 * If a future ruling gives the class a dispatch-visible effect, this case is where it fails
 * first, and that is the intent: it is a pin on a ruled reading, not a licence.
 */
void test_class_bits_do_not_switch_the_fanout_loop() {
    std::printf("the class bits are carried and read back; they switch nothing at dispatch:\n");
    trace_t conflate;
    trace_t immediate;
    trace_t stream;
    auto c_sink = [&conflate](const rope_t& v) { note(conflate, v); };
    auto i_sink = [&immediate](const rope_t& v) { note(immediate, v); };
    auto s_sink = [&stream](const rope_t& v) { note(stream, v); };

    graph_t g;
    const path_t p("/p/classes");
    const vertex_handle_t v = g.register_vertex(p, role_t::STORED_VALUE);
    check(g.subscribe(p, c_sink, policy_of(delivery_class_t::CONFLATE)).has_value() &&
              g.subscribe(p, i_sink, policy_of(delivery_class_t::IMMEDIATE)).has_value() &&
              g.subscribe(p, s_sink, policy_of(delivery_class_t::STREAM)).has_value(),
          "three edges on one producer, differing only in bits 6-7");

    for (std::uint8_t b = 1; b <= 3; ++b) (void)g.write(v, make_value({b}));
    check(is_run(conflate, 3) && is_run(immediate, 3) && is_run(stream, 3),
          "all three observe the same three deliveries — the class selects nothing HERE; "
          "depth is the receiving vertex's own role (RFC-0025 §4.6.1 clause 2)");

    // …and the bits themselves survive the round trip, so "carried, not consulted" is the
    // accurate description rather than "dropped".
    constexpr delivery_policy_t batch = policy_of(delivery_class_t::BATCH);
    check(batch.delivery_class() == delivery_class_t::BATCH && batch.reliability() == 0 &&
              batch.priority() == 0 && !batch.durability_request(),
          "a class-only policy decodes to that class and disturbs no other field");
}

}  // namespace

int main() {
    test_immediate_delivers_every_write_in_order();
    test_stream_class_no_conflate_at_a_stream_receiver();
    test_flush_emission_follows_the_source_role();
    test_attach_forward_never_backfills_the_ring();
    test_class_bits_do_not_switch_the_fanout_loop();
    return tr::testing::summary("delivery_class honouring (RFC-0025 §4.1, as amended)");
}
