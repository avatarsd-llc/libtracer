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

/** @brief The `epoll` interest of @p s . */
epoll_event interest_of(wait_source_t& s) noexcept {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (s.write.load(std::memory_order_acquire) ? EPOLLOUT : 0U);
    ev.data.ptr = &s;
    return ev;
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
    bool done = false;
    locked([&] {
        epoll_event ev = interest_of(s);
        done = !s.attached &&
               ::epoll_ctl(fd_[0], EPOLL_CTL_ADD, s.fd.load(std::memory_order_relaxed), &ev) == 0;
        s.attached = s.attached || done;
    });
    return done;
}

void wait_module_t::detach(wait_source_t& s) noexcept {
    locked([&] {
        if (!s.attached) return;
        ::epoll_ctl(fd_[0], EPOLL_CTL_DEL, s.fd.load(std::memory_order_relaxed), nullptr);
        s.attached = false;
        gen_.fetch_add(1, std::memory_order_acq_rel);
    });
}

void wait_module_t::want_write(wait_source_t& s, bool on) noexcept {
    if (s.write.exchange(on, std::memory_order_acq_rel) == on) return;
    // No lock: `epoll_ctl` is safe against a concurrent wait. A source that is not attached
    // answers ENOENT and keeps the bit for its next attach.
    epoll_event ev = interest_of(s);
    ::epoll_ctl(fd_[0], EPOLL_CTL_MOD, s.fd.load(std::memory_order_relaxed), &ev);
}

void wait_module_t::run() noexcept {
    driver_.store(detail::this_thread_id(), std::memory_order_release);
    std::array<epoll_event, kBatch> ev;
    while (!stopping_.load(std::memory_order_acquire)) {
        const std::uint32_t seen = gen_.load(std::memory_order_acquire);
        const int n = ::epoll_wait(fd_[0], ev.data(), kBatch, -1);
        const std::lock_guard lock(m_);
        // A detach since `seen` may have freed a source this batch names: stop at the first
        // bump, and the sources still ready come back next pass.
        for (int i = 0; i < n && gen_.load(std::memory_order_acquire) == seen; ++i) {
            auto* const s = static_cast<wait_source_t*>(ev[static_cast<std::size_t>(i)].data.ptr);
            if (s != nullptr) s->fire(s->ctx, *s, ready_of(ev[static_cast<std::size_t>(i)].events));
        }
    }
    driver_.store(detail::unowned_thread_id(), std::memory_order_release);
}

void wait_module_t::stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    ring();
}

void wait_module_t::ring() noexcept {
    // The counter stays readable once rung, so a stopped module never blocks again.
    const std::uint64_t one = 1;
    (void)!::write(fd_[1], &one, sizeof(one));
}

}  // namespace tr::net
