/**
 * @file
 * @brief #1071 — a departing WS peer's eviction must cost that peer's own edges, not the
 *        graph's whole subscribed set, on the task that serves every other session.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the other host suites over this link: the REAL chip translation unit
 * (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against the `esp_http_server`
 * fake, so a peer hanging up is a real `free_ctx` on the server's own task. What is different
 * here is that a real `graph_t` and `fwd_router_t` sit behind it, because the cost under test
 * is paid in the graph: the link's departure notifier reaches `fwd_router_t::link_down`, which
 * calls `graph_t::evict_link_edges`.
 *
 * The regression is a LATENCY one and latency is not directly assertable, so the instrument is
 * `graph_t::link_edge_candidates` — the number of vertices that eviction will visit. Before the
 * per-link index it was "every vertex in the graph holding any subscriber edge", so ONE browser
 * tab's hangup was priced by every OTHER peer's subscriptions, synchronously, inside `free_ctx`,
 * on the single task that owns accept/receive/close for the whole server. That is the shape the
 * bystanders below exist to create.
 *
 * The two halves of the chain are pinned in different places on purpose, and neither is
 * duplicated here: that a departure REACHES the notifier at all is
 * `httpd_ws_eviction_lock_test`'s subject (it asserts the notifier fires naming `p0`), and that
 * the eviction is exact is `edge_eviction_test`'s. This file pins the composition — a real
 * hangup on the server task, through the real link, landing in a bounded eviction.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_httpd.hpp"
#include "fwd_frame_builder.hpp"
#include "graph_sinks.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"
#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::httpd_ws_link_t;
using tr::testing::b_path;
using tr::testing::make_value;
using tr::view::rope_t;

/** @brief Poll granularity for the flags the two tasks hand each other. */
constexpr auto kTick = 1ms;
/** @brief How long a flag the other task is expected to set is waited on. */
constexpr auto kPatience = 5000ms;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A SUBSCRIBER TLV carrying @p marker as its PATH — the minimum a bind needs. */
std::vector<std::byte> b_subscriber(std::string_view marker) {
    const std::vector<std::byte> body = b_path({marker});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::SUBSCRIBER, tr::wire::opt_t{.pl = true}, body);
    return out;
}

/** @brief Bind one remote subscriber at @p v admitted over @p link. */
bool wire_sub(graph_t& g, vertex_handle_t v, std::string_view link, std::string_view marker) {
    return g
        .subscribe_wire(v, make_value(b_subscriber(marker)),
                        make_value(b_path({std::string(link)})), std::string(link))
        .has_value();
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/**
 * @brief A thread standing in for the `esp_http_server` task: it, and only it, drains the
 *        control queue, so anything posted to it runs there — as on silicon, where `free_ctx`
 *        fires on that one task. Same stand-in the sibling host suites use.
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

void test_departure_cost_is_bounded_by_the_departing_peer() {
    std::printf("#1071 — a WS peer's departure costs its OWN edges:\n");
    server_task_t task;
    graph_t g;
    fwd_router_t router(g);
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, /*peer_named=*/true);
    check(link->ok(), "the adopting link registered its URI");
    // add_child installs the bus peer-down notifier, so a session's departure reaches
    // fwd_router_t::link_down under the peer's ROUTABLE name (#994) — `p<slot>`.
    check(router.add_child("ws", *link), "the link is registered as a routed bus child");

    // The departing peer subscribes on ONE vertex.
    vertex_handle_t mine = g.register_vertex(path_t("/mine"), role_t::STORED_VALUE);
    std::size_t mine_hits = 0, bystander_hits = 0;
    const tr::testing::remote_sink_guard_t sink_guard(
        g, [&](const tr::graph::remote_delivery_t& d, const rope_t&) {
            (d.link == "p0" ? mine_hits : bystander_hits) += 1;
        });
    claim_session(task, 700);  // lands in slot 0 ⇒ routable name "p0"
    claim_session(task, 701);  // a second live session, so the close is not the last one
    check(wire_sub(g, mine, "p0", "m0"), "the peer that will depart subscribes on /mine");
    check(g.link_edge_candidates("p0") == 1, "its departure visits exactly one vertex");

    // Forty bystander vertices subscribed by OTHER peers — the graph the old walk charged
    // this peer's hangup for. Nothing here belongs to p0.
    for (int i = 0; i < 40; ++i) {
        const std::string p = "/bystander" + std::to_string(i);
        vertex_handle_t b = g.register_vertex(path_t(p.c_str()), role_t::STORED_VALUE);
        check(wire_sub(g, b, "other" + std::to_string(i), "b"), "a bystander peer subscribes");
    }
    check(g.link_edge_candidates("p0") == 1,
          "40 bystander subscriptions later, the departure STILL visits one vertex");

    check(g.write(path_t("/mine"), rope_t{make_value({0x01})}).has_value(),
          "write /mine while the peer is up");
    check(mine_hits == 1, "the departing peer's edge is live (the test is not vacuous)");

    // THE DEPARTURE. On the server task, as httpd's own close path does it: close_session
    // runs free_ctx, which notifies, which evicts.
    mine_hits = 0;
    fake_httpd::instance().post([] { fake_httpd::instance().close_session(700); });
    check(wait_for([&] { return g.link_edge_candidates("p0") == 0; }, kPatience),
          "the hangup reached the eviction and cleared the peer's candidates");

    check(g.write(path_t("/mine"), rope_t{make_value({0x02})}).has_value(),
          "write /mine after the hangup");
    check(mine_hits == 0, "the departed peer's edge was reclaimed — it receives nothing");

    // The bystanders are untouched: this was a scoped eviction, not a graph-wide sweep.
    bystander_hits = 0;
    check(g.write(path_t("/bystander0"), rope_t{make_value({0x03})}).has_value(),
          "write a bystander's vertex after the hangup");
    check(bystander_hits == 1, "a bystander peer's edge still delivers");
    check(g.link_edge_candidates("other0") == 1, "and its own candidate list is intact");

    task.run_on_task([] { fake_httpd::instance().close_all(); });
    link.reset();
}

}  // namespace

int main() {
    std::printf("== httpd_ws_departure_cost_test ==\n");
    test_departure_cost_is_bounded_by_the_departing_peer();
    std::printf("%s\n", g_failures == 0 ? "httpd_ws_departure_cost: ALL PASS (0 failures)"
                                        : "httpd_ws_departure_cost: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
