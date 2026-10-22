/**
 * @file
 * @brief #2263 — a stream server's broadcast holds no lock across its writes, so a write
 *        that will not finish freezes no other sender, no accept and no departure.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The defect: `slot_server_t::broadcast` walked the slot table under `peers_m_`, one
 * `sendmsg` per peer, and the same lock serves accept, teardown, name resolution and the
 * poll set. At N peers that is N syscalls during which every other sender, the accept path
 * and the receive loop wait.
 *
 * The test makes ONE write slow on purpose: this binary links with `--wrap=sendmsg` and
 * `--wrap=send`, and the wrapper parks the first server write made while armed until the
 * test releases it (or a 3 s backstop, so a pre-fix build reddens instead of hanging). While
 * that broadcast is parked inside its write:
 *
 *  1. name resolution, a second broadcast, a directed send and a NEW accept all complete,
 *     and the peers the parked write is not aimed at receive their records;
 *  2. a peer that is not the parked one leaves, its slot is recycled by a newcomer, and the
 *     parked walk then resumes over it without touching a closed descriptor;
 *  3. broadcasts race peer churn for a while, the case ThreadSanitizer judges (run it with
 *     `setarch $(uname -m) -R` here).
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "libtracer/transport_tcp.hpp"
#include "test_support.hpp"

using tr::testing::check;
using namespace std::chrono_literals;
using clock_t_ = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_armed{false};       /**< @brief The next server write is parked. */
std::atomic<int> g_parked_fd{-1};       /**< @brief The fd whose write is parked, or -1. */
std::atomic<bool> g_entered{false};     /**< @brief The parked write has begun. */
std::atomic<bool> g_release{false};     /**< @brief Let the parked write go. */
std::atomic<bool> g_expired{false};     /**< @brief The backstop, not the test, let it go. */
std::atomic<std::uint32_t> g_server{0}; /**< @brief The server's port: only ITS writes park. */

/** @brief The local port of socket @p fd's peer, or 0. */
std::uint16_t peer_port(int fd) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

/** @brief The local port of socket @p fd, or 0. */
std::uint16_t local_port(int fd) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

/** @brief Park this write if it is the server's first one since arming, or on the parked fd. */
void maybe_park(int fd) {
    if (g_armed.load() && local_port(fd) == g_server.load()) {
        int none = -1;
        if (g_parked_fd.compare_exchange_strong(none, fd)) g_armed.store(false);
    }
    if (fd != g_parked_fd.load() || g_release.load()) return;
    g_entered.store(true);
    const auto until = clock_t_::now() + 3s;  // the backstop: a pre-fix build reddens, not hangs
    while (!g_release.load() && clock_t_::now() < until) std::this_thread::sleep_for(1ms);
    if (!g_release.load()) g_expired.store(true);
}

}  // namespace

extern "C" {
ssize_t __real_sendmsg(int fd, const msghdr* msg, int flags);
ssize_t __real_send(int fd, const void* buf, std::size_t len, int flags);

/** @brief The link-time wrap of `sendmsg(2)`: park, then the real call. */
ssize_t __wrap_sendmsg(int fd, const msghdr* msg, int flags) {
    maybe_park(fd);
    return __real_sendmsg(fd, msg, flags);
}
/** @brief The link-time wrap of `send(2)`: park, then the real call. */
ssize_t __wrap_send(int fd, const void* buf, std::size_t len, int flags) {
    maybe_park(fd);
    return __real_send(fd, buf, len, flags);
}
}

namespace {

/** @brief Arm the park for the server's next write. */
void arm() {
    g_release.store(false);
    g_expired.store(false);
    g_entered.store(false);
    g_parked_fd.store(-1);
    g_armed.store(true);
}

/** @brief Release the parked write and disarm. */
void release() {
    g_armed.store(false);
    g_release.store(true);
}

/** @brief Wait up to @p d for @p pred. */
template <class Pred>
bool wait_for(Pred pred, std::chrono::milliseconds d = 2000ms) {
    const auto until = clock_t_::now() + d;
    while (!pred()) {
        if (clock_t_::now() >= until) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

/** @brief A raw client of the server: connects, reads records, never writes. */
struct client_t {
    int fd = -1; /**< @brief The connected socket. */

    /** @brief Connect to the loopback @p port with a @p rcv_ms receive timeout. */
    explicit client_t(std::uint16_t port, int rcv_ms = 300) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            ::close(fd);
            fd = -1;
            return;
        }
        const timeval tv{.tv_sec = rcv_ms / 1000, .tv_usec = (rcv_ms % 1000) * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~client_t() {
        if (fd >= 0) ::close(fd);
    }
    client_t(const client_t&) = delete;
    client_t& operator=(const client_t&) = delete;

    /** @brief This client's local port: the server-side socket's peer port. */
    [[nodiscard]] std::uint16_t port() const { return local_port(fd); }

    /** @brief Read exactly @p n bytes, or fail on EOF or the receive timeout. */
    bool read_n(std::byte* dst, std::size_t n) const {
        for (std::size_t off = 0; off < n;) {
            const ssize_t r = ::recv(fd, dst + off, n - off, 0);
            if (r <= 0) return false;
            off += static_cast<std::size_t>(r);
        }
        return true;
    }

    /** @brief The next length-prefixed record's body, or nothing within the timeout. */
    [[nodiscard]] std::optional<std::vector<std::byte>> record() const {
        std::array<std::byte, 4> p{};
        if (!read_n(p.data(), p.size())) return std::nullopt;
        std::size_t len = 0;
        for (std::size_t i = 0; i < 4; ++i) len |= std::to_integer<std::size_t>(p[i]) << (8 * i);
        std::vector<std::byte> body(len);
        if (!read_n(body.data(), len)) return std::nullopt;
        return body;
    }
};

/** @brief A @p n byte frame of @p mark bytes. */
std::vector<std::byte> frame(std::uint8_t mark, std::size_t n = 64) {
    return std::vector<std::byte>(n, std::byte{mark});
}

/** @brief The server's open peer names. */
std::set<std::string> names(tr::net::tcp_server_transport_t& server) {
    std::set<std::string> out;
    server.bus()->enumerate_peers([&](std::string_view p) { out.emplace(p); });
    return out;
}

/** @brief A peer-named server with clients @p a and @p b admitted. */
struct rig_t {
    tr::net::tcp_server_transport_t server{0, {.peer_named = true}}; /**< @brief Under test. */
    std::optional<client_t> a, b;                                    /**< @brief Two peers. */

    rig_t() {
        g_server.store(server.local_port());
        a.emplace(server.local_port());
        b.emplace(server.local_port());
        check(wait_for([&] { return names(server).size() == 2; }), "two peers admitted");
    }
    ~rig_t() { g_server.store(0); }
    rig_t(const rig_t&) = delete;
    rig_t& operator=(const rig_t&) = delete;

    /** @brief Broadcast @p f on a thread, parked inside its first write; the parked client
     *         and the other one are returned through @p parked and @p other. */
    std::thread park_broadcast(const std::vector<std::byte>& f, client_t*& parked,
                               client_t*& other) {
        arm();
        std::thread t([this, &f] { server.send(std::span(f)); });
        check(wait_for([] { return g_entered.load(); }), "the broadcast is parked in a write");
        const std::uint16_t port = peer_port(g_parked_fd.load());
        parked = a->port() == port ? &*a : &*b;
        other = parked == &*a ? &*b : &*a;
        check(parked->port() == port, "the parked write is aimed at one of the two peers");
        return t;
    }
};

/** @brief Arm 1: nothing else waits on a parked broadcast. */
void test_parked_write_holds_nobody() {
    std::printf("a broadcast parked in one peer's write holds no other sender or accept:\n");
    rig_t r;
    const auto x = frame(0x11);
    client_t* parked = nullptr;
    client_t* other = nullptr;
    std::thread t = r.park_broadcast(x, parked, other);

    auto t0 = clock_t_::now();
    check(names(r.server).size() == 2 && clock_t_::now() - t0 < 500ms,
          "name resolution answers at once while the write is parked");
    const auto y = frame(0x22);
    t0 = clock_t_::now();
    r.server.send(std::span(y));
    check(clock_t_::now() - t0 < 500ms, "and a second broadcast returns at once");
    bool other_got_y = false;
    for (int i = 0; i < 2 && !other_got_y; ++i) other_got_y = other->record() == y;
    check(other_got_y, "the peer the parked write is not aimed at receives the second record");

    const std::set<std::string> before = names(r.server);
    client_t c(r.server.local_port());
    check(wait_for([&] { return names(r.server).size() == 3; }, 1000ms),
          "a new peer is accepted while the write is parked");
    std::string c_name;
    for (const std::string& n : names(r.server))
        if (before.count(n) == 0) c_name = n;
    tr::net::transport_t* const to_c = r.server.bus()->peer_link(c_name);
    check(to_c != nullptr, "and resolves by name");
    const auto z = frame(0x33);
    if (to_c != nullptr) to_c->send(std::span(z));
    bool c_got_z = false;
    for (int i = 0; i < 3 && !c_got_z; ++i) c_got_z = c.record() == z;
    check(c_got_z, "and a directed send reaches it");
    check(!g_expired.load(), "all of it while the first write stayed parked");

    release();
    t.join();
    check(parked->record() == x, "the parked peer gets its record once the write goes");
    check(parked->record() == y, "then the second one, queued behind it in order");
}

/** @brief Arm 2: a peer leaves, and its slot is recycled, under a parked walk. */
void test_peer_leaves_mid_broadcast() {
    std::printf("a peer that leaves mid-broadcast, and its successor, are safe:\n");
    rig_t r;
    const auto x = frame(0x44);
    client_t* parked = nullptr;
    client_t* other = nullptr;
    std::thread t = r.park_broadcast(x, parked, other);

    const bool other_is_b = other == &*r.b;
    (other_is_b ? r.b : r.a).reset();  // the peer the walk has not reached yet departs
    check(wait_for([&] { return names(r.server).size() == 1; }, 1000ms),
          "its teardown runs while the broadcast is parked");
    client_t n(r.server.local_port());
    check(wait_for([&] { return names(r.server).size() == 2; }, 1000ms),
          "and a newcomer takes the freed slot");
    check(!g_expired.load(), "both while the broadcast stayed parked");

    release();
    t.join();
    check(parked->record() == x, "the parked peer gets the record");
    // The newcomer may take the record (it opened before the walk reached its slot) or not;
    // either way what it reads is whole, never a torn or foreign write.
    const std::optional<std::vector<std::byte>> got = n.record();
    check(!got || *got == x, "the newcomer reads the whole record or nothing");
    const auto y = frame(0x55);
    r.server.send(std::span(y));
    check(parked->record() == y, "the server still broadcasts to the survivor");
    check(n.record() == y, "and to the newcomer");
}

/** @brief Arm 3: broadcasts race peer churn (the ThreadSanitizer case). */
void test_broadcasts_race_churn() {
    std::printf("broadcasts racing peer arrivals and departures:\n");
    tr::net::tcp_server_transport_t server(0, {.peer_named = true});
    const std::uint16_t port = server.local_port();
    std::atomic<bool> stop{false};
    std::vector<std::thread> senders;
    for (int s = 0; s < 3; ++s)
        senders.emplace_back([&, s] {
            const auto f = frame(static_cast<std::uint8_t>(0x60 + s), 256);
            while (!stop.load()) server.send(std::span(f));
        });
    std::size_t admitted = 0;
    for (int i = 0; i < 200; ++i) {
        client_t c(port, /*rcv_ms=*/20);
        admitted += c.fd >= 0 ? 1 : 0;
        if (i % 4 == 0) (void)c.record();  // some read a little, most leave at once
    }
    stop.store(true);
    for (std::thread& s : senders) s.join();
    check(admitted == 200, "every churned peer connected");
    check(wait_for([&] { return names(server).empty(); }), "and every one was torn down");
}

}  // namespace

int main() {
    if constexpr (!tr::net::kBusLinks)
        return tr::testing::skipped("server_broadcast_unlocked",
                                    "this build closed the ADR-0044 bus module out "
                                    "(kBusLinks = false); the peers under test are resolved "
                                    "through the bus facet");
    std::printf("== #2263: a server broadcast holds no lock across its writes ==\n");
    test_parked_write_holds_nobody();
    test_peer_leaves_mid_broadcast();
    test_broadcasts_race_churn();
    return tr::testing::summary("server_broadcast_unlocked_test");
}
