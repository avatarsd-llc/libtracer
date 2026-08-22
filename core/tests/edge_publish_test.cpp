/**
 * @file
 * @brief The published edge array and its edge pin (#635): a snapshot sees the OLD edge set
 *        or the NEW one and never a torn mixture, an unsubscribe stops delivering at once,
 *        the pin is never held across a dispatch, and nothing is freed under a reader or
 *        leaked at teardown.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `vertex_t::snapshot_edges` used to copy the slot table out under the vertex STRIPE mutex, so
 * two vertices that merely hashed to the same stripe serialised their publishes against each
 * other — ×16.6 at twenty-four threads, with aggregate throughput FALLING past four. It now
 * copies a published, immutable-after-publish array out under a bounded per-participant edge
 * pin, and the control plane frees a displaced array on its own thread once no participant
 * announces it.
 *
 * This test is built to fail without that machinery being correct, and it is shaped by what
 * would NOT fail:
 *
 *   - **Publishers and mutators run on the SAME vertex, concurrently.** A test that subscribes
 *     up front and only then writes never displaces an array while a reader is inside one, so
 *     the whole reclamation question goes unexercised and ASan stays green whatever the code
 *     does. Here a churn thread adds and clears edges for the entire timed window.
 *   - **The delivered value is CHECKED, not counted.** A torn snapshot's most likely shape is a
 *     dispatch view assembled half from one array and half from another, which a delivery
 *     counter cannot see. Each callback validates its own context's magic word and the payload
 *     it received.
 *   - **A subscriber callback RE-ENTERS the graph.** The pin is non-nestable by construction
 *     (one cell per thread) and `detail_ep::pin_t` asserts the cell is empty on entry, so a
 *     design that held the pin across `dispatch_edge` aborts here in every debug build — which
 *     is exactly what the ASan and TSan legs are.
 *   - **More publisher threads than `kEdgePinSlots`** in one arm, so the stripe-mutex fallback
 *     for a thread that cannot claim a cell is executed rather than merely documented.
 *
 * Not gated on a sanitizer: with TSan it is a race detector's test, with ASan/LSan it is the
 * use-after-free and teardown-flush test, and plain it is a smoke test that the churn neither
 * loses a live subscription nor delivers to a cleared one.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "graph_sinks.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::remote_delivery_t;
using tr::graph::role_t;
using tr::graph::subscription_t;
using tr::view::rope_t;
using tr::view::view_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief A subscriber context that can prove it was not delivered to after teardown. */
struct sink_t {
    static constexpr std::uint32_t kMagic = 0x5E'11'AB'1EU;
    std::uint32_t magic = kMagic;     /**< @brief Cleared before the object dies. */
    std::atomic<std::size_t> hits{0}; /**< @brief Deliveries observed. */
    std::atomic<std::size_t> torn{0}; /**< @brief Deliveries with a corrupt context. */
};

/** @brief The plain sink callback: validate the context, count the delivery. */
void on_value(void* ctx, const rope_t&) {
    auto* s = static_cast<sink_t*>(ctx);
    if (s->magic != sink_t::kMagic) {
        s->torn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s->hits.fetch_add(1, std::memory_order_relaxed);
}

/** @brief A 4-byte VALUE payload built once and written by borrow (no per-write allocation). */
struct payload_t {
    std::vector<std::byte> bytes;
    [[nodiscard]] view_t view() const { return view_t::over(tr::view::borrow_const(bytes)); }
};

[[nodiscard]] payload_t make_payload(std::byte fill) {
    payload_t p;
    std::vector<std::byte> body(4, fill);
    tr::wire::emit_tlv(p.bytes, tr::wire::type_t::VALUE, tr::wire::opt_t{}, body);
    return p;
}

/**
 * @brief Publishers and a subscribe/unsubscribe churn run together on ONE vertex.
 *
 * The property under test is the one the immutable array exists to provide: a snapshot
 * observes the edge set as it was at SOME publish, never a mixture of two. Nothing here
 * asserts a delivery count — the churn makes that inherently racy — but every delivery must
 * land on a live context, and the vertex must still deliver to the standing subscriber
 * afterwards, which a lost or double-freed array would break.
 *
 * @param publishers How many threads write concurrently. Passing more than
 *        `tr::graph::kEdgePinSlots` is what drives the stripe-mutex fallback.
 */
void test_concurrent_publish_and_churn(std::size_t publishers, const char* label) {
    std::printf("concurrent publish + subscribe/unsubscribe churn (%s):\n", label);
    graph_t g;
    const path_t src = *path_t::parse("/t/churn/src");
    (void)g.register_vertex(src, role_t::STORED_VALUE);

    sink_t standing;
    const auto sub = g.subscribe(src, on_value, &standing);
    check(sub.has_value(), "the standing subscription is admitted");
    if (!sub) return;

    const payload_t p = make_payload(std::byte{0x11});
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> writes{0};
    std::atomic<std::size_t> started{0};

    std::vector<std::thread> ts;
    ts.reserve(publishers + 1);
    for (std::size_t i = 0; i < publishers; ++i) {
        ts.emplace_back([&]() {
            const auto batch = [&] {
                for (int k = 0; k < 64; ++k) (void)g.write(src, p.view());
                writes.fetch_add(64, std::memory_order_relaxed);
            };
            // One unconditional batch before `stop` is ever consulted, then announce: the
            // churn's work is BOUNDED, so without this it could finish and set `stop` before
            // a publisher's first iteration and turn a scheduling accident into a red
            // "the publishers ran" (#1378).
            batch();
            started.fetch_add(1, std::memory_order_release);
            while (!stop.load(std::memory_order_relaxed)) batch();
        });
    }
    // The churn thread: each iteration DISPLACES the published array twice (an append and a
    // clear), so publishers are constantly copying an array the churn is retiring.
    std::vector<sink_t> transient(8);
    ts.emplace_back([&]() {
        // Bounded wait for the publishers to be in their loop, so the overlap this test is
        // about is established rather than hoped for. A host that cannot schedule them
        // inside the deadline still runs the churn — the guards below report it.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (started.load(std::memory_order_acquire) < publishers &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        for (std::size_t round = 0; round < 4000 && !stop.load(std::memory_order_relaxed);
             ++round) {
            sink_t& t = transient[round % transient.size()];
            const auto s = g.subscribe(src, on_value, &t);
            if (s) (void)g.unsubscribe(*s);
        }
        stop.store(true, std::memory_order_relaxed);
    });
    for (auto& th : ts) th.join();

    // Both are structural rather than statistical: every publisher is joined above and each
    // ran a batch no `stop` could cut short, and `g.write` delivers to the standing edge
    // inline on the writing thread (#1378).
    check(writes.load() >= 64 * publishers, "every publisher ran at least its first batch");
    check(standing.hits.load() > 0, "the standing subscriber received deliveries throughout");
    check(standing.torn.load() == 0, "no delivery reached a torn standing context");
    std::size_t torn = 0;
    for (const sink_t& t : transient) torn += t.torn.load();
    check(torn == 0, "no delivery reached a torn transient context");

    // The array must still be correct after all that churn: one more publish, one more hit.
    const std::size_t before = standing.hits.load();
    check(g.write(src, p.view()).has_value(), "a post-churn write succeeds");
    check(standing.hits.load() == before + 1,
          "the post-churn publish delivers exactly once to the surviving edge");
}

/**
 * @brief An unsubscribed edge stops receiving IMMEDIATELY — the monotone liveness bit.
 *
 * The compacting republish behind an unsubscribe is allowed to soft-fail on OOM; the
 * deactivation is not, and this is the property that says so. It also covers the case the
 * published array's index mapping could get wrong: clearing the MIDDLE slot of three must
 * silence exactly that one.
 */
void test_unsubscribe_stops_delivery_at_once() {
    std::printf("an unsubscribe stops delivery at once (and only for its own slot):\n");
    graph_t g;
    const path_t src = *path_t::parse("/t/stop/src");
    (void)g.register_vertex(src, role_t::STORED_VALUE);
    sink_t a, b, c;
    const auto sa = g.subscribe(src, on_value, &a);
    const auto sb = g.subscribe(src, on_value, &b);
    const auto sc = g.subscribe(src, on_value, &c);
    check(sa && sb && sc, "three edges are admitted");
    if (!sa || !sb || !sc) return;

    const payload_t p = make_payload(std::byte{0x22});
    (void)g.write(src, p.view());
    check(a.hits == 1 && b.hits == 1 && c.hits == 1, "all three receive the first publish");

    check(g.unsubscribe(*sb).has_value(), "the middle edge unsubscribes");
    (void)g.write(src, p.view());
    check(a.hits == 2 && c.hits == 2, "the outer two still receive");
    check(b.hits == 1, "the cleared middle edge receives nothing more");

    // A re-subscribe REUSES the cleared slot (RFC-0009 §D.2) — the published array must pick
    // the reused slot back up rather than keep publishing the shell.
    sink_t d;
    const auto sd = g.subscribe(src, on_value, &d);
    // The handle is opaque, so the reuse is observed the only way a caller can observe it:
    // the fresh handle compares EQUAL to the cleared one — same producer, same slot index.
    check(sd.has_value() && *sd == *sb, "the re-subscribe reuses the cleared slot");
    (void)g.write(src, p.view());
    check(d.hits == 1, "the reused slot delivers");
    check(b.hits == 1, "and the departed edge still receives nothing");
}

/** @brief The context of a callback that re-enters the graph from inside a delivery. */
struct reentrant_ctx_t {
    graph_t* g = nullptr;
    const payload_t* p = nullptr;
    path_t nested{};
    path_t self{};
    std::atomic<std::size_t> hits{0};
    std::atomic<std::size_t> depth{0};
    std::atomic<std::size_t> max_depth{0};
};

/**
 * @brief Deliver, then publish again from inside the callback — a nested `fan_out`.
 *
 * This is the shape the edge pin's non-nestability has to survive. It re-enters twice over:
 * once onto ANOTHER vertex (the ordinary bubbling / re-dispatch shape) and, at depth 0 only,
 * once onto the SAME vertex, which re-enters `snapshot_edges` on the very array the outer
 * dispatch was copied from. If the pin were still announced, `pin_t`'s constructor assertion
 * fires.
 */
void on_reentrant(void* ctx, const rope_t&) {
    auto* r = static_cast<reentrant_ctx_t*>(ctx);
    const std::size_t d = r->depth.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t m = r->max_depth.load(std::memory_order_relaxed); d + 1 > m;)
        if (r->max_depth.compare_exchange_weak(m, d + 1, std::memory_order_relaxed)) break;
    r->hits.fetch_add(1, std::memory_order_relaxed);
    (void)r->g->write(r->nested, r->p->view());
    if (d == 0) (void)r->g->write(r->self, r->p->view());
    r->depth.fetch_sub(1, std::memory_order_relaxed);
}

/** @brief The pin is released before dispatch, so a re-entrant callback always claims a
 *         fresh empty cell (the assertion in `detail_ep::pin_t` is the actual gate). */
void test_reentrant_callback_finds_an_empty_pin() {
    std::printf("a re-entrant subscriber callback finds an empty pin cell:\n");
    graph_t g;
    const path_t src = *path_t::parse("/t/re/src");
    const path_t other = *path_t::parse("/t/re/other");
    (void)g.register_vertex(src, role_t::STORED_VALUE);
    (void)g.register_vertex(other, role_t::STORED_VALUE);

    const payload_t p = make_payload(std::byte{0x33});
    reentrant_ctx_t r;
    r.g = &g;
    r.p = &p;
    r.nested = other;
    r.self = src;
    sink_t tail;
    check(g.subscribe(other, on_value, &tail).has_value(), "the nested target has an edge");
    check(g.subscribe(src, on_reentrant, &r).has_value(), "the re-entrant edge is admitted");

    check(g.write(src, p.view()).has_value(), "the outer publish succeeds");
    check(r.hits.load() >= 2, "the re-entrant callback ran nested as well as outer");
    check(r.max_depth.load() >= 2, "the nested delivery genuinely re-entered (depth >= 2)");
    check(tail.hits.load() >= 2, "the nested publishes were delivered");
    check(tail.torn.load() == 0, "no nested delivery saw a torn context");
}

/**
 * @brief A wide fan-out (past `kInlineFanout`) under churn — the overflow/heap copy path.
 *
 * The inline stack buffer and the thread-local overflow vector are two DIFFERENT copy loops
 * out of the published array, and only the second one reserves. Exercising just the narrow
 * one would leave the wide path's bounds untested against an array whose entry count now
 * includes inactive shells.
 */
void test_wide_fanout_under_churn() {
    std::printf("a wide fan-out copies the published array correctly under churn:\n");
    graph_t g;
    const path_t src = *path_t::parse("/t/wide/src");
    (void)g.register_vertex(src, role_t::STORED_VALUE);
    constexpr std::size_t kWide = 40;  // > vertex_t::kInlineFanout (8)
    std::vector<sink_t> sinks(kWide);
    std::vector<subscription_t> subs;
    for (std::size_t i = 0; i < kWide; ++i)
        if (const auto s = g.subscribe(src, on_value, &sinks[i])) subs.push_back(*s);
    check(subs.size() == kWide, "every wide subscribe was admitted");
    const payload_t p = make_payload(std::byte{0x44});
    (void)g.write(src, p.view());
    std::size_t delivered = 0;
    for (const sink_t& s : sinks) delivered += s.hits.load();
    check(delivered == subs.size(), "every wide edge received exactly one delivery");

    // Clear every other edge, then publish again: the surviving half — and only it — receives.
    for (std::size_t i = 0; i < subs.size(); i += 2) (void)g.unsubscribe(subs[i]);
    (void)g.write(src, p.view());
    std::size_t second = 0;
    for (std::size_t i = 0; i < sinks.size(); ++i)
        if (sinks[i].hits.load() == 2) ++second;
    check(second == subs.size() - (subs.size() + 1) / 2,
          "exactly the surviving half received the second publish");
}

/**
 * @brief The REMOTE half of the churn arm: the SHARED subscription cold half (#1442) read by
 *        publishers while a mutator creates, publishes, retires and frees the records.
 *
 * Since #1442 a published entry no longer OWNS a copy of the wire/gate half — it holds an
 * intrusive refcounted share of the admitting slot's record, and the last holder out frees it.
 * The holders are the slot (dropped by the clear) and every published array naming it (dropped
 * when `scan_retired_edges` reaches that array, which is a DIFFERENT thread from the one that
 * published it, and may be two threads at once). Every arm above churns LOCAL callback edges,
 * whose cold half is null — so none of that machinery is exercised by them at all, and TSan
 * would stay green whatever the refcount did.
 *
 * Here the churn admits and clears a WIRE subscriber every round, while the publishers read
 * that record's `link` and `caller` strings out of whichever array they pinned. The delivered
 * bytes are CHECKED, not counted: a record freed one holder too early most likely delivers the
 * right number of times with the wrong bytes, and a counter cannot see that.
 *
 * ### The overlap latch (#1501)
 *
 * "A remote delivery landed while the wire edge was live" is a claim about an OVERLAP, and
 * the `started` latch #1378 added proves only a STARTUP: that each publisher finished one
 * batch *before* the churn began — i.e. before any wire edge existed. Nothing then made the
 * two windows meet. The churn thread has no blocking point, so the live window per round is
 * just the gap between `subscribe_wire` returning and the clearing field-write running ON THE
 * SAME THREAD — sub-microsecond, with nothing inviting a publisher in. On a loaded box the
 * publishers announce, get descheduled, the churn runs all 2000 rounds and sets `stop`, and
 * `remote_hits` is still zero: a scheduler-dependent assertion, which this repo rules a defect
 * rather than a test style (the calibrated-spin line, #1419).
 *
 * So the overlap is now ENFORCED rather than hoped for. For its first round only, the churn
 * holds the wire edge OPEN — it does not issue the clearing write — until a remote delivery
 * has actually landed on it, waiting on a `std::promise` the sink fulfils exactly once. The
 * wait is edge-triggered on that delivery, not a calibrated duration; its deadline exists so a
 * genuinely broken delivery path FAILS the check instead of hanging the suite. Every later
 * round runs the original tight admit/clear churn untouched, so the refcount race this arm
 * exists to exercise is not blunted — only the first round is widened, and only once.
 *
 * Raising the round count or widening every window would merely re-tune the bar; it would not
 * remove the scheduler dependency, which is why neither is done.
 */
void test_remote_cold_half_under_churn(std::size_t publishers) {
    std::printf("concurrent publish + REMOTE subscribe/unsubscribe churn (shared cold half):\n");
    graph_t g;
    const path_t src = *path_t::parse("/t/churn/remote");
    const auto v = g.register_vertex(src, role_t::STORED_VALUE);

    // 64-byte names: past the small-string buffer, so the record genuinely owns heap bytes a
    // premature free would hand back while a publisher is copying them.
    const std::string link(64, 'L');
    const std::string caller(64, 'C');
    std::atomic<std::size_t> remote_hits{0};
    std::atomic<std::size_t> remote_torn{0};
    // The overlap latch: fulfilled by the FIRST remote delivery of either kind. Torn counts
    // too — a torn first delivery must fail its own check below, not stall the churn to the
    // deadline and then fail two. `signalled` is read RELAXED on the steady-state path, so
    // after the one transition every later delivery pays a plain load and no RMW.
    std::promise<void> first_remote;
    std::future<void> first_remote_f = first_remote.get_future();
    std::atomic<bool> signalled{false};
    const tr::testing::remote_sink_guard_t sink(g, [&](const remote_delivery_t& d, const rope_t&) {
        const bool ok = d.link.size() == 64 && d.caller.size() == 64 &&
                        d.link.find_first_not_of('L') == std::string_view::npos &&
                        d.caller.find_first_not_of('C') == std::string_view::npos &&
                        !d.return_route.bytes().empty();
        (ok ? remote_hits : remote_torn).fetch_add(1, std::memory_order_relaxed);
        if (!signalled.load(std::memory_order_relaxed) &&
            !signalled.exchange(true, std::memory_order_acq_rel))
            first_remote.set_value();
    });

    sink_t standing;  // slot 0, so the churn's wire edge always reoccupies slot 1
    check(g.subscribe(src, on_value, &standing).has_value(), "the standing local edge is admitted");
    const auto clear_fp = path_t::parse("/t/churn/remote:subscribers[1]");
    check(clear_fp.has_value(), "the clear field-path parses");
    if (!clear_fp) return;

    const payload_t p = make_payload(std::byte{0x66});
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> started{0};
    std::atomic<std::size_t> rounds{0};
    std::atomic<bool> overlapped{false};  // the churn saw a delivery land on a LIVE wire edge

    std::vector<std::thread> ts;
    ts.reserve(publishers + 1);
    for (std::size_t i = 0; i < publishers; ++i) {
        ts.emplace_back([&]() {
            const auto batch = [&] {
                for (int k = 0; k < 64; ++k) (void)g.write(src, p.view());
            };
            batch();  // one unconditional batch before `stop` is consulted (#1378)
            started.fetch_add(1, std::memory_order_release);
            while (!stop.load(std::memory_order_relaxed)) batch();
        });
    }
    ts.emplace_back([&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (started.load(std::memory_order_acquire) < publishers &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        // The overlap deadline is armed once the publishers are up, so a slow start eats the
        // startup budget above rather than this one.
        const auto overlap_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (std::size_t round = 0; round < 2000 && !stop.load(std::memory_order_relaxed);
             ++round) {
            if (!g.subscribe_wire(v, make_value({0x04, 0x40, 0x00, 0x00}),
                                  make_value({0x06, 0x00, 0x00, 0x00}), link, view_t{}, caller))
                break;
            rounds.fetch_add(1, std::memory_order_relaxed);
            // The wire edge is LIVE from here until the clearing write below. Until one
            // delivery has actually landed on it, do not close that window — block on the
            // delivery itself instead of racing a publisher for a sub-microsecond gap. Once
            // latched this is a plain relaxed load and the churn is the original tight loop.
            if (!overlapped.load(std::memory_order_relaxed))
                overlapped.store(
                    first_remote_f.wait_until(overlap_deadline) == std::future_status::ready,
                    std::memory_order_relaxed);
            (void)g.write(v, clear_fp->field(), make_value({0x09, 0x00, 0x00, 0x00}));
        }
        stop.store(true, std::memory_order_relaxed);
    });
    for (auto& th : ts) th.join();

    check(rounds.load() > 0, "the remote churn ran (admissions happened)");
    // The overlap is enforced, not sampled: the churn held its first wire edge open until a
    // delivery landed on it. A false here means the delivery path never reached the sink
    // within the deadline while the edge was live — a real defect, not a lost scheduler race.
    check(overlapped.load(), "the churn held its wire edge live until a delivery landed on it");
    check(remote_hits.load() > 0, "the churned remote edge was delivered to while it was live");
    check(remote_torn.load() == 0,
          "every remote delivery carried its record's OWN link, caller and route bytes");
    check(standing.torn.load() == 0, "no local delivery reached a torn context");
}

/**
 * @brief Churn a graph to destruction: every parked array must be freed by the teardown flush.
 *
 * The contract `edge_block_t`'s destructor states — the graph outlives the threads that
 * published through it — is what makes the flush safe, and this is the arm the LSan leg reads:
 * a retire list that leaked its parked arrays reports here and nowhere else.
 */
void test_teardown_flushes_the_retire_list() {
    std::printf("graph teardown flushes the published + parked edge arrays:\n");
    {
        graph_t g;
        const path_t src = *path_t::parse("/t/flush/src");
        (void)g.register_vertex(src, role_t::STORED_VALUE);
        sink_t s;
        const payload_t p = make_payload(std::byte{0x55});
        // Each pair displaces two arrays; a publish between them keeps a reader's copy loop
        // interleaved with the retire pushes on this same thread.
        for (int i = 0; i < 256; ++i) {
            const auto sub = g.subscribe(src, on_value, &s);
            (void)g.write(src, p.view());
            if (sub) (void)g.unsubscribe(*sub);
        }
        check(s.hits.load() == 256, "every write inside the churn delivered once");
    }
    check(true, "the graph destructed without a leak (LSan is the real assertion)");
}

}  // namespace

int main() {
    std::printf("#635 published edge array + edge pin\n\n");
    test_unsubscribe_stops_delivery_at_once();
    test_reentrant_callback_finds_an_empty_pin();
    test_wide_fanout_under_churn();
    test_concurrent_publish_and_churn(4, "4 publishers — every thread owns a pin cell");
    // Deliberately past tr::graph::kEdgePinSlots so the threads that cannot claim a cell take
    // the stripe-mutex fallback. Correctness must not depend on the constant.
    test_concurrent_publish_and_churn(tr::graph::kEdgePinSlots + 4,
                                      "more publishers than kEdgePinSlots — mutex fallback");
    test_remote_cold_half_under_churn(4);
    test_teardown_flushes_the_retire_list();
    return tr::testing::summary("edge_publish");
}
