/**
 * @file
 * @brief The node's wait module through the transport vertex (#2147 slice 3): a link the
 *        built-in udp, tcp or ws factory builds from a SPEC puts its sockets on the module the
 *        `transport_vertex_t` was given and spawns no receive thread of its own.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The module is driven by a thread of the test's own, started before anything is counted. Each
 * kind gets one SPEC-created LISTEN link and one SPEC-created DIAL link to it, the way the wire
 * creates them (`register_module`, then a `SPEC` write to the module's creator endpoint). A
 * listener spawns no thread. A DIAL link of these point-to-point kinds is self-healing where
 * the build carries the engine (`kSelfHealLinks`): its dial runs on the engine's one worker,
 * and its socket still waits on the module, so the link counts that one thread and no other.
 * Only threads the links add are counted, so a runtime's helper thread is never taken for one.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/builtin_transports.hpp"
#include "libtracer/conn_spec.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/wait.hpp"

namespace {

using namespace std::chrono_literals;

int g_failures = 0;

/** @brief Record one check, printing it either way. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief Wait (up to 2 s) until @p pred holds; answer whether it did. */
template <class Pred>
bool eventually(Pred pred) {
    for (int i = 0; i < 1000 && !pred(); ++i) std::this_thread::sleep_for(2ms);
    return pred();
}

/** @brief The ids of the process's threads. */
std::vector<long> thread_ids() {
    std::vector<long> ids;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/task"))
        ids.push_back(std::stol(e.path().filename().string()));
    return ids;
}

/** @brief How many threads run now that were not in @p old . */
std::size_t spawned_since(const std::vector<long>& old) {
    std::size_t n = 0;
    for (const long id : thread_ids())
        if (std::find(old.begin(), old.end(), id) == old.end()) ++n;
    return n;
}

/** @brief A port free right now on @p type : bind an ephemeral one, read it back, close it. */
std::uint16_t free_port(int type) {
    const int fd = ::socket(AF_INET, type, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    (void)::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(fd);
    return ntohs(a.sin_port);
}

/** @brief The 1-byte link state a connection vertex at @p path publishes, or 0xFF. */
std::uint8_t link_state(tr::graph::graph_t& g, const std::string& path) {
    const auto h = g.find(tr::graph::path_t::parse(path)->key());
    if (!h) return 0xFF;
    const auto v = g.read(*h);
    if (!v) return 0xFF;
    const auto bytes = (*v)->materialize().bytes();
    return bytes.empty() ? 0xFF : static_cast<std::uint8_t>(bytes.back());
}

/** @brief One built-in kind under test. */
struct kind_arm_t {
    std::string_view kind;   /**< @brief The SPECs' `kind`. */
    std::string_view server; /**< @brief The LISTEN module the arm declares. */
    std::string_view client; /**< @brief The DIAL module the arm declares. */
    int type;                /**< @brief The socket type the listener binds. */
};

constexpr kind_arm_t kArms[] = {
    {"udp", tr::net::kUdpServerSuggestedModule, tr::net::kUdpClientSuggestedModule, SOCK_DGRAM},
    {"tcp", tr::net::kTcpServerSuggestedModule, tr::net::kTcpClientSuggestedModule, SOCK_STREAM},
    {"ws", tr::net::kWsServerSuggestedModule, tr::net::kWsClientSuggestedModule, SOCK_STREAM},
};

void run_arm(const kind_arm_t& arm) {
    std::printf("%.*s:\n", static_cast<int>(arm.kind.size()), arm.kind.data());
    tr::net::wait_module_t module;
    // The driver starts before the census: it is the application's thread, not a link's.
    std::thread driver([&] { module.run(); });
    const std::vector<long> before = thread_ids();
    {
        tr::graph::graph_t g;
        tr::net::fwd_router_t router(g);
        tr::net::transport_vertex_t net(g, router, "/net", &tr::mem::net_backend(),
                                        &tr::mem::net_source(), nullptr, &module);
        check(net.wait_module() == &module, "wait_module() answers what the ctor was given");
        (void)net.register_module(std::string(arm.server), arm.kind, tr::net::conn_role_t::LISTEN);
        (void)net.register_module(std::string(arm.client), arm.kind, tr::net::conn_role_t::DIAL);
        const std::uint16_t port = free_port(arm.type);
        const std::string server = "/net/" + std::string(arm.server);
        const std::string client = "/net/" + std::string(arm.client);

        check(
            g.write(tr::graph::path_t(server + "/conn"), tr::net::conn_spec("srv", port, arm.kind))
                .has_value(),
            "the SPEC write created the listener");
        check(link_state(g, server + "/srv") ==
                  static_cast<std::uint8_t>(tr::net::link_state_t::LISTENING),
              "the listener is listening");
        check(spawned_since(before) == 0, "the listener spawned no thread");

        check(g.write(tr::graph::path_t(client + "/conn"),
                      tr::net::conn_spec("cli", port, arm.kind, "127.0.0.1"))
                  .has_value(),
              "the SPEC write created the dialer");
        if constexpr (tr::net::kSelfHealLinks)
            (void)net.acquire_link("net/" + std::string(arm.client) + "/cli");
        check(eventually([&] {
                  return link_state(g, client + "/cli") ==
                         static_cast<std::uint8_t>(tr::net::link_state_t::UP);
              }),
              "the dialer comes up, served by the module");
        const std::size_t engine = tr::net::kSelfHealLinks ? 1 : 0;
        std::printf("   threads the two links added: %zu (self-heal engine workers: %zu)\n",
                    spawned_since(before), engine);
        check(spawned_since(before) == engine,
              "the dialer adds no receive thread: only its self-heal engine's worker");
    }
    check(eventually([&] { return spawned_since(before) == 0; }),
          "the links' teardown leaves no thread behind");
    module.stop();
    driver.join();
}

}  // namespace

int main() {
    std::printf("== #2147: the node's wait module through the transport vertex ==\n");
    for (const kind_arm_t& arm : kArms) run_arm(arm);
    std::printf("node_wait_module_test: %s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAIL",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
