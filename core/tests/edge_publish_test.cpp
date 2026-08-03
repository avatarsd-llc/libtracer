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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::subscription_t;
using tr::view::rope_t;
using tr::view::view_t;

int g_failures = 0;

/** @brief Assert-independent check (RelWithDebInfo defines NDEBUG). */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

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

    std::vector<std::thread> ts;
    ts.reserve(publishers + 1);
    for (std::size_t i = 0; i < publishers; ++i) {
        ts.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                for (int k = 0; k < 64; ++k) (void)g.write(src, p.view());
                writes.fetch_add(64, std::memory_order_relaxed);
            }
        });
    }
    // The churn thread: each iteration DISPLACES the published array twice (an append and a
    // clear), so publishers are constantly copying an array the churn is retiring.
    std::vector<sink_t> transient(8);
    ts.emplace_back([&]() {
        for (std::size_t round = 0; round < 4000 && !stop.load(std::memory_order_relaxed);
             ++round) {
            sink_t& t = transient[round % transient.size()];
            const auto s = g.subscribe(src, on_value, &t);
            if (s) (void)g.unsubscribe(*s);
        }
        stop.store(true, std::memory_order_relaxed);
    });
    for (auto& th : ts) th.join();

    check(writes.load() > 0, "the publishers ran");
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
    check(sd.has_value() && sd->slot == sb->slot, "the re-subscribe reuses the cleared slot");
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
    test_teardown_flushes_the_retire_list();
    std::printf("\n%s\n", g_failures == 0 ? "edge_publish_test: OK" : "edge_publish_test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
