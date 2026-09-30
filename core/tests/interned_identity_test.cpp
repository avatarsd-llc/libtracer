/**
 * @file
 * @brief RFC-0028 slice 8 (#1621, #1622): the 32-bit write sequence's modular compare, and the
 *        interned (link, caller) subject a remote edge's cold half holds instead of two strings.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Covers:
 *  - the WRAP: `write_seq_advanced` / `write_seq_distance` across 2^32, and a real vertex's
 *    `wait_for_change` fed a snapshot one bump before the wrap (the await predicate itself);
 *  - the sizes the slice pins (`subscriber_remote_t` 56 B, the sequence a lock-free 32-bit
 *    atomic);
 *  - the subject table: dedup by value, the link bit, the shared spelling, release and reuse,
 *    the dedup index's backward-shift delete under churn across several chunks;
 *  - the graph's two admission doors bind and deliver the interned names, and every reference
 *    is dropped when the graph goes;
 *  - concurrent intern / resolve / release (the TSan leg).
 *
 * The sweep half of the brief has nothing to wrap: the propagate sweep's IF_NEWER test is its
 * pending set (RFC-0008 §B), never a sequence compare, and the wire does not carry the
 * sequence. `current_seq` has exactly one in-tree consumer — `graph_t::await` — and it goes
 * through `wait_for_change`, which is what the wrap case below drives.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/subject_table.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/vertex.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::intern_subject;
using tr::graph::live_subject_count;
using tr::graph::path_t;
using tr::graph::release_subject;
using tr::graph::role_t;
using tr::graph::subject_id_t;
using tr::graph::subject_names;
using tr::graph::subscriber_remote_t;
using tr::graph::vertex_handle_t;
using tr::graph::write_seq_advanced;
using tr::graph::write_seq_distance;
using tr::graph::write_seq_t;
using tr::testing::check;
using tr::testing::make_value;
using tr::wire::opt_t;
using tr::wire::type_t;

// -- the pinned sizes -----------------------------------------------------------------------

static_assert(sizeof(write_seq_t) == 4, "the write sequence is 32-bit on every target (D6)");
static_assert(std::atomic<write_seq_t>::is_always_lock_free,
              "a lock-free 32-bit atomic: no libatomic call per publish on rv32");
static_assert(sizeof(void*) != 8 || sizeof(subscriber_remote_t) <= 56,
              "RFC-0028 §6.8 gate: the cold edge half is at most 56 B on a 64-bit host");
static_assert(sizeof(subject_id_t) == 4, "the interned subject is one 4-byte id (D8)");

/** @brief The modular compare, pure: across the wrap, at the aliasing distance, and backwards. */
void test_seq_wrap_arithmetic() {
    std::printf("write sequence: modular compare across 2^32:\n");
    constexpr write_seq_t kMax = 0xFFFFFFFFu;
    static_assert(write_seq_advanced(kMax, 0), "one bump across the wrap is a change");
    static_assert(write_seq_distance(kMax, 0) == 1, "and it is ONE bump, not 2^32 - 1");
    static_assert(write_seq_distance(kMax - 2, 5) == 8, "a distance straddling the wrap");
    static_assert(!write_seq_advanced(7, 7), "no bump is no change");

    // Every snapshot within 4096 of the wrap, every distance up to 4096: advanced iff d != 0,
    // and the distance read back exactly.
    bool all = true;
    for (write_seq_t back = 0; back < 4096; ++back) {
        const write_seq_t seq0 = static_cast<write_seq_t>(0u - back);
        for (write_seq_t d = 0; d < 4096; d += 7) {
            const write_seq_t now = static_cast<write_seq_t>(seq0 + d);
            all = all && write_seq_advanced(seq0, now) == (d != 0) &&
                  write_seq_distance(seq0, now) == d;
        }
    }
    check(all, "advanced iff at least one bump, for snapshots up to 4096 before the wrap");
    // The documented alias: exactly 2^32 bumps read as none. Stated so a change to it is seen.
    check(!write_seq_advanced(123, static_cast<write_seq_t>(123 + 0x100000000ull)),
          "exactly 2^32 bumps alias to 'no change' (the documented 49-days-at-1kHz edge)");
}

/** @brief The await predicate on a REAL vertex, fed a snapshot one bump before the wrap. */
void test_await_across_wrap() {
    std::printf("write sequence: wait_for_change across the wrap:\n");
    tr::graph::vertex_t v{role_t::STORED_VALUE, {}, {}};
    const write_seq_t now = v.current_seq();
    // A waiter whose snapshot was taken one bump BEFORE `now` — spelled across the wrap, since a
    // fresh vertex starts at 0, so the snapshot is 0xFFFFFFFF. The predicate must report the
    // change without blocking, exactly as it would for any other one-bump distance; an
    // ordered `now > seq0` compare would block here until the timeout.
    const write_seq_t before = static_cast<write_seq_t>(now - 1);
    check(now == 0 && before == 0xFFFFFFFFu, "the snapshot straddles the wrap");
    check(v.wait_for_change(before, 0ms),
          "a snapshot one bump behind, across the wrap, is a "
          "change — immediately");
    check(!v.wait_for_change(now, 5ms), "the current sequence is not a change (timeout)");
    v.note_write();
    check(write_seq_distance(now, v.current_seq()) == 1, "one bump moves the sequence by one");
    check(v.wait_for_change(now, 0ms), "and the old snapshot now sees the change");
}

/** @brief Dedup by value, the link bit, the shared spelling, release. */
void test_subject_table_basics() {
    std::printf("subject table: intern / resolve / release:\n");
    const std::size_t base = live_subject_count();

    const auto none = intern_subject({}, {});
    check(none.has_value() && !none->valid(), "the empty pair is the default id, no entry");
    check(live_subject_count() == base, "and takes nothing");

    const auto a = intern_subject("p3", "p3");
    const auto b = intern_subject("p3", "p3");
    check(a && b && a->valid() && *a == *b, "the same pair interns to the same id");
    check(a->has_link(), "a non-empty link sets the link bit");
    check(live_subject_count() == base + 1, "one entry for both references");
    check(subject_names(*a).link == "p3" && subject_names(*a).caller == "p3",
          "the default subject resolves both names");

    const auto c = intern_subject("p3", "p3/writer-7");
    check(c && *c != *a, "a per-writer subject over the same link is a different pair");
    check(subject_names(*c).link == "p3" && subject_names(*c).caller == "p3/writer-7",
          "and resolves both of its spellings");

    const auto d = intern_subject({}, "cli");
    check(d && d->valid() && !d->has_link(), "a caller-only pair has no link bit");
    check(subject_names(*d).link.empty() && subject_names(*d).caller == "cli",
          "and resolves an empty link");

    const auto e = intern_subject("ab", "c");
    const auto f = intern_subject("a", "bc");
    check(e && f && *e != *f, "the separator keeps ('ab','c') and ('a','bc') apart");

    for (const auto& id : {a, b, c, d, e, f}) release_subject(*id);
    check(live_subject_count() == base, "every reference released frees every entry");
    release_subject(subject_id_t{});  // a no-op, by contract
}

/** @brief Churn across several chunks: the dedup index survives backward-shift deletes. */
void test_subject_table_churn() {
    std::printf("subject table: churn across chunks:\n");
    const std::size_t base = live_subject_count();
    constexpr int kN = 600;  // chunks 0..4 (16 + 32 + 64 + 128 + 256 < 600 + base)
    std::vector<subject_id_t> ids;
    ids.reserve(kN);
    bool ok = true;
    for (int i = 0; i < kN; ++i) {
        const std::string link = "n" + std::to_string(i % 37);
        const std::string caller = "w" + std::to_string(i);
        const auto id = intern_subject(link, caller);
        ok = ok && id.has_value();
        if (id) ids.push_back(*id);
    }
    check(ok && live_subject_count() == base + kN, "600 distinct pairs intern");
    // Free every third, then check the rest still resolve AND still dedup to themselves.
    for (int i = 0; i < kN; i += 3) release_subject(ids[static_cast<std::size_t>(i)]);
    bool resolve = true;
    bool dedup = true;
    for (int i = 0; i < kN; ++i) {
        if (i % 3 == 0) continue;
        const subject_id_t id = ids[static_cast<std::size_t>(i)];
        const std::string link = "n" + std::to_string(i % 37);
        const std::string caller = "w" + std::to_string(i);
        resolve = resolve && subject_names(id).link == link && subject_names(id).caller == caller;
        const auto again = intern_subject(link, caller);
        dedup = dedup && again && *again == id;
        if (again) release_subject(*again);
    }
    check(resolve, "survivors resolve to their own names after the deletes");
    check(dedup, "and the dedup index still finds every one of them");
    // Re-intern the freed ones: they reuse freed indices rather than growing the table.
    std::vector<subject_id_t> back;
    for (int i = 0; i < kN; i += 3) {
        const auto id = intern_subject("n" + std::to_string(i % 37), "w" + std::to_string(i));
        if (id) back.push_back(*id);
    }
    check(live_subject_count() == base + kN, "the freed pairs re-intern");
    for (const subject_id_t id : back) release_subject(id);
    for (int i = 0; i < kN; ++i)
        if (i % 3 != 0) release_subject(ids[static_cast<std::size_t>(i)]);
    check(live_subject_count() == base, "and the table drains back to where it started");
}

/** @brief PATH{ NAME @p seg }. */
std::vector<std::byte> b_path(std::string_view seg) {
    std::vector<std::byte> body;
    (void)tr::wire::emit_path_segment(body, seg);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief SUBSCRIBER{ PATH @p marker }. */
std::vector<std::byte> b_subscriber(std::string_view marker) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path(marker));
    return out;
}

/** @brief The graph's wire door binds the interned names, and deliveries carry them. */
void test_graph_binds_interned_names() {
    std::printf("graph: subscribe_wire binds the interned (link, caller) subject:\n");
    const std::size_t base = live_subject_count();
    {
        graph_t g;
        vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
        std::vector<std::string> seen;
        const tr::testing::remote_sink_guard_t sink(
            g, [&](const tr::graph::remote_delivery_t& d, const tr::graph::value_t&) {
                seen.push_back(std::string(d.link) + "|" + std::string(d.caller));
            });
        // Two edges over the same link with the default subject share ONE entry; a third with a
        // per-writer subject takes a second.
        check(g.subscribe_wire(v, make_value(b_subscriber("a")), make_value(b_path("cli")), "cli")
                  .has_value(),
              "default-subject edge 1");
        check(g.subscribe_wire(v, make_value(b_subscriber("b")), make_value(b_path("cli")), "cli")
                  .has_value(),
              "default-subject edge 2");
        check(g.subscribe_wire(v, make_value(b_subscriber("c")), make_value(b_path("cli")), "cli",
                               {}, "cli/writer")
                  .has_value(),
              "per-writer-subject edge");
        check(live_subject_count() == base + 2, "three edges, two interned subjects");

        check(g.write(v, make_value({0x2A})).has_value(), "publish");
        std::size_t dflt = 0;
        std::size_t writer = 0;
        for (const std::string& s : seen) {
            dflt += s == "cli|cli" ? 1 : 0;
            writer += s == "cli|cli/writer" ? 1 : 0;
        }
        check(seen.size() == 3 && dflt == 2 && writer == 1,
              "every delivery carries its edge's link and caller names");
        check(g.evict_link_edges("cli") == 3, "eviction still matches the interned link by name");
    }
    check(live_subject_count() == base, "the graph's teardown dropped every subject reference");
}

/** @brief Concurrent intern / resolve / release of shared and private pairs (the TSan leg). */
void test_subject_table_concurrent() {
    std::printf("subject table: concurrent intern / resolve / release:\n");
    const std::size_t base = live_subject_count();
    const auto shared = intern_subject("bus", "bus");
    std::atomic<bool> bad{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([t, &bad] {
            for (int i = 0; i < 2000; ++i) {
                const std::string own = "t" + std::to_string(t) + "-" + std::to_string(i % 50);
                const auto a = intern_subject("bus", "bus");
                const auto b = intern_subject(own, own);
                if (!a || !b) {
                    bad = true;
                    continue;
                }
                if (subject_names(*a).caller != "bus" || subject_names(*b).link != own) bad = true;
                release_subject(*b);
                release_subject(*a);
            }
        });
    }
    for (std::thread& th : ts) th.join();
    check(!bad.load(), "every held id resolved to its own names under contention");
    release_subject(*shared);
    check(live_subject_count() == base, "and every reference balanced");
}

}  // namespace

int main() {
    test_seq_wrap_arithmetic();
    test_await_across_wrap();
    test_subject_table_basics();
    test_subject_table_churn();
    test_graph_binds_interned_names();
    test_subject_table_concurrent();
    return tr::testing::summary("interned_identity");
}
