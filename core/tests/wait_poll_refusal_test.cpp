/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief The poll adapter's refusal at attach (#2147, the #2156 rule on the wait module): a
 *        newcomer the module's table or spare set cannot hold is refused, and nobody attached
 *        is evicted.
 *
 * The poll adapter grows its table of sources and its descriptor sets only at attach, from
 * the module's own source, so attach is the one place a refusal lands. This suite draws the
 * module from a source that, once armed, refuses every block, fills the module until an
 * attach is refused, and then connects a newcomer to a TCP server on that module: the server's
 * refused registration must close the newcomer, keep the first peer open and enumerated, and
 * admit the next peer once the refusal lifts. Built with `wait_poll.cpp` ahead of the archive,
 * so the archive's own adapter is never pulled in.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
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

#include "libtracer/mem_heap.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/wait.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using namespace std::chrono_literals;

/** @brief The process heap, refusing every block while @ref refuse is set. */
class refusing_source_t final : public tr::mem::block_source_t {
   public:
    refusing_source_t() noexcept : block_source_t("refusing") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse.load(std::memory_order_relaxed)) {
            refused.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }
    std::atomic<bool> refuse{false};     /**< @brief Refuse every block while set. */
    std::atomic<std::size_t> refused{0}; /**< @brief Blocks refused so far. */
};

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

void test_refused_attach_refuses_the_newcomer() {
    std::printf("-- a refused attach refuses the newcomer and evicts nobody\n");
    refusing_source_t src;
    tr::net::wait_module_t module(src);
    check(module.ok(), "the module opened");
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

        // Fill the module to the end of what it holds, then refuse every growth.
        src.refuse.store(true, std::memory_order_relaxed);
        std::vector<std::unique_ptr<idle_t>> idle;
        bool full = false;
        for (int i = 0; i < 4096 && !full; ++i) {
            auto s = std::make_unique<idle_t>();
            full = !module.attach(s->src);
            if (!full) idle.push_back(std::move(s));
        }
        check(full && src.refused.load() >= 1,
              "an attach the table or spare set cannot hold is refused");

        const int newcomer = tcp_connect(server.local_port());
        pollfd p{newcomer, POLLIN, 0};
        char byte = 0;
        check(newcomer >= 0 && ::poll(&p, 1, 2000) == 1 && ::recv(newcomer, &byte, 1, 0) == 0,
              "the newcomer is closed, not admitted unwaited");
        check(peers() == 1, "the first peer is still admitted (nobody evicted)");
        check(::recv(first, &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN,
              "the first peer's connection is still open");

        src.refuse.store(false, std::memory_order_relaxed);
        const int next = tcp_connect(server.local_port());
        check(next >= 0 && eventually([&] { return peers() == 2; }),
              "once the refusal lifts the same server admits the next peer");
        for (auto& s : idle) module.detach(s->src);
        for (const int fd : {first, newcomer, next})
            if (fd >= 0) ::close(fd);
        module.stop();
        driver.join();
    }
}

}  // namespace

int main() {
    std::printf("== #2147: the poll adapter refuses at attach ==\n");
    test_refused_attach_refuses_the_newcomer();
    return tr::testing::summary("wait_poll_refusal_test");
}
