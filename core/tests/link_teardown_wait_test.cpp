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
 *     open). Both complete.
 *  3. **Two links removing each other.** A removal of `y` arrives on `x` and a removal of `x`
 *     arrives on `y`, both queued behind the held creation. Each teardown runs on the other
 *     link's callback, so neither may wait for the other: both complete, and once B's graph
 *     frees them neither link leaves a thread behind.
 *
 *  4. **A deferred teardown keeps the link's descriptor open until its destructor** (TCP).
 *  5. **collect() from a callback frees nothing.**
 *  6. **Removal from a callback, then collect() from an app thread, leaves no thread and no
 *     descriptor behind.**
 *  7. **A shut session's buffers live until the destructor** (WebSocket, with a second
 *     session holding half an upgrade request): the removal gives back none of the link's
 *     connection state; collect() gives back all of it.
 *  8. **A stream listener removed by another link's write while a peer arrives**: the
 *     teardown's walk of the sessions is ordered after the accept that grew them (TSan).
 *  9. **A transport shut down by its own callback while it shares one `.wait` module** with
 *     other links (the transport alone, no vertex): the shut-down completes without a
 *     deadlock, the module keeps delivering to the other links, and the destructor, off the
 *     callback thread, closes the socket.
 *
 * A teardown made on a thread that runs link callbacks waits for nothing and closes nothing;
 * the joins and closes happen at the graph's collect(), outside every callback.
 *
 * Each wait is bounded: a regression prints `[FAIL]` and exits non-zero at once rather than
 * hanging ctest, since the threads it leaves stuck could not be joined.
 *
 * Ablations, by hand: joining a link's thread in place from a callback thread
 * (`posix_endpoint_t::stop_driver` always joining) reds section 3 (stuck); for section 1 the
 * self-join fails on a POSIX host and the thread ends unjoined, which TSan reports as a thread
 * leak (on a chip that join waits forever). Keeping the control-plane lock across the
 * teardown's waits reds section 2 (stuck). Closing on a deferred teardown reds sections 4
 * and 5; dropping collect()'s early return reds section 5; resetting a shut session's
 * buffers reds section 7.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
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

/** @brief How many of the process's open sockets are bound to local @p port . */
std::size_t sockets_on(std::uint16_t port) {
    std::size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) {
        const int fd = std::stoi(e.path().filename().string());
        sockaddr_in a{};
        socklen_t len = sizeof a;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0 &&
            a.sin_family == AF_INET && ntohs(a.sin_port) == port)
            ++n;
    }
    return n;
}

/** @brief A port free right now, for @p type (a UDP port by default). */
std::uint16_t free_port(int type = SOCK_DGRAM) {
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

/** @brief A heap store that counts the bytes it has out, for a link's connection state. */
struct counting_source_t final : tr::mem::block_source_t {
    std::atomic<std::size_t> out{0};
    counting_source_t() noexcept : block_source_t("counting") {}
    void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* const p = tr::mem::heap_source_t::acquire(bytes, align);
        if (p != nullptr) out.fetch_add(bytes, std::memory_order_relaxed);
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        out.fetch_sub(bytes, std::memory_order_relaxed);
        tr::mem::heap_source_t::reclaim(p, bytes, align);
    }
};

/** @brief Node B, under test: UDP listeners, and the `gate` kind behind a held latch. */
struct node_b_t {
    tr::graph::graph_t g;
    tr::net::fwd_router_t router{g};
    tr::net::transport_vertex_t net;
    explicit node_b_t(tr::net::wait_module_t* wait, const tr::net::link_memory_t* memory = nullptr)
        : net(g, router, "/net", &tr::mem::net_backend(), &tr::mem::net_source(), memory, wait) {
        (void)net.register_module(std::string(tr::net::kUdpServerSuggestedModule), "udp",
                                  conn_role_t::LISTEN);
        (void)net.register_module(std::string(tr::net::kTcpServerSuggestedModule), "tcp",
                                  conn_role_t::LISTEN);
        (void)net.register_module(std::string(tr::net::kWsServerSuggestedModule), "ws",
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
    /** @brief Create the @p kind listener @p name on @p port . */
    bool listen(std::string_view name, std::uint16_t port, std::string_view kind = "udp") {
        const std::string conn = "/net/" + std::string(kind) + "-server/conn";
        return g.write(path_t(conn), tr::net::conn_spec(name, port, kind)).has_value();
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
        (void)net.register_module(std::string(tr::net::kTcpClientSuggestedModule), "tcp",
                                  conn_role_t::DIAL);
        (void)net.register_module(std::string(tr::net::kWsClientSuggestedModule), "ws",
                                  conn_role_t::DIAL);
    }
    /** @brief Dial @p port as @p kind connection @p name , and hold it up. */
    bool dial(std::string_view name, std::uint16_t port, std::string_view kind = "udp") {
        const std::string module = "net/" + std::string(kind) + "-client/";
        if (!g.write(path_t("/" + module + "conn"),
                     tr::net::conn_spec(name, port, kind, "127.0.0.1"))
                 .has_value())
            return false;
        if constexpr (tr::net::kSelfHealLinks) (void)net.acquire_link(module + std::string(name));
        // A write sent before the dialer is up is dropped, so wait for it.
        const std::string path = "/" + module + std::string(name);
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
    /** @brief Write @p payload to @p dst on B, through the @p kind dialer @p via . */
    bool write(tr::net::fwd_router_t::origin_t& slot, std::string_view via, std::string_view dst,
               const tr::view::view_t& payload, std::string_view kind = "udp") {
        const std::string to =
            "/net/" + std::string(kind) + "-client/" + std::string(via) + std::string(dst);
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
        check(eventually([&] {
                  b.g.collect();  // until no frame still holds the removed link
                  return spawned_since(before) == 0;
              }),
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

void test_two_links_removing_each_other() {
    std::printf("-- two links whose callbacks remove each other\n");
    node_b_t b(nullptr);
    node_a_t a;
    const std::uint16_t px = free_port();
    const std::uint16_t py = free_port();
    check(a.dial("tox", px) && a.dial("toy", py), "A dials twice");
    const std::vector<long> before = thread_ids();
    check(b.listen("x", px) && b.listen("y", py), "B listens twice");

    g_gate.entered.store(0);
    g_gate.close();
    std::thread holder([&] {
        (void)b.g.write(path_t("/net/gate-client/conn"), tr::net::conn_spec("held2", 0, "gate"));
    });
    check_or_exit(eventually([] { return g_gate.entered.load() == 1; }),
                  "a creation holds B's control plane");
    tr::net::fwd_router_t::origin_t rx{&ignore_reply, nullptr};
    tr::net::fwd_router_t::origin_t ry{&ignore_reply, nullptr};
    check(a.write(rx, "tox", "/net/udp-server/conn", tr::net::conn_remove("y")),
          "the removal of y is sent over x");
    check(a.write(ry, "toy", "/net/udp-server/conn", tr::net::conn_remove("x")),
          "the removal of x is sent over y");
    std::this_thread::sleep_for(100ms);  // both callbacks now wait behind the held creation
    g_gate.release();
    holder.join();
    check_or_exit(
        eventually([&] { return !b.has("net/udp-server/x") && !b.has("net/udp-server/y"); }),
        "both teardowns complete");
    (void)a.router.cancel(rx);
    (void)a.router.cancel(ry);
    check_or_exit(eventually([&] {
                      b.g.collect();
                      return spawned_since(before) == 0;
                  }),
                  "once B frees them, neither link leaves a thread behind");
}

/** @brief B's stream listener `srv` (TCP unless named), removed by a write A sends over a
 *         connection to it. */
struct removed_by_its_own_write_t {
    counting_source_t state;  // B's links' connection state: sessions and their buffers
    tr::net::link_memory_t memory{.state = &state};
    std::size_t state_before = 0;  // B's connection state just before the removal
    node_b_t b{nullptr, &memory};
    node_a_t a;
    std::uint16_t port = 0;
    std::vector<long> link_threads;  // B's link's own
    tr::net::fwd_router_t::origin_t slot{&ignore_reply, nullptr};
    std::string kind;
    int half_open = -1;  // a raw connection whose WebSocket upgrade request is half sent
    explicit removed_by_its_own_write_t(std::string_view k = "tcp", bool with_half_open = false)
        : kind(k) {
        const std::vector<long> before = thread_ids();
        bool listening = false;  // a port another test takes first is tried again
        for (int i = 0; i < 8 && !listening; ++i)
            listening = b.listen("srv", port = free_port(SOCK_STREAM), kind);
        check(listening, "B listens");
        for (const long id : thread_ids())
            if (std::find(before.begin(), before.end(), id) == before.end())
                link_threads.push_back(id);
        check(a.dial("tob", port, kind), "A connects to B");
        check(eventually([&] { return sockets_on(port) == 2; }),
              "B holds its listening socket and the accepted connection");
        if (with_half_open) {
            half_open = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(port);
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const char req[] = "GET / HTTP/1.1\r\nHost: b\r\nUpgrade: websocket\r\n";
            check(::connect(half_open, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0 &&
                      ::send(half_open, req, sizeof req - 1, 0) ==
                          static_cast<ssize_t>(sizeof req - 1),
                  "a second session holds half an upgrade request");
            check(eventually([&] { return sockets_on(port) == 3; }), "B accepted it");
        }
        std::this_thread::sleep_for(50ms);  // the sessions settle
        state_before = state.out.load();
        check(a.write(slot, "tob", "/net/" + kind + "-server/conn", tr::net::conn_remove("srv"),
                      kind),
              "A sends B the removal of the link it arrives on");
        check_or_exit(eventually([&] { return !b.has("net/" + kind + "-server/srv"); }),
                      "the link's teardown, run on its own callback, completes");
    }
    ~removed_by_its_own_write_t() {
        (void)a.router.cancel(slot);
        if (half_open >= 0) ::close(half_open);
    }
    /** @brief Call B's collect() until the removed link is freed (a frame may still hold it
     *         at the first call); answer whether B's sockets on the port all went. */
    bool collect_until_freed() {
        return eventually([&] {
            b.g.collect();
            return sockets_on(port) == 0;
        });
    }
};

/** @brief Runs @p fn once on a thread that runs link callbacks: a wait module's own. */
template <class Fn>
void on_a_callback_thread(Fn fn) {
    tr::net::wait_module_t module;
    int p[2];
    check_or_exit(::pipe(p) == 0, "a pipe for the callback");
    struct ctx_t {
        Fn* fn;
        std::atomic<bool> done{false};
    } ctx{&fn};
    tr::net::wait_source_t src;
    src.ctx = &ctx;
    src.fd.store(p[0]);
    src.fire = [](void* c, tr::net::wait_source_t& self, std::uint8_t) {
        auto* x = static_cast<ctx_t*>(c);
        char byte = 0;
        if (::read(self.fd.load(), &byte, 1) < 0) byte = 0;
        if (!x->done.load()) (*x->fn)();
        x->done.store(true);
    };
    check_or_exit(module.attach(src), "the callback source is on the module");
    std::thread driver([&] { module.run(); });
    const char byte = 1;
    check_or_exit(::write(p[1], &byte, 1) == 1, "the callback is woken");
    check_or_exit(eventually([&] { return ctx.done.load(); }), "the callback ran");
    module.detach(src);
    module.stop();
    driver.join();
    ::close(p[0]);
    ::close(p[1]);
}

void test_deferred_teardown_keeps_the_descriptor_open_until_the_destructor() {
    std::printf("-- a deferred teardown keeps the link's descriptor open until its destructor\n");
    removed_by_its_own_write_t t;
    std::this_thread::sleep_for(50ms);  // the link's callback sees its shut-down sockets
    check(sockets_on(t.port) == 2, "removed, B's link still holds both its descriptors open");
    check(t.collect_until_freed(), "the destructor, at collect(), closes them");
}

void test_collect_from_a_callback_frees_nothing() {
    std::printf("-- collect() from a callback frees nothing\n");
    removed_by_its_own_write_t t;
    on_a_callback_thread([&] { t.b.g.collect(); });
    check(sockets_on(t.port) == 2, "collect() on a callback thread left the removed link whole");
    check(t.collect_until_freed(), "the next collect() from an application thread frees it");
}

void test_removal_from_a_callback_then_collect_leaves_nothing_behind() {
    std::printf("-- removal from a callback, then collect() from an app thread\n");
    removed_by_its_own_write_t t;
    check(t.collect_until_freed(), "no descriptor is left behind");
    check(!t.link_threads.empty(), "B's link ran a thread of its own");
    check(eventually([&] {
              const std::vector<long> now = thread_ids();
              return std::none_of(t.link_threads.begin(), t.link_threads.end(), [&](long id) {
                  return std::find(now.begin(), now.end(), id) != now.end();
              });
          }),
          "and no thread of B's link");
}

void test_a_shut_sessions_buffers_live_until_the_destructor() {
    std::printf("-- a WebSocket session removed by its own message keeps its buffers\n");
    // The removal is decoded from the session's own assembler, on its own callback: freeing
    // the session's buffers there would pull them from under that callback.
    removed_by_its_own_write_t t("ws", true);
    std::this_thread::sleep_for(50ms);  // the link's callback sees its shut-down sockets
    check(t.state_before != 0 && t.state.out.load() == t.state_before,
          "removed, B's link gave back none of its connection state");
    check(sockets_on(t.port) == 3, "and still holds every descriptor open");
    check(t.collect_until_freed() && t.state.out.load() == 0,
          "the destructor, at collect(), frees the sessions and their buffers");
}

/** @brief Where the slot hook parks the listener's thread. Relaxed atomics on purpose: the
 *         park must order nothing between the listener's thread and the others. */
struct accept_park_t {
    std::atomic<bool> armed{false};
    std::atomic<bool> reached{false};
    std::atomic<bool> released{false};
} g_accept_park;

void test_listener_removed_by_another_link_while_peers_arrive() {
    std::printf("-- a stream listener removed by another link's write while a peer arrives\n");
    // The listener's own thread grows its session table for an arriving peer and is parked
    // right after (the slot hook), holding no lock and before the session goes on the wait. A
    // removal queued on the UDP link's thread behind the held control plane then runs, so the
    // teardown's walk of the sessions follows that growth with nothing ordering the two but
    // the lock the walk and the accept share: without it, TSan reports the walk.
    struct hook_guard_t {
        ~hook_guard_t() { tr::net::detail::slot_taken_hook = nullptr; }
    } const guard;
    g_accept_park.reached.store(false, std::memory_order_relaxed);
    g_accept_park.released.store(false, std::memory_order_relaxed);
    tr::net::detail::slot_taken_hook = [] {
        if (!g_accept_park.armed.exchange(false, std::memory_order_relaxed)) return;
        g_accept_park.reached.store(true, std::memory_order_relaxed);
        for (int i = 0; i < 5000 && !g_accept_park.released.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(2ms);
    };

    counting_source_t state;
    tr::net::link_memory_t memory{.state = &state};
    node_b_t b(nullptr, &memory);
    node_a_t a;
    const std::uint16_t pctl = free_port();
    check(a.dial("toctl", pctl), "A dials B's control link");
    check(b.listen("ctl", pctl), "B listens on UDP");
    std::uint16_t psrv = 0;
    bool listening = false;
    for (int i = 0; i < 8 && !listening; ++i)
        listening = b.listen("srv", psrv = free_port(SOCK_STREAM), "tcp");
    check(listening, "B listens on TCP");

    // Queue the removal behind a creation that holds B's control plane.
    g_gate.entered.store(0);
    g_gate.close();
    std::thread holder([&] {
        (void)b.g.write(path_t("/net/gate-client/conn"), tr::net::conn_spec("held3", 0, "gate"));
    });
    check_or_exit(eventually([] { return g_gate.entered.load() == 1; }),
                  "a creation holds B's control plane");
    tr::net::fwd_router_t::origin_t slot{&ignore_reply, nullptr};
    check(a.write(slot, "toctl", "/net/tcp-server/conn", tr::net::conn_remove("srv")),
          "A sends B the removal of the listener over another link");
    std::this_thread::sleep_for(100ms);  // it waits behind the held creation

    g_accept_park.armed.store(true, std::memory_order_relaxed);
    const int peer = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(psrv);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(::connect(peer, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0,
          "a peer connects to the listener");
    check_or_exit(eventually([] { return g_accept_park.reached.load(std::memory_order_relaxed); }),
                  "the listener grew its session table for it and is parked");
    g_gate.release();
    holder.join();
    check_or_exit(eventually([&] { return !b.has("net/tcp-server/srv"); }),
                  "the listener's teardown completes while its accept is parked");
    g_accept_park.released.store(true, std::memory_order_relaxed);
    // Released, the accept attaches the session it took before the walk; on the shut link it
    // tears it down at once, so the peer reads end-of-stream before any collect().
    timeval tv{.tv_sec = 2, .tv_usec = 0};
    (void)::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char byte = 0;
    check(::recv(peer, &byte, 1, 0) == 0,
          "the session the accept attached after the teardown is shut down at once");
    (void)a.router.cancel(slot);
    check(eventually([&] {
              b.g.collect();
              return sockets_on(psrv) == 0;
          }),
          "collect() frees the listener and its session");
    ::close(peer);
}

void test_transport_shut_down_by_its_own_callback_on_a_shared_module() {
    std::printf("-- a transport shut down by its own callback, sharing one .wait module\n");
    tr::net::wait_module_t module;
    std::thread driver([&] { module.run(); });
    const std::uint16_t pa = free_port();
    const std::uint16_t pb = free_port();
    const std::uint16_t pc = free_port();
    const std::vector<long> before = thread_ids();
    {
        tr::net::udp_transport_t a(pa, "127.0.0.1", pb, tr::net::udp_config_t{.wait = &module});
        auto b = std::make_unique<tr::net::udp_transport_t>(pb, "127.0.0.1", pa,
                                                            tr::net::udp_config_t{.wait = &module});
        tr::net::udp_transport_t c(pc, "127.0.0.1", pa, tr::net::udp_config_t{.wait = &module});
        check(a.ok() && b->ok() && c.ok(), "three UDP links up on one wait module");
        std::atomic<bool> shut{false};
        auto b_rx = [&](std::span<const std::byte>) {
            if (!shut.exchange(true)) b->shut_down();  // from b's own callback, on the module
        };
        b->set_receiver(b_rx);
        std::atomic<int> to_a{0};
        auto a_rx = [&](std::span<const std::byte>) { to_a.fetch_add(1); };
        a.set_receiver(a_rx);
        check(spawned_since(before) == 0, "the links spawned no thread");
        const std::array<std::byte, 4> frame{};
        a.send(std::span<const std::byte>(frame));
        check_or_exit(eventually([&] { return shut.load(); }),
                      "b shut itself down from its own callback without a deadlock");
        c.send(std::span<const std::byte>(frame));
        check_or_exit(eventually([&] { return to_a.load() == 1; }),
                      "the shared module still delivers to the other links");
        b.reset();  // off the callback thread, with the module still running
        check(sockets_on(pb) == 0, "b's destructor closed its socket");
        check(spawned_since(before) == 0, "the teardown spawned no thread");
        a.shut_down();
        c.shut_down();
    }
    module.stop();
    driver.join();
}

}  // namespace

int main() {
    std::printf("== #2751: link teardown never waits on a callback holding the lock it needs ==\n");
    test_removed_by_its_own_write(false);
    test_removed_by_its_own_write(true);
    test_removed_while_its_callback_waits_for_the_control_plane();
    test_two_links_removing_each_other();
    test_deferred_teardown_keeps_the_descriptor_open_until_the_destructor();
    test_collect_from_a_callback_frees_nothing();
    test_removal_from_a_callback_then_collect_leaves_nothing_behind();
    test_a_shut_sessions_buffers_live_until_the_destructor();
    test_listener_removed_by_another_link_while_peers_arrive();
    test_transport_shut_down_by_its_own_callback_on_a_shared_module();
    std::printf("link_teardown_wait_test: %s (%d failures)\n",
                g_failures == 0 ? "ALL PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
