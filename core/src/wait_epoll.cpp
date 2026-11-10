/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief The WIDE host's wait-module adapter (ADR-0087 §3, #2147): one level-triggered `epoll`
 *        set, woken by an `eventfd`, never by a timeout.
 *
 * Level-triggered on purpose: a batch the driver drops (a source was detached while it was
 * taken) loses nothing, since every source still ready is reported again on the next pass, and
 * a link may read one frame per pass and leave the rest for the next.
 */

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <mutex>

#include "libtracer/mem_heap.hpp"
#include "libtracer/wait.hpp"

namespace tr::net {

namespace {
/** @brief Events taken per pass: a larger burst is served over several passes. */
constexpr int kBatch = 16;

/** @brief The `epoll` interest of @p s with write interest @p write . */
epoll_event interest_of(wait_source_t& s, bool write) noexcept {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (write ? EPOLLOUT : 0U);
    ev.data.ptr = &s;
    return ev;
}

/**
 * @brief Register @p s in @p ep with operation @p op and its write flag as it reads now, then
 *        again for as long as the flag moved meanwhile.
 *
 * An arm (a sender) and a disarm (the driver) can cross: each flips the flag, then registers
 * it, with no lock between. Whichever registers last re-reads the flag after and registers it
 * once more if it moved, so the kernel always ends with the flag's last value, never a held
 * frame stranded with write interest unregistered. The loop runs again only while another
 * thread is flipping the flag: it is no wait.
 * @return The first registration's result.
 */
int register_interest(int ep, int op, wait_source_t& s) noexcept {
    bool write = s.write.load(std::memory_order_acquire);
    epoll_event ev = interest_of(s, write);
    const int rc = ::epoll_ctl(ep, op, s.fd.load(std::memory_order_relaxed), &ev);
    for (bool now = s.write.load(std::memory_order_acquire); rc == 0 && now != write;
         now = s.write.load(std::memory_order_acquire)) {
        write = now;
        ev = interest_of(s, write);
        ::epoll_ctl(ep, EPOLL_CTL_MOD, s.fd.load(std::memory_order_relaxed), &ev);
    }
    return rc;
}

/** @brief The @ref ready_t bits of the `epoll` events @p e . */
std::uint8_t ready_of(std::uint32_t e) noexcept {
    return static_cast<std::uint8_t>(
        ((e & EPOLLIN) != 0 ? static_cast<unsigned>(ready_t::READ) : 0U) |
        ((e & EPOLLOUT) != 0 ? static_cast<unsigned>(ready_t::WRITE) : 0U) |
        ((e & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) != 0 ? static_cast<unsigned>(ready_t::HANGUP)
                                                       : 0U));
}
}  // namespace

wait_module_t::wait_module_t(mem::block_source_t& src) noexcept : sources_(src) {
    fd_[0] = ::epoll_create1(EPOLL_CLOEXEC);
    fd_[1] = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (!ok()) return;
    // The wake is the one registration with no source: a null pointer marks it.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;
    if (::epoll_ctl(fd_[0], EPOLL_CTL_ADD, fd_[1], &ev) != 0) {
        ::close(fd_[1]);
        fd_[1] = -1;
    }
}

wait_module_t::wait_module_t() noexcept : wait_module_t(mem::net_source()) {}

wait_module_t::~wait_module_t() {
    for (const int fd : fd_)
        if (fd >= 0) ::close(fd);
}

bool wait_module_t::attach(wait_source_t& s) noexcept {
    // No dispatch lock: a source the set does not hold yet cannot be in the batch the driver
    // is running, so there is nothing to wait out, and `epoll_ctl` is safe against a
    // concurrent wait. A creator that holds its own lock (a transport vertex building a link
    // under its control-plane lock) therefore never waits on a callback that takes that lock.
    // The flag is set before the registration: once the kernel holds the source, a callback
    // may detach it, and that detach must find it attached.
    if (s.attached.load(std::memory_order_relaxed)) return false;
    s.attached.store(true, std::memory_order_relaxed);
    if (register_interest(fd_[0], EPOLL_CTL_ADD, s) == 0) return true;
    s.attached.store(false, std::memory_order_relaxed);
    return false;
}

void wait_module_t::detach(wait_source_t& s) noexcept {
    locked([&] {
        if (!s.attached.load(std::memory_order_relaxed)) return;
        ::epoll_ctl(fd_[0], EPOLL_CTL_DEL, s.fd.load(std::memory_order_relaxed), nullptr);
        s.attached.store(false, std::memory_order_relaxed);
        gen_.fetch_add(1);  // seq_cst: the other half of fire_marked's handshake
    });
    await_quiet(s);
}

void wait_module_t::want_write(wait_source_t& s, bool on) noexcept {
    if (s.write.exchange(on, std::memory_order_acq_rel) == on) return;
    // No lock: `epoll_ctl` is safe against a concurrent wait, and the registration converges
    // with a crossing flip. A source that is not attached answers ENOENT and keeps the bit:
    // its attach registers the flag as it then reads, and converges the same way.
    (void)register_interest(fd_[0], EPOLL_CTL_MOD, s);
}

void wait_module_t::run() noexcept {
    driver_.store(detail::this_thread_id());  // seq_cst: see wait_module_t::locked
    ++g_callback_depth;
    std::array<epoll_event, kBatch> ev;
    while (!stopping_.load()) {
        const std::uint32_t seen = gen_.load();
        const int n = ::epoll_wait(fd_[0], ev.data(), kBatch, -1);
        // No lock across a callback; a detach since `seen` stops the batch, and the sources
        // still ready come back next pass.
        for (int i = 0; i < n; ++i) {
            auto* const s = static_cast<wait_source_t*>(ev[static_cast<std::size_t>(i)].data.ptr);
            if (s != nullptr &&
                !fire_marked(*s, ready_of(ev[static_cast<std::size_t>(i)].events), seen))
                break;
        }
    }
    --g_callback_depth;
    driver_.store(detail::unowned_thread_id());
}

void wait_module_t::stop() noexcept {
    stopping_.store(true);
    ring();
}

void wait_module_t::ring() noexcept {
    // The counter stays readable once rung, so a stopped module never blocks again.
    const std::uint64_t one = 1;
    (void)!::write(fd_[1], &one, sizeof(one));
}

}  // namespace tr::net
