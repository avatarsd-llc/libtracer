/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief The wait-module adapter for a target with BSD sockets and no `epoll` (lwIP on an
 *        ESP-IDF chip, ADR-0071): one `poll(2)` over the attached sockets with no timeout,
 *        woken by a loopback datagram doorbell (#2147).
 *
 * The table of attached sources and the descriptor set rebuilt from it on every pass are both
 * drawn from the module's own source and grown, never shrunk. A table that cannot grow refuses
 * the newcomer at attach; a set that cannot grow sheds the newest source with a HANGUP, so its
 * link drops it as it would any departed peer. Nobody attached is evicted for a newcomer.
 * Every change to the table or to a write interest rings the doorbell, so a pass in flight is
 * rebuilt before it can wait on a stale set.
 *
 * lwIP has no `eventfd` and no pipe, so the doorbell is a UDP socket bound to the loopback
 * address and connected to itself (lwIP's loopback interface, `LWIP_NETIF_LOOPBACK`): it costs
 * one socket per module.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <mutex>

#include "libtracer/mem_heap.hpp"
#include "libtracer/wait.hpp"

namespace tr::net {

namespace {
#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif

/** @brief A UDP socket on the loopback address, connected to itself, or -1. */
int open_doorbell() noexcept {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), len) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&a), len) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/** @brief The @ref ready_t bits of the `poll(2)` result @p e . */
std::uint8_t ready_of(short e) noexcept {
    return static_cast<std::uint8_t>(
        ((e & POLLIN) != 0 ? static_cast<unsigned>(ready_t::READ) : 0U) |
        ((e & POLLOUT) != 0 ? static_cast<unsigned>(ready_t::WRITE) : 0U) |
        ((e & (POLLHUP | POLLERR | POLLNVAL)) != 0 ? static_cast<unsigned>(ready_t::HANGUP) : 0U));
}
}  // namespace

wait_module_t::wait_module_t(mem::block_source_t& src) noexcept : sources_(src) {
    fd_[0] = fd_[1] = open_doorbell();
}

wait_module_t::wait_module_t() noexcept : wait_module_t(mem::net_source()) {}

wait_module_t::~wait_module_t() {
    if (fd_[0] >= 0) ::close(fd_[0]);
}

bool wait_module_t::attach(wait_source_t& s) noexcept {
    bool done = false;
    locked([&] {
        // A table that cannot grow refuses the newcomer; nobody attached is evicted.
        done = !s.attached && sources_.push_back(&s);
        s.attached = s.attached || done;
        gen_.fetch_add(1, std::memory_order_acq_rel);
    });
    ring();
    return done;
}

void wait_module_t::detach(wait_source_t& s) noexcept {
    locked([&] {
        if (!s.attached) return;
        std::size_t i = 0;
        while (sources_[i] != &s) ++i;
        sources_.erase_at(i);
        s.attached = false;
        gen_.fetch_add(1, std::memory_order_acq_rel);
    });
    ring();
}

void wait_module_t::want_write(wait_source_t& s, bool on) noexcept {
    if (s.write.exchange(on, std::memory_order_acq_rel) != on) ring();
}

void wait_module_t::run() noexcept {
    driver_.store(detail::this_thread_id(), std::memory_order_release);
    mem::block_array_t<pollfd> set(sources_.source());
    pollfd bell{fd_[0], POLLIN, 0};  // the doorbell alone, when the set cannot be grown
    std::unique_lock lock(m_);
    while (!stopping_.load(std::memory_order_acquire)) {
        // Rebuild under the lock: the doorbell, then every source in table order. A set that
        // cannot be grown to the table sheds the newest source: it is detached and told it
        // hung up, so its link sheds it, and the pass starts again.
        set.clear();
        if (!set.reserve(sources_.size() + 1) && !sources_.empty()) {
            wait_source_t& shed = *sources_.back();
            detach(shed);
            shed.fire(shed.ctx, shed, static_cast<std::uint8_t>(ready_t::HANGUP));
            continue;
        }
        (void)set.push_back(bell);
        for (wait_source_t* const s : sources_) {
            const short ev = static_cast<short>(
                POLLIN | (s->write.load(std::memory_order_acquire) ? POLLOUT : 0));
            (void)set.push_back(pollfd{s->fd.load(std::memory_order_relaxed), ev, 0});
        }
        const std::uint32_t seen = gen_.load(std::memory_order_acquire);
        lock.unlock();
        pollfd* const fds = set.empty() ? &bell : set.data();
        const int n = ::poll(fds, static_cast<nfds_t>(set.empty() ? 1 : set.size()), -1);
        lock.lock();
        if (n <= 0 || gen_.load(std::memory_order_acquire) != seen) continue;
        if ((fds[0].revents & POLLIN) != 0) {
            char drain[16];
            while (::recv(fd_[0], drain, sizeof(drain), MSG_DONTWAIT) > 0) {
            }
        }
        // A detach or attach since `seen` changed the table this set mirrors: stop there.
        for (std::size_t i = 1; i < set.size() && gen_.load(std::memory_order_acquire) == seen; ++i)
            if (set[i].revents != 0)
                sources_[i - 1]->fire(sources_[i - 1]->ctx, *sources_[i - 1],
                                      ready_of(set[i].revents));
    }
    lock.unlock();
    driver_.store(detail::unowned_thread_id(), std::memory_order_release);
}

void wait_module_t::stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    ring();
}

void wait_module_t::ring() noexcept {
    const char one = 1;
    (void)::send(fd_[1], &one, sizeof(one), MSG_DONTWAIT | MSG_NOSIGNAL);
}

}  // namespace tr::net
