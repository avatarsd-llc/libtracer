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
 * The table of attached sources and the descriptor sets rebuilt from it are all drawn from the
 * module's own source and grown, never shrunk. Every growth happens at attach, so attach is the
 * one place a refusal lands: a newcomer the table or a set cannot hold is refused there, and
 * nobody attached is ever evicted for it. There are two sets: the driver polls the live one
 * with the lock released, so an attach grows the spare, and the driver swaps the spare in
 * when the live one is too small. A pass therefore never fails to rebuild. Every change to the
 * table or to a write interest rings the doorbell, so a pass in flight is rebuilt before it
 * can wait on a stale set.
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

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <thread>

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

/** @brief The poll adapter's two descriptor sets: the live one the driver polls, and the spare
 *         an attach grows while that poll is in flight. */
struct wait_module_t::state_t {
    /** @brief Both sets draw from @p src . */
    explicit state_t(mem::block_source_t& src) noexcept
        : set{mem::block_array_t<pollfd>(src), mem::block_array_t<pollfd>(src)} {}
    mem::block_array_t<pollfd> set[2]; /**< @brief The two sets. */
    unsigned live = 0; /**< @brief The one the driver polls. Driver, under the lock. */
};

wait_module_t::wait_module_t(mem::block_source_t& src) noexcept : sources_(src) {
    // The live set holds the doorbell from the start, so a module with no source still waits
    // on its wake.
    state_ = mem::make_poly<state_t>(src, src);
    if (!state_ || !state_->set[0].reserve(1)) return;
    fd_[0] = fd_[1] = open_doorbell();
}

wait_module_t::wait_module_t() noexcept : wait_module_t(mem::net_source()) {}

wait_module_t::~wait_module_t() {
    if (fd_[0] >= 0) ::close(fd_[0]);
}

bool wait_module_t::attach(wait_source_t& s) noexcept {
    bool done = false;
    locked([&] {
        // Grow the spare set to the table plus the newcomer and the doorbell, then the table: a
        // refusal of either refuses the newcomer, and nobody attached is evicted.
        done = !s.attached.load(std::memory_order_relaxed) &&
               state_->set[state_->live ^ 1U].reserve(sources_.size() + 2) &&
               sources_.push_back(&s);
        if (done) s.attached.store(true, std::memory_order_relaxed);
        gen_.fetch_add(1, std::memory_order_acq_rel);
    });
    ring();
    return done;
}

void wait_module_t::detach(wait_source_t& s) noexcept {
    locked([&] {
        if (!s.attached.load(std::memory_order_relaxed)) return;
        std::size_t i = 0;
        while (sources_[i] != &s) ++i;
        sources_.erase_at(i);
        s.attached.store(false, std::memory_order_relaxed);
        gen_.fetch_add(1, std::memory_order_acq_rel);
    });
    ring();
    await_quiet(s);
}

void wait_module_t::await_quiet(const wait_source_t& s) const noexcept {
    // The driver marks a source firing under m_ and fires it with m_ released: a detach that
    // unlinked the source waits for that one callback only, never for the batch around it.
    if (driver_.load() == detail::this_thread_id()) return;
    while (firing_.load(std::memory_order_acquire) == &s) std::this_thread::yield();
}

void wait_module_t::want_write(wait_source_t& s, bool on) noexcept {
    if (s.write.exchange(on, std::memory_order_acq_rel) != on) ring();
}

void wait_module_t::run() noexcept {
    driver_.store(detail::this_thread_id());  // seq_cst: see wait_module_t::locked
    std::unique_lock lock(m_);
    while (!stopping_.load()) {
        // Rebuild under the lock: the doorbell, then every source in table order. The spare
        // was grown by the last attach to hold the whole table, so when the live set is too
        // small the spare is swapped in, and every push below fits.
        if (state_->set[state_->live].capacity() < sources_.size() + 1) state_->live ^= 1U;
        mem::block_array_t<pollfd>& set = state_->set[state_->live];
        set.clear();
        (void)set.push_back(pollfd{fd_[0], POLLIN, 0});
        for (wait_source_t* const s : sources_) {
            const short ev = static_cast<short>(
                POLLIN | (s->write.load(std::memory_order_acquire) ? POLLOUT : 0));
            (void)set.push_back(pollfd{s->fd.load(std::memory_order_relaxed), ev, 0});
        }
        const std::uint32_t seen = gen_.load(std::memory_order_acquire);
        lock.unlock();
        const int n = ::poll(set.data(), static_cast<nfds_t>(set.size()), -1);
        lock.lock();
        if (n <= 0 || gen_.load(std::memory_order_acquire) != seen) continue;
        if ((set[0].revents & POLLIN) != 0) {
            char drain[16];
            while (::recv(fd_[0], drain, sizeof(drain), MSG_DONTWAIT) > 0) {
            }
        }
        // A detach or attach since `seen` changed the table this set mirrors: stop there. The
        // lock is released across each callback, with the source marked firing under it.
        for (std::size_t i = 1; i < set.size() && gen_.load(std::memory_order_acquire) == seen;
             ++i) {
            if (set[i].revents == 0) continue;
            wait_source_t* const s = sources_[i - 1];
            firing_.store(s, std::memory_order_relaxed);
            lock.unlock();
            s->fire(s->ctx, *s, ready_of(set[i].revents));
            lock.lock();
            firing_.store(nullptr, std::memory_order_relaxed);
        }
    }
    lock.unlock();
    driver_.store(detail::unowned_thread_id());
}

void wait_module_t::stop() noexcept {
    stopping_.store(true);
    ring();
}

void wait_module_t::ring() noexcept {
    // No wake may be lost: a stop or a set change that never reaches a blocked pass would
    // leave it blocked. A full doorbell is already readable; a send lwIP refuses for want of a
    // buffer is retried until it goes.
    const char one = 1;
    while (::send(fd_[1], &one, sizeof(one), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 &&
           (errno == ENOMEM || errno == ENOBUFS || errno == EINTR))
        std::this_thread::yield();
}

}  // namespace tr::net
