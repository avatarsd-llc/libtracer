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
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/conn_spec.hpp"
#include "libtracer/self_heal_link.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_tcp.hpp"
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

/**
 * @brief The FLIP itself (#1548): the BUILT-IN `tcp` kind, end to end through the engine —
 *        a real socket, a real listener, a real remote hangup, a real heal.
 *
 * Every test above scripts a fake kind, which pins the ENGINE but says nothing about whether
 * any kind the library ships actually reaches it — and until this flip none did. This case is
 * the gate on that: it drives `tcp`, registered by the stock `transport_vertex_t` ctor, and
 * asserts the four claims the flip makes.
 *
 *  1. **Creation with the peer DOWN succeeds** and mints the vertex `DORMANT`. This is the
 *     behaviour change embedders see (`core/CHANGELOG.md`): the same SPEC used to answer
 *     `TRANSPORT_DOWN` because the factory ran a synchronous `connect` on the creation path.
 *  2. **The link IS the engine** (`self_heal_link_t`), and it answers `delivers_ropes()` the
 *     way `tcp_transport_t` does — the static declaration in `kBuiltinPointToPointTraits`
 *     has to match the socket it stands in for, because `fwd_router_t::add_child` installs
 *     the matching receiver ONCE, on the engine, before any socket exists.
 *  3. **A standing binding brings a REAL socket up** against a real listener.
 *  4. **FACTORY RE-RUNNABILITY, demonstrated rather than asserted**: the listener is
 *     destroyed under the live connection and a fresh one bound on the same port. The engine
 *     re-runs the built-in `tcp` factory — the same lambda, a second and later time — and the
 *     connection returns to `UP` on a freshly constructed socket, with the vertex never
 *     retired and the routing identity never rebound.
 *
 * The LISTEN half is checked to be UNTOUCHED (RFC-0014 §4: a LISTEN link ignores refcount and
 * binds eagerly) — it is created here and reports `LISTENING` at creation, with a real
 * `transport_tcp_server` behind it, not an engine.
 */
void test_builtin_tcp_kind_runs_through_the_engine() {
    std::printf("S5 flip (#1548): the BUILT-IN `tcp` kind is engine-managed end to end:\n");
    graph_t node;
    fwd_router_t router(node);
    // The FULL ctor — the one that registers the built-in udp/tcp/ws catalog entries.
    transport_vertex_t net(node, router);
    check(net.register_module("tcp-client", "tcp", conn_role_t::DIAL).has_value(),
          "the application declares the tcp DIAL module (ADR-0073 §4)");
    check(net.register_module("tcp-server", "tcp", conn_role_t::LISTEN).has_value(),
          "and the tcp LISTEN module");

    // A port with nothing behind it: bind an ephemeral listener, take the port it was
    // granted, and drop it. A connect there is refused, which is exactly the "peer is down"
    // condition creation used to fail on.
    std::uint16_t port = 0;
    {
        const tr::net::transport_tcp_server probe(0);
        check(probe.ok(), "an ephemeral probe listener bound");
        port = probe.local_port();
    }
    check(port != 0, "the probe reported its granted port");

    // 1. Creation against a DEAD peer.
    tr::net::conn_spec_t spec("a");
    spec.kind("tcp").addr("127.0.0.1").port(port).backoff_ms(5).connect_timeout_ms(4000);
    const auto w = node.write(path_t("/net/tcp-client/conn"), spec.view());
    check(w.has_value(), "SPEC{a, kind=tcp} SUCCEEDS with nothing listening (the #1548 change)");
    check(state_byte(node, "/net/tcp-client/a") == static_cast<std::uint8_t>(link_state_t::DORMANT),
          "the built-in kind's DIAL vertex is minted DORMANT — no socket at creation");

    // 2. The link is the engine, answering the statically declared capability.
    tr::net::transport_t* const link = net.link_of("net/tcp-client/a");
    check(dynamic_cast<tr::net::self_heal_link_t*>(link) != nullptr,
          "the connection's link IS the S5 engine, not a tcp_transport_t");
    check(link != nullptr && link->delivers_ropes(),
          "and it declares delivers_ropes — the value tcp_transport_t itself answers");
    check(!link->link_up(), "link_up() is false while dormant");

    // 3. The peer comes up; a standing binding dials a REAL socket.
    auto peer = std::make_unique<tr::net::transport_tcp_server>(port);
    check(peer->ok(), "a real listener bound the same port");
    check(net.acquire_link("net/tcp-client/a").has_value(), "acquire_link takes the standing hold");
    check(await_state(node, "/net/tcp-client/a", link_state_t::UP),
          "the engine ran the BUILT-IN factory and reached UP over a real TCP socket");
    check(link->link_up(), "link_up() is true once UP");

    // 4. Re-runnability: kill the listener under the live connection, bind a fresh one, and
    // the engine's re-run of the same factory lambda brings the link back.
    peer.reset();
    check(await_state(node, "/net/tcp-client/a", link_state_t::RECONNECTING),
          "the remote hangup is seen and, with a standing binding held, publishes RECONNECTING");
    peer = std::make_unique<tr::net::transport_tcp_server>(port);
    check(peer->ok(), "a replacement listener bound the same port (SO_REUSEADDR)");
    check(await_state(node, "/net/tcp-client/a", link_state_t::UP),
          "the RE-RUN built-in factory produced a fresh live socket — UP again, same vertex");
    check(node.find(path_t::parse("/net/tcp-client/a")->key()).has_value(),
          "the connection vertex was never retired across the heal (stable routing identity)");

    // The last release closes the socket and re-dormants (RFC-0014 §4).
    check(net.release_link("net/tcp-client/a").has_value(), "release_link drops the hold");
    check(await_state(node, "/net/tcp-client/a", link_state_t::DORMANT),
          "the last release closes the socket and re-dormants the built-in link");

    // The LISTEN half of the SAME built-in kind is untouched: eager, and not an engine.
    const auto wl = node.write(
        path_t("/net/tcp-server/conn"),
        tr::net::conn_spec_t("srv").kind("tcp").role(conn_role_t::LISTEN).port(0).view());
    check(wl.has_value(), "SPEC{srv, kind=tcp, role=LISTEN} creates the listener");
    check(state_byte(node, "/net/tcp-server/srv") ==
              static_cast<std::uint8_t>(link_state_t::LISTENING),
          "a LISTEN link still binds EAGERLY and reports LISTENING at creation (RFC-0014 §4)");
    check(
        dynamic_cast<tr::net::transport_tcp_server*>(net.link_of("net/tcp-server/srv")) != nullptr,
        "and its link is the real listener — the flip is DIAL-only");
}

/** @brief The raw bytes of a conformance vector's `input.bin`. */
std::vector<std::byte> vector_bytes(std::string_view case_dir) {
    const std::filesystem::path p =
        std::filesystem::path{LIBTRACER_VECTORS_DIR} / case_dir / "input.bin";
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    return out;
}

/** @brief The whole published VALUE TLV at @p path — the bytes a peer's read serves. */
std::vector<std::byte> published_bytes(graph_t& g, std::string_view path) {
    const auto h = g.find(path_t::parse(path)->key());
    if (!h) return {};
    const auto v = g.read(*h);
    if (!v) return {};
    const auto bytes = (*v)->materialize().bytes();
    return {bytes.begin(), bytes.end()};
}

/**
 * @brief RFC-0014 S7-B — the `conn/liveness-enum` vector bound to the live connection vertex.
 *
 * The harness that scores the vector decodes and re-encodes its five bytes and stops there
 * (HARNESS.md §"What a vector gates"). For this vector that boundary is unusually sharp: a
 * one-byte `VALUE` round-trips in ANY conforming core no matter what the byte means to it, so
 * `input.bin` alone cannot tell `DORMANT = 0x00` from any other assignment of the six states.
 * Reverse the enum outright and the vector still scores `ok` in all three cores. This is where
 * that claim can be false.
 *
 * Two things are asserted, and they are different claims:
 *
 * - **The vector's own bytes** are what the production door emits — not a hand-written TLV that
 *   merely happens to decode the same. A fresh engine-managed `DIAL` creation is read back
 *   through `graph_t::read` and compared to `input.bin` in full, header included, so the
 *   envelope (`VALUE`, `opt = 0`, length 1) is pinned as well as the payload byte.
 * - **All six enumerators**, driven through `set_link_state` — the public liveness door a
 *   provided link reports on — and read back off the vertex. This is the arm that fails when
 *   the mapping moves: the vector file is silent about `UP`, and `UP = 0x03` (not `0x01`) is
 *   exactly the kind of "obvious" reassignment a second core would make unprompted.
 *
 * `BIND_FAILED` (0x05) is driven here through the same public door even though no reference
 * code path assigns it yet — a peer must be able to read it, so the byte is pinned rather than
 * left for the first implementation that needs it to choose.
 *
 * The **transitions** (`dormant→dialing→up`, `up→reconnecting→up`, `listen→listening`) are the
 * engine tests above; the `refcount-0→dormant` clauses are `test_lone_oneshot_failure_redormants`,
 * `test_loss_at_refcount_zero_redormants` and `test_standing_binding_selfheals` — and they pin
 * RFC-0014 §4.1's three MUSTs ONLY, never its MAY. See the HARNESS.md row.
 */
void test_conformance_vectors() {
    std::printf("S7-B: the conn/liveness-enum vector is the live vertex's own value bytes:\n");
    dial_script_t script;  // outlives `net`: the engine's factory copy holds its address
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_engine_module(net, script);
    const script_guard_t guard{script};  // bounded teardown even on a failing test

    const auto vec = vector_bytes("conn/liveness-enum");
    check(vec.size() == 5, "the conn/liveness-enum vector loaded (5 bytes)");

    (void)node.write(path_t("/net/fake-client/conn"), fake_spec("a", 1));
    check(published_bytes(node, "/net/fake-client/a") == vec,
          "a fresh DIAL creation publishes the vector's bytes EXACTLY — envelope included");

    // The six-value sweep. `set_link_state` is the public liveness door (the one a provided
    // link reports through), so this is the production emitter, not a test builder.
    struct case_t {
        link_state_t state;
        std::uint8_t byte;
        const char* name;
    };
    static constexpr case_t kTable[] = {
        {link_state_t::DORMANT, 0x00, "DORMANT"},
        {link_state_t::DIALING, 0x01, "DIALING"},
        {link_state_t::RECONNECTING, 0x02, "RECONNECTING"},
        {link_state_t::UP, 0x03, "UP"},
        {link_state_t::LISTENING, 0x04, "LISTENING"},
        {link_state_t::BIND_FAILED, 0x05, "BIND_FAILED"},
    };
    for (const case_t& c : kTable) {
        std::printf("  link_state_t::%s\n", c.name);
        check(net.set_link_state("net/fake-client/a", c.state).has_value(),
              "... set_link_state publishes the state");
        const auto pub = published_bytes(node, "/net/fake-client/a");
        const bool ok = pub.size() == 5 && pub[0] == std::byte{0x01} && pub[1] == std::byte{0x00} &&
                        pub[2] == std::byte{0x01} && pub[3] == std::byte{0x00} &&
                        pub[4] == std::byte{c.byte};
        check(ok, "... reads back as a 1-byte VALUE carrying RFC-0014 §4's table byte");
    }

    // The enum is SIX values and no seventh: every table byte is distinct, they are exactly
    // 0..5, and nothing sits between them. A phantom seventh state (RFC-0014 §4's prose names
    // a `healing` that no enumerator has ever had — PR #1475's erratum) could not be placed
    // anywhere in this range without moving a byte the vector and the sweep above already pin.
    std::uint32_t seen = 0;
    for (const case_t& c : kTable) seen |= 1u << (static_cast<std::uint8_t>(c.state) & 0x1Fu);
    check(seen == 0x3F, "the six ENUMERATORS are exactly 0x00..0x05 — dense, distinct, no seventh");
    check(static_cast<std::uint8_t>(link_state_t::DORMANT) == 0,
          "DORMANT is the FALSY default the superseded binary set_link_state(name,bool) used");
}

}  // namespace

int main() {
    // The subject is a MODULE (#1470): with `kSelfHealLinks = false` the engine is not in
    // this image and `register_transport_type` refuses the `self_heal_dial` kind every case
    // below registers, so there is nothing here to exercise. State the skip rather than
    // crash about a feature the binary was never built with (the `bus` suites' #1438 shape).
    if constexpr (!tr::net::kSelfHealLinks)
        return tr::testing::skipped("link_liveness",
                                    "kSelfHealLinks = false — the RFC-0014 S5 engine is not "
                                    "in this build");
    std::printf("== RFC-0014 §4 S5: the link-liveness engine (#492) ==\n");
    test_engine_creation_is_dormant();
    test_op_autowakes_and_dials();
    test_lone_oneshot_failure_redormants();
    test_loss_at_refcount_zero_redormants();
    test_standing_binding_selfheals();
    test_release_during_heal_stops_retry();
    test_remove_while_healing_tears_down();
    test_builtin_tcp_kind_runs_through_the_engine();
    test_conformance_vectors();
    return tr::testing::summary("link_liveness");
}
