/**
 * @file
 * @brief A transport vertex retires its connection handler and drains in-flight calls on
 *        destruction.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `register_module` mints the `<net_root>/<module>/conn` creator endpoint, a HANDLER vertex
 * whose `on_write` context belongs to the `transport_vertex_t`, and every connection it makes
 * is a graph vertex and a router child. The graph and the router outlive the plane, so its
 * destructor retires the endpoint once the calls already inside it have returned, and
 * detaches each connection from the router and the graph before closing it.
 *
 * The rounds below build a plane, let two app-owned listener threads dial its endpoint
 * through the router, destroy the plane while they are still dialling, and keep dialling.
 * They check that the endpoint is gone and that a write to it is refused; under the
 * sanitizer lanes (`-fsanitize=address`, and `-fsanitize=thread` under
 * `setarch $(uname -m) -R` here) they also check the drain. The connection checks then
 * forward through a surviving link, and read a bus connection's `:children[]`, after the
 * plane is gone, and expect neither to reach the closed link.
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
#include "libtracer/mem_poly_ptr.hpp"
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

/** @brief A link the plane constructs and owns. It counts every call that reaches it, and it
 *         is a bus, so its connection vertex lists its one peer under `:children[]`. */
struct owned_link_t final : tr::net::transport_t, tr::net::bus_link_t {
    std::atomic<int>& reached;
    explicit owned_link_t(std::atomic<int>& r) noexcept : reached(r) {}
    void send(std::span<const std::byte>) override { reached.fetch_add(1); }
    [[nodiscard]] tr::net::bus_link_t* bus() override { return this; }
    void enumerate_peers(const peer_visitor_t& visit) const override {
        reached.fetch_add(1);
        visit("p0");
    }
    [[nodiscard]] tr::net::transport_t* peer_link(std::string_view peer) override {
        return peer == "p0" ? this : nullptr;
    }
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t peer,
                                             std::span<char>) const override {
        return peer.valid() ? "p0" : std::string_view{};
    }
};

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

/**
 * @brief A plane whose module `m` constructs its links (kind `own`), with one connection,
 *        `/net/m/x`, made through the endpoint and owned by the plane.
 */
std::unique_ptr<transport_vertex_t> plane_owning_x(graph_t& g, fwd_router_t& router,
                                                   std::atomic<int>& reached) {
    auto net = std::make_unique<transport_vertex_t>(g, router);
    std::atomic<int>* const r = &reached;
    net->register_transport_type(
        "own",
        [r](const tr::net::conn_settings_t&, const tr::wire::tlv_node_t*,
            tr::mem::block_source_t& src) -> tr::graph::result_t<tr::net::transport_ptr_t> {
            return tr::mem::make_poly<owned_link_t>(src, *r);
        });
    check(net->register_module("m", "own", conn_role_t::DIAL).has_value(), "module m is declared");
    tr::net::conn_spec_t spec("x");
    spec.kind("own");
    check(g.write(path_t("/net/m/conn"), spec.view()).has_value(),
          "the plane constructs and owns /net/m/x");
    return net;
}

/** @brief A link that outlives the plane forwards to its connection after it is destroyed. */
void surviving_link_forwards_after_destruction() {
    graph_t g;
    fwd_router_t router(g);
    listener_t up;
    (void)router.add_child(kListener, up);
    std::atomic<int> reached{0};
    auto net = plane_owning_x(g, router, reached);
    const std::vector<std::byte> frame =
        tr::testing::b_fwd(tr::graph::fwd_op_t::READ, tr::testing::b_path({"net", "m", "x", "p0"}),
                           tr::testing::b_path({"reply"}));
    router.on_frame(kListener, frame);
    check(reached.load() > 0, "a forward from the surviving link reaches the owned connection");
    net.reset();
    const int at_reset = reached.load();
    router.on_frame(kListener, frame);
    check(reached.load() == at_reset,
          "after destruction the same forward no longer reaches the closed link");
    check(!g.find(path_t("/net/m/x").key()), "and the connection vertex is retired");
    (void)router.remove_child(kListener);
}

/** @brief A bus connection's `:children[]` read after the plane is destroyed. */
void bus_children_read_after_destruction() {
    graph_t g;
    fwd_router_t router(g);
    std::atomic<int> reached{0};
    auto net = plane_owning_x(g, router, reached);
    const path_t children = *path_t::parse("/net/m/x:children[]");
    if constexpr (tr::net::kBusLinks) {
        check(g.read(children).has_value(), "the bus connection lists its peers while it lives");
        check(reached.load() > 0, "and the listing asks the link");
    }
    net.reset();
    const int at_reset = reached.load();
    const auto after = g.read(children);
    check(!after && after.error() == tr::graph::status_t::NOT_FOUND,
          "after destruction its :children[] answers NOT_FOUND");
    check(reached.load() == at_reset, "without asking the closed link");
}

}  // namespace

int main() {
    std::printf("transport_vertex_t destruction retires its connection handler and drains\n");
    routed_dial_reaches_the_endpoint();
    destruction_under_live_dial();
    surviving_link_forwards_after_destruction();
    bus_children_read_after_destruction();
    return tr::testing::summary("transport_vertex_teardown");
}
