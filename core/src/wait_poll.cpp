/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief The wait-module adapter for a target with BSD sockets and no `epoll` (lwIP on an
 *        ESP-IDF chip, ADR-0071): one `select(2)` over the attached sockets with no timeout,
 *        woken by a loopback datagram doorbell (#2147).
 *
 * Receiver pays (#2147): every attached source carries its own link in the module's list
 * (`wait_source_t::next`), in the record its link owns, and the descriptor sets are the
 * fixed-size `fd_set`s of `select(2)`. The module draws nothing and grows nothing, so an attach
 * is never refused for memory; a socket numbered past `FD_SETSIZE` is refused instead, which an
 * lwIP socket never is (its numbers start below it). `select` is lwIP's own wait: its `poll`
 * is built on the same scan. Every change to the list or to a write interest rings the
 * doorbell, so a pass in flight is rebuilt before it can wait on a stale set.
 *
 * lwIP has no `eventfd` and no pipe, so the doorbell is a UDP socket bound to the loopback
 * address and connected to itself (lwIP's loopback interface, `LWIP_NETIF_LOOPBACK`): it costs
 * one socket per module.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <mutex>

#include "libtracer/wait.hpp"

namespace tr::net {

// The adapter gate, the shape self_heal_link.cpp states for kSelfHealLinks: this adapter links
// its sources through their own records, so a build that compiles it binds the member that
// gives them the link.
static_assert(kListedWaitSources,
              "wait_poll.cpp (the lwIP select adapter) is compiled on a build whose config binds "
              "kListedWaitSources = false. Bind `static constexpr bool kListedWaitSources = "
              "true;` in libtracer/config_override.hpp (the ESP-IDF component does so for every "
              "chip target).");

namespace {
/** @brief @p s 's list link, as the atomic both the lock holder and the driver use. */
std::atomic_ref<wait_source_t*> link_of(wait_source_t& s) noexcept {
    return std::atomic_ref<wait_source_t*>(s.next);
}

#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif

/** @brief A UDP socket on the loopback address, connected to itself, or -1: also when its
 *         number is past `FD_SETSIZE`, which an lwIP socket never is but a busy host's may be,
 *         so the wait never sets a bit past its set and the module reports itself not ok. */
int open_doorbell() noexcept {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), len) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&a), len) != 0 || fd >= FD_SETSIZE) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/** @brief @ref ready_t::HANGUP when @p fd is no open socket, else nothing: what a failed
 *         `select` reports to each source, as `poll` reported `POLLNVAL`. */
std::uint8_t hangup_of(int fd) noexcept {
    return (::fcntl(fd, F_GETFL) < 0 && errno == EBADF) ? static_cast<std::uint8_t>(ready_t::HANGUP)
                                                        : 0U;
}

/** @brief The @ref ready_t bits @p fd holds in @p rd and @p wr . A failed or hung-up socket
 *         is readable to `select`, and the read that follows reports which. */
std::uint8_t ready_of(int fd, const fd_set& rd, const fd_set& wr) noexcept {
    return static_cast<std::uint8_t>(
        (FD_ISSET(fd, &rd) ? static_cast<unsigned>(ready_t::READ) : 0U) |
        (FD_ISSET(fd, &wr) ? static_cast<unsigned>(ready_t::WRITE) : 0U));
}
}  // namespace

wait_module_t::wait_module_t() noexcept : fd_{open_doorbell(), -1} { fd_[1] = fd_[0]; }

wait_module_t::~wait_module_t() {
    if (fd_[0] >= 0) ::close(fd_[0]);
}

bool wait_module_t::attach(wait_source_t& s) noexcept {
    const int fd = s.fd.load(std::memory_order_relaxed);
    bool done = false;
    locked([&] {
        // In at the head: a pass in flight walks from the head it took, so it never meets the
        // newcomer and needs no generation bump to stay sound; the doorbell rebuilds it.
        done = !s.attached.load(std::memory_order_relaxed) && fd >= 0 && fd < FD_SETSIZE;
        if (!done) return;
        link_of(s).store(head_, std::memory_order_relaxed);
        head_ = &s;
        s.attached.store(true, std::memory_order_relaxed);
    });
    ring();
    return done;
}

void wait_module_t::detach(wait_source_t& s) noexcept {
    locked([&] {
        if (!s.attached.load(std::memory_order_relaxed)) return;
        // Unlinked, but its own link is left as it was: a pass that already holds `s` reads it
        // only after this bump, which stops that pass first.
        wait_source_t* at = nullptr;
        for (wait_source_t* p = head_; p != &s; p = link_of(*p).load(std::memory_order_relaxed))
            at = p;
        wait_source_t* const after = link_of(s).load(std::memory_order_relaxed);
        if (at == nullptr) head_ = after;
        if (at != nullptr) link_of(*at).store(after, std::memory_order_relaxed);
        s.attached.store(false, std::memory_order_relaxed);
        gen_.fetch_add(1);  // seq_cst: the other half of fire_marked's handshake
    });
    ring();
    await_quiet(s);
}

void wait_module_t::want_write(wait_source_t& s, bool on) noexcept {
    if (s.write.exchange(on, std::memory_order_acq_rel) != on) ring();
}

void wait_module_t::run() noexcept {
    driver_.store(detail::this_thread_id());  // seq_cst: see wait_module_t::locked
    const callback_thread_scope_t delivering;
    std::unique_lock lock(m_);
    while (!stopping_.load()) {
        // Rebuild under the lock, where the list changes: the sets are the doorbell and every
        // attached socket, with write interest where it is armed.
        const std::uint32_t seen = gen_.load();
        wait_source_t* const first = head_;
        fd_set rd;
        fd_set wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(fd_[0], &rd);
        int top = fd_[0];
        for (wait_source_t* s = first; s != nullptr;
             s = link_of(*s).load(std::memory_order_relaxed)) {
            const int fd = s->fd.load(std::memory_order_relaxed);
            FD_SET(fd, &rd);
            if (s->write.load(std::memory_order_acquire)) FD_SET(fd, &wr);
            top = std::max(top, fd);
        }
        lock.unlock();
        int left = ::select(top + 1, &rd, &wr, nullptr, nullptr);
        // A socket closed while still attached fails the whole wait (EBADF): each source then
        // hears whether its own socket is the bad one, as a hang-up its read reports, so a
        // link that broke the detach-before-close rule is told rather than spun on.
        const bool failed = left < 0 && errno != EINTR;
        if (left > 0 && FD_ISSET(fd_[0], &rd)) {
            --left;
            char drain[16];
            while (::recv(fd_[0], drain, sizeof(drain), MSG_DONTWAIT) > 0) {
            }
        }
        // No lock across a callback; a detach since `seen` stops the batch, and the sources
        // still ready come back next pass. Each source is read only under its firing mark, so
        // a detach of it has either stopped this walk or waits for it; its successor is taken
        // before its callback runs, which may detach it. `select` counted the ready bits, so the
        // walk ends at the last one.
        for (wait_source_t* s = first; s != nullptr && (left > 0 || failed);) {
            wait_source_t* next = nullptr;
            const bool live = fire_marked(*s, seen, [&] {
                next = link_of(*s).load(std::memory_order_relaxed);
                const int fd = s->fd.load(std::memory_order_relaxed);
                const std::uint8_t ready = failed ? hangup_of(fd) : ready_of(fd, rd, wr);
                left -= (ready & 1U) + ((ready >> 1U) & 1U);  // READ and WRITE bits count one each
                if (ready != 0) s->fire(s->ctx, *s, ready);
            });
            if (!live) break;
            s = next;
        }
        lock.lock();
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
