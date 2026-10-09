/**
 * @file
 * @brief A transport vertex retires its connection handler and drains in-flight calls on
 *        destruction.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `register_module` mints the `<net_root>/<module>/conn` creator endpoint, a HANDLER vertex
 * whose `on_write` context belongs to the `transport_vertex_t`. The graph outlives the plane,
 * and so can a link the plane does not own: an app-owned listener wired straight into the
 * router keeps delivering creation writes to that endpoint for as long as its receive thread
 * runs. So the plane's destructor owes the graph two things before its state goes: the
 * endpoint must stop resolving (a write that arrives afterwards answers like any other absent
 * path), and a write already inside the endpoint must finish first.
 *
 * Each round builds a plane, lets two app-owned listener threads dial its endpoint through
 * the router, and destroys the plane while they are still dialling — then lets them keep
 * dialling the now-plane-less graph. Built for the sanitizer lanes (`-fsanitize=address` and
 * `-fsanitize=thread`, the latter under `setarch $(uname -m) -R` here), where a write that
 * reached the endpoint's context after the plane was gone is reported. Without a sanitizer
 * the post-destruction checks still hold: the endpoint is absent, and a dial answers false.
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/conn_spec.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/path.hpp"
#include "libtracer/transport_vertex.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;

using tr::testing::check;

/** @brief The app-owned listener: registered on the router by the app, never by the plane. */
struct listener_t : tr::net::transport_t {
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
};

/** @brief The app-owned listener's router name — outside the plane's `/net` subtree. */
constexpr std::string_view kListener = "app/up";

/** @brief Plane lifetimes the test runs; each one is destroyed under a live dial. */
constexpr int kRounds = 200;

/**
 * @brief `FWD{ op=WRITE, dst=/net/m/conn, src, SPEC{ name = @p name } }` — a creation dial,
 *        as the listener's receive thread hands it to the router.
 */
std::vector<std::byte> dial_frame(std::string_view name) {
    const std::vector<std::byte> payload = tr::net::conn_spec_t(name).bytes();
    return tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE, tr::testing::b_path({"net", "m", "conn"}),
                              tr::testing::b_path({"reply"}), {}, payload);
}

/**
 * @brief The routed dial reaches the endpoint at all — the non-vacuity check for the rounds
 *        below: a staged link becomes a connection only through `/net/m/conn`'s handler.
 */
void routed_dial_reaches_the_endpoint() {
    graph_t g;
    fwd_router_t router(g);
    listener_t up;
    (void)router.add_child(kListener, up);
    listener_t staged;
    transport_vertex_t net(g, router);
    check(net.register_module("m", "", conn_role_t::DIAL).has_value(), "module m is declared");
    net.provide_link("m", "x", staged);
    router.on_frame(kListener, dial_frame("x"));
    check(net.settings_of("net/m/x") != nullptr,
          "a dial from the app-owned listener creates through the endpoint's handler");
    (void)router.remove_child(kListener);
}

/** @brief Destroy the plane while two app-owned listener threads dial its endpoint. */
void destruction_under_live_dial() {
    const path_t endpoint("/net/m/conn");
    // A name nothing stages: every dial runs the handler and is refused there, so the
    // endpoint is exercised on every frame without growing the connection table.
    const std::vector<std::byte> frame = dial_frame("absent");
    int still_resolving = 0;
    int answered_after = 0;
    for (int r = 0; r < kRounds; ++r) {
        graph_t g;
        fwd_router_t router(g);
        listener_t up;
        (void)router.add_child(kListener, up);
        auto net = std::make_unique<transport_vertex_t>(g, router);
        check(net->register_module("m", "", conn_role_t::DIAL).has_value(), "module m is declared");

        std::atomic<bool> stop{false};
        std::atomic<int> dials{0};
        const auto dialler = [&] {
            while (!stop.load(std::memory_order_relaxed)) {
                router.on_frame(kListener, frame);
                dials.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread a(dialler);
        std::thread b(dialler);
        // Destroy only once both listeners are demonstrably dialling, so the destructor
        // races live handler calls rather than an idle endpoint.
        while (dials.load(std::memory_order_relaxed) < 8) std::this_thread::yield();
        net.reset();
        // And keep dialling the plane-less graph for a while: the listener does not know the
        // plane is gone, and a write to the endpoint must now answer as an absent path.
        const int at_reset = dials.load(std::memory_order_relaxed);
        while (dials.load(std::memory_order_relaxed) < at_reset + 8) std::this_thread::yield();
        stop.store(true, std::memory_order_relaxed);
        a.join();
        b.join();

        if (g.find(endpoint.key())) ++still_resolving;
        if (g.write(endpoint, tr::net::conn_spec_t("absent").view())) ++answered_after;
        (void)router.remove_child(kListener);
    }
    check(still_resolving == 0, "the destroyed plane's creator endpoint no longer resolves");
    check(answered_after == 0, "a write to the retired endpoint is refused as an absent path");
}

}  // namespace

int main() {
    std::printf("transport_vertex_t destruction retires its connection handler and drains\n");
    routed_dial_reaches_the_endpoint();
    destruction_under_live_dial();
    return tr::testing::summary("transport_vertex_teardown");
}
