/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief The poll adapter's one refusal at attach (#2147, the #2156 rule on the wait module):
 *        a socket numbered past `FD_SETSIZE` is refused, and nobody attached is evicted.
 *
 * The receiving link pays for its own place in the adapter's list, and the descriptor sets
 * are fixed `fd_set`s, so the module draws no memory and an attach is never refused for it.
 * This suite raises the descriptor limit, fills the numbers below `FD_SETSIZE`, and connects
 * a newcomer to a TCP server on the module: the newcomer's socket lands past the set, and the
 * server's refused registration must close it, keep the first peer open and enumerated, and
 * admit the next peer once low numbers are free again. Built with `wait_poll.cpp` ahead of the
 * archive, so the archive's own adapter is never pulled in.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/transport_tcp.hpp"
#include "libtracer/wait.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using namespace std::chrono_literals;

/** @brief An idle socketpair end registered with no work to do. */
struct idle_t {
    tr::net::wait_source_t src;
    int pair[2] = {-1, -1};
    idle_t() {
        (void)::socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
        src.fd.store(pair[0]);
        src.fire = [](void*, tr::net::wait_source_t&, std::uint8_t) {};
    }
    ~idle_t() {
        ::close(pair[0]);
        ::close(pair[1]);
    }
};

/** @brief A blocking TCP connect to 127.0.0.1:@p port , or -1. */
int tcp_connect(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) return fd;
    if (fd >= 0) ::close(fd);
    return -1;
}

/** @brief Poll @p done for up to 2 s. */
template <class Pred>
bool eventually(Pred done) {
    for (int i = 0; i < 200; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return done();
}

/** @brief Raise the soft descriptor limit past `FD_SETSIZE`; false when the hard one is lower. */
bool room_past_the_set() {
    rlimit r{};
    if (::getrlimit(RLIMIT_NOFILE, &r) != 0 || r.rlim_max < FD_SETSIZE + 64) return false;
    r.rlim_cur = FD_SETSIZE + 64;
    return ::setrlimit(RLIMIT_NOFILE, &r) == 0;
}

void test_refused_attach_refuses_the_newcomer() {
    std::printf("-- a socket past the set is refused, and nobody is evicted\n");
    tr::net::wait_module_t module;
    check(module.ok(), "the module opened, drawing no memory");
    std::thread driver([&module] { module.run(); });
    {
        tr::net::tcp_server_config_t cfg;
        cfg.wait = &module;
        tr::net::tcp_server_transport_t server(0, cfg);
        check(server.ok(), "the tcp server is on the module");
        const auto peers = [&server] {
            std::size_t n = 0;
            server.enumerate_peers([&n](std::string_view) { ++n; });
            return n;
        };
        const int first = tcp_connect(server.local_port());
        check(first >= 0 && eventually([&] { return peers() == 1; }), "the first peer is admitted");

        // Every number below the set taken, so the next accepted socket lands past it.
        std::vector<int> fill;
        for (int fd = ::dup(0); fd >= 0 && fd < FD_SETSIZE; fd = ::dup(0)) fill.push_back(fd);
        idle_t past;
        const int high = ::fcntl(past.pair[0], F_DUPFD, FD_SETSIZE);
        ::close(past.pair[0]);
        past.pair[0] = high;
        past.src.fd.store(high);
        check(high >= FD_SETSIZE && !module.attach(past.src),
              "a socket numbered past FD_SETSIZE is refused at attach");

        const int newcomer = tcp_connect(server.local_port());
        pollfd p{newcomer, POLLIN, 0};
        char byte = 0;
        check(newcomer >= 0 && ::poll(&p, 1, 2000) == 1 && ::recv(newcomer, &byte, 1, 0) == 0,
              "the newcomer is closed, not admitted unwaited");
        check(peers() == 1, "the first peer is still admitted (nobody evicted)");
        check(::recv(first, &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN,
              "the first peer's connection is still open");

        for (const int fd : fill) ::close(fd);
        const int next = tcp_connect(server.local_port());
        check(next >= 0 && eventually([&] { return peers() == 2; }),
              "once low numbers are free the same server admits the next peer");
        for (const int fd : {first, newcomer, next})
            if (fd >= 0) ::close(fd);
        module.stop();
        driver.join();
    }
}

}  // namespace

int main() {
    std::printf("== #2147: the poll adapter refuses at attach ==\n");
    if (!room_past_the_set())
        return tr::testing::skipped("wait_poll_refusal_test",
                                    "the hard descriptor limit is below FD_SETSIZE");
    test_refused_attach_refuses_the_newcomer();
    return tr::testing::summary("wait_poll_refusal_test");
}
