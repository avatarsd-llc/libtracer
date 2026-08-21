/**
 * @file
 * @brief RFC-0014 §4 S5 (#492) — the link-liveness ENGINE drives `link_state_t` through
 *        its DIAL state machine: dormant creation, auto-wake dial on demand
 *        (`connect_timeout`-bounded), self-heal with `backoff` while a standing binding
 *        holds, fail-fast on `RECONNECTING`, and close-to-dormant on the last release.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every transition is exercised DETERMINISTICALLY: the scripted transport factory below
 * BLOCKS each dial attempt on a condition variable until the test queues its outcome, so
 * the in-flight states (`DIALING`, `RECONNECTING`-mid-attempt) are held open for
 * inspection rather than raced; the published transitions are then awaited on the
 * connection vertex itself (`graph_t::await` in a re-read loop — edge-triggered, no
 * sleeps as rendezvous). Built under the sanitizer set like every threaded test here.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <span>
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

/** @brief Live fake sockets — release-to-dormant must reap down to zero. */
std::atomic<int> g_socks_alive{0};

/**
 * @brief The engine's inner socket, controllable from the test: counts sends and can
 *        `die()` on demand (firing the down-notifier exactly as a remote hangup would).
 */
struct fake_sock_t final : tr::net::transport_t {
    /** @brief Frames this socket carried — the "the op was SERVED" census. */
    std::atomic<std::size_t> sent{0};

    fake_sock_t() { g_socks_alive.fetch_add(1); }
    ~fake_sock_t() override { g_socks_alive.fetch_sub(1); }
    fake_sock_t(const fake_sock_t&) = delete;
    fake_sock_t& operator=(const fake_sock_t&) = delete;

    /** @brief Record the frame; the wire itself is not under test. */
    void send(std::span<const std::byte>) override { sent.fetch_add(1); }

    /** @brief The remote hangup: fire the down-notifier (a protected base seam). */
    void die() { notify_down(); }
};

/**
 * @brief The scripted dial: each factory run REGISTERS itself (`attempts`), then BLOCKS
 *        until the test queues an outcome — so an attempt in flight is a state the test
 *        holds open and inspects, not a race it hopes to win.
 */
struct dial_script_t {
    std::mutex m;
    std::condition_variable cv;
    std::deque<bool> outcomes;       /**< @brief Next attempts' verdicts (FIFO). */
    bool auto_fail = false;          /**< @brief Empty queue answers DOWN immediately —
                                                 armed before a teardown so the engine's
                                                 in-flight-attempt join is bounded, the
                                                 way a real connect deadline bounds it. */
    int attempts = 0;                /**< @brief Factory runs so far. */
    std::vector<fake_sock_t*> built; /**< @brief Every socket ever constructed. */
    tr::net::conn_settings_t last;   /**< @brief The settings the factory last saw. */

    /** @brief Queue the next attempt's verdict and release a blocked factory. */
    void script(bool up) {
        const std::lock_guard l(m);
        outcomes.push_back(up);
        cv.notify_all();
    }

    /** @brief Block until the factory has run @p n times total (the attempt rendezvous). */
    [[nodiscard]] bool await_attempts(int n) {
        std::unique_lock l(m);
        return cv.wait_for(l, 10s, [&] { return attempts >= n; });
    }
};

/**
 * @brief Arms the script's auto-fail on scope exit — declared AFTER the
 *        `transport_vertex_t`, so it runs BEFORE the engines' teardown joins their
 *        worker: even a test failing mid-heal tears down bounded instead of hanging on a
 *        gated factory (a real factory's connect deadline provides this bound).
 */
struct script_guard_t {
    dial_script_t& s;
    ~script_guard_t() {
        const std::lock_guard l(s.m);
        s.auto_fail = true;
        s.cv.notify_all();
    }
};

/**
 * @brief Register the scripted `fake` kind (engine-managed) and its DIAL module.
 *
 * @p script MUST be declared before the `transport_vertex_t` (the engine's factory copy
 * holds its address for the engine's lifetime — the usual test stack order).
 */
void declare_fake_engine_module(transport_vertex_t& net, dial_script_t& script) {
    dial_script_t* const s = &script;
    net.register_transport_type(
        "fake",
        [s](const tr::net::conn_settings_t& settings,
            const tr::wire::tlv_t*) -> tr::graph::result_t<std::unique_ptr<tr::net::transport_t>> {
            std::unique_lock l(s->m);
            s->last = settings;
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

/** @brief The published 1-byte liveness VALUE at @p path (0xFF = absent/unwritten). */
std::uint8_t state_byte(graph_t& g, std::string_view path) {
    const auto h = g.find(path_t::parse(path)->key());
    if (!h) return 0xFF;
    const auto v = g.read(*h);
    if (!v) return 0xFF;
    const auto bytes = (*v)->materialize().bytes();
    return bytes.empty() ? 0xFF : static_cast<std::uint8_t>(bytes.back());
}

/**
 * @brief Await the vertex publishing @p want — the edge-triggered rendezvous: re-read,
 *        and between reads block on `graph_t::await` (never a sleep). The 10 s cap is a
 *        BACKSTOP for a genuinely broken engine, not a synchronization window.
 */
[[nodiscard]] bool await_state(graph_t& g, std::string_view path, link_state_t want) {
    const auto parsed = path_t::parse(path);
    if (!parsed) return false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (state_byte(g, path) != static_cast<std::uint8_t>(want)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        (void)g.await(*parsed, 100ms);
    }
    return true;
}

/** @brief One dropped-frame census read off the engine link. */
std::uint64_t dropped_tx(transport_vertex_t& net, std::string_view qualified) {
    tr::net::transport_t* const link = net.link_of(qualified);
    return link != nullptr ? link->drop_stats().dropped_tx : ~0ULL;
}

/** @brief The creating SPEC for the engine-managed `fake` kind, endpoint-door spelling. */
tr::view::view_t fake_spec(std::string_view name, std::uint32_t backoff_ms,
                           std::uint32_t connect_timeout_ms = 0) {
    tr::net::conn_spec_t spec(name);
    spec.kind("fake").addr("203.0.113.1").port(9);
    if (backoff_ms != 0) spec.backoff_ms(backoff_ms);
    if (connect_timeout_ms != 0) spec.connect_timeout_ms(connect_timeout_ms);
    return spec.view();
}

void test_engine_creation_is_dormant() {
    std::printf("S5: an engine-managed DIAL creation mints the vertex DORMANT, no socket:\n");
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_engine_module(net, script);
    const script_guard_t guard{script};  // bounded teardown even on a failing test

    const auto w = node.write(path_t("/net/fake-client/conn"), fake_spec("a", 1));
    check(w.has_value(), "SPEC{a, kind=fake} creates via the creator endpoint");
    check(node.find(path_t::parse("/net/fake-client/a")->key()).has_value(),
          "the connection vertex exists (creation is explicit, RFC-0014 §3)");
    check(
        state_byte(node, "/net/fake-client/a") == static_cast<std::uint8_t>(link_state_t::DORMANT),
        "the vertex value is DORMANT (0x00) — refcount 0, no socket (RFC-0014 §4)");
    {
        const std::lock_guard l(script.m);
        check(script.attempts == 0, "the factory has NOT run — creation dials nothing");
    }
    check(net.settings_of("net/fake-client/a") != nullptr &&
              net.settings_of("net/fake-client/a")->backoff_ms == 1,
          "the parsed settings carry the SPEC's backoff");

    // A misconfigured SPEC is refused AT CREATION, not deferred to the first dial: the
    // universal DIAL keys are gated even though the factory does not run.
    tr::net::conn_spec_t bad("b");
    bad.kind("fake");  // no addr, no port
    const auto wb = node.write(path_t("/net/fake-client/conn"), bad.view());
    check(!wb.has_value() && wb.error() == status_t::TYPE_MISMATCH,
          "a DIAL SPEC without addr/port refuses TYPE_MISMATCH at the write");
}

void test_op_autowakes_and_dials() {
    std::printf("S5: any op auto-wakes a dormant link — DIALING while in flight, UP on success:\n");
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_engine_module(net, script);
    const script_guard_t guard{script};  // bounded teardown even on a failing test
    (void)node.write(path_t("/net/fake-client/conn"), fake_spec("a", 1));
    tr::net::transport_t* const link = net.link_of("net/fake-client/a");
    check(link != nullptr, "link_of answers the engine link");
    check(!link->link_up(), "link_up() is false while dormant (#1059)");

    // The op blocks in the engine while the factory is gated — DIALING is held open.
    const std::byte frame[1] = {std::byte{0x00}};
    auto op = std::async(std::launch::async, [&] { link->send(frame); });
    check(script.await_attempts(1), "the send triggered a dial (the factory ran)");
    check(await_state(node, "/net/fake-client/a", link_state_t::DIALING),
          "the vertex publishes DIALING (0x01) while the attempt is in flight");
    script.script(true);
    op.get();
    check(await_state(node, "/net/fake-client/a", link_state_t::UP),
          "the vertex publishes UP (0x03) on the successful dial");
    check(link->link_up(), "link_up() is true once UP");
    check(script.built.size() == 1 && script.built[0]->sent.load() == 1,
          "the woken op was SERVED on the new socket (not dropped)");
    check(dropped_tx(net, "net/fake-client/a") == 0, "nothing was dropped on the wake path");

    // While UP, ops go straight through — no second dial.
    link->send(frame);
    check(script.built[0]->sent.load() == 2, "an op on an UP link sends directly");
    {
        const std::lock_guard l(script.m);
        check(script.attempts == 1, "no re-dial happened for an op on an UP link");
    }
    check(script.last.connect_timeout_ms == tr::net::kDefaultConnectTimeoutMs,
          "an absent connect_timeout resolves to the engine default (RFC-0014 §4)");
}

void test_lone_oneshot_failure_redormants() {
    std::printf("S5: a lone one-shot's failed dial re-dormants — NO background retry:\n");
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_engine_module(net, script);
    const script_guard_t guard{script};  // bounded teardown even on a failing test
    (void)node.write(path_t("/net/fake-client/conn"), fake_spec("a", 1));
    tr::net::transport_t* const link = net.link_of("net/fake-client/a");

    script.script(false);  // pre-queued: the attempt concludes without blocking
    const std::byte frame[1] = {std::byte{0x00}};
    link->send(frame);  // returns once the one attempt concluded: link-down, dropped
    check(dropped_tx(net, "net/fake-client/a") == 1, "the op was dropped (link-down) and counted");
    check(await_state(node, "/net/fake-client/a", link_state_t::DORMANT),
          "the vertex re-publishes DORMANT — refcount 0 means no self-heal (RFC-0014 §4)");
    {
        const std::lock_guard l(script.m);
        check(script.attempts == 1, "exactly ONE attempt ran — no background retry began");
    }
}

void test_loss_at_refcount_zero_redormants() {
    std::printf("S5: loss while UP at refcount 0 → DORMANT, NO background retry (§4.1 MUST 2):\n");
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_engine_module(net, script);
    const script_guard_t guard{script};  // bounded teardown even on a failing test
    (void)node.write(path_t("/net/fake-client/conn"), fake_spec("a", 1));
    tr::net::transport_t* const link = net.link_of("net/fake-client/a");

    // Wake by op only — NO acquire, so the socket comes up with refcount 0 the whole
    // time. Keeping it up past the op is §4.1's MAY, which this engine exercises.
    script.script(true);  // pre-queued: the attempt concludes without blocking
    const std::byte frame[1] = {std::byte{0x00}};
    link->send(frame);
    check(await_state(node, "/net/fake-client/a", link_state_t::UP),
          "the op-woken link is UP with no standing binding (§4.1's MAY: up until loss)");
    check(script.built.size() == 1 && script.built[0]->sent.load() == 1, "the op was SERVED");

    // The remote hangs up with nothing holding the link: dormant, and the retry loop must
    // never start (§4.1 MUST 1/MUST 2 — the arm the S7 vectors pin).
    script.built[0]->die();
    check(await_state(node, "/net/fake-client/a", link_state_t::DORMANT),
          "loss at refcount 0 re-publishes DORMANT — no self-heal (RFC-0014 §4.1)");
    // The reap is the bounded POSITIVE rendezvous for "the engine has finished handling
    // the loss"; with backoff at 1 ms a retry loop would have dialed many times over by
    // the time the dead socket is destroyed. Same backstop shape as the release path.
    const auto reap_deadline = std::chrono::steady_clock::now() + 10s;
    while (g_socks_alive.load() != 0 && std::chrono::steady_clock::now() < reap_deadline) {
        std::this_thread::yield();  // the worker reaps off-thread; bounded backstop
    }
    check(g_socks_alive.load() == 0, "the dead socket was destroyed (no socket at rest)");
    {
        const std::lock_guard l(script.m);
        check(script.attempts == 1, "exactly ONE attempt ever ran — no background retry began");
    }

    // Dormant again means dormant in full: the NEXT op wakes it exactly as the first did.
    script.script(true);
    link->send(frame);
    check(await_state(node, "/net/fake-client/a", link_state_t::UP),
          "an op after the loss re-wakes the link");
    {
        const std::lock_guard l(script.m);
        check(script.attempts == 2, "the fresh dial is the op's own — the second attempt overall");
    }
    check(script.built.size() == 2 && script.built[1]->sent.load() == 1,
          "the post-loss op was served on a freshly constructed socket");
}

void test_standing_binding_selfheals() {
    std::printf("S5: loss while a standing binding holds → RECONNECTING, backoff retry, UP:\n");
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_engine_module(net, script);
    const script_guard_t guard{script};  // bounded teardown even on a failing test
    (void)node.write(path_t("/net/fake-client/conn"),
                     fake_spec("a", /*backoff_ms=*/1, /*connect_timeout_ms=*/8000));

    // The standing binding (RFC-0014 §4): acquire kicks the dormant link toward UP.
    check(net.acquire_link("net/fake-client/a").has_value(), "acquire_link takes the hold");
    check(script.await_attempts(1), "the acquire kicked a dial");
    script.script(true);
    check(await_state(node, "/net/fake-client/a", link_state_t::UP), "the link comes UP");

    // Remote hangup: the engine must self-heal, not evict. First retry FAILS (backoff
    // path), second succeeds.
    script.built[0]->die();
    check(await_state(node, "/net/fake-client/a", link_state_t::RECONNECTING),
          "loss with refcount > 0 publishes RECONNECTING (0x02)");
    check(script.await_attempts(2), "the self-heal loop dialed again");
    // While the retry attempt is gated in the factory, ops FAIL FAST — never block on a
    // dead peer (RFC-0014 §4). The elapsed bound is what distinguishes fail-fast from a
    // timeout-drop: this connection's connect_timeout is 8 s (see fake_spec below), so a
    // send that waited out the in-flight attempt would take all of it, while the genuine
    // fail-fast path performs no wait at all — half the timeout is margin, not a window.
    const std::uint64_t drops_before = dropped_tx(net, "net/fake-client/a");
    const std::byte frame[1] = {std::byte{0x00}};
    const auto t0 = std::chrono::steady_clock::now();
    net.link_of("net/fake-client/a")->send(frame);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    check(dropped_tx(net, "net/fake-client/a") == drops_before + 1,
          "an op on a RECONNECTING link is dropped and counted");
    check(elapsed < 4s, "and it failed FAST — no connect_timeout wait on a healing link");
    script.script(false);  // retry #1 concludes down → backoff (1 ms)
    check(script.await_attempts(3), "the loop retried after the backoff interval");
    script.script(true);  // retry #2 succeeds
    check(await_state(node, "/net/fake-client/a", link_state_t::UP),
          "self-heal reaches UP again (up → reconnecting → up)");
    check(script.built.size() == 2, "the heal constructed a fresh socket");

    // The last release closes the healthy socket: refcount → 0 → close, dormant (§4).
    check(net.release_link("net/fake-client/a").has_value(), "release_link drops the hold");
    check(await_state(node, "/net/fake-client/a", link_state_t::DORMANT),
          "the last release re-dormants the link");
    const auto reap_deadline = std::chrono::steady_clock::now() + 10s;
    while (g_socks_alive.load() != 0 && std::chrono::steady_clock::now() < reap_deadline) {
        std::this_thread::yield();  // the worker reaps off-thread; bounded backstop
    }
    check(g_socks_alive.load() == 0, "the closed socket was destroyed (no socket at rest)");
}

void test_release_during_heal_stops_retry() {
    std::printf("S5: the last release DURING a heal stops the retry loop (dormant, no dial):\n");
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_engine_module(net, script);
    const script_guard_t guard{script};  // bounded teardown even on a failing test
    (void)node.write(path_t("/net/fake-client/conn"), fake_spec("a", 1));

    check(net.acquire_link("net/fake-client/a").has_value(), "acquire_link takes the hold");
    check(script.await_attempts(1), "the acquire kicked a dial");
    script.script(true);
    check(await_state(node, "/net/fake-client/a", link_state_t::UP), "the link comes UP");
    script.built[0]->die();
    check(await_state(node, "/net/fake-client/a", link_state_t::RECONNECTING), "the heal begins");
    check(script.await_attempts(2), "a retry attempt is in flight (gated in the factory)");
    // Release while the attempt is in flight: the loop must stop at its next gate.
    check(net.release_link("net/fake-client/a").has_value(), "release during the heal");
    script.script(false);  // the in-flight attempt concludes down
    check(await_state(node, "/net/fake-client/a", link_state_t::DORMANT),
          "refcount 0 at the gate → dormant, stop retrying (RFC-0014 §4)");
    {
        const std::lock_guard l(script.m);
        check(script.attempts == 2, "no further attempt ran after the last release");
    }
}

void test_remove_while_healing_tears_down() {
    std::printf("S5: NAME-removal during a heal stops the engine and retires the vertex:\n");
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_engine_module(net, script);
    const script_guard_t guard{script};  // bounded teardown even on a failing test
    (void)node.write(path_t("/net/fake-client/conn"), fake_spec("a", 1));

    check(net.acquire_link("net/fake-client/a").has_value(), "acquire_link takes the hold");
    check(script.await_attempts(1), "the acquire kicked a dial");
    script.script(false);  // first attempt fails → heal loop (refs > 0)
    check(await_state(node, "/net/fake-client/a", link_state_t::RECONNECTING),
          "the engine is healing");
    // The engine joins its in-flight attempt on teardown, so the script must keep
    // answering: arm auto-fail BEFORE the remove (a real factory's own connect deadline
    // is what bounds this wait in production — the join is bounded, not instant).
    {
        const std::lock_guard l(script.m);
        script.auto_fail = true;
        script.cv.notify_all();
    }
    const auto rm = node.write(path_t("/net/fake-client/conn"), tr::net::conn_remove("a"));
    check(rm.has_value(), "NAME{a} removes the healing connection (hard teardown)");
    check(!node.find(path_t::parse("/net/fake-client/a")->key()).has_value(),
          "the vertex is retired — no ghost engine keeps publishing");
    check(g_socks_alive.load() == 0, "no socket survives the removal");
}

}  // namespace

int main() {
    std::printf("== RFC-0014 §4 S5: the link-liveness engine (#492) ==\n");
    test_engine_creation_is_dormant();
    test_op_autowakes_and_dials();
    test_lone_oneshot_failure_redormants();
    test_loss_at_refcount_zero_redormants();
    test_standing_binding_selfheals();
    test_release_during_heal_stops_retry();
    test_remove_while_healing_tears_down();
    return tr::testing::summary("link_liveness");
}
