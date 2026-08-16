/**
 * @file
 * @brief #1049 — `graph_t`'s five configuration seams under a setter storm: no reader is
 *        ever handed a destroyed target, a torn pair, or a half-mutated container.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The L4 analogue of `fwd_sink_race_test` (#914), and a strictly worse defect than the one
 * that test pins. The router's sinks were a torn `{fn, ctx}` PAIR; `graph_t`'s three
 * callback seams were `std::function`s, and assigning a `std::function` DESTROYS the old
 * target — freeing its captured state while a reader may be inside the call. That is a
 * use-after-free, not a torn read, and it reached three places at once: the remote-delivery
 * sink on the write hot path, the subject resolver the ACL gate consults on every gated read
 * and write, and the subscription observer. The router's `sink_slot_t` does not transfer to a
 * `std::function` (it statically requires a pointer-sized function pointer), so the fix was
 * to NARROW the three to the ADR-0047 `{fn, ctx}` shape and publish them through that slot.
 *
 * Two further members were declared under the same retired "configure before frames flow"
 * comment and are covered here too. Neither is a callback, so the slot cannot hold them:
 * the creatable-child-type catalog is a `std::map` a public verb inserts into while the
 * in-band creation path — driven by a PEER's bytes — walks it, and the node identity record
 * is a buffer that install and clear both free while `read_identity` tests its emptiness and
 * then memcpys it. Both are control-plane cold, so both took a lock.
 *
 * @par The detectors
 * For the three callback seams the detector is functional and needs no sanitizer: a
 * self-identifying context. Seam A is only ever installed with probe A, seam B only with
 * probe B, and the installed function checks the tag of the probe it was handed — so a torn
 * publish is a tag mismatch, and a call into a destroyed target is (at minimum) a tag
 * mismatch as well. For the identity record the storm's own outcome is the detector: every
 * answer must be one of the two LEGAL ones, the 60-byte record or absent. The catalog's
 * detector is the weakest of the five and deliberately so — see @ref child_catalog_flip_race.
 *
 * @par Why every flipper is a CLOSED LOOP, and why that is not a detail
 * A flipper that publishes back-to-back — `set(A); set(B); set(A); …` — looks like the
 * strongest possible storm and is in fact the WEAKEST. `sink_slot_t` reads as EMPTY for the
 * duration of a publish, by design, so what a reader actually gets is decided by the ratio
 * between the cost of one `set` and the cost of the loop branch between two of them. That
 * ratio is a property of the MICROARCHITECTURE, not of the test: on x86-64's TSO the release
 * fence inside `set` is free, so a reader still gets through; on aarch64 the same fence is a
 * real `dmb ish` and the release store is an `stlr`, the publish window widens by an order of
 * magnitude, and the settled window stays a single branch. This suite was first written that
 * way and CI proved the point — the aarch64 leg reported **0 of 5000** subscribes reaching an
 * observer. Every safety assertion "passed", because nothing had happened for them to be
 * about. The torn-pair detector was not flaky there; it was VACUOUS, and only the vacuity
 * guard noticed.
 *
 * So each flipper here publishes and then BLOCKS until the storm is observed to have
 * dispatched through what it just published, before publishing again. Three consequences,
 * and the first is the one that matters most:
 *
 *  - The race gets STRONGER, not weaker. Every publish now lands on a reader that is actively
 *    dispatching rather than on one that has been shut out, which is the maximum useful
 *    collision density — one publish per dispatch.
 *  - Non-vacuity becomes DETERMINISTIC rather than lucky. While the flipper waits, the slot is
 *    settled and stays settled, so a running reader must get through; "the seam was reached
 *    during the storm" is then true by construction on any microarchitecture, not by timing.
 *  - It still fails LOUDLY if the interleave genuinely cannot be produced. If no dispatch is
 *    ever observed the flipper waits out the storm, the counters stay at zero, and the guards
 *    fire. Nothing silently passes a check it did not earn.
 *
 * Two further guards close the remaining holes. An ARMING handshake makes the reader wait for
 * the flipper's first publish, so a descheduled flipper can no longer let the whole storm run
 * against an empty slot. And each storm EXTENDS itself — past its nominal length, up to a
 * per-scenario ceiling — until at least one handshake has completed, failing against that
 * ceiling rather than passing on a host where the two threads never interleave at all.
 *
 * @par What "the race was exercised" is allowed to assert
 * Two quantities, split because only one of them is portable. PUBLISHES is the flipper's own
 * work, floored at ONE FULL BURST — the amount a flipper is guaranteed to issue once it runs
 * at all (@ref kPublishFloor). HANDSHAKES (completed publish→dispatch cycles) cost a context
 * switch each, so how many are achievable is a measurement of the SCHEDULER — pinned to one
 * core the storms manage single digits, on 31 cores tens of thousands — and flooring that
 * reproduces exactly the environment-dependent verdict this file exists to remove. It is
 * therefore asserted only to be NON-ZERO, and printed. Both mistakes were made here first
 * and caught by pinning the suite to one core, which is now part of how this file is
 * verified; the fixes were to make @ref await_progress yield, and to stop asserting numbers
 * the scheduler owns.
 *
 * Every scenario also carries a SETTLED positive control: after the storm, with the seam
 * installed and no flipper running, every single operation must reach it, exactly.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief A seam context that knows which seam it belongs to. */
struct probe_t {
    char tag;                  /**< @brief 'A' or 'B' — whose context this is. */
    std::atomic<long> hits{0}; /**< @brief Dispatches that reached the right seam. */
};

/** @brief Contexts handed to the wrong seam, or to a destroyed one — must stay zero. */
std::atomic<long> g_torn{0};

/**
 * @brief Every dispatch that reached ANY seam, right context or wrong.
 *
 * What a closed-loop flipper waits on: it is the observation "the storm got through the pair
 * I just published". Bumped before the tag is judged, so a TORN dispatch also counts as
 * progress — a flipper that only advanced on clean dispatches would stall exactly when the
 * defect it hunts is firing.
 */
std::atomic<long> g_dispatches{0};

/**
 * @brief Score one dispatch: the seam was handed a context, and it must be ITS context.
 * @param ctx      The `void*` the graph passed back.
 * @param expected The tag of the probe this seam is only ever installed with.
 */
void score(void* ctx, char expected) {
    g_dispatches.fetch_add(1, std::memory_order_relaxed);
    auto* const p = static_cast<probe_t*>(ctx);
    if (p == nullptr || p->tag != expected) {
        g_torn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    p->hits.fetch_add(1, std::memory_order_relaxed);
}

void sink_a(void* ctx, const tr::graph::remote_delivery_t&, const rope_t&) { score(ctx, 'A'); }
void sink_b(void* ctx, const tr::graph::remote_delivery_t&, const rope_t&) { score(ctx, 'B'); }

std::expected<tr::graph::subject_token_t, tr::wire::err_t> resolver_a(void* ctx,
                                                                      std::string_view caller) {
    score(ctx, 'A');
    return tr::graph::subject_token_t(caller.size(), std::byte{'a'});
}
std::expected<tr::graph::subject_token_t, tr::wire::err_t> resolver_b(void* ctx,
                                                                      std::string_view caller) {
    score(ctx, 'B');
    return tr::graph::subject_token_t(caller.size(), std::byte{'a'});
}

void observer_a(void* ctx, const tr::graph::sub_event_t&) { score(ctx, 'A'); }
void observer_b(void* ctx, const tr::graph::sub_event_t&) { score(ctx, 'B'); }

/**
 * @brief Block until @p counter moves off @p before, or until the storm ends.
 *
 * The flipper's half of the closed loop. Never blocks the storm — only the flipper ever
 * waits — so there is no way to deadlock the pair.
 *
 * @par Why it YIELDS, and why a pure spin was wrong
 * What this loop waits for can only be produced by another thread, so on a machine with fewer
 * runnable cores than the test has threads a pure spin is a livelock in all but name: the
 * flipper burns its whole scheduler timeslice waiting for a reader that cannot run until the
 * flipper stops. That is not hypothetical — the first version of this loop spun, and pinned to
 * ONE core the sink and resolver storms completed 63 and 84 closed-loop cycles against a floor
 * of a thousand, extended themselves to their ceilings looking for more, and failed. A floor
 * that holds on a 31-core host and not on one core is the same class of environment-dependent
 * verdict as the aarch64 failure this file's closed loop exists to remove, so it gets the same
 * treatment — fix the mechanism — rather than a lowered floor. The short spin first keeps the
 * common case, a dispatch already in flight on another core, free of syscalls.
 *
 * @retval false The storm ended first; the flipper stops without another publish.
 */
[[nodiscard]] bool await_progress(const std::atomic<long>& counter, long before,
                                  const std::atomic<bool>& stop) {
    for (int spins = 0; counter.load(std::memory_order_relaxed) == before; ++spins) {
        if (stop.load(std::memory_order_relaxed)) return false;
        if (spins > 64) std::this_thread::yield();
    }
    return true;
}

/**
 * @brief Should the storm keep going past its nominal length?
 *
 * True while the flipper has not yet completed @p floor closed-loop cycles and @p ceiling has
 * not been reached. On a healthy host this is false the first time it is asked, because the
 * closed loop couples the flipper's rate to the storm's own: one cycle per dispatch. The
 * ceiling is per-scenario rather than a fixed multiple because the subscribe storm is
 * QUADRATIC in its length — a `:subscribers[]` append republishes the slot vector — so
 * "ten times longer" is a hundred times the work there.
 *
 * A host that cannot interleave two threads within the ceiling will not do it in ten
 * ceilings, and the scenario must FAIL there rather than report a verdict about a race that
 * never happened.
 *
 * @param done     Operations issued so far.
 * @param nominal  The storm's nominal length.
 * @param ceiling  The most operations to issue before giving up and failing.
 * @param cycles   The flipper's completed closed-loop cycles.
 * @param floor    How many of those the scenario needs before its verdict means anything.
 */
[[nodiscard]] bool keep_storming(int done, int nominal, int ceiling,
                                 const std::atomic<long>& cycles, long floor) {
    if (done < nominal) return true;
    if (done >= ceiling) return false;
    return cycles.load(std::memory_order_relaxed) < floor;
}

/**
 * @brief Publishes a flipper must have issued inside the storm: ONE FULL BURST.
 *
 * Deliberately not a throughput claim, and this number was walked down twice before it was
 * honest. A gated flipper issues `burst x handshakes` publishes, and handshakes cost a
 * context switch each, so ANY floor above a single burst is a measurement of the scheduler
 * wearing a different hat: at a floor of 1000 the identity storm passed on 31 cores with
 * 7.3 million publishes and failed on one core with 195. What a single burst IS guaranteed
 * to give, once the flipper runs at all, is a burst — so that is what is asserted, and it
 * sits below the smallest burst any scenario here issues. The pressure actually achieved is
 * PRINTED, per scenario, rather than gated: that is the number a human reads when a host
 * looks degenerate, and it is not a number a test can portably demand.
 */
constexpr long kPublishFloor = 100;

/** @brief `PATH{ NAME segs… }` — the canonical key payload, via the production emitters. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief `SUBSCRIBER{ PATH @p marker }` — one wire subscription record. */
std::vector<std::byte> b_subscriber(std::string_view marker) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, b_path({marker}));
    return out;
}

/** @brief `SPEC{ NAME "type" @p type, NAME "name" @p name }` — one in-band creation. */
view_t b_spec(std::string_view type, std::string_view name) {
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return make_value(out);
}

/** @brief Bind one wire subscriber at @p v arriving over @p link. */
bool wire_sub(graph_t& g, vertex_handle_t v, std::string_view link, std::string_view marker) {
    return g
        .subscribe_wire(v, make_value(b_subscriber(marker)), make_value(b_path({link})),
                        std::string(link))
        .has_value();
}

constexpr int kWrites = 120000;

/** @brief The settled positive-control batch each scenario runs after its storm. */
constexpr int kSettled = 100;

/**
 * @brief One closed-loop publish for a callback seam: install, then wait to be dispatched to.
 *
 * The progress sample is taken AFTER @p install returns, so the dispatch that satisfies it
 * provably began at or after the new pair was published — the loop cannot be satisfied by a
 * call that was already in flight against the old one.
 *
 * @retval false The storm ended while waiting; the flipper stops.
 */
template <typename Install>
[[nodiscard]] bool publish_and_settle(Install install, std::atomic<long>& cycles,
                                      const std::atomic<bool>& stop) {
    install();
    const long before = g_dispatches.load(std::memory_order_relaxed);
    if (!await_progress(g_dispatches, before, stop)) return false;
    cycles.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/**
 * @brief Back-to-back publishes per closed-loop cycle — the COLLISION half of the flipper.
 *
 * Determinism and collision density are not the same requirement, and satisfying only the
 * first is a real regression: a flipper that publishes exactly once per dispatch is
 * beautifully deterministic and much too polite to catch anything. Measured against the
 * parent commit, publishing one-per-dispatch dropped the subscription-observer scenario from
 * reddening every run to reddening NONE — the observer's window (a reader inside the call
 * whose target is being destroyed) is short next to a `:subscribers[]` append, so it needs
 * many assignments per operation, not one. So each cycle bursts first and settles second.
 */
constexpr int kBurst = 256;

/**
 * @brief One flipper cycle: @ref kBurst dense A/B publishes, then one publish that WAITS.
 *
 * The burst supplies the pressure; the trailing @ref publish_and_settle supplies the
 * guarantee. Because every cycle ends in a settle, dispatches >= cycles on any
 * microarchitecture — which is what makes the non-vacuity verdict independent of timing —
 * while the burst keeps the assignment density that actually catches the defect.
 *
 * @retval false The storm ended; the flipper stops.
 */
template <typename PublishA, typename PublishB>
[[nodiscard]] bool burst_then_settle(PublishA publish_a, PublishB publish_b,
                                     std::atomic<long>& cycles, std::atomic<long>& publishes,
                                     const std::atomic<bool>& stop) {
    int k = 0;
    for (; k < kBurst && !stop.load(std::memory_order_relaxed); ++k) {
        publish_a();
        publish_b();
    }
    publishes.fetch_add(2 * k + 1, std::memory_order_relaxed);
    return publish_and_settle(publish_a, cycles, stop);
}

/**
 * @brief The remote-delivery sink — the WRITE HOT PATH — flipped under a write storm.
 */
void remote_sink_flip_race() {
    std::printf("remote-delivery sink, flipped under a write storm:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);
    check(wire_sub(g, v, "cli", "c0"), "one REMOTE subscriber edge, so the sink is reached");

    probe_t a{'A'};
    probe_t b{'B'};
    const long torn_before = g_torn.load();
    std::atomic<bool> go{false};
    std::atomic<bool> armed{false};
    std::atomic<bool> stop{false};
    std::atomic<long> cycles{0};
    std::atomic<long> publishes{0};
    std::atomic<long> ok_writes{0};
    std::atomic<int> issued{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        // Arm with A installed, so the storm never runs against an empty slot for a reason
        // that has nothing to do with the seam under test.
        g.configure_remote_delivery_sink(&sink_a, &a);
        armed.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            if (!burst_then_settle([&] { g.configure_remote_delivery_sink(&sink_a, &a); },
                                   [&] { g.configure_remote_delivery_sink(&sink_b, &b); }, cycles,
                                   publishes, stop))
                break;
        }
    });
    std::thread writer([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!armed.load(std::memory_order_acquire)) {
        }
        int i = 0;
        for (; keep_storming(i, kWrites, 10 * kWrites, cycles, 1); ++i)
            if (g.write(v, make_value({0x41})).has_value()) ok_writes.fetch_add(1);
        issued.store(i, std::memory_order_relaxed);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    writer.join();

    const long hits = a.hits.load() + b.hits.load();
    std::printf("    (%ld of %d writes reached a sink; %ld publishes, %ld handshakes)\n", hits,
                issued.load(), publishes.load(), cycles.load());
    check(ok_writes.load() == issued.load(), "every write still succeeded under the storm");
    check(publishes.load() >= kPublishFloor, "the flipper published inside the storm");
    check(cycles.load() > 0, "flipper and storm interleaved (>=1 publish/dispatch handshake)");
    check(hits > 0, "the sink was reachable DURING the storm (the torn check is not vacuous)");
    check(g_torn.load() == torn_before, "no sink was handed the other sink's context");
    // Settled positive control: no flipper, so EVERY write must reach the sink, exactly.
    g.configure_remote_delivery_sink(&sink_a, &a);
    const long before = a.hits.load();
    for (int i = 0; i < kSettled; ++i) (void)g.write(v, make_value({0x41}));
    check(a.hits.load() - before == kSettled, "settled: every write reaches the sink, exactly");
}

/**
 * @brief The subject resolver — the ACL gate, on every gated read and write.
 */
void subject_resolver_flip_race() {
    std::printf("subject resolver, flipped under a gated read/write storm:\n");
    graph_t g;
    const vertex_handle_t v = g.register_vertex(path_t("/v"), role_t::STORED_VALUE);

    probe_t a{'A'};
    probe_t b{'B'};
    const long torn_before = g_torn.load();
    std::atomic<bool> go{false};
    std::atomic<bool> armed{false};
    std::atomic<bool> stop{false};
    std::atomic<long> cycles{0};
    std::atomic<long> publishes{0};
    std::atomic<long> ok_ops{0};
    std::atomic<int> issued{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        g.configure_subject_resolver(&resolver_a, &a);
        armed.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            if (!burst_then_settle([&] { g.configure_subject_resolver(&resolver_a, &a); },
                                   [&] { g.configure_subject_resolver(&resolver_b, &b); }, cycles,
                                   publishes, stop))
                break;
        }
    });
    std::thread ops([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!armed.load(std::memory_order_acquire)) {
        }
        // A NON-EMPTY caller is what reaches the resolver at all: the empty (local) context
        // is settled as trusted before it runs (#905). No ACE bears on /v, so every op is
        // allowed whichever resolver answered — the assertion is about WHOSE context each
        // was handed, not about the verdict.
        int i = 0;
        for (; keep_storming(i, kWrites, 10 * kWrites, cycles, 1); ++i) {
            if (g.write(v, make_value({0x42}), "peer-a").has_value()) ok_ops.fetch_add(1);
            if (g.read(v, "peer-a").has_value()) ok_ops.fetch_add(1);
        }
        issued.store(i, std::memory_order_relaxed);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    ops.join();

    const long hits = a.hits.load() + b.hits.load();
    std::printf("    (%ld of %d gated ops consulted a resolver; %ld publishes, %ld handshakes)\n",
                hits, 2 * issued.load(), publishes.load(), cycles.load());
    check(ok_ops.load() == 2 * issued.load(), "every gated op still succeeded under the storm");
    check(publishes.load() >= kPublishFloor, "the flipper published inside the storm");
    check(cycles.load() > 0, "flipper and storm interleaved (>=1 publish/dispatch handshake)");
    check(hits > 0, "the resolver was reachable DURING the storm (the torn check is not vacuous)");
    check(g_torn.load() == torn_before, "no resolver was handed the other resolver's context");
    // Settled positive control — no flipper, so every gated read must consult the resolver.
    g.configure_subject_resolver(&resolver_a, &a);
    const long before = a.hits.load();
    for (int i = 0; i < kSettled; ++i) (void)g.read(v, "peer-a");
    check(a.hits.load() - before == kSettled, "settled: every gated read consults the resolver");
}

constexpr int kSubs = 60000;

/**
 * @brief How many producer vertices the subscribe storm spreads its edges over.
 *
 * A `:subscribers[]` append republishes the vertex's whole slot array, so piling every edge
 * onto ONE vertex makes the storm quadratic and caps its length at a few thousand
 * operations — far too few for this seam. Its detection window is a reader inside
 * `notify_subscription`'s call, a handful of nanoseconds against a subscribe measured in
 * microseconds, so what it needs above all is OPERATIONS, and the single-vertex shape could
 * not supply them: it reddened the parent commit on roughly half of runs, which is not
 * coverage. Spreading the same storm over many vertices keeps each append short, so the
 * storm runs an order of magnitude longer in less wall time.
 */
constexpr int kSubVertices = 128;

/**
 * @brief The subscription observer — the subscribe / clear path.
 *
 * The scenario CI's aarch64 leg caught with 0 of 5000 dispatches, which is what the closed
 * loop in this file exists to fix.
 */
void subscription_observer_flip_race() {
    std::printf("subscription observer, flipped under a wire-subscribe storm:\n");
    graph_t g;
    std::vector<vertex_handle_t> vs;
    vs.reserve(kSubVertices);
    for (int i = 0; i < kSubVertices; ++i)
        vs.push_back(g.register_vertex(path_t("/v" + std::to_string(i)), role_t::STORED_VALUE));

    probe_t a{'A'};
    probe_t b{'B'};
    const long torn_before = g_torn.load();
    std::atomic<bool> go{false};
    std::atomic<bool> armed{false};
    std::atomic<bool> stop{false};
    std::atomic<long> cycles{0};
    std::atomic<long> publishes{0};
    std::atomic<long> ok_subs{0};
    std::atomic<int> issued{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        g.configure_subscription_observer(&observer_a, &a);
        armed.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            if (!burst_then_settle([&] { g.configure_subscription_observer(&observer_a, &a); },
                                   [&] { g.configure_subscription_observer(&observer_b, &b); },
                                   cycles, publishes, stop))
                break;
        }
    });
    std::thread subscriber([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!armed.load(std::memory_order_acquire)) {
        }
        // subscribe_wire carries a non-EMPTY caller (the link NAME), which is exactly the
        // EXTERNAL discrimination the observer fires on.
        int i = 0;
        for (; keep_storming(i, kSubs, 4 * kSubs, cycles, 1); ++i)
            if (wire_sub(g, vs[static_cast<std::size_t>(i) % vs.size()], "cli", "c"))
                ok_subs.fetch_add(1);
        issued.store(i, std::memory_order_relaxed);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    subscriber.join();

    const long hits = a.hits.load() + b.hits.load();
    std::printf("    (%ld of %d subscribes reached an observer; %ld publishes, %ld handshakes)\n",
                hits, issued.load(), publishes.load(), cycles.load());
    check(ok_subs.load() == issued.load(), "every subscribe still landed under the storm");
    check(publishes.load() >= kPublishFloor, "the flipper published inside the storm");
    check(cycles.load() > 0, "flipper and storm interleaved (>=1 publish/dispatch handshake)");
    check(hits > 0, "the observer was reachable DURING the storm (the torn check is not vacuous)");
    check(g_torn.load() == torn_before, "no observer was handed the other observer's context");
    // Settled positive control — no flipper, so every subscribe must reach the observer.
    g.configure_subscription_observer(&observer_a, &a);
    const long before = a.hits.load();
    for (int i = 0; i < kSettled; ++i)
        (void)wire_sub(g, vs[static_cast<std::size_t>(i) % vs.size()], "cli", "c");
    check(a.hits.load() - before == kSettled, "settled: every subscribe reaches the observer");
}

constexpr int kCreates = 200000;

/**
 * @brief How many distinct child names the creation storm cycles over.
 *
 * Small on purpose: the first pass creates them and every pass after answers `PATH_IN_USE`
 * from the factory, so the catalog lookup runs in full on every attempt while the vertex
 * count stays at this number. See @ref child_catalog_flip_race.
 */
constexpr int kCatalogNames = 64;

/**
 * @brief The creatable-child-type catalog: a public insert against a tree a PEER's bytes walk.
 *
 * The map is the member the `{fn, ctx}` publication cannot reach, so this scenario's verdict
 * is the set of legal outcomes rather than a context tag: every in-band creation must find the
 * built-in `stored_value` entry the constructor registered and never removes.
 *
 * @par Why this flipper is NOT closed-loop, unlike the other four
 * The other four wait for a dispatch because their detector needs the reader to GET THROUGH.
 * This one is the opposite: its detector needs a lookup to land on a tree mid-rotation, so
 * what it wants is insert DENSITY, and throttling the flipper to one insert per lookup would
 * weaken it. It runs unthrottled for that reason, and the closed loop is replaced by the two
 * guards that do transfer — arming, so the storm cannot run before the catalog is being
 * mutated, and a floor on registrations observed during the storm, so "the race was
 * exercised" is asserted rather than assumed.
 *
 * @par Why the names CYCLE
 * The odds of one lookup landing on a rotation are small, so the only lever is the NUMBER of
 * lookups — and issuing a fresh name each time couples that number to vertex allocation,
 * which caps it. Cycling over @ref kCatalogNames names means every attempt after the first
 * pass runs the catalog lookup IN FULL and then answers `PATH_IN_USE` from the factory,
 * allocating nothing. The storm issues an order of magnitude more lookups for the same
 * memory, and `SCHEMA_NOT_FOUND` — the lookup MISSING an entry that is present and never
 * removed — becomes the direct detector rather than a shortfall inferred from a count.
 *
 * @par Its detector is probabilistic, by nature, and that is not a CI hazard
 * A torn red-black-tree walk is undefined behaviour, and undefined behaviour cannot be made
 * deterministically observable from the outside: against the parent commit this reddens on
 * most runs, not on every one, where the other four scenarios redden on all of them. That
 * asymmetry costs CI nothing, because it runs in the direction that cannot flake: on a
 * CORRECT build every assertion here holds deterministically — there is no coincidence any
 * of them is waiting for. The guarantee in the other direction is the sanitizer's, and this
 * scenario is a live TSan/ASan vehicle.
 */
void child_catalog_flip_race() {
    std::printf("child-type catalog, registered under an in-band creation storm:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    // Every catalog key the flipper will insert, formatted before the storm starts.
    constexpr int kFillers = 200000;
    std::vector<std::string> fillers;
    fillers.reserve(kFillers);
    for (int i = 0; i < kFillers; ++i) fillers.push_back("filler" + std::to_string(i));

    std::atomic<bool> go{false};
    std::atomic<bool> armed{false};
    std::atomic<bool> stop{false};
    std::atomic<long> registrations{0};
    std::atomic<long> created{0};
    std::atomic<long> missed{0};
    std::atomic<long> illegal{0};
    std::atomic<int> issued{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        // Distinct keys, so the tree genuinely GROWS and rebalances rather than
        // insert_or_assign-ing one node in place. BOUNDED at kFillers, then cycled: the
        // catalog has no erase, and under a sanitizer the reader storm runs slowly enough
        // that an unbounded flipper would allocate a fresh `std::function` and map node per
        // iteration for minutes. kFillers is sized so the GROWTH phase outlasts the whole
        // creation storm on a release build while still being bounded; cycling afterwards
        // still mutates the map the reader walks.
        //
        // The names are built UP FRONT, outside the timed loop. This flipper's whole job is
        // insert density — how many rebalances it can drive through the tree while the
        // creator is walking it — and formatting a name per iteration was costing more than
        // the map insert it existed to perform.
        for (int i = 0; !stop.load(std::memory_order_relaxed); ++i) {
            g.register_child_type(
                fillers[static_cast<std::size_t>(i) % fillers.size()],
                [](graph_t& gg, std::vector<std::byte> key, const tr::wire::tlv_t*) {
                    return gg.register_vertex_key(std::move(key), role_t::STORED_VALUE);
                });
            registrations.fetch_add(1, std::memory_order_relaxed);
            armed.store(true, std::memory_order_release);
        }
    });
    std::thread creator([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!armed.load(std::memory_order_acquire)) {
        }
        int i = 0;
        for (; keep_storming(i, kCreates, 10 * kCreates, registrations, kPublishFloor); ++i) {
            const auto w = g.write(path_t("/dev:children[]"),
                                   b_spec("stored_value", "c" + std::to_string(i % kCatalogNames)));
            if (w.has_value()) {
                created.fetch_add(1);
            } else if (w.error() == status_t::SCHEMA_NOT_FOUND) {
                // THE detector: `stored_value` is registered by the constructor and never
                // removed, so a lookup can only answer "no such type" by having walked a
                // tree that was being rebalanced under it.
                missed.fetch_add(1);
            } else if (w.error() != status_t::PATH_IN_USE && w.error() != status_t::BACKPRESSURE) {
                illegal.fetch_add(1);
            }
        }
        issued.store(i, std::memory_order_relaxed);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    creator.join();

    std::printf("    (%d lookups, %ld created, %ld missed; %ld registrations raced them)\n",
                issued.load(), created.load(), missed.load(), registrations.load());
    check(registrations.load() >= kPublishFloor,
          "the catalog was being mutated THROUGHOUT the storm");
    // Non-vacuity: the lookups have to have REACHED the factory, or "none missed" is a
    // statement about nothing. Every attempt past the first pass answers PATH_IN_USE, which
    // only the factory can produce, so a full first pass is the evidence.
    check(created.load() == kCatalogNames, "every name was created once — the factory ran");
    check(missed.load() == 0, "no lookup missed the catalog entry that is always present");
    check(illegal.load() == 0, "no creation answered outside the legal status set");
}

constexpr int kIdentityReads = 120000;

/**
 * @brief Rotations per identity cycle — a quarter of @ref kBurst, and deliberately so.
 *
 * Every rotation here takes the identity lock, and each cycle then waits on THREE reads
 * rather than one, so a full-sized burst makes the flipper hog the lock and starve the very
 * reader it is waiting for: at @ref kBurst the storm had to extend to three times its
 * nominal length to bank its cycles. A sixty-four-deep burst still puts dozens of rotations
 * between consecutive reads — which is the pressure the window needs — while leaving the
 * storm at its nominal length.
 */
constexpr int kIdentityBurst = 64;

/**
 * @brief The node identity record: install / clear against the read served ABOVE the gate.
 *
 * The one member on #1049's list backing an operation served to an UNAUTHENTICATED peer by
 * design (RFC-0011 §C.2 — a TOFU peer must be able to pin the key before it can prove
 * anything), which is what makes its use-after-free remotely reachable. `read_identity` tests
 * emptiness and then memcpys the buffer; install and clear each free it. The verdict is that
 * every answer is one of the LEGAL ones — absent, or an ed25519 record whose 60 bytes carry a
 * key this graph actually installed — never a third thing, and never a read of freed memory
 * (loudest under ASan/TSan). The length is checked AND the content, because a read straddling
 * the swap can measure the right length off the wrong buffer.
 *
 * The closed loop here is PER OUTCOME, which is what makes the two-sided positive control
 * deterministic: after an install the flipper waits until a read is seen to be SERVED, and
 * after a clear until one is seen to be ABSENT. "The record was served" and "the clear was
 * observed" are then true by construction rather than by duty cycle — they were latent flakes
 * of exactly the class the aarch64 leg caught in the observer scenario.
 */
void identity_flip_race() {
    std::printf("node identity record, rotated under a pre-auth :identity read storm:\n");
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    const auto p = path_t::parse("/dev:identity");
    check(p.has_value(), "/dev:identity parses");
    const auto v = g.find(p->key());
    check(v.has_value(), "/dev resolves");

    const std::vector<std::byte> key_a(32, std::byte{0x11});
    const std::vector<std::byte> key_b(32, std::byte{0x22});

    std::atomic<bool> go{false};
    std::atomic<bool> armed{false};
    std::atomic<bool> stop{false};
    std::atomic<long> cycles{0};
    std::atomic<long> publishes{0};
    std::atomic<long> served{0};
    std::atomic<long> absent{0};
    std::atomic<long> illegal{0};
    std::atomic<int> issued{0};

    std::thread flipper([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        (void)g.set_identity(0x01, key_a);
        armed.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            // Dense rotations first, exactly as the callback flippers burst: the window here
            // is a reader between `identity_record_.empty()` and the memcpy of its bytes, and
            // one rotation per read does not fill it often enough to redden the parent commit
            // reliably.
            int k = 0;
            for (; k < kIdentityBurst && !stop.load(std::memory_order_relaxed); ++k) {
                (void)g.set_identity(0x01, key_a);
                (void)g.set_identity(0x01, key_b);
                g.clear_identity();
            }
            publishes.fetch_add(3 * k + 3, std::memory_order_relaxed);
            // Then each phase waits for the outcome IT produces to be observed, so a full
            // cycle proves both halves of the two-sided control regardless of timing.
            (void)g.set_identity(0x01, key_b);
            const long served_b = served.load(std::memory_order_relaxed);
            if (!await_progress(served, served_b, stop)) break;
            g.clear_identity();
            const long cleared = absent.load(std::memory_order_relaxed);
            if (!await_progress(absent, cleared, stop)) break;
            (void)g.set_identity(0x01, key_a);
            const long served_a = served.load(std::memory_order_relaxed);
            if (!await_progress(served, served_a, stop)) break;
            cycles.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread reader([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!armed.load(std::memory_order_acquire)) {
        }
        int i = 0;
        for (; keep_storming(i, kIdentityReads, 10 * kIdentityReads, cycles, 1); ++i) {
            // "mallory" — an uncredentialed caller, which is precisely who this facet is
            // reachable by, and the reason a UAF here is a remote primitive rather than a
            // host-side footgun.
            const auto r = g.read(*v, p->field(), "mallory");
            if (!r) {
                if (r.error() == status_t::SCHEMA_NOT_FOUND) {
                    absent.fetch_add(1);
                } else {
                    illegal.fetch_add(1);
                }
                continue;
            }
            const view_t flat = r->flatten();
            const auto bytes = flat.bytes();
            // 60 bytes is the RFC-0011 §B ed25519 record, pinned by identity_test. Any other
            // size is a record this graph never built.
            //
            // The length alone is a WEAK detector and is not what this scenario rests on: a
            // read that straddles the swap can pick up a stale size beside a fresh pointer —
            // or a freed buffer — and still measure 60. So the CONTENT is checked too. Only
            // two records are ever installed here and each one's 32-byte key is a single
            // repeated byte, so the trailing key must be all-`0x11` or all-`0x22`; anything
            // else is bytes this graph never wrote, whatever their length.
            if (bytes.size() != 60) {
                illegal.fetch_add(1);
                continue;
            }
            const std::span<const std::byte> key = bytes.subspan(bytes.size() - 32);
            const std::byte first = key[0];
            bool uniform = first == std::byte{0x11} || first == std::byte{0x22};
            for (const std::byte c : key) uniform = uniform && c == first;
            if (!uniform) {
                illegal.fetch_add(1);
                continue;
            }
            served.fetch_add(1);
        }
        issued.store(i, std::memory_order_relaxed);
        stop.store(true, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    flipper.join();
    reader.join();

    std::printf("    (%ld served, %ld absent, of %d reads; %ld rotation cycles)\n", served.load(),
                absent.load(), issued.load(), cycles.load());
    check(illegal.load() == 0, "every :identity answer was the 60-byte record or absent");
    check(publishes.load() >= kPublishFloor, "the record was rotated inside the read storm");
    check(cycles.load() > 0, "flipper and storm interleaved (>=1 full rotation handshake)");
    // Two-sided positive control, deterministic under the per-outcome loop above: a run that
    // only ever saw "absent" would prove nothing about the read of a live buffer, and one
    // that never saw "absent" never raced a clear.
    check(served.load() > 0, "the record WAS served during the rotation storm");
    check(absent.load() > 0, "the clear WAS observed during the rotation storm");
}

}  // namespace

int main() {
    std::printf("graph_t configuration-seam publish coherence (#1049):\n");
    remote_sink_flip_race();
    subject_resolver_flip_race();
    subscription_observer_flip_race();
    child_catalog_flip_race();
    identity_flip_race();
    return tr::testing::summary("graph_config_race");
}
