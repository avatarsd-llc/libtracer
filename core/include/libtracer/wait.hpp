/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief `tr::net::wait_module_t`, the one owner of every socket wait on a node (ADR-0087 §3,
 *        #2147), and `tr::net::wait_source_t`, one socket's registration with it.
 *
 * A link registers each socket it reads with the node's wait module and owns no thread for it.
 * Whatever drives the module calls @ref tr::net::wait_module_t::run, and every ready source's
 * callback runs there, to completion: the read, the decode, the delivery. The wait has no
 * timeout. It blocks until a socket is ready or @ref tr::net::wait_module_t::stop wakes it,
 * so a quiet node wakes 0 times a second, and the library reads no clock to wait.
 *
 * The adapter is the build's, picked by which translation unit compiles (ADR-0071, never an
 * in-source `#ifdef`):
 *
 * | Target | TU | Wait | Wake |
 * | --- | --- | --- | --- |
 * | Linux host | `wait_epoll.cpp` | one `epoll` set, level-triggered | an `eventfd` |
 * | lwIP sockets (ESP-IDF chip) | `wait_poll.cpp` | `poll(2)` over the registered sockets | a
 * loopback datagram socket |
 *
 * Both run one conformance suite (`wait_module_test`). The lwIP-callback adapter of ADR-0087
 * §3, where a callback only hands a buffer over and rings the app's task, is #2148's.
 *
 * **Who drives it.** The application, from the thread or task it chooses, by calling
 * @ref tr::net::wait_module_t::run, and a link given that module through its config's `wait`
 * member spawns no thread. A link given none falls back to a thread of its own that drives a
 * private module holding only that link's sockets: the thread-per-link adapter, kept for
 * applications that do not drive a module.
 *
 * **One slow sink delays every link on the module** (ADR-0087 §Consequences): delivery runs
 * inline on the driving thread. A callback that blocks is an application bug the module makes
 * visible.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/sync.hpp"
#include "libtracer/thread_id.hpp"

namespace tr::net {

/** @brief The readiness bits a @ref tr::net::wait_source_t callback is handed. */
enum class ready_t : std::uint8_t {
    NONE = 0,   /**< @brief Nothing. */
    READ = 1,   /**< @brief Readable, or a listening socket has a connection to accept. */
    WRITE = 2,  /**< @brief Writable, reported only while write interest is armed. */
    HANGUP = 4, /**< @brief The peer hung up or the socket failed: a read reports which. */
};

/** @brief True when @p set says a read is due: readable, or hung up (the read reports it). */
[[nodiscard]] constexpr bool readable(std::uint8_t set) noexcept {
    return (set & (static_cast<std::uint8_t>(ready_t::READ) |
                   static_cast<std::uint8_t>(ready_t::HANGUP))) != 0;
}

/** @brief True when @p set says the socket is writable. */
[[nodiscard]] constexpr bool writable(std::uint8_t set) noexcept {
    return (set & static_cast<std::uint8_t>(ready_t::WRITE)) != 0;
}

/**
 * @brief One socket registered with a @ref tr::net::wait_module_t, as the `{fire, ctx}`
 *        waiter of ADR-0084 §3 plus the descriptor.
 *
 * The link that owns the socket owns this record and pays for it (24 B on a 64-bit host). Set @ref
 * fire, @ref ctx and @ref fd before
 * @ref wait_module_t::attach, and keep the record alive and unmoved until
 * @ref wait_module_t::detach returns.
 */
struct wait_source_t {
    /**
     * @brief Runs on the driving thread when the socket is ready.
     * @param ctx   The record's @ref ctx.
     * @param self  This record.
     * @param ready The @ref ready_t bits that hold.
     */
    using fire_t = void (*)(void* ctx, wait_source_t& self, std::uint8_t ready);

    fire_t fire = nullptr; /**< @brief The callback. */
    void* ctx = nullptr;   /**< @brief Its context: the owning link. */
    /** @brief The socket; -1 for none. Changed only while the record is detached. Atomic
     *         because a link's senders read it too (a server session's fd is this one). */
    std::atomic<int> fd{-1};
    /** @brief Write interest, as last armed (@ref wait_module_t::want_write). */
    std::atomic<bool> write{false};
    /** @brief True while attached. Changed only under the module's dispatch lock. */
    bool attached = false;
};

/**
 * @brief The node's one wait module (ADR-0087 §3): every socket wait on the node, with no
 *        timeout.
 *
 * Thread safety: @ref attach, @ref detach, @ref want_write and @ref stop may be called from any
 * thread, and from inside a callback. A @ref detach from another thread waits out the callbacks
 * the driver is running, so once it returns the source's callback never runs again; it must
 * not be called holding a lock a callback takes. @ref run is called by one thread at a time.
 */
class wait_module_t {
   public:
    /**
     * @brief Opens the wait and its wake. @ref ok reports whether both opened.
     * @param src Where the poll adapter draws its table of attached sources and its two
     *            descriptor sets (one entry per source plus the wake, grown at attach). The
     *            epoll adapter draws nothing. Must outlive the module.
     */
    explicit wait_module_t(mem::block_source_t& src) noexcept;
    /** @brief Opens with the poll adapter's set drawn from the process net sub-pool. */
    wait_module_t() noexcept;
    /** @brief Closes the wait and its wake. Every source must have been detached and
     *         @ref run must have returned. */
    ~wait_module_t();

    wait_module_t(const wait_module_t&) = delete;
    wait_module_t& operator=(const wait_module_t&) = delete;

    /** @brief True when the wait and its wake both opened. */
    [[nodiscard]] bool ok() const noexcept { return fd_[0] >= 0 && fd_[1] >= 0; }

    /**
     * @brief Start waiting on @p s : read interest, plus write interest when `s.write` is set.
     * @retval false The platform refused the registration, or the poll adapter's table or
     *         spare set could not be grown to hold it; nothing was attached, and nobody already
     *         attached was evicted. Attach is the only place the poll adapter grows, so a
     *         dispatch pass never fails for want of memory.
     */
    [[nodiscard]] bool attach(wait_source_t& s) noexcept;

    /**
     * @brief Stop waiting on @p s . A no-op for a detached source.
     *
     * From another thread it waits for the callbacks in flight to finish; from inside a
     * callback it returns at once, and the rest of that batch is redelivered next pass.
     */
    void detach(wait_source_t& s) noexcept;

    /**
     * @brief Arm or disarm write interest on @p s .
     *
     * A link arms it inside its egress writer role when a write leaves a frame held, so the
     * socket cannot be closed under the call, and disarms it on the driving thread once the
     * frame is out. A no-op when the interest is already as asked.
     */
    void want_write(wait_source_t& s, bool on) noexcept;

    /**
     * @brief Dispatch ready sources until @ref stop is called. The caller's thread is the node's
     * I/O context, and every callback runs on it.
     *
     * Blocks with no timeout between passes. Returns at once if @ref stop was already called.
     */
    void run() noexcept;

    /** @brief Make @ref run return: wakes it if it is blocked. Idempotent. */
    void stop() noexcept;

   private:
    /** @brief Run @p op under the dispatch lock, or straight away on the driving thread,
     *         which already holds it. */
    template <class Op>
    void locked(Op&& op) noexcept {
        if (driver_.load(std::memory_order_acquire) == detail::this_thread_id()) {
            op();
            return;
        }
        const std::lock_guard lock(m_);
        op();
    }
    /** @brief Wake a blocked @ref run (the stop and, on the poll adapter, a set change). */
    void ring() noexcept;

    /** @brief The adapter's two descriptors: the `epoll` set and its `eventfd`, or the poll
     *         adapter's loopback doorbell socket twice (its sending and receiving end). */
    int fd_[2] = {-1, -1};
    /** @brief Bumped by every detach (and, on the poll adapter, every attach): a batch taken
     *         before a bump may name a source that is gone, so it is dropped. */
    std::atomic<std::uint32_t> gen_{0};
    std::atomic<bool> stopping_{false}; /**< @brief Set by @ref stop. */
    /** @brief The thread inside @ref run, or none. */
    std::atomic<detail::thread_id_t> driver_{detail::unowned_thread_id()};
    tr::mutex_t m_; /**< @brief Held while a batch is dispatched. */
    /** @brief The poll adapter's attached sources, drawn from its source; empty on epoll. */
    mem::block_array_t<wait_source_t*> sources_;
    struct state_t;  // the adapter's own state, defined in its translation unit
    /** @brief The poll adapter's descriptor sets, drawn from its source when it opens; none on
     *         epoll. */
    mem::poly_ptr_t<state_t> state_;
};

}  // namespace tr::net
