/**
 * @file
 * @brief The node-wide link memory default (#2574): a link the built-in udp, tcp or ws
 *        factory builds from a SPEC draws its io, state and stack from the default the
 *        `transport_vertex_t` was given, and from the net pool when it was given none.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Each arm makes one SPEC-created LISTEN link of one built-in kind, the way the wire creates
 * one (`register_module`, then a `SPEC` write to the module's creator endpoint). The vertex's
 * own tables and the router's planes sit on a separate counted source, so every draw seen on
 * the three plane sources is the link's own:
 *  - `stack`: the link's receive thread (every kind);
 *  - `state`: a connected peer's session (tcp and ws; a udp link keeps no peer state);
 *  - `io`:    the link's egress store, read back off the link (`transport_t::egress_source`).
 *
 * With no default the three counted sources see nothing, the link's egress store is the
 * vertex's own (`egress_src`, as before), and a connected peer's session draws from the net
 * pool. A factory that names memory of its own keeps it: the default is not read.
 *
 * How a net-pool draw is seen depends on the build's root, so the test picks its instrument
 * at start (@ref net_pool_keeps_census). A slab-pool or arena root keeps a census, read through
 * `stats().in_use`. A heap root (`kSlabPool = false`, the narrow CI leg) makes `net_source()`
 * the platform heap, which keeps none, so there the test counts the nothrow `operator new`
 * calls the heap source makes; it replaces those two global forms for that, which is why it
 * is its own binary. Every counted source here draws through the THROWING forms, so the count
 * is the net pool's alone.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <string_view>
#include <thread>

#include "libtracer/builtin_transports.hpp"
#include "libtracer/conn_spec.hpp"
#include "libtracer/mem_source_backend.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_ws.hpp"

/** @brief Global nothrow `operator new` calls: the heap source's draws (see the file note). */
std::atomic<std::size_t> g_nothrow_news{0};

/** @brief Counted, then the throwing form, so the default `operator delete` matches it. */
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    g_nothrow_news.fetch_add(1, std::memory_order_relaxed);
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

/** @brief The aligned twin of the form above. */
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    g_nothrow_news.fetch_add(1, std::memory_order_relaxed);
    try {
        return ::operator new(size, align);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

namespace {

using namespace std::chrono_literals;

int g_failures = 0;

/** @brief Record one check, printing it either way. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief A forwarding source over the platform heap that counts the blocks it serves. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    explicit counting_source_t(const char* name) noexcept : block_source_t(name) {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        // The throwing form, so the nothrow count below stays the net pool's own.
        void* p = nullptr;
        try {
            p = ::operator new(bytes, std::align_val_t{align});
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
        draws.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        ::operator delete(p, bytes, std::align_val_t{align});
    }
    std::atomic<std::size_t> draws{0}; /**< @brief Blocks served in all. */
};

/** @brief One built-in kind under test. */
struct kind_arm_t {
    std::string_view kind;   /**< @brief The SPEC's `kind`. */
    std::string_view module; /**< @brief The LISTEN module the arm declares. */
    bool keeps_peers;        /**< @brief Whether a connected peer draws a session (`state`). */
};

constexpr kind_arm_t kArms[] = {
    {"udp", tr::net::kUdpServerSuggestedModule, false},
    {"tcp", tr::net::kTcpServerSuggestedModule, true},
    {"ws", tr::net::kWsServerSuggestedModule, true},
};

/** @brief A TCP port free right now: bind an ephemeral port, read it back, close it. */
std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    (void)::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(fd);
    return ntohs(a.sin_port);
}

/** @brief Connect a plain TCP socket to `127.0.0.1:port`; the caller closes it. */
int connect_peer(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    return fd;
}

/** @brief Wait (up to 2 s) until @p pred holds; answer whether it did. */
template <class Pred>
bool eventually(Pred pred) {
    for (int i = 0; i < 1000 && !pred(); ++i) std::this_thread::sleep_for(2ms);
    return pred();
}

/** @brief Whether the net pool keeps a census (`stats().in_use`): probed once, in `main`. */
bool g_net_census = false;

/** @brief Probe @ref g_net_census: draw one block from the net pool and see its balance move. */
bool net_pool_keeps_census() {
    tr::mem::block_source_t& net = tr::mem::net_source();
    const std::size_t before = net.stats().in_use;
    void* const p = net.try_alloc(64, alignof(std::max_align_t));
    const bool moved = net.stats().in_use > before;
    if (p != nullptr) net.release(p, 64, alignof(std::max_align_t));
    return moved;
}

/** @brief The net pool's draws so far: its census where it keeps one, else the nothrow heap
 *         draws it is made of. Only ever compared with an earlier reading. */
std::size_t net_draws() {
    return g_net_census ? tr::mem::net_source().stats().in_use
                        : g_nothrow_news.load(std::memory_order_relaxed);
}

/** @brief What @ref egress_of reads off a link. */
struct egress_probe_t {
    const tr::mem::block_source_t* egress = nullptr; /**< @brief The link's egress store. */
};

/** @brief The egress store of connection @p name's owned link, or null if it has none. */
const tr::mem::block_source_t* egress_of(const tr::net::transport_vertex_t& net,
                                         const std::string& name) {
    egress_probe_t probe;
    const bool found = net.with_link(
        name,
        [](void* ctx, tr::net::transport_t& link) {
            static_cast<egress_probe_t*>(ctx)->egress = &link.egress_source();
        },
        &probe);
    return found ? probe.egress : nullptr;
}

/**
 * @brief One SPEC-created LISTEN link of @p arm's kind, with the node-wide default when
 *        @p with_default is set and with none otherwise.
 */
void run_arm(const kind_arm_t& arm, bool with_default) {
    std::printf("%.*s, %s:\n", static_cast<int>(arm.kind.size()), arm.kind.data(),
                with_default ? "node-wide default set" : "no default");
    counting_source_t plane("plane");  // the vertex's tables and the router's planes
    tr::mem::source_backend_t plane_rx(plane);
    counting_source_t io("io");
    counting_source_t state("state");
    counting_source_t stack("stack");
    const tr::net::link_memory_t node{.io = &io, .state = &state, .stack = &stack};
    const std::uint16_t port = free_port();
    const std::string name = "net/" + std::string(arm.module) + "/peer";

    tr::graph::graph_t g;
    tr::net::fwd_router_t router(
        g, tr::net::router_planes_t{
               .link_src = &plane, .rx = &plane, .flat = &plane_rx, .egress = &plane_rx});
    int peer = -1;
    {
        tr::net::transport_vertex_t net(g, router, "/net", &plane_rx, &plane,
                                        with_default ? &node : nullptr);
        check(net.link_memory() == (with_default ? &node : nullptr),
              "link_memory() answers what the ctor was given");
        (void)net.register_module(std::string(arm.module), arm.kind, tr::net::conn_role_t::LISTEN);
        const auto w = g.write(tr::graph::path_t("/net/" + std::string(arm.module) + "/conn"),
                               tr::net::conn_spec("peer", port, arm.kind));
        check(w.has_value(), "the SPEC write created the listener");

        const tr::mem::block_source_t* const egress = egress_of(net, name);
        check(egress == (with_default ? &io : &plane),
              with_default ? "io: the link's egress store is the default's io"
                           : "io: the link's egress store is the vertex's egress_src, as before");
        if (with_default) {
            check(eventually([&] { return stack.draws.load() > 0; }),
                  "stack: the receive thread drew from the default's stack");
        }

        if (arm.keeps_peers) {
            const std::size_t state_idle = state.draws.load();
            const std::size_t net_idle = net_draws();
            peer = connect_peer(port);
            if (with_default) {
                check(eventually([&] { return state.draws.load() > state_idle; }),
                      "state: the connected peer's session drew from the default's state");
            } else {
                check(eventually([&] { return net_draws() > net_idle; }),
                      "state: the connected peer's session drew from the net pool");
            }
        }
        if (!with_default) {
            check(io.draws.load() == 0 && state.draws.load() == 0 && stack.draws.load() == 0,
                  "no default: the plane sources were never asked");
        }
    }
    if (peer >= 0) ::close(peer);
    g.collect();
}

/**
 * @brief A factory that names memory of its own keeps it: the default the vertex holds is not
 *        read, and every plane lands on the factory's sources.
 */
void own_memory_wins() {
    std::printf("tcp, a factory naming its own memory, default set:\n");
    counting_source_t plane("plane");
    tr::mem::source_backend_t plane_rx(plane);
    counting_source_t def_io("default-io");
    counting_source_t def_state("default-state");
    counting_source_t def_stack("default-stack");
    counting_source_t own("own");
    tr::mem::source_backend_t own_rx(own);
    const tr::net::link_memory_t node{.io = &def_io, .state = &def_state, .stack = &def_stack};
    const std::uint16_t port = free_port();

    tr::graph::graph_t g;
    tr::net::fwd_router_t router(
        g, tr::net::router_planes_t{
               .link_src = &plane, .rx = &plane, .flat = &plane_rx, .egress = &plane_rx});
    int peer = -1;
    {
        tr::net::transport_vertex_t net(g, router, "/net", &plane_rx, tr::net::slim_net, &plane,
                                        &node);
        net.register_transport_type(
            "tcp", [&own, &own_rx](const tr::net::conn_settings_t& s, const tr::wire::tlv_node_t*,
                                   tr::mem::block_source_t& src) {
                return tr::net::with_egress_source(
                    tr::net::make_checked<tr::net::tcp_server_transport_t>(
                        src, s.port,
                        tr::net::tcp_server_config_t{
                            .memory = {.rx = &own_rx, .io = &own, .state = &own, .stack = &own}}),
                    &own);
            });
        (void)net.register_module(std::string(tr::net::kTcpServerSuggestedModule), "tcp",
                                  tr::net::conn_role_t::LISTEN);
        const auto w = g.write(tr::graph::path_t("/net/tcp-server/conn"),
                               tr::net::conn_spec("peer", port, "tcp"));
        check(w.has_value(), "the SPEC write created the listener");
        check(egress_of(net, "net/tcp-server/peer") == &own, "io: the factory's own source");
        const std::size_t own_idle = own.draws.load();
        peer = connect_peer(port);
        check(eventually([&] { return own.draws.load() > own_idle; }),
              "state and stack: the peer and the receive thread drew from the factory's own");
        check(
            def_io.draws.load() == 0 && def_state.draws.load() == 0 && def_stack.draws.load() == 0,
            "the node-wide default was never asked");
    }
    if (peer >= 0) ::close(peer);
    g.collect();
}

}  // namespace

int main() {
    g_net_census = net_pool_keeps_census();
    std::printf("the net pool %s\n", g_net_census ? "keeps a census: reading stats().in_use"
                                                  : "is the heap: counting its nothrow draws");
    for (const kind_arm_t& arm : kArms) {
        run_arm(arm, true);
        run_arm(arm, false);
    }
    own_memory_wins();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
