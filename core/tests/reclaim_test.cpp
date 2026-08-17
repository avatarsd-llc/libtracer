/**
 * @file
 * @brief The ADR-0080 reclamation seam — WHEN a retired subscription's `{fn, ctx}` pair
 *        becomes safe to free (#894), asserted against the policy this build bound.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `fan_out` snapshots a vertex's edge set under an edge pin and dispatches OUTSIDE every
 * lock. The one leg of that snapshot the library owns no copy of is the subscriber's own
 * `{fn, callback_ctx}` pair — ADR-0041 §2's "the snapshot owns its copies" covers the target
 * key and the routes (refcount clones), never a `void*` the subscriber allocated. So a
 * snapshot taken before `unsubscribe()` still invokes the callback AFTER `unsubscribe()`
 * returned success, and before ADR-0080 nothing bounded the wait.
 *
 * ADR-0080 answers with a build-time-closed policy rather than a runtime contract, and this
 * file pins what each shipped policy promises. Six properties:
 *
 *   (a) the ORDINARY unsubscribe — from outside any delivery — is quiescent ON RETURN: the
 *       release hook has already run when `unsubscribe()` hands control back, so a caller
 *       may free its context there. This is the case that dominates, and under
 *       `reclaim_strict` it is the only case;
 *   (b) the RE-ENTRANT unsubscribe, pinned DETERMINISTICALLY rather than raced — the first
 *       of two edges on one vertex unsubscribes the second from inside the very fan-out
 *       that is walking the snapshot. The victim IS still invoked after `unsubscribe()`
 *       returned; its ctx is NOT released at that moment; and it IS released by the time
 *       the `write()` that started the fan-out returns. No `collect()`, no poll, no wait;
 *   (c) a hook is owed only for a call that RETIRED something — a default-constructed
 *       handle and a second unsubscribe of the same slot both answer `NOT_FOUND` and run
 *       nothing, because a caller may share one hook across handles;
 *   (d) the one-argument overload is unchanged in what it retires and simply carries no
 *       signal; a path→path edge has no in-process callback to release at all;
 *   (e) NESTING — a re-entrant unsubscribe issued two dispatch levels deep is released when
 *       the OUTERMOST delivery unwinds, not the inner one, because an inner return leaves
 *       the outer fan-out still walking its own snapshot;
 *   (f) the bounded park REFUSES rather than corrupts: past `kDeferredReleaseSlots`
 *       re-entrant retirements in one dispatch stack the pair is dropped and its hook never
 *       runs — a deliberate leak in preference to a use-after-free — and the drop counter
 *       says so.
 *
 * Every assertion here is policy-INDEPENDENT in shape; (b), (e) and (f) are guarded on
 * `reclaim_policy_t::kReentrantUnsubscribe`, since `reclaim_strict` forbids the operation
 * they exercise and debug-asserts on it.
 */

#include "libtracer/reclaim.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reclaim_policy_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subscription_t;

using tr::testing::check;

/** @brief A frame the vertex accepts as a value — one four-byte VALUE TLV. */
[[nodiscard]] std::vector<std::byte> value_frame() {
    std::vector<std::byte> body(4, std::byte{0x42});
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, tr::wire::type_t::VALUE, tr::wire::opt_t{}, body);
    return frame;
}

/** @brief Write @p frame to @p p — the verb that drives a fan-out. */
[[nodiscard]] bool write_once(graph_t& g, const path_t& p, const std::vector<std::byte>& frame) {
    return g.write(p, tr::view::view_t::over(tr::view::borrow_const(frame))).has_value();
}

/**
 * @brief A subscription context whose RELEASE is observable without dereferencing it.
 *
 * Every field is read after the ctx is nominally dead, so nothing here may depend on the
 * object still being valid — which is exactly why the counters are separate atomics rather
 * than a "freed" flag the sink would have to trust.
 */
struct release_probe_t {
    std::atomic<int> hits{0};                   /**< @brief Deliveries this ctx received. */
    std::atomic<int> released{0};               /**< @brief Release-hook runs (must end at 1). */
    std::atomic<bool> hit_after_release{false}; /**< @brief The failure the seam exists to stop. */
};

/** @brief The subscriber: counts, and reports whether it ran after its own release. */
void probe_sink(void* ctx, const tr::view::rope_t&) {
    auto* p = static_cast<release_probe_t*>(ctx);
    p->hits.fetch_add(1, std::memory_order_relaxed);
    if (p->released.load(std::memory_order_acquire) != 0)
        p->hit_after_release.store(true, std::memory_order_release);
}

/** @brief The release hook — the ONLY place a ctx is declared dead. */
void probe_release(void* ctx) {
    static_cast<release_probe_t*>(ctx)->released.fetch_add(1, std::memory_order_release);
}

/** @brief Bumped by @ref stray_release — must stay 0: a failed unsubscribe owes no signal. */
std::atomic<int> g_stray_releases{0};

/** @brief A release hook handed to an unsubscribe that CANNOT succeed. */
void stray_release(void*) { g_stray_releases.fetch_add(1, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// (a) The ordinary unsubscribe: the grace point IS the return.

void test_unsubscribe_outside_dispatch_is_quiescent_on_return() {
    std::printf("#894 (a): an unsubscribe from outside a delivery releases BEFORE it returns:\n");
    graph_t g;
    const path_t src = *path_t::parse("/t/reclaim/plain");
    (void)g.register_vertex(src, role_t::STORED_VALUE);

    release_probe_t probe;
    const auto s = g.subscribe(src, probe_sink, &probe);
    check(s.has_value(), "the subscription is admitted");
    if (!s) return;

    const std::vector<std::byte> frame = value_frame();
    check(write_once(g, src, frame), "a write fans out to it");
    check(probe.hits.load() == 1, "and it received the delivery");

    check(g.unsubscribe(*s, &probe_release).has_value(), "unsubscribe succeeds");
    // THE guarantee, for the case that dominates: no poll, no wait, no second verb.
    check(probe.released.load() == 1,
          "and the release hook ALREADY RAN — the caller may free its ctx on this return");

    check(write_once(g, src, frame), "a later write still succeeds");
    check(probe.hits.load() == 1, "but the retired edge receives nothing");
    check(!probe.hit_after_release.load(), "so no delivery ever landed on a released context");
}

// ---------------------------------------------------------------------------
// (b) The re-entrant unsubscribe — the #894 hazard itself, made a certainty.
//
// Two edges on one vertex; the first one's callback unsubscribes the second. The snapshot the
// fan-out is walking was taken before that call, so the second callback runs FROM IT — one
// thread, no timing window, and the invocation is provably after `unsubscribe` returned,
// because the same call stack made it.

/** @brief What the first edge's callback needs to unsubscribe the second mid-fan-out. */
struct unsubscriber_t {
    graph_t* g{nullptr};                     /**< @brief The graph to unsubscribe on. */
    subscription_t victim{};                 /**< @brief The subscription cleared mid-dispatch. */
    release_probe_t* victim_ctx{nullptr};    /**< @brief Whose ctx must survive the fan-out. */
    std::atomic<bool> done{false};           /**< @brief The unsubscribe RETURNED. */
    std::atomic<bool> ok{false};             /**< @brief …and it reported success. */
    std::atomic<int> released_at_return{-1}; /**< @brief The hook count AT that return. */
};

/** @brief Unsubscribe the sibling edge from inside a delivery the same fan-out is walking. */
void unsubscribing_sink(void* ctx, const tr::view::rope_t&) {
    auto* u = static_cast<unsubscriber_t*>(ctx);
    if (u->done.load(std::memory_order_acquire)) return;  // only the first delivery unsubscribes
    const bool ok = u->g->unsubscribe(u->victim, &probe_release).has_value();
    // Sampled INSIDE the dispatch: this is the observation that separates `reclaim_local`
    // from a policy that frees on return.
    u->released_at_return.store(u->victim_ctx->released.load(std::memory_order_acquire),
                                std::memory_order_release);
    u->ok.store(ok, std::memory_order_release);
    u->done.store(true, std::memory_order_release);
}

void test_reentrant_unsubscribe_releases_at_dispatch_exit() {
    std::printf("#894 (b): a re-entrant unsubscribe releases when the dispatch stack unwinds:\n");
    graph_t g;
    const path_t src = *path_t::parse("/t/reclaim/reentrant");
    (void)g.register_vertex(src, role_t::STORED_VALUE);

    release_probe_t victim;
    unsubscriber_t driver;
    driver.g = &g;
    driver.victim_ctx = &victim;

    // Slot order is admission order and fan-out walks the snapshot in it, so the
    // unsubscriber is admitted FIRST and the victim second.
    const auto s0 = g.subscribe(src, unsubscribing_sink, &driver);
    check(s0.has_value(), "the unsubscribing edge is admitted (slot 0)");
    const auto s1 = g.subscribe(src, probe_sink, &victim);
    check(s1.has_value(), "the victim edge is admitted (slot 1)");
    if (!s0 || !s1) return;
    driver.victim = *s1;

    const std::vector<std::byte> frame = value_frame();
    check(write_once(g, src, frame), "one write fans out to both edges");

    check(driver.done.load() && driver.ok.load(), "the mid-dispatch unsubscribe RETURNED SUCCESS");
    // THE #894 hazard, observed rather than argued: the snapshot invoked the cleared edge
    // after `unsubscribe` returned.
    check(victim.hits.load() == 1,
          "the unsubscribed callback was STILL INVOKED after unsubscribe() returned");
    // …and nothing had freed its ctx to do that under. This is the assertion that fails on a
    // policy which frees at the return.
    check(driver.released_at_return.load() == 0,
          "its ctx was NOT released at that return — a re-entrant unsubscribe DEFERS");
    // The grace point: the outermost dispatch's exit, i.e. before `write()` handed back.
    check(victim.released.load() == 1,
          "and the hook RAN by the time write() returned — the dispatch stack unwound to 0");
    check(!victim.hit_after_release.load(),
          "so no delivery ever landed on a released context (the #894 use-after-free)");

    // Parking is a lifetime mechanism, not a stay of execution.
    check(write_once(g, src, frame), "a second write fans out");
    check(victim.hits.load() == 1,
          "the unsubscribed edge receives nothing from a snapshot taken AFTER the clear");
    check(victim.released.load() == 1, "and the hook is never run a second time");
}

// ---------------------------------------------------------------------------
// (e) Nesting: the grace point is the OUTERMOST exit, not the innermost.
//
// `/t/reclaim/outer` fans out to a callback that publishes to `/t/reclaim/inner`, whose own
// callback does the re-entrant unsubscribe. When the inner fan-out exits, the OUTER one is
// still walking its snapshot, so releasing there would be exactly the bug.

/** @brief The nested driver: publishes one level down, then checks where the release landed. */
struct nested_t {
    graph_t* g{nullptr};                    /**< @brief The graph both vertices live in. */
    path_t inner{};                         /**< @brief The vertex the outer sink publishes to. */
    std::vector<std::byte> frame{};         /**< @brief What it publishes. */
    subscription_t victim{};                /**< @brief Cleared from the INNER dispatch. */
    release_probe_t* victim_ctx{nullptr};   /**< @brief Whose release is being located. */
    std::atomic<int> released_at_inner{-1}; /**< @brief Hook count when the inner fan-out ended. */
    std::atomic<bool> fired{false};         /**< @brief The inner publish happened. */
};

/** @brief The INNER sink: unsubscribes the victim from two dispatch levels down. */
void nested_inner_sink(void* ctx, const tr::view::rope_t&) {
    auto* n = static_cast<nested_t*>(ctx);
    (void)n->g->unsubscribe(n->victim, &probe_release);
}

/** @brief The OUTER sink: drives one nested publish, then samples the hook count. */
void nested_outer_sink(void* ctx, const tr::view::rope_t&) {
    auto* n = static_cast<nested_t*>(ctx);
    if (n->fired.exchange(true)) return;
    (void)n->g->write(n->inner, tr::view::view_t::over(tr::view::borrow_const(n->frame)));
    // The inner fan-out has fully returned here — and we are STILL inside the outer one.
    n->released_at_inner.store(n->victim_ctx->released.load(std::memory_order_acquire),
                               std::memory_order_release);
}

void test_release_waits_for_the_outermost_dispatch() {
    std::printf("#894 (e): the grace point is the OUTERMOST dispatch exit, not an inner one:\n");
    graph_t g;
    const path_t outer = *path_t::parse("/t/reclaim/outer");
    const path_t inner = *path_t::parse("/t/reclaim/inner");
    (void)g.register_vertex(outer, role_t::STORED_VALUE);
    (void)g.register_vertex(inner, role_t::STORED_VALUE);

    release_probe_t victim;
    nested_t n;
    n.g = &g;
    n.inner = inner;
    n.frame = value_frame();
    n.victim_ctx = &victim;

    const auto s_outer = g.subscribe(outer, nested_outer_sink, &n);
    const auto s_inner = g.subscribe(inner, nested_inner_sink, &n);
    // The victim rides the OUTER vertex, so the outer fan-out's snapshot names it while the
    // inner dispatch retires it — the aliasing that makes an inner-exit release unsafe.
    const auto s_victim = g.subscribe(outer, probe_sink, &victim);
    check(s_outer.has_value() && s_inner.has_value() && s_victim.has_value(),
          "all three edges are admitted");
    if (!s_outer || !s_inner || !s_victim) return;
    n.victim = *s_victim;

    check(write_once(g, outer, n.frame), "one write drives a two-level dispatch stack");
    check(n.fired.load(), "the outer sink ran and published one level down");
    check(n.released_at_inner.load() == 0,
          "the INNER fan-out's exit released NOTHING — the outer is still walking its snapshot");
    check(victim.released.load() == 1, "and the OUTERMOST exit released it, exactly once");
    check(!victim.hit_after_release.load(), "with no delivery onto a released context");
}

// ---------------------------------------------------------------------------
// (f) The bounded park refuses rather than corrupts.

/** @brief What the overflow driver needs: many victims retired from one dispatch. */
struct flood_t {
    graph_t* g{nullptr};                   /**< @brief The graph to unsubscribe on. */
    std::vector<subscription_t> victims{}; /**< @brief All retired from ONE delivery. */
    std::atomic<bool> fired{false};        /**< @brief Only the first delivery floods. */
};

/** @brief Retire every victim from inside one delivery — more than the park can hold. */
void flooding_sink(void* ctx, const tr::view::rope_t&) {
    auto* f = static_cast<flood_t*>(ctx);
    if (f->fired.exchange(true)) return;
    for (const subscription_t& v : f->victims) (void)f->g->unsubscribe(v, &probe_release);
}

void test_park_overflow_drops_rather_than_frees() {
    std::printf("#894 (f): an exhausted park DROPS the pair — a leak, never a use-after-free:\n");
    constexpr std::size_t kOver = tr::graph::kDeferredReleaseSlots + 4;
    graph_t g;
    const path_t src = *path_t::parse("/t/reclaim/flood");
    (void)g.register_vertex(src, role_t::STORED_VALUE);

    flood_t driver;
    driver.g = &g;
    const auto s0 = g.subscribe(src, flooding_sink, &driver);
    check(s0.has_value(), "the flooding edge is admitted first");
    if (!s0) return;

    // `deque`, not `vector`: the probes are subscribed BY ADDRESS, so a reallocation partway
    // through would hand the graph pointers into freed storage.
    std::deque<release_probe_t> probes(kOver);
    for (release_probe_t& p : probes) {
        const auto s = g.subscribe(src, probe_sink, &p);
        // Quiet: kOver PASS lines say nothing the verdict below does not.
        tr::testing::check_quiet(s.has_value(), "each victim edge is admitted");
        if (!s) return;
        driver.victims.push_back(*s);
    }
    check(driver.victims.size() == kOver, "kDeferredReleaseSlots + 4 victim edges are admitted");

    const std::uint64_t drops_before = graph_t::deferred_release_drops();
    check(write_once(g, src, value_frame()), "one write retires all of them from one delivery");

    int released = 0;
    bool any_late_hit = false;
    for (const release_probe_t& p : probes) {
        released += p.released.load();
        any_late_hit = any_late_hit || p.hit_after_release.load();
    }
    check(released == static_cast<int>(tr::graph::kDeferredReleaseSlots),
          "exactly kDeferredReleaseSlots hooks ran — the park held what it promised");
    check(graph_t::deferred_release_drops() - drops_before ==
              kOver - tr::graph::kDeferredReleaseSlots,
          "and every pair beyond the bound is COUNTED as a drop, not silently lost");
    check(!any_late_hit, "no delivery landed on a released context — the refusal is the safe side");
}

// ---------------------------------------------------------------------------
// (c)+(d) Who is owed a signal, and who is not.

void test_only_a_retiring_unsubscribe_signals() {
    std::printf("#894 (c,d): a hook runs only for a call that actually retired an edge:\n");
    graph_t g;
    const path_t src = *path_t::parse("/t/reclaim/who/src");
    const path_t dst = *path_t::parse("/t/reclaim/who/dst");
    (void)g.register_vertex(src, role_t::STORED_VALUE);
    (void)g.register_vertex(dst, role_t::STORED_VALUE);

    // A default-constructed handle names nothing: NOT_FOUND, and nothing is owed.
    const auto none = g.unsubscribe(subscription_t{}, &stray_release);
    check(!none && none.error() == status_t::NOT_FOUND, "a default handle unsubscribes NOT_FOUND");
    check(g_stray_releases.load() == 0, "and never runs the hook it was given");

    // A double unsubscribe retires one edge, so it signals ONCE.
    release_probe_t twice;
    const auto s = g.subscribe(src, probe_sink, &twice);
    check(s.has_value(), "a callback subscription is admitted");
    if (!s) return;
    check(g.unsubscribe(*s, &probe_release).has_value(), "the first unsubscribe succeeds");
    check(twice.released.load() == 1, "and releases the ctx");
    const auto again = g.unsubscribe(*s, &stray_release);
    check(!again && again.error() == status_t::NOT_FOUND, "the second answers NOT_FOUND");
    check(twice.released.load() == 1, "so the ctx is released EXACTLY once");
    check(g_stray_releases.load() == 0, "and the second call's hook never ran");

    // The one-argument overload is unchanged: it retires, and carries no signal.
    release_probe_t self_managed;
    const auto s2 = g.subscribe(src, probe_sink, &self_managed);
    check(s2.has_value(), "a second callback subscription is admitted");
    if (!s2) return;
    check(g.unsubscribe(*s2).has_value(), "the one-argument unsubscribe succeeds");
    check(self_managed.released.load() == 0, "with no hook, the library frees nothing itself");
    check(write_once(g, src, value_frame()), "a write after both unsubscribes succeeds");
    check(self_managed.hits.load() == 0, "and neither retired edge receives it");

    // A path->path edge carries no in-process callback: there is nothing to release, ever.
    check(g.subscribe(src, dst).has_value(), "a path->path subscription is admitted");
    check(write_once(g, src, value_frame()), "and it delivers without touching the seam");
}

}  // namespace

int main() {
    std::printf("reclamation policy bound by this build: %.*s\n",
                static_cast<int>(reclaim_policy_t::kName.size()), reclaim_policy_t::kName.data());
    test_unsubscribe_outside_dispatch_is_quiescent_on_return();
    test_only_a_retiring_unsubscribe_signals();
    if constexpr (reclaim_policy_t::kReentrantUnsubscribe) {
        test_reentrant_unsubscribe_releases_at_dispatch_exit();
        test_release_waits_for_the_outermost_dispatch();
        test_park_overflow_drops_rather_than_frees();
    } else {
        // `reclaim_strict` forbids the operation the three cases above exercise and
        // debug-asserts on it, so running them would be asserting that an abort happens.
        std::printf("(b,e,f) skipped: this build forbids re-entrant unsubscribe\n");
    }
    return tr::testing::summary("reclaim");
}
