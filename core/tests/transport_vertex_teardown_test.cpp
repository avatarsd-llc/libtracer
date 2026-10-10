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
 * plane is gone, and expect neither to reach the closed link. Last, a forward and a listing
 * are each held inside a connection's link while it is removed: the removal shuts the link
 * down and parks it, and only a `collect()` after every router frame that could reach it has
 * left destroys it.
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
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
#include "libtracer/mem_source.hpp"
#include "libtracer/path.hpp"
#include "libtracer/transport_vertex.hpp"
#include "test_support.hpp"

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
/** @brief While alive, allocations are not reported as leaks. */
using lsan_scope_t = __lsan::ScopedDisabler;
#else
/** @brief No leak checker in this build: nothing to disable. */
struct lsan_scope_t {};
#endif

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;

using tr::testing::check;

/** @brief What an `owned_link_t` reports, held outside it so it can be read after it is gone. */
struct probe_t {
    std::atomic<int> reached{0};            /**< @brief Calls that reached the link. */
    std::atomic<bool> shut{false};          /**< @brief `shut_down()` has run. */
    std::atomic<bool> closed{false};        /**< @brief The link's destructor has run. */
    std::atomic<bool> hold{false};          /**< @brief Hold the next call inside the link. */
    std::atomic<bool> entered{false};       /**< @brief A held call is inside the link. */
    std::atomic<bool> closed_inside{false}; /**< @brief The held call saw the link destroyed. */
    std::atomic<bool> stay{false};          /**< @brief Keep the held call inside past shut-down. */

    /** @brief A held call: wait, inside the link, until the removal has dealt with it (and,
     *         while @ref stay is set, until the test lets it go). */
    void hold_inside() {
        if (!hold.exchange(false)) return;
        entered.store(true);
        while (!closed.load() && (!shut.load() || stay.load())) std::this_thread::yield();
        if (closed.load()) closed_inside.store(true);
    }
};

/** @brief A link the plane constructs and owns. It counts every call that reaches it, and it
 *         is a bus, so its connection vertex lists its one peer under `:children[]` until it
 *         is shut down. */
struct owned_link_t final : tr::net::transport_t, tr::net::bus_link_t {
    probe_t& p;
    explicit owned_link_t(probe_t& probe) noexcept : p(probe) {}
    ~owned_link_t() override { p.closed.store(true); }
    void shut_down() override { p.shut.store(true); }
    void send(std::span<const std::byte>) override {
        p.reached.fetch_add(1);
        p.hold_inside();
    }
    [[nodiscard]] tr::net::bus_link_t* bus() override { return this; }
    void enumerate_peers(const peer_visitor_t& visit) const override {
        p.reached.fetch_add(1);
        p.hold_inside();
        if (!p.shut.load()) visit("p0");
    }
    [[nodiscard]] tr::net::transport_t* peer_link(std::string_view peer) override {
        return peer == "p0" ? this : nullptr;
    }
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t peer,
                                             std::span<char>) const override {
        return peer.valid() ? "p0" : std::string_view{};
    }
};

/** @brief A point-to-point link the plane constructs and owns: the egress a bound path names.
 *         It reads its probe through `this` after a held call, so a call that outlived the
 *         link's destruction touches freed memory. */
struct p2p_link_t final : tr::net::transport_t {
    probe_t& p;
    explicit p2p_link_t(probe_t& probe) noexcept : p(probe) {}
    ~p2p_link_t() override { p.closed.store(true); }
    void shut_down() override { p.shut.store(true); }
    void send(std::span<const std::byte>) override {
        p.hold_inside();
        p.reached.fetch_add(1);
    }
};

/** @brief The app-owned listener: registered on the router by the app, never by the plane. */
struct listener_t : tr::net::transport_t {
    /** @brief Hand @p f up this link's receiver, as its receive thread would. */
    void deliver(std::span<const std::byte> f) { rx_.deliver_borrowed(f); }
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
                                                   probe_t& probe) {
    auto net = std::make_unique<transport_vertex_t>(g, router);
    probe_t* const r = &probe;
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

/**
 * @brief A plane whose module `q` constructs point-to-point links (kind `p2p`), with one
 *        connection, `/net/q/y`, made through the endpoint and owned by the plane.
 */
std::unique_ptr<transport_vertex_t> plane_owning_y(graph_t& g, fwd_router_t& router,
                                                   probe_t& probe) {
    auto net = std::make_unique<transport_vertex_t>(g, router);
    probe_t* const r = &probe;
    net->register_transport_type(
        "p2p",
        [r](const tr::net::conn_settings_t&, const tr::wire::tlv_node_t*,
            tr::mem::block_source_t& src) -> tr::graph::result_t<tr::net::transport_ptr_t> {
            return tr::mem::make_poly<p2p_link_t>(src, *r);
        });
    check(net->register_module("q", "p2p", conn_role_t::DIAL).has_value(), "module q is declared");
    tr::net::conn_spec_t spec("y");
    spec.kind("p2p");
    check(g.write(path_t("/net/q/conn"), spec.view()).has_value(),
          "the plane constructs and owns /net/q/y");
    return net;
}

/** @brief A link that outlives the plane forwards to its connection after it is destroyed. */
void surviving_link_forwards_after_destruction() {
    graph_t g;
    fwd_router_t router(g);
    listener_t up;
    (void)router.add_child(kListener, up);
    probe_t p;
    auto net = plane_owning_x(g, router, p);
    const std::vector<std::byte> frame =
        tr::testing::b_fwd(tr::graph::fwd_op_t::READ, tr::testing::b_path({"net", "m", "x", "p0"}),
                           tr::testing::b_path({"reply"}));
    router.on_frame(kListener, frame);
    check(p.reached.load() > 0, "a forward from the surviving link reaches the owned connection");
    net.reset();
    const int at_reset = p.reached.load();
    router.on_frame(kListener, frame);
    check(p.reached.load() == at_reset,
          "after destruction the same forward no longer reaches the closed link");
    check(!g.find(path_t("/net/m/x").key()), "and the connection vertex is retired");
    (void)router.remove_child(kListener);
}

/** @brief A bus connection's `:children[]` read after the plane is destroyed. */
void bus_children_read_after_destruction() {
    graph_t g;
    fwd_router_t router(g);
    probe_t p;
    auto net = plane_owning_x(g, router, p);
    const path_t children = *path_t::parse("/net/m/x:children[]");
    if constexpr (tr::net::kBusLinks) {
        check(g.read(children).has_value(), "the bus connection lists its peers while it lives");
        check(p.reached.load() > 0, "and the listing asks the link");
    }
    net.reset();
    const int at_reset = p.reached.load();
    const auto after = g.read(children);
    check(!after && after.error() == tr::graph::status_t::NOT_FOUND,
          "after destruction its :children[] answers NOT_FOUND");
    check(p.reached.load() == at_reset, "without asking the closed link");
}

/** @brief A heap-backed source a test can close, so the graph's table draws are refused. */
class gate_source_t final : public tr::mem::block_source_t {
   public:
    gate_source_t() noexcept : tr::mem::block_source_t("gate") {}
    /** @brief Serve from the heap unless closed. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return closed ? nullptr : tr::mem::heap_source().try_alloc(bytes, align);
    }
    /** @brief Return to the heap. */
    void release(void* ptr, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(ptr, bytes, align);
    }
    bool closed = false; /**< @brief Refuse every request while set. */
};

/**
 * @brief Remove `/net/m/x` while @p call is held inside its link, on another thread.
 *
 * The handshake is the link's own: the held call waits inside until the removal has either
 * shut the link down or destroyed it, and records which. Shut down is the contract; destroyed
 * under the call is what it must never be.
 */
template <class Call>
void remove_with_call_inside(graph_t& g, transport_vertex_t& net, probe_t& p, Call call) {
    p.hold.store(true);
    std::thread inside(call);
    while (!p.entered.load()) std::this_thread::yield();
    (void)net.remove_connection("net/m/x");
    inside.join();
    check(!p.closed_inside.load(), "the link is not destroyed under a call still inside it");
    check(p.shut.load() && !p.closed.load(), "removal shut it down and kept it");
    g.collect();
    check(p.closed.load(), "and the first collect() after the call left destroys it");
}

/**
 * @brief Remove @p conn while a router frame @p call is held inside its link, and collect
 *        repeatedly before letting it go: the link must outlive every one of those calls.
 */
template <class Call>
void collect_with_frame_inside(graph_t& g, transport_vertex_t& net, probe_t& p, Call call,
                               std::string_view conn = "net/m/x") {
    p.stay.store(true);
    p.hold.store(true);
    std::thread inside(call);
    while (!p.entered.load()) std::this_thread::yield();
    (void)net.remove_connection(conn);
    for (int i = 0; i < 3; ++i) g.collect();
    check(p.shut.load() && !p.closed.load(),
          "three collect() calls with a frame still inside the removed link keep it");
    p.stay.store(false);
    inside.join();
    check(!p.closed_inside.load(), "the link was never destroyed under the frame");
    g.collect();
    check(p.closed.load(), "the first collect() after the frame left destroys it");
}

/** @brief A forward a listener's receiver handed up, held inside the removed link. */
void forward_held_across_collects() {
    graph_t g;
    fwd_router_t router(g);
    listener_t up;
    (void)router.add_child(kListener, up);
    probe_t p;
    auto net = plane_owning_x(g, router, p);
    const std::vector<std::byte> frame =
        tr::testing::b_fwd(tr::graph::fwd_op_t::READ, tr::testing::b_path({"net", "m", "x", "p0"}),
                           tr::testing::b_path({"reply"}));
    collect_with_frame_inside(g, *net, p, [&] { up.deliver(frame); });
    (void)router.remove_child(kListener);
}

/** @brief An app thread's origination, held inside the removed link. */
void origination_held_across_collects() {
    graph_t g;
    fwd_router_t router(g);
    probe_t p;
    auto net = plane_owning_x(g, router, p);
    fwd_router_t::origin_t slot([](void*, const tr::view::rope_t&) {}, nullptr);
    const path_t dst("/net/m/x/p0");
    collect_with_frame_inside(
        g, *net, p, [&] { (void)router.originate(slot, tr::graph::fwd_op_t::READ, dst, {}); });
    (void)router.cancel(slot);
}

/** @brief An app thread's bound send, held inside the removed link by its callback. */
void bound_send_held_across_collects() {
    graph_t g;
    fwd_router_t router(g);
    probe_t p;
    auto net = plane_owning_y(g, router, p);
    const std::optional<tr::wire::path_ref_element_t> e = router.connection_ref("net/q/y");
    check(e.has_value(), "the plane's point-to-point connection is bindable");
    path_t dst("/net/q/y/p0");
    check(e && dst.bind(std::span<const tr::wire::path_ref_element_t>(&*e, 1)),
          "an app path binds to it");
    std::vector<std::byte> frame = tr::testing::b_fwd(
        tr::graph::fwd_op_t::READ, tr::testing::b_path({"p0"}), tr::testing::b_path({"reply"}));
    bool sent = false;
    collect_with_frame_inside(
        g, *net, p,
        [&] {
            sent = router.bound_send(
                dst, tr::graph::acl_right_t::READ,
                [](void* c, tr::net::transport_t& link, std::span<const std::byte>) {
                    link.send(*static_cast<const std::vector<std::byte>*>(c));
                },
                &frame);
        },
        "net/q/y");
    check(sent && p.reached.load() == 1, "the bound send reached the link once");
}

/** @brief An app thread's `with_link` call, held inside the removed link by its callback. */
void with_link_held_across_collects() {
    graph_t g;
    fwd_router_t router(g);
    probe_t p;
    auto net = plane_owning_y(g, router, p);
    const std::vector<std::byte> frame(4, std::byte{0x5a});
    bool found = false;
    collect_with_frame_inside(
        g, *net, p,
        [&] {
            found = net->with_link(
                "net/q/y",
                [](void* c, tr::net::transport_t& link) {
                    link.send(*static_cast<const std::vector<std::byte>*>(c));
                },
                const_cast<std::vector<std::byte>*>(&frame));
        },
        "net/q/y");
    check(found && p.reached.load() == 1, "with_link called back once on the live link");
    check(!net->with_link(
              "net/q/y", [](void*, tr::net::transport_t&) {}, nullptr),
          "with_link of a removed connection does not call back");
}

/** @brief A forward that looked the link up before its removal is still inside it. */
void forward_in_flight_across_removal() {
    graph_t g;
    fwd_router_t router(g);
    listener_t up;
    (void)router.add_child(kListener, up);
    probe_t p;
    auto net = plane_owning_x(g, router, p);
    const std::vector<std::byte> frame =
        tr::testing::b_fwd(tr::graph::fwd_op_t::READ, tr::testing::b_path({"net", "m", "x", "p0"}),
                           tr::testing::b_path({"reply"}));
    remove_with_call_inside(g, *net, p, [&] { router.on_frame(kListener, frame); });
    (void)router.remove_child(kListener);
}

/** @brief A `:children[]` listing that loaded the seam before the retire is still inside. */
void listing_in_flight_across_removal() {
    graph_t g;
    fwd_router_t router(g);
    probe_t p;
    auto net = plane_owning_x(g, router, p);
    const path_t children = *path_t::parse("/net/m/x:children[]");
    remove_with_call_inside(g, *net, p, [&] { (void)g.read(children); });
}

/** @brief A removal whose retire is refused keeps the shut-down link for the graph's life. */
void refused_retire_keeps_the_link() {
    gate_source_t gate;
    graph_t g(gate);
    fwd_router_t router(g);
    probe_t p;
    std::unique_ptr<transport_vertex_t> net;
    {
        // The link outlives a refused retire on purpose, for the graph's lifetime: it is not
        // a leak this test should report.
        [[maybe_unused]] const lsan_scope_t ignore;
        net = plane_owning_x(g, router, p);
    }
    const path_t x("/net/m/x");
    gate.closed = true;
    (void)net->remove_connection("net/m/x");
    gate.closed = false;
    check(g.find(x.key()).has_value(), "the retire was refused (the vertex is still registered)");
    check(p.shut.load(), "the removal shut the link down anyway");
    g.collect();
    g.collect();
    check(!p.closed.load(), "and collect() keeps it, since the vertex still names it");
    check(g.read(*path_t::parse("/net/m/x:children[]")).has_value(),
          "so a :children[] read is still served, by the shut-down link");
}

}  // namespace

int main() {
    std::printf("transport_vertex_t destruction retires its connection handler and drains\n");
    routed_dial_reaches_the_endpoint();
    destruction_under_live_dial();
    surviving_link_forwards_after_destruction();
    forward_in_flight_across_removal();
    bus_children_read_after_destruction();
    bound_send_held_across_collects();
    with_link_held_across_collects();
    if constexpr (tr::net::kBusLinks) {
        forward_held_across_collects();
        origination_held_across_collects();
        listing_in_flight_across_removal();
        refused_retire_keeps_the_link();
    }
    return tr::testing::summary("transport_vertex_teardown");
}
