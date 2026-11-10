/**
 * @file
 * @brief A link's teardown never waits for one of its callbacks while holding a lock that
 *        callback can take (#2751), and a link removed from its own callback leaves no thread
 *        running once it is freed.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two nodes in one process. Node B, under test, listens on UDP; node A dials it and sends B
 * control-plane writes (a removal `NAME`, a creation `SPEC`), so each one runs on the receive
 * path of the B link it arrived on.
 *
 *  1. **Removed by its own write.** A removal of B's link `srv` arrives on `srv` itself.
 *     The teardown runs on `srv`'s own callback, so it cannot wait for that callback: it
 *     completes, and once B's graph frees the link no thread of it is left. Checked with the
 *     link on a thread of its own and on the node's wait module.
 *  2. **Removed by another link's write while its own callback waits for the control plane.**
 *     A removal of B's link `x` arrives on B's link `y` while a creation that arrived on `x`
 *     is waiting for the same control plane (both queued behind a creation the test holds
 *     open). The teardown of `x` waits for `x`'s callback, which needs the control plane the
 *     removal has finished with: it completes.
 *
 * Each wait is bounded: a regression prints `[FAIL]` and exits non-zero at once rather than
 * hanging ctest, since the threads it leaves stuck could not be joined.
 *
 * Ablations, by hand: keeping the control-plane lock across the teardown's waits reds
 * section 2 (stuck). Joining a link's own thread from itself passes on a POSIX host, where
 * the join fails and the thread ends unjoined, but under TSan it is reported as a thread
 * leak; on a chip that join waits forever.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/builtin_transports.hpp"
#include "libtracer/conn_spec.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/wait.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::path_t;
using tr::net::conn_role_t;

int g_failures = 0;

/** @brief Record one check, printing it either way. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief A check whose failure leaves threads stuck for good: report and leave at once. */
void check_or_exit(bool ok, const char* what) {
    check(ok, what);
    if (!ok) {
        std::printf("link_teardown_wait_test: FAIL (stuck; exiting)\n");
        std::fflush(stdout);
        std::_Exit(1);
    }
}

/** @brief Wait (up to 3 s) until @p pred holds; answer whether it did. */
template <class Pred>
bool eventually(Pred pred) {
    for (int i = 0; i < 1500 && !pred(); ++i) std::this_thread::sleep_for(2ms);
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

/** @brief A UDP port free right now. */
std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    (void)::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(fd);
    return ntohs(a.sin_port);
}

/** @brief A latch the `gate` kind's factory waits on, so a creation holds the control plane. */
struct gate_t {
    std::mutex m;
    std::condition_variable cv;
    bool open = true;
    std::atomic<int> entered{0};
    void close() {
        const std::lock_guard l(m);
        open = false;
    }
    void release() {
        {
            const std::lock_guard l(m);
            open = true;
        }
        cv.notify_all();
    }
    void pass() {
        entered.fetch_add(1);
        std::unique_lock l(m);
        cv.wait(l, [this] { return open; });
    }
} g_gate;

/** @brief The link the `gate` kind builds: no thread, no socket. */
struct gate_link_t : tr::net::transport_t {
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
};

/** @brief A reply sink that keeps nothing: these writes are judged by their effect. */
void ignore_reply(void*, const tr::view::rope_t&) {}

/** @brief Node B, under test: UDP listeners, and the `gate` kind behind a held latch. */
struct node_b_t {
    tr::graph::graph_t g;
    tr::net::fwd_router_t router{g};
    tr::net::transport_vertex_t net;
    explicit node_b_t(tr::net::wait_module_t* wait)
        : net(g, router, "/net", &tr::mem::net_backend(), &tr::mem::net_source(), nullptr, wait) {
        (void)net.register_module(std::string(tr::net::kUdpServerSuggestedModule), "udp",
                                  conn_role_t::LISTEN);
        (void)net.register_module("gate-client", "gate", conn_role_t::DIAL);
        net.register_transport_type(
            "gate",
            [](const tr::net::conn_settings_t&, const tr::wire::tlv_node_t*,
               tr::mem::block_source_t& src) -> tr::graph::result_t<tr::net::transport_ptr_t> {
                g_gate.pass();
                return tr::net::make_transport<gate_link_t>(src);
            });
    }
    /** @brief Create the UDP listener @p name on @p port . */
    bool listen(std::string_view name, std::uint16_t port) {
        return g.write(path_t("/net/udp-server/conn"), tr::net::conn_spec(name, port, "udp"))
            .has_value();
    }
    /** @brief Whether connection @p qualified still exists. */
    bool has(std::string_view qualified) { return net.settings_of(qualified).has_value(); }
};

/** @brief Node A: one UDP dialer per B listener, the path A's writes reach B by. */
struct node_a_t {
    tr::graph::graph_t g;
    tr::net::fwd_router_t router{g};
    tr::net::transport_vertex_t net{g, router};
    node_a_t() {
        (void)net.register_module(std::string(tr::net::kUdpClientSuggestedModule), "udp",
                                  conn_role_t::DIAL);
    }
    /** @brief Dial @p port as connection @p name , and hold it up. */
    bool dial(std::string_view name, std::uint16_t port) {
        if (!g.write(path_t("/net/udp-client/conn"),
                     tr::net::conn_spec(name, port, "udp", "127.0.0.1"))
                 .has_value())
            return false;
        if constexpr (tr::net::kSelfHealLinks)
            (void)net.acquire_link("net/udp-client/" + std::string(name));
        // A write sent before the dialer is up is dropped, so wait for it.
        const std::string path = "/net/udp-client/" + std::string(name);
        return eventually([&] {
            const auto h = g.find(path_t::parse(path)->key());
            if (!h) return false;
            const auto v = g.read(*h);
            if (!v) return false;
            const auto bytes = (*v)->materialize().bytes();
            return !bytes.empty() &&
                   bytes.back() == static_cast<std::byte>(tr::net::link_state_t::UP);
        });
    }
    /** @brief Write @p payload to @p dst on B, through the dialer @p via . */
    bool write(tr::net::fwd_router_t::origin_t& slot, std::string_view via, std::string_view dst,
               const tr::view::view_t& payload) {
        const std::string to = "/net/udp-client/" + std::string(via) + std::string(dst);
        return router.originate(slot, tr::graph::fwd_op_t::WRITE, path_t(to), payload.bytes())
            .has_value();
    }
};

void test_removed_by_its_own_write(bool on_module) {
    std::printf("-- a link removed by a write that arrived on it (%s)\n",
                on_module ? "node wait module" : "a thread of its own");
    tr::net::wait_module_t module;
    std::thread driver;
    if (on_module) driver = std::thread([&] { module.run(); });
    {
        node_b_t b(on_module ? &module : nullptr);
        node_a_t a;
        const std::uint16_t port = free_port();
        // A first, so the census below counts B's link alone (a UDP dial needs no listener).
        check(a.dial("tob", port), "A dials B");
        const std::vector<long> before = thread_ids();
        check(b.listen("srv", port), "B listens");
        tr::net::fwd_router_t::origin_t slot{&ignore_reply, nullptr};
        check(a.write(slot, "tob", "/net/udp-server/conn", tr::net::conn_remove("srv")),
              "A sends B the removal of the link it arrives on");
        check_or_exit(eventually([&] { return !b.has("net/udp-server/srv"); }),
                      "the link's teardown, run on its own callback, completes");
        (void)a.router.cancel(slot);
        b.g.collect();
        check(eventually([&] { return spawned_since(before) == 0; }),
              "once B frees it, the removed link leaves no thread behind");
    }
    if (on_module) {
        module.stop();
        driver.join();
    }
}

void test_removed_while_its_callback_waits_for_the_control_plane() {
    std::printf("-- a link removed by another link's write while its own callback waits\n");
    node_b_t b(nullptr);
    node_a_t a;
    const std::uint16_t px = free_port();
    const std::uint16_t py = free_port();
    check(b.listen("x", px) && b.listen("y", py), "B listens twice");
    check(a.dial("tox", px) && a.dial("toy", py), "A dials both");

    // Hold B's control plane: a creation whose factory waits on the latch.
    g_gate.close();
    std::thread holder([&] {
        (void)b.g.write(path_t("/net/gate-client/conn"), tr::net::conn_spec("held", 0, "gate"));
    });
    check_or_exit(eventually([] { return g_gate.entered.load() == 1; }),
                  "a creation holds B's control plane");
    // Queue the removal of `x` (arriving on `y`) first, then a creation arriving on `x`, so
    // the removal is the first waiter and `x`'s callback waits behind it.
    tr::net::fwd_router_t::origin_t remove{&ignore_reply, nullptr};
    tr::net::fwd_router_t::origin_t create{&ignore_reply, nullptr};
    check(a.write(remove, "toy", "/net/udp-server/conn", tr::net::conn_remove("x")),
          "the removal of x is sent over y");
    std::this_thread::sleep_for(100ms);
    check(a.write(create, "tox", "/net/gate-client/conn", tr::net::conn_spec("late", 0, "gate")),
          "a creation is sent over x");
    std::this_thread::sleep_for(100ms);
    g_gate.release();
    holder.join();
    check_or_exit(eventually([&] { return !b.has("net/udp-server/x"); }),
                  "the teardown of x completes while x's callback waited for the control plane");
    check_or_exit(eventually([&] { return b.has("net/gate-client/late"); }),
                  "and x's own write still ran to completion");
    (void)a.router.cancel(remove);
    (void)a.router.cancel(create);
    b.g.collect();
}

}  // namespace

int main() {
    std::printf("== #2751: link teardown never waits on a callback holding the lock it needs ==\n");
    test_removed_by_its_own_write(false);
    test_removed_by_its_own_write(true);
    test_removed_while_its_callback_waits_for_the_control_plane();
    std::printf("link_teardown_wait_test: %s (%d failures)\n",
                g_failures == 0 ? "ALL PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
