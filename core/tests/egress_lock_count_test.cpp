/**
 * @file
 * @brief #2145 / #2310 — a dialer send takes no mutex: none when the peer is idle, and none
 *        when the record has to wait.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The egress module elects its writer by try-lock (ADR-0087 §2), and "nothing waits" is one
 * atomic read, so a send that finds the peer idle takes no mutex at all; one that finds a
 * write in flight takes the peer's own lane lock, a bit of its role byte and no mutex
 * (#2310), to put its frame in a lane. Before #2145 the
 * idle send took the queue mutex three times: the election, the empty pop and the re-check.
 *
 * The count comes from interposing `pthread_mutex_lock` and `pthread_mutex_trylock` in this
 * test executable, per thread. `std::mutex` reaches them through libstdc++'s gthread layer.
 * Under a sanitizer, whose runtime intercepts the same entry points, nothing is interposed,
 * and where the interposer is not reached the self-check in `main` skips rather than passing
 * vacuously.
 */

#include <dlfcn.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "libtracer/egress.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_ws.hpp"
#include "test_support.hpp"

namespace {

/** @brief Mutex acquisitions made by this thread through the interposed entry points. */
thread_local unsigned t_locks = 0;

}  // namespace

#if defined(__GLIBC__) && !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
#define TR_LOCK_COUNT_INTERPOSED 1
extern "C" {
/** @brief Counting interposer for `pthread_mutex_lock`: the real call, plus a tally. */
int pthread_mutex_lock(pthread_mutex_t* m) noexcept {
    static auto real =
        reinterpret_cast<int (*)(pthread_mutex_t*)>(::dlsym(RTLD_NEXT, "pthread_mutex_lock"));
    ++t_locks;
    return real(m);
}
/** @brief Counting interposer for `pthread_mutex_trylock`: the real call, plus a tally. */
int pthread_mutex_trylock(pthread_mutex_t* m) noexcept {
    static auto real =
        reinterpret_cast<int (*)(pthread_mutex_t*)>(::dlsym(RTLD_NEXT, "pthread_mutex_trylock"));
    ++t_locks;
    return real(m);
}
}
#endif

namespace {

using namespace std::chrono_literals;
using tr::net::egress_frame_t;
using tr::net::egress_peer_t;
using tr::net::egress_step_t;
using tr::net::egress_t;
using tr::testing::check;

/** @brief The mutexes this thread takes while @p op runs. */
template <class F>
unsigned locks_in(F&& op) {
    const unsigned before = t_locks;
    op();
    return t_locks - before;
}

/** @brief One frame of @p n bytes, built only when the record has to wait. */
egress_frame_t* frame_of(std::size_t n) {
    static const std::vector<std::byte> body(64, std::byte{0x5A});
    const std::span<const std::byte> one[1] = {std::span(body).first(n)};
    return egress_frame_t::make(tr::mem::heap_source(), one);
}

/** @brief The egress module itself: neither an idle send nor a queued one takes a mutex. */
void test_egress_counts() {
    std::printf("an egress send takes no mutex, idle or queued:\n");
    egress_t eg(tr::mem::heap_source());
    egress_peer_t p;
    eg.open(p);
    const tr::graph::delivery_lane_t lane{};
    const auto write = [](const egress_frame_t&, std::size_t) { return egress_step_t{64}; };
    const auto send = [&](auto&& direct) {
        tr::net::egress_once_t once([] { return frame_of(64); });
        return eg.send(p, lane, once, direct, write, [] {});
    };

    const unsigned idle = locks_in([&] { (void)send([] { return egress_step_t{64}; }); });
    check(idle == 0, "a send to an idle peer takes no mutex");

    // A writer stuck inside its write holds the role; a second sender queues behind it.
    std::atomic<bool> in_write{false};
    std::atomic<bool> release{false};
    std::thread writer([&] {
        (void)send([&] {
            in_write.store(true);
            while (!release.load()) std::this_thread::yield();
            return egress_step_t{64};
        });
    });
    while (!in_write.load()) std::this_thread::yield();
    const unsigned queued = locks_in([&] { (void)send([] { return egress_step_t{64}; }); });
    check(queued == 0, "a send behind a write in flight takes no mutex (the peer's lane bit)");
    release.store(true);
    writer.join();
    eg.close(p);
}

/** @brief Wait until @p done holds, or @p budget runs out. */
template <class Done>
bool wait_until(Done done, std::chrono::milliseconds budget) {
    const auto until = std::chrono::steady_clock::now() + budget;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

/** @brief The two dialers, end to end: a send to a peer that keeps up takes no mutex. */
void test_dialer_counts() {
    std::printf("a dialer send to a peer that keeps up takes no mutex:\n");
    const std::array<std::byte, 64> payload{};
    {
        tr::net::tcp_server_transport_t server(0);
        tr::net::tcp_transport_t dialer("127.0.0.1", server.local_port());
        check(dialer.ok() && wait_until([&] { return dialer.link_up(); }, 2s), "tcp dialed");
        dialer.start_receiving();
        dialer.send(std::span<const std::byte>(payload));  // warm-up: lazy state settles
        std::this_thread::sleep_for(20ms);
        const unsigned n = locks_in([&] { dialer.send(std::span<const std::byte>(payload)); });
        std::printf("    tcp dialer send: %u mutex(es)\n", n);
        check(n == 0, "a tcp dialer send takes no mutex");
    }
    {
        tr::net::ws_server_transport_t server(0);
        tr::net::ws_client_transport_t dialer("127.0.0.1", server.local_port());
        check(dialer.ok() && wait_until([&] { return dialer.link_up(); }, 2s), "ws dialed");
        dialer.start_receiving();
        dialer.send(std::span<const std::byte>(payload));
        std::this_thread::sleep_for(20ms);
        const unsigned n = locks_in([&] { dialer.send(std::span<const std::byte>(payload)); });
        std::printf("    ws client send: %u mutex(es)\n", n);
        check(n == 0, "a ws client send takes no mutex");
    }
}

}  // namespace

int main() {
    std::mutex probe;
    const unsigned seen = locks_in([&] {
        probe.lock();
        probe.unlock();
    });
    if (seen == 0) {
        std::printf("SKIP: pthread_mutex_lock is not interposable here\n");
        return 77;
    }
    test_egress_counts();
    test_dialer_counts();
    return tr::testing::summary("egress_lock_count");
}
