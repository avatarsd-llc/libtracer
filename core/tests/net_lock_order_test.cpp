/**
 * @file
 * @brief RFC-0014 S6 (#492) — the control-plane lock-order invariant: `transport_vertex_t`
 *        never holds `ctl_m_` across a call that can re-enter it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The hazard S6 makes live: RFC-0014 §4's standing-binding seam
 * (`acquire_link`/`release_link`) is driven by the ROUTING plane, and what the routing
 * plane watches to know when to drive it is the connection vertex's own liveness. So the
 * subscriber of a liveness publish calls straight back into the control plane — and
 * `ctl_m_` is a plain, NON-RECURSIVE `std::mutex`. Published from under it, that is a
 * self-deadlock on one thread; joined from under it (`self_heal_link_t::stop()` inside
 * `remove_connection`), it is a two-thread one.
 *
 * Both are DEADLOCKS, not data races, so the instrument is a bounded watchdog rather than
 * a sanitizer report: every exercise runs on its own thread and is waited for with a
 * deadline. A regression therefore FAILS — `[FAIL]`, non-zero exit — instead of hanging
 * ctest out to its timeout with no diagnosis. (Under the `tsan` job the same binary gets a
 * second net for free: TSan's lock-order-inversion detector sees the nesting itself.)
 *
 * Ablations, run by hand, one at a time (both verified):
 *
 * 1. Revert `set_link_state_locked` to publish inside phase 1 —
 *    `txn.publish(…)` → `return graph_.write(it->second.vertex, link_state_value(state));`
 *    Sections 1 and 2 go RED (`the publish returned`, `the creation returned`).
 * 2. Restore `remove_connection_locked`'s in-line `it->second.engine->stop()` in place of
 *    `txn.stop_engine(…)`. Section 3 goes RED (`the teardown returned`).
 *
 * Neither hangs the suite: the watchdog reports and the process exits non-zero. In a
 * `-DNDEBUG` build those are the whole diagnosis; in an assertive one the `ctl_txn_t`
 * ownership stamp aborts ablation 1 at its acquisition site first, which is the point of
 * the stamp — ablation 2 is a CROSS-thread deadlock and only the watchdog sees it.
 *
 * The third leg of the S6 discipline — `ops_m_`, which is what keeps a deferred publish
 * from landing on a vertex a concurrent teardown is retiring — is gated next door in
 * `net_control_plane_race_test` under TSan, because its regression is a data race rather
 * than a deadlock. Dropping the `OPERATION` scope from `set_link_state` / `remove_connection`
 * reds that test with a `graph_t::write` vs `revert_to_placeholder` report.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/conn_spec.hpp"
#include "libtracer/self_heal_link.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::status_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::link_state_t;
using tr::net::transport_vertex_t;

using tr::testing::check;

/** @brief How long a non-deadlocked exercise is given before it is called a deadlock. */
constexpr auto kWatchdog = 20s;

/**
 * @brief Run @p body on its own thread and answer whether it RETURNED within @ref kWatchdog.
 *
 * A deadlocked body never returns, so the thread cannot be joined and the future cannot be
 * destroyed without blocking. That is why the caller is expected to abandon the process
 * (see `main`) once this answers false: the point is to REPORT the deadlock, and a report
 * that has to join the deadlocked thread to be printed is no report.
 */
[[nodiscard]] bool completes(std::function<void()> body) {
    // `new`, never a stack future: on the timeout path this is deliberately LEAKED, because
    // ~future would block on the wedged thread and swallow the diagnosis.
    auto* const fut = new std::future<void>(std::async(std::launch::async, std::move(body)));
    if (fut->wait_for(kWatchdog) == std::future_status::ready) {
        fut->get();
        delete fut;
        return true;
    }
    return false;
}

/** @brief Live fake sockets, so a leak on a teardown path is visible. */
std::atomic<int> g_socks_alive{0};

/** @brief A minimal transport: it carries nothing and can report itself down on demand. */
struct fake_sock_t final : tr::net::transport_t {
    fake_sock_t() { g_socks_alive.fetch_add(1); }
    ~fake_sock_t() override { g_socks_alive.fetch_sub(1); }
    fake_sock_t(const fake_sock_t&) = delete;
    fake_sock_t& operator=(const fake_sock_t&) = delete;

    /** @brief The wire is not under test — a frame is accepted and dropped. */
    void send(std::span<const std::byte>) override {}

    /** @brief The remote hangup: fire the down-notifier (a protected base seam). */
    void die() { notify_down(); }
};

/**
 * @brief The scripted dial of `link_liveness_test`, reduced to what a lock-order exercise
 *        needs: every attempt blocks until the test queues an outcome, or auto-fails.
 */
struct dial_script_t {
    std::mutex m;
    std::condition_variable cv;
    std::deque<bool> outcomes;       /**< @brief Next attempts' verdicts (FIFO). */
    bool auto_fail = false;          /**< @brief Empty queue answers DOWN immediately. */
    int attempts = 0;                /**< @brief Factory runs so far. */
    std::vector<fake_sock_t*> built; /**< @brief Every socket ever constructed. */

    /** @brief Queue the next attempt's verdict and release a blocked factory. */
    void script(bool up) {
        const std::lock_guard l(m);
        outcomes.push_back(up);
        cv.notify_all();
    }

    /** @brief Block until the factory has run @p n times total (the attempt rendezvous). */
    [[nodiscard]] bool await_attempts(int n) {
        std::unique_lock l(m);
        return cv.wait_for(l, kWatchdog, [&] { return attempts >= n; });
    }

    /** @brief Answer every further attempt DOWN — bounds a teardown's in-flight join. */
    void arm_auto_fail() {
        const std::lock_guard l(m);
        auto_fail = true;
        cv.notify_all();
    }
};

/**
 * @brief A section's quiescence barrier: unblock every parked factory, THEN destroy the
 *        transport vertex — so its engine workers are JOINED while the subscriber they
 *        publish into is still alive.
 *
 * Two hazards are closed here, and only the second one needs the vertex to be optional.
 *
 * 1. **The parked factory.** A section that fails a check early leaves a dial attempt
 *    blocked on `dial_script_t::cv`, and the engine's teardown join then waits out the
 *    watchdog. `arm_auto_fail` releases it first (this is what `link_liveness_test.cpp`'s
 *    `script_guard_t` does), which is why it must run BEFORE the vertex is torn down.
 *
 * 2. **The outliving subscriber.** A liveness subscriber (`standing_binding_t`,
 *    `worker_fanout_binding_t`) holds a `std::string`, and the engine WORKER publishes
 *    into it from another thread. `graph_t::unsubscribe` is NOT a barrier against that:
 *    per @ref tr::graph::subscription_t's reclamation section, the fan-out snapshots a
 *    vertex's edges and dispatches outside every lock, so a snapshot taken before the
 *    retirement still names the subscriber — the plain (hook-less) `unsubscribe` retires
 *    the edge and returns, it does not wait. The only real barrier is the PUBLISHER
 *    stopping, and that is `~transport_vertex_t` → `~conn_t` → `~self_heal_link_t` →
 *    `stop()`, which joins the worker (and publishes nothing itself, so the fan-out this
 *    releases cannot re-enter a half-dead section). Ordinary stack order runs it too LATE:
 *    the subscriber is declared after the vertex it borrows, so the subscriber dies first
 *    and a delivery already in flight writes into a freed string — the `~standing_binding_t`
 *    / `operator delete` race TSan reported at #1484. Holding the vertex in a
 *    `std::optional` and resetting it from a guard declared AFTER the subscriber inverts
 *    that, on every exit path including the early `return`s.
 */
struct section_quiesce_t {
    std::optional<transport_vertex_t>& net; /**< @brief The vertex destroyed by this guard. */
    dial_script_t& script;                  /**< @brief The script that frees its workers. */

    /**
     * @brief Unblock, then join: the order the two hazards above require.
     *
     * IDEMPOTENT, and callable early — a section that wants to READ its subscriber's
     * counters must quiesce before sampling them (see section 2), and the destructor is
     * then a backstop rather than the only discharge. A second call re-arms an already
     * armed script and resets an already empty optional; both are no-ops.
     */
    void discharge() {
        script.arm_auto_fail();
        net.reset();
    }

    /** @brief The backstop discharge — every exit path, including the early `return`s. */
    ~section_quiesce_t() { discharge(); }
};

/**
 * @brief Register the scripted engine-managed `fake` kind and its DIAL module.
 *
 * @param net    The transport vertex the `fake` kind is registered on.
 * @param script The dial script the factory consults. It **MUST** be declared BEFORE the
 *               @p net it is handed to: the engine's factory copy holds this address, and
 *               a worker can still be inside the factory while `~transport_vertex_t` runs.
 *               Declared the other way round, `~dial_script_t` destroys the mutex and
 *               condition variable out from under a live worker — which is a data race
 *               TSan reports rather than a hang, and it is what broke `main` at #1484.
 */
void declare_fake_engine_module(transport_vertex_t& net, dial_script_t& script) {
    dial_script_t* const s = &script;
    net.register_transport_type(
        "fake",
        [s](const tr::net::conn_settings_t&,
            const tr::wire::tlv_t*) -> tr::graph::result_t<std::unique_ptr<tr::net::transport_t>> {
            std::unique_lock l(s->m);
            ++s->attempts;
            s->cv.notify_all();
            s->cv.wait(l, [&] { return !s->outcomes.empty() || s->auto_fail; });
            if (s->outcomes.empty()) return std::unexpected(status_t::TRANSPORT_DOWN);
            const bool up = s->outcomes.front();
            s->outcomes.pop_front();
            if (!up) return std::unexpected(status_t::TRANSPORT_DOWN);
            auto sock = std::make_unique<fake_sock_t>();
            s->built.push_back(sock.get());
            return sock;
        },
        tr::net::transport_kind_traits_t{.self_heal_dial = true, .delivers_ropes = false});
    (void)net.register_module("fake-client", "fake", conn_role_t::DIAL);
}

/** @brief The creating SPEC for the engine-managed `fake` kind, endpoint-door spelling. */
tr::view::view_t fake_spec(std::string_view name, std::uint32_t backoff_ms) {
    tr::net::conn_spec_t spec(name);
    spec.kind("fake").addr("203.0.113.1").port(9).backoff_ms(backoff_ms);
    return spec.view();
}

/**
 * @brief THE routing-plane subscriber S6 wires: it watches a connection's liveness and
 *        drives the RFC-0014 §4 standing-binding seam from what it sees.
 *
 * Every door it knocks on takes `ctl_m_`. Pre-S6 the very first delivery wedged: the
 * publish that produced it was made with `ctl_m_` held, on this same thread.
 */
struct standing_binding_t {
    transport_vertex_t& net;
    std::string qualified;
    std::atomic<int> deliveries{0}; /**< @brief Liveness values seen. */
    std::atomic<int> reentries{0};  /**< @brief Control-plane calls that RETURNED. */
    std::atomic<bool> held{false};  /**< @brief Whether the hold is currently taken. */

    /** @brief One liveness delivery: re-enter the control plane, both doors. */
    void operator()(const tr::view::rope_t&) {
        deliveries.fetch_add(1);
        // Both the refcount seam and a plain reader — a routing plane drives both, and
        // both take `ctl_m_`.
        if (held.exchange(true)) {
            (void)net.release_link(qualified);
            held.store(false);
        } else {
            (void)net.acquire_link(qualified);
        }
        (void)net.link_of(qualified);
        (void)net.settings_of(qualified);
        reentries.fetch_add(1);
    }
};

/**
 * @brief Section 1 — a liveness publish must not hold `ctl_m_` over its own fan-out.
 *
 * One thread, no timing: `set_link_state` publishes, the subscriber is dispatched inline
 * from that publish, and it calls `acquire_link`. Pre-S6 that is a non-recursive mutex
 * taken twice on one thread and the call never returns.
 */
void test_publish_does_not_hold_ctl_over_its_fan_out() {
    std::printf("S6: a liveness publish does not hold ctl_m_ across its subscriber fan-out:\n");
    graph_t node;
    fwd_router_t router(node);
    // Plain stack order, no `section_quiesce_t`: this section declares no engine-managed
    // kind, so the vertex owns no worker and the ONLY thread that ever publishes into the
    // binding below is the `completes()` thread this function already waits on. There is no
    // second publisher to join, hence nothing for the barrier to do.
    transport_vertex_t net(node, router);
    fake_sock_t sock;
    net.provide_link("manual", "a", sock);
    // The staged link's module: declared with no kind, because nothing is constructed here —
    // the staging outranks any factory — and the declaration is what mints the creator endpoint
    // the SPEC below is written to.
    (void)net.register_module("manual", "", conn_role_t::DIAL);
    const auto made = node.write(path_t("/net/manual/conn"), tr::net::conn_spec_t("a").view());
    check(made.has_value(), "the staged link is wired in as /net/manual/a");

    standing_binding_t binding{net, "net/manual/a"};
    const auto sub = node.subscribe(*path_t::parse("/net/manual/a"), binding);
    check(sub.has_value(), "the routing-plane subscriber binds to the connection's liveness");

    const bool done = completes([&] {
        const auto r = net.set_link_state("net/manual/a", link_state_t::UP);
        check(r.has_value(), "set_link_state answers the write's status");
    });
    check(done, "the publish returned — ctl_m_ was NOT held across the fan-out");
    if (!done) return;  // wedged: the checks below would read a half-run exercise
    check(binding.deliveries.load() == 1, "the subscriber saw the transition");
    check(binding.reentries.load() == 1,
          "and re-entered acquire_link / link_of / settings_of from inside the delivery");
    (void)node.unsubscribe(*sub);
}

/**
 * @brief Section 2 — creation's BIRTH liveness is under the same rule.
 *
 * A creator-endpoint create publishes `UP`/`LISTENING`/`DORMANT` before it
 * returns, and that publish fans out exactly like any other. Pre-S6 it ran deep inside
 * `make_connection_locked`, with `ctl_m_` held over the whole creation.
 */
void test_creation_birth_publish_is_outside_ctl() {
    std::printf("S6: a creation's birth liveness publishes with ctl_m_ already released:\n");
    graph_t node;
    fwd_router_t router(node);
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    std::optional<transport_vertex_t> net_slot;
    net_slot.emplace(node, router);
    transport_vertex_t& net = *net_slot;
    declare_fake_engine_module(net, script);

    // Subscribe to the vertex BEFORE it exists: an ancestor subscription reaches the
    // creation's own publish, which is the only way to observe a birth transition.
    standing_binding_t binding{net, "net/fake-client/a"};
    // AFTER the binding, so the engine workers are joined while the binding still owns its
    // string — see `section_quiesce_t`. Bounded teardown even on a failing check.
    section_quiesce_t quiesce{net_slot, script};
    const auto sub = node.subscribe(*path_t::parse("/net/fake-client"), binding);
    check(sub.has_value(), "a subscriber sits above the module (RFC-0005 bubbling)");

    const bool done = completes([&] {
        const auto w = node.write(path_t("/net/fake-client/conn"), fake_spec("a", 1));
        check(w.has_value(), "SPEC{a, kind=fake} creates via the creator endpoint");
    });
    check(done, "the creation returned — its birth publish did not re-enter ctl_m_");
    if (!done) return;

    // QUIESCE FIRST, then read the counters. The engine's worker is a second publisher into
    // this same binding, and `deliveries` / `reentries` are bumped at the two ENDS of one
    // delivery — so a live sample that catches the worker between them reads `reentries <
    // deliveries` and indicts a re-entry that was merely still running. That is the same
    // window as the lifetime race above, seen through the assertion instead of through the
    // allocator, and the same barrier closes it: the guard's `arm_auto_fail` + vertex
    // destruction joins the worker, after which the two counters are stable and the
    // equality is the property it claims to be — "no delivery wedged", not "no delivery is
    // mid-flight". `reset()` is idempotent, so the guard still covers the paths above.
    quiesce.discharge();

    check(binding.deliveries.load() >= 1, "the birth DORMANT reached the subscriber");
    check(binding.reentries.load() == binding.deliveries.load(),
          "every delivery's control-plane re-entry returned");
    (void)node.unsubscribe(*sub);
}

/**
 * @brief Holds ONE worker-thread fan-out open, so the teardown below is certainly inside
 *        `remove_connection` when the fan-out's control-plane re-entry lands.
 *
 * The hold is a fixed wait, and it is deliberately NOT a rendezvous: there is no condition
 * to test, because the state it would test for — "the other thread is inside `ctl_m_`" — is
 * private to the class under test and unobservable by construction. What the wait does is
 * widen a microsecond window to a fifth of a second, which is what turns "the ablation
 * deadlocks if you are lucky" into "the ablation deadlocks". Same instrument as
 * `dial_script_t`'s gate, pointed at the publish rather than at the dial.
 */
struct fanout_gate_t {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false;                /**< @brief A WORKER-thread fan-out is open. */
    static constexpr auto kHold = 200ms; /**< @brief How long it is held open. */

    /** @brief Block until a worker-thread delivery has opened the gate. */
    [[nodiscard]] bool await_entered() {
        std::unique_lock l(m);
        return cv.wait_for(l, kWatchdog, [&] { return entered; });
    }
};

/**
 * @brief The section-3 subscriber: on the FIRST delivery that arrives on a thread other
 *        than the test's, it announces itself, holds the fan-out open, and only then
 *        re-enters the control plane.
 */
struct worker_fanout_binding_t {
    transport_vertex_t& net;
    std::string qualified;
    std::thread::id test_thread = std::this_thread::get_id();
    fanout_gate_t gate{};
    std::atomic<int> reentries{0}; /**< @brief Control-plane calls that RETURNED. */

    /** @brief One liveness delivery; the interesting ones run on the engine's worker. */
    void operator()(const tr::view::rope_t&) {
        if (std::this_thread::get_id() != test_thread) {
            bool first = false;
            {
                const std::lock_guard l(gate.m);
                first = !gate.entered;
                gate.entered = true;
            }
            gate.cv.notify_all();
            if (first) std::this_thread::sleep_for(fanout_gate_t::kHold);
        }
        // THE re-entry: RFC-0014 §4's standing-binding seam, driven from a liveness
        // delivery — which is what the routing plane does with it.
        (void)net.acquire_link(qualified);
        reentries.fetch_add(1);
    }
};

/**
 * @brief Section 3 — teardown must not hold `ctl_m_` across the engine worker's JOIN.
 *
 * `remove_connection` stops the S5 engine, and `stop()` joins the worker that is the sole
 * publisher of liveness. The worker publishes with its own `m_` released precisely so the
 * fan-out may take graph locks — and that fan-out reaches a routing-plane subscriber whose
 * `acquire_link` wants `ctl_m_`. Join it from under `ctl_m_` and the two threads are
 * deadlocked: teardown waits for the worker, the worker waits for teardown's lock.
 */
void test_teardown_does_not_hold_ctl_over_the_engine_join() {
    std::printf("S6: teardown does not hold ctl_m_ across the engine worker's join:\n");
    graph_t node;
    fwd_router_t router(node);
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    std::optional<transport_vertex_t> net_slot;
    net_slot.emplace(node, router);
    transport_vertex_t& net = *net_slot;
    declare_fake_engine_module(net, script);
    (void)node.write(path_t("/net/fake-client/conn"), fake_spec("a", 1));
    // Armed up front so a dial that does reach the factory concludes on its own — the
    // teardown's in-flight-attempt join stays bounded whatever this test does.
    script.arm_auto_fail();

    // Subscribed AFTER creation, so the first delivery this binding sees is a WORKER
    // publish rather than the birth DORMANT (which section 2 already covers).
    worker_fanout_binding_t binding{net, "net/fake-client/a"};
    // AFTER the binding — same barrier as section 2, and this section is the one whose
    // subscriber is designed to be sitting on the WORKER thread when the section ends.
    section_quiesce_t quiesce{net_slot, script};
    const auto sub = node.subscribe(*path_t::parse("/net/fake-client/a"), binding);
    check(sub.has_value(), "the routing-plane subscriber binds to the connection's liveness");

    // The standing binding kicks the dial: the worker moves DORMANT → DIALING and publishes
    // that transition itself. The gate opens inside that publish.
    check(net.acquire_link("net/fake-client/a").has_value(), "acquire_link takes the hold");
    check(binding.gate.await_entered(), "the engine's worker is inside a liveness fan-out");

    const bool done = completes([&] {
        const auto rm = node.write(path_t("/net/fake-client/conn"), tr::net::conn_remove("a"));
        check(rm.has_value(), "NAME{a} removes the connection while its worker publishes");
    });
    check(done, "the teardown returned — the engine join was NOT under ctl_m_");
    if (!done) return;
    check(binding.reentries.load() >= 1,
          "and the worker's own re-entry into acquire_link returned too");
    check(!node.find(path_t::parse("/net/fake-client/a")->key()).has_value(),
          "the vertex retired — teardown ran to completion, not just past the join");
    check(g_socks_alive.load() == 0, "no socket survives the removal");
    (void)node.unsubscribe(*sub);
}

}  // namespace

int main() {
    // See link_liveness_test's main: every exercise here registers a `self_heal_dial` kind,
    // which a `kSelfHealLinks = false` build refuses to catalogue (#1470).
    if constexpr (!tr::net::kSelfHealLinks)
        return tr::testing::skipped("net_lock_order",
                                    "kSelfHealLinks = false — the RFC-0014 S5 engine is not "
                                    "in this build");
    std::printf("== RFC-0014 S6: the control-plane lock-order invariant (#492) ==\n");
    test_publish_does_not_hold_ctl_over_its_fan_out();
    test_creation_birth_publish_is_outside_ctl();
    test_teardown_does_not_hold_ctl_over_the_engine_join();
    const int rc = tr::testing::summary("net_lock_order");
    // A wedged exercise leaves a thread blocked on `ctl_m_` forever, so an ordinary return
    // would run static destructors and hang AFTER the verdict was printed — the vacuous-pass
    // shape in reverse. Leave by the door that cannot block, having already reported.
    std::fflush(stdout);
    if (rc != 0) std::_Exit(rc);
    return rc;
}
