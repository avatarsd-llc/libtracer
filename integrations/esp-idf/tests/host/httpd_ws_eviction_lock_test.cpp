/**
 * @file
 * @brief #960 — the peer-departure eviction notifier must NOT run under the handler gate.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the other four host suites over this link: the REAL chip
 * translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against
 * the `esp_http_server` fake, so the session table runs the link's `free_ctx` on a task
 * of its own and a peer hanging up is a real call into `on_session_closed`.
 *
 * The property under test is a LOCK DISCIPLINE, and it has two halves that pull in
 * opposite directions — which is why they are pinned together:
 *
 *   1. the gate's MUTEX is released across the notification. `gate_t::m` is the mutex
 *      every dispatch into this link takes (the URI handler, the send override, the
 *      queued send) and the one a destructor blocks on; `fwd_router_t::link_down` behind
 *      the notifier walks every subscribed vertex under the graph's own locks and is
 *      bounded by nothing this link owns. `bus_link_t::notify_peer_down` documents the
 *      precondition outright ("with none of its internal locks held"), and both core
 *      reference servers honour it — one implementation since #871,
 *      `slot_server_t::teardown_slot`, inherited by both. This link did not.
 *
 *   2. the destructor still JOINS a notification in flight. That is the lifetime half:
 *      the notifier dereferences the link and hands a name to the routing plane, whose
 *      ctx `set_peer_down_notifier` requires to outlive every possible notification. The
 *      fix moves that wait from the mutex to the gate's existing `depth`/`cv` barrier —
 *      the same one a URI-handler frame registers on — so it must not have gone missing.
 *
 * Half 1 is what reddens without the fix. Half 2 holds in BOTH states by construction and
 * is here as a regression pin, not as evidence: a later change that fires the notifier
 * with the gate released AND without registering on the barrier would pass half 1 and
 * fail half 2.
 *
 * A third case shares this file because it is the same seam seen from the other mode
 * (#889): constructed FLAT, this link's only eviction hook is the WHOLE link's, so it must
 * fire on the LAST session's departure and on no earlier one.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "fake_httpd.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::httpd_ws_link_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/**
 * @brief How long the parked notifier waits for the test to release it.
 *
 * A SAFETY valve, not a measurement: it exists so a failing run terminates instead of
 * wedging. It must stay far larger than @ref kObserverWindow — an early version had the
 * two equal, and the notifier's deadline then expired FIRST, unblocking the observer just
 * in time to make the assertion pass against the unfixed link. The margin is the whole
 * instrument.
 */
constexpr auto kSafety = 5000ms;
/**
 * @brief How long the observer is given to acquire the handler gate — the measurement.
 *
 * Generous against scheduling noise (the acquisition is uncontended once the notifier
 * stops holding it) and far inside @ref kSafety, so expiry can only mean "the notifier is
 * still holding the mutex".
 */
constexpr auto kObserverWindow = 400ms;
/** @brief How long a flag the other task is expected to set is waited on. */
constexpr auto kPatience = 1500ms;
/** @brief Poll granularity for the flags the two tasks hand each other. */
constexpr auto kTick = 1ms;

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/**
 * @brief A thread standing in for the `esp_http_server` task: it, and only it, drains the
 *        control queue, so anything posted to it runs there — as on silicon, where
 *        `free_ctx` fires on that one task.
 */
class server_task_t {
   public:
    server_task_t() : th_([this] { loop(); }) {}
    ~server_task_t() {
        run_.store(false);
        th_.join();
    }
    /** @brief Post @p action to the task and block until it has finished. */
    void run_on_task(std::function<void()> action) {
        std::atomic<bool> done{false};
        fake_httpd::instance().post([&] {
            action();
            done.store(true);
        });
        while (!done.load()) std::this_thread::sleep_for(kTick);
    }

   private:
    void loop() {
        while (run_.load()) {
            (void)fake_httpd::instance().run_pending();
            std::this_thread::sleep_for(kTick);
        }
    }
    std::atomic<bool> run_{true};
    std::thread th_;
};

/** @brief A minimal frame body — the link only has to accept and route it. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/** @brief Deliver one frame to @p fd through the route its session latched. */
esp_err_t deliver(int fd) { return fake_httpd::instance().deliver_frame(fd, kBody); }

/** @brief Admit @p fd and claim it as a peer, on the server task. */
void claim_session(server_task_t& task, int fd) {
    fake_httpd::instance().open_session(fd);
    task.run_on_task([fd] { (void)deliver(fd); });
}

/** @brief Spin until @p pred holds or @p bound elapses; true iff it held. */
template <typename F>
bool wait_for(F pred, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(kTick);
    }
    return true;
}

/**
 * @brief What the test's stand-in for `fwd_router_t::link_down` records and blocks on.
 *
 * The real notifier walks the graph; this one just refuses to return until the test says
 * so, which is the only property that matters here — it makes "while the notifier is in
 * flight" a window an observer on another task can be measured inside of.
 */
struct probe_t {
    std::atomic<unsigned> calls{0};   /**< @brief Departure notifications observed. */
    std::atomic<bool> entered{false}; /**< @brief The notifier is in flight right now. */
    std::atomic<bool> release{false}; /**< @brief Set by the test to let it return. */
    std::atomic<bool> saw{false};     /**< @brief What it observed just before returning. */
    std::string peer;                 /**< @brief The name it was handed. */
};

/** @brief The `peer_down_fn_t` thunk installed on the link. */
void on_peer_down(void* ctx, tr::net::peer_handle_t, std::string_view peer) {
    auto* const p = static_cast<probe_t*>(ctx);
    p->peer = std::string(peer);
    p->calls.fetch_add(1);
    p->entered.store(true);
    p->saw.store(wait_for([p] { return p->release.load(); }, kSafety));
    p->entered.store(false);
}

// ---------------------------------------------------------------------------
// Half 1 — the gate's mutex is RELEASED across the notification (the redden).
// ---------------------------------------------------------------------------
void test_gate_released_across_notifier() {
    std::printf("departure notifier vs the handler gate:\n");
    server_task_t task;
    probe_t probe;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, /*peer_named=*/true);
    check(link->ok(), "the adopting link registered its URI");
    link->set_peer_down_notifier(&on_peer_down, &probe);

    claim_session(task, 700);
    claim_session(task, 701);
    const unsigned before = probe.calls.load();

    // Peer 700 hangs up ON THE SERVER TASK, as httpd's own close path does. Posted rather
    // than run_on_task: the notifier parks that task, and this thread is the observer.
    fake_httpd::instance().post([] { fake_httpd::instance().close_session(700); });
    check(wait_for([&] { return probe.entered.load(); }, kPatience),
          "the departed peer's session close reached the eviction notifier");
    check(probe.calls.load() > before, "the notification fired for the departure");
    // `p0` — the departure seam carries the ROUTABLE name (#994), which is what a listener
    // would have to spell to act on the departure. Peer 700 landed in slot 0.
    check(probe.peer == "p0", "it named the peer that departed, not another one");

    // THE MEASUREMENT. A second acquirer of `gate_t::m`, on another task, while the
    // notifier is still in flight. The URI handler's admission take is the door the fake
    // can reach; on silicon the second acquirer is the destructor's close_gate, whose
    // block is not separately observable because it must wait for the notification
    // anyway. Under the defect this call sits on the mutex for the notifier's whole
    // duration, so `arrived` is still false when the notifier looks.
    std::atomic<bool> arrived{false};
    std::thread observer([&] {
        (void)deliver(701);
        arrived.store(true);
    });
    const bool inside = wait_for([&] { return arrived.load(); }, kObserverWindow);
    probe.release.store(true);  // AFTER the measurement, never as part of it
    observer.join();
    check(inside, "a second acquirer of the handler gate got through WHILE the notifier ran");
    check(wait_for([&] { return !probe.entered.load(); }, kPatience),
          "the notifier returned once the test released it");
    check(probe.saw.load(), "it was released by the test, not by its own safety valve");

    // The observer's frame latched `server_task_` to the observer thread (ws_handler does
    // that from wherever it runs). Put it back before the teardown, or close_gate would
    // mistake this thread for the server's and skip its join.
    task.run_on_task([] { (void)deliver(701); });
    link.reset();
    task.run_on_task([] { fake_httpd::instance().close_all(); });
}

// ---------------------------------------------------------------------------
// Half 2 — the destructor still JOINS a notification in flight (the pin).
// ---------------------------------------------------------------------------
void test_dtor_joins_the_notification() {
    std::printf("destructor vs a departure notification in flight:\n");
    server_task_t task;
    probe_t probe;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, /*peer_named=*/true);
    link->set_peer_down_notifier(&on_peer_down, &probe);
    claim_session(task, 800);

    fake_httpd::instance().post([] { fake_httpd::instance().close_session(800); });
    check(wait_for([&] { return probe.entered.load(); }, kPatience),
          "the departure notifier is in flight");

    std::atomic<bool> dtor_returned{false};
    std::thread destroyer([&] {
        link.reset();
        dtor_returned.store(true);
    });
    // Give the destructor a real chance to get it wrong: it is either on the gate's mutex
    // (pre-fix) or on the gate's condition variable (post-fix), and both must hold it.
    std::this_thread::sleep_for(150ms);
    check(!dtor_returned.load(),
          "the destructor did NOT return while the notifier was still running");

    probe.release.store(true);
    check(wait_for([&] { return dtor_returned.load(); }, kPatience),
          "and it returns once the notification completes");
    destroyer.join();
    task.run_on_task([] { fake_httpd::instance().close_all(); });
}

// ---------------------------------------------------------------------------
// #889 — a FLAT link reports the whole link down only on the LAST departure.
// ---------------------------------------------------------------------------

/** @brief The `transport_t::down_fn_t` thunk: counts WHOLE-LINK down notifications. */
void on_link_down(void* ctx) { static_cast<std::atomic<unsigned>*>(ctx)->fetch_add(1); }

/**
 * @brief #889 — the flat half of the same departure seam the two halves above pin.
 *
 * Constructed FLAT (`peer_named=false`, the default) this link has no per-peer routing
 * identity: every session it carries answers to the ONE registered child NAME, so its only
 * eviction seam is `transport_t::notify_down`, which `fwd_router_t::link_down` answers by
 * dropping every subscriber edge under that name. Both defaults that make this reachable
 * are the defaults — flat mode and `max_peers = 0` — so a two-tab deployment that never
 * asked for the bus facet had one tab's hangup evict the other tab's subscriptions.
 *
 * The notifier must therefore stay silent while any session is still open and fire exactly
 * once when the last one goes. Same rule as `slot_server_t::teardown_slot`, in the link
 * that cannot inherit it.
 */
void test_flat_link_down_only_on_last_session() {
    std::printf("flat link: notify_down only on the LAST departure (#889):\n");
    server_task_t task;
    std::atomic<unsigned> downs{0};
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, /*peer_named=*/false);
    check(link->ok(), "the adopting link registered its URI");
    check(link->bus() == nullptr, "a flat link exposes no bus facet");
    link->set_down_notifier(&on_link_down, &downs);

    // Both sessions provably claimed before anything closes — an assertion about "the last
    // session" is vacuous if the second peer never got admitted.
    claim_session(task, 900);
    claim_session(task, 901);
    std::size_t open_peers = 0;
    link->enumerate_peers([&](std::string_view) { ++open_peers; });
    check(open_peers == 2, "both peers are open on the flat link");

    // --- the mid-life close ---
    task.run_on_task([] { fake_httpd::instance().close_session(900); });
    check(wait_for(
              [&] {
                  std::size_t n = 0;
                  link->enumerate_peers([&](std::string_view) { ++n; });
                  return n == 1;
              },
              kPatience),
          "the departed peer's slot was reclaimed (its departure seam ran)");
    check(downs.load() == 0, "a mid-life close did NOT report the whole link down (#889)");

    // …and the survivor still routes: a frame from it is still accepted end to end.
    // `check` is called back HERE, not inside the posted action: g_failures belongs to
    // this thread, and the suite's other cases keep it that way too.
    esp_err_t survivor_rc = ESP_FAIL;
    task.run_on_task([&] { survivor_rc = deliver(901); });
    check(survivor_rc == ESP_OK, "the surviving peer still routes after the close");

    // --- the last close: NOW the link is down, exactly once ---
    task.run_on_task([] { fake_httpd::instance().close_session(901); });
    check(wait_for([&] { return downs.load() >= 1; }, kPatience),
          "the LAST session's departure reported the link down");
    check(downs.load() == 1, "…exactly once");

    link.reset();
    task.run_on_task([] { fake_httpd::instance().close_all(); });
}

}  // namespace

int main() {
    test_gate_released_across_notifier();
    test_dtor_joins_the_notification();
    test_flat_link_down_only_on_last_session();
    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
