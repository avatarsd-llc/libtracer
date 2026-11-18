/**
 * @file
 * @brief The teardown table (#2813, `link_teardown_table.hpp`) over every host link adapter:
 *        UDP, TCP dial and listen, the TCP and WebSocket stream servers and the WebSocket
 *        client.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Each connection's peer is a link of the opposite role, and the table asks it whether it saw
 * the close (`peer_saw_close`): a dialer's `link_up`, a one-peer listener's, or a server's
 * open sessions. UDP has no connection and leaves it out.
 *
 * Each socket adapter runs the table twice: on a thread of its own (the thread-per-link
 * fallback) and on a node wait module that another thread drives, where the link owns no thread
 * and its callbacks share the module's. The same table runs over CAN in `transport_can_test`
 * (an in-memory bus whose link delivers on a receive thread of its own, as the SocketCAN and TWAI
 * links do), over QUIC and WebTransport in `quic_test` and `webtransport_test`, and over the
 * ESP-IDF WebSocket client in `esp_ws_client_teardown_test`.
 *
 * Built under TSan in the sanitizer legs: rows 2 and 3 race the owner's `shut_down` against the
 * link's own callback, and row 3 sends on the link from that callback across it.
 *
 * Ablation, by hand: a `shut_down` that joins the fallback thread, or detaches with a wait, off
 * a callback thread (the pre-#2813 branch) sticks at row 2 on every socket adapter. An attach
 * that does not read the shut mark reds row 5 on the TCP listener and the held-accept case.
 */

#include "link_teardown_table.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>

#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/wait.hpp"
#include "test_support.hpp"

namespace {

/**
 * @brief Row 5's lever: the socket reads and accepts one thread makes, counted, and parked
 *        while held: a read before it runs, an accept once it has returned its peer. The
 *        link's calls reach it through the link-time wraps below
 *        (`-Wl,--wrap=recvmsg,--wrap=recv,--wrap=accept` on this target); every other
 *        thread's go straight through.
 */
struct io_watch_t {
    std::mutex m;
    std::condition_variable cv;
    std::thread::id thread;    /**< @brief The watched thread; none when unset. */
    bool hold = false;         /**< @brief Its next call parks until released. */
    bool held = false;         /**< @brief A call is parked. */
    std::atomic<int> calls{0}; /**< @brief Its calls since the watch began. */

    /** @brief Called on entry to every wrapped call. */
    void enter() {
        std::unique_lock lk(m);
        if (std::this_thread::get_id() != thread) return;
        calls.fetch_add(1);
        held = hold;
        cv.wait(lk, [this] { return !hold; });
        held = false;
    }
};

io_watch_t& io_watch() {
    static io_watch_t w;
    return w;
}

}  // namespace

extern "C" ssize_t __real_recvmsg(int fd, msghdr* msg, int flags);
extern "C" ssize_t __wrap_recvmsg(int fd, msghdr* msg, int flags) {
    io_watch().enter();
    return __real_recvmsg(fd, msg, flags);
}
extern "C" ssize_t __real_recv(int fd, void* buf, std::size_t len, int flags);
extern "C" ssize_t __wrap_recv(int fd, void* buf, std::size_t len, int flags) {
    io_watch().enter();
    return __real_recv(fd, buf, len, flags);
}
extern "C" int __real_accept(int fd, sockaddr* addr, socklen_t* len);
extern "C" int __wrap_accept(int fd, sockaddr* addr, socklen_t* len) {
    const int peer = __real_accept(fd, addr, len);
    io_watch().enter();
    return peer;
}

namespace {

using namespace std::chrono_literals;
using tr::net::transport_t;
using tr::net::wait_module_t;

const std::array<std::byte, 4> kFrame{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                      std::byte{0x04}};

/** @brief The peer's one frame. */
std::span<const std::byte> frame() { return std::span<const std::byte>(kFrame); }

/** @brief A node wait module and the thread that drives it, or neither (@p kOn false). */
template <bool kOn>
struct module_t {
    module_t() {
        if constexpr (kOn) driver = std::thread([this] { module.run(); });
    }
    ~module_t() {
        if constexpr (kOn) {
            module.stop();
            driver.join();
        }
    }
    module_t(const module_t&) = delete;
    module_t& operator=(const module_t&) = delete;
    /** @brief What the link's config names: the module, or null for the fallback. */
    wait_module_t* wait() { return kOn ? &module : nullptr; }
    wait_module_t module;
    std::thread driver;
};

/** @brief The rig name for @p kOn . */
constexpr const char* on(bool kOn, const char* fallback, const char* module) {
    return kOn ? module : fallback;
}

/** @brief Row 5's static members, over @ref io_watch. */
struct io_rig_t {
    /** @brief Watch @p thread , parking its next read or accept while @p hold . */
    static void watch_io(std::thread::id thread, bool hold) {
        io_watch_t& w = io_watch();
        const std::lock_guard lk(w.m);
        w.thread = thread;
        w.hold = hold;
        w.calls.store(0);
        w.cv.notify_all();
    }
    /** @brief Let a parked call go; later ones are counted and pass. */
    static void release_io() {
        io_watch_t& w = io_watch();
        const std::lock_guard lk(w.m);
        w.hold = false;
        w.cv.notify_all();
    }
    /** @brief A call is parked. */
    static bool io_held() {
        io_watch_t& w = io_watch();
        const std::lock_guard lk(w.m);
        return w.held;
    }
    /** @brief The watched thread's calls since the watch began. */
    static int io_calls() { return io_watch().calls.load(); }
};

/** @brief UDP: the link under test learns its peer from the first datagram. */
template <bool kOn>
struct udp_rig_t {
    static constexpr const char* name = on(kOn, "udp", "udp on a wait module");
    udp_rig_t() {
        link_ = std::make_unique<tr::net::udp_transport_t>(
            0, "", 0, tr::net::udp_config_t{.wait = m_.wait()});
        peer_ = std::make_unique<tr::net::udp_transport_t>(0, "127.0.0.1", link_->local_port());
    }
    bool ok() { return link_->ok() && peer_->ok(); }
    transport_t& link() { return *link_; }
    void poke() { peer_->send(frame()); }
    void destroy_link() { link_.reset(); }
    module_t<kOn> m_;
    std::unique_ptr<tr::net::udp_transport_t> link_;
    std::unique_ptr<tr::net::udp_transport_t> peer_;
};

/** @brief TCP, the link under test listening for its one peer. */
template <bool kOn>
struct tcp_listen_rig_t : io_rig_t {
    static constexpr const char* name = on(kOn, "tcp listen", "tcp listen on a wait module");
    tcp_listen_rig_t() {
        link_ =
            std::make_unique<tr::net::tcp_transport_t>(0, tr::net::tcp_config_t{.wait = m_.wait()});
        peer_ = std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", link_->local_port());
        (void)peer_->start_receiving();
    }
    bool ok() { return link_->ok() && peer_->ok(); }
    transport_t& link() { return *link_; }
    void poke() { peer_->send(frame()); }
    void destroy_link() { link_.reset(); }
    void drop_peer() { peer_.reset(); }
    bool peer_saw_close() { return !peer_->link_up(); }
    module_t<kOn> m_;
    std::unique_ptr<tr::net::tcp_transport_t> link_;
    std::unique_ptr<tr::net::tcp_transport_t> peer_;
};

/** @brief TCP, the link under test dialling a listener that sends it frames. */
template <bool kOn>
struct tcp_dial_rig_t : io_rig_t {
    static constexpr const char* name = on(kOn, "tcp dial", "tcp dial on a wait module");
    tcp_dial_rig_t() {
        peer_ = std::make_unique<tr::net::tcp_transport_t>(0);
        peer_->set_receiver(heard_rx_);
        link_ = std::make_unique<tr::net::tcp_transport_t>(
            "127.0.0.1", peer_->local_port(), tr::net::tcp_config_t{.wait = m_.wait()});
        // The listener sends only once its accept has finished: a frame it heard from the link
        // was read on the session that accept opened.
        up_ = tr::testing::teardown::eventually([&] {
            link_->send(frame());
            std::this_thread::sleep_for(5ms);
            return heard_.load() != 0;
        });
    }
    ~tcp_dial_rig_t() {
        link_.reset();
        peer_.reset();
    }
    tcp_dial_rig_t(const tcp_dial_rig_t&) = delete;
    tcp_dial_rig_t& operator=(const tcp_dial_rig_t&) = delete;
    bool ok() { return up_ && link_->ok() && peer_->ok(); }
    transport_t& link() { return *link_; }
    void poke() { peer_->send(frame()); }
    void destroy_link() { link_.reset(); }
    void drop_peer() { peer_.reset(); }
    /** @brief The one-peer listener is between peers again. */
    bool peer_saw_close() { return !peer_->link_up(); }
    module_t<kOn> m_;
    std::atomic<int> heard_{0};
    std::function<void(std::span<const std::byte>)> heard_rx_ = [this](std::span<const std::byte>) {
        heard_.fetch_add(1);
    };
    std::unique_ptr<tr::net::tcp_transport_t> peer_;
    std::unique_ptr<tr::net::tcp_transport_t> link_;
    bool up_ = false;
};

/** @brief The TCP stream server (slot server), one dialled session. */
template <bool kOn>
struct tcp_server_rig_t : io_rig_t {
    static constexpr const char* name = on(kOn, "tcp server", "tcp server on a wait module");
    tcp_server_rig_t() {
        link_ = std::make_unique<tr::net::tcp_server_transport_t>(
            0, tr::net::tcp_server_config_t{.wait = m_.wait()});
        peer_ = std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", link_->local_port());
        (void)peer_->start_receiving();
    }
    bool ok() { return link_->ok() && peer_->ok(); }
    transport_t& link() { return *link_; }
    void poke() { peer_->send(frame()); }
    void destroy_link() { link_.reset(); }
    void drop_peer() { peer_.reset(); }
    bool peer_saw_close() { return !peer_->link_up(); }
    module_t<kOn> m_;
    std::unique_ptr<tr::net::tcp_server_transport_t> link_;
    std::unique_ptr<tr::net::tcp_transport_t> peer_;
};

/** @brief The WebSocket stream server, one client session. */
template <bool kOn>
struct ws_server_rig_t : io_rig_t {
    static constexpr const char* name = on(kOn, "ws server", "ws server on a wait module");
    ws_server_rig_t() {
        link_ = std::make_unique<tr::net::ws_server_transport_t>(
            0, tr::net::ws_server_config_t{.wait = m_.wait()});
        peer_ = std::make_unique<tr::net::ws_client_transport_t>("127.0.0.1", link_->local_port());
        (void)peer_->start_receiving();
    }
    bool ok() { return link_->ok() && peer_->ok(); }
    transport_t& link() { return *link_; }
    void poke() { peer_->send(frame()); }
    void destroy_link() { link_.reset(); }
    void drop_peer() { peer_.reset(); }
    bool peer_saw_close() { return !peer_->link_up(); }
    module_t<kOn> m_;
    std::unique_ptr<tr::net::ws_server_transport_t> link_;
    std::unique_ptr<tr::net::ws_client_transport_t> peer_;
};

/** @brief The WebSocket client, its server sending it frames. */
template <bool kOn>
struct ws_client_rig_t : io_rig_t {
    static constexpr const char* name = on(kOn, "ws client", "ws client on a wait module");
    ws_client_rig_t() {
        peer_ = std::make_unique<tr::net::ws_server_transport_t>(0);
        link_ = std::make_unique<tr::net::ws_client_transport_t>(
            "127.0.0.1", peer_->local_port(), tr::net::ws_client_config_t{.wait = m_.wait()});
    }
    bool ok() { return link_->ok() && peer_->ok(); }
    transport_t& link() { return *link_; }
    void poke() { peer_->send(frame()); }
    void destroy_link() { link_.reset(); }
    void drop_peer() { peer_.reset(); }
    /** @brief The server holds no session any more. */
    bool peer_saw_close() {
        int sessions = 0;
        auto count = [&](std::string_view) { ++sessions; };
        peer_->enumerate_peers(count);
        return sessions == 0;
    }
    module_t<kOn> m_;
    std::unique_ptr<tr::net::ws_server_transport_t> peer_;
    std::unique_ptr<tr::net::ws_client_transport_t> link_;
};

void check(bool ok, std::string_view what) { tr::testing::check(ok, what); }

/**
 * @brief The one-peer TCP listener held between its accept and taking the peer, across the
 *        owner's shut_down: the link takes no session from a peer it accepted before the
 *        shut-down but takes after it. The peer sees its connection close, nothing it sends is
 *        delivered, the link stays off the wait, and the destructor returns.
 */
void test_listener_held_before_its_accept() {
    using tr::testing::teardown::eventually;
    using tr::testing::teardown::kQuiet;
    using tr::testing::teardown::returns_within;
    using tr::testing::teardown::stuck_unless;
    std::printf("  tcp listen on a wait module, held between its accept and the session\n");
    module_t<true> m;
    auto link =
        std::make_unique<tr::net::tcp_transport_t>(0, tr::net::tcp_config_t{.wait = m.wait()});
    std::atomic<int> entries{0};
    auto rx = [&](std::span<const std::byte>) { entries.fetch_add(1); };
    link->set_receiver(rx);
    io_rig_t::watch_io(m.driver.get_id(), true);
    check(link->ok() && link->start_receiving(), "the listener came up");
    tr::net::tcp_transport_t peer("127.0.0.1", link->local_port());
    (void)peer.start_receiving();
    stuck_unless(eventually([] { return io_rig_t::io_held(); }), "tcp listen",
                 "held after its accept");
    std::atomic<bool> shut{false};
    const bool returned = returns_within([&] { link->shut_down(); }, shut);
    io_rig_t::release_io();
    stuck_unless(returned, "tcp listen", "shut_down (it waited)");
    check(eventually([&] { return !peer.link_up(); }),
          "the peer accepted after shut_down sees its connection close");
    for (int i = 0; i < 5; ++i) peer.send(frame());
    std::this_thread::sleep_for(kQuiet);
    const int calls = io_rig_t::io_calls();
    std::this_thread::sleep_for(kQuiet);
    check(entries.load() == 0, "nothing the peer sent is delivered");
    check(io_rig_t::io_calls() == calls,
          "the link stays off the wait: no read or accept runs after the held one");
    std::atomic<bool> gone{false};
    stuck_unless(returns_within([&] { link.reset(); }, gone), "tcp listen", "destroy");
    check(gone.load(), "the destructor joins and returns");
    io_rig_t::watch_io(std::thread::id{}, false);
}

template <template <bool> class Rig>
void both_waits() {
    tr::testing::teardown::run_table<Rig<false>>(check);
    tr::testing::teardown::run_table<Rig<true>>(check);
}

}  // namespace

int main() {
    std::printf("== #2813: shut_down never waits; the destructor is the one join ==\n");
    both_waits<udp_rig_t>();
    both_waits<tcp_listen_rig_t>();
    both_waits<tcp_dial_rig_t>();
    both_waits<tcp_server_rig_t>();
    both_waits<ws_server_rig_t>();
    both_waits<ws_client_rig_t>();
    test_listener_held_before_its_accept();
    return tr::testing::summary("link_teardown_table");
}
