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
 * | lwIP sockets (ESP-IDF chip) | `wait_poll.cpp` | `select(2)` over the registered sockets | a
 * loopback datagram socket |
 *
 * Both run one conformance suite (`wait_module_test`). The lwIP-callback adapter of ADR-0087
 * §3, where a callback only hands a buffer over and rings the app's task, is #2148's.
 *
 * **Who drives it.** The application, from the thread or task it chooses, by calling
 * @ref tr::net::wait_module_t::run, and a link given that module through its config's `wait`
 * member spawns no thread. A link given none falls back to a thread of its own that drives a
 * private module holding only that link's sockets: the thread-per-link adapter, kept for
 * applications that do not drive a module. The config's `wait` stays a runtime pointer while
 * some links still lack the node module; once every link has it, the fallback becomes a
 * compile-time policy (ADR-0071 §Amendment, ruling 44).
 *
 * **Who pays for the table: the receiving link.** A source's place in the lwIP adapter's list
 * is its own @ref tr::net::wait_source_t::next, in the record its link owns, so each peer is
 * paid by the link it arrived on. The module draws no memory and never grows, on either
 * adapter, so an attach is never refused for want of it (#2147). The link exists only in a
 * build that compiles that adapter (`%tr::net::kListedWaitSources`); an `epoll` build
 * carries none.
 *
 * **A detach waits for its own source only.** The driving thread holds no lock while a callback
 * runs. A @ref tr::net::wait_module_t::detach from another thread waits only for the callback
 * of the source it detaches, if one is in flight, never for the rest of the batch, so a link
 * torn down under a lock that another link's callback takes does not stop either thread. What
 * remains is the wait a link's own receive thread always had: tearing a link down while
 * holding a lock that THAT link's own callback takes still stops both threads, so a caller
 * must not do that. From inside a callback on the same module both calls return at once.
 * Neither call waits once the module is stopped and its driver has left
 * @ref tr::net::wait_module_t::run.
 *
 * **One slow sink delays every link on the module** (ADR-0087 §Consequences): delivery runs
 * inline on the driving thread. A callback that blocks is an application bug the module makes
 * visible.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "libtracer/config.hpp"
#include "libtracer/park.hpp"
#include "libtracer/sync.hpp"
#include "libtracer/thread_id.hpp"

namespace tr::net {

/** @brief The empty stand-in for @ref tr::net::wait_source_t::next on a build whose adapter
 *         keeps no list (`%tr::net::kListedWaitSources` false). */
struct unlisted_t {};

/** @brief The readiness bits a @ref tr::net::wait_source_t callback is handed. */
enum class ready_t : std::uint8_t {
    NONE = 0,   /**< @brief Nothing. */
    READ = 1,   /**< @brief Readable, or a listening socket has a connection to accept. */
    WRITE = 2,  /**< @brief Writable, reported only while write interest is armed. */
    HANGUP = 4, /**< @brief The peer hung up or the socket failed: a read reports which. The
                     `epoll` adapter reports it from the kernel; the lwIP adapter, whose
                     `select` folds a hang-up into READ, only for a socket closed while still
                     attached. */
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
 * The link that owns the socket owns this record and pays for it (24 B on a 64-bit host),
 * and on the lwIP adapter its place in the list too (@ref next, one pointer more). Set @ref
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
    /** @brief True while attached. Set by @ref wait_module_t::attach before it registers the
     *         socket (with no lock on the epoll adapter: a callback reads it only once the kernel
     *         reports the socket); cleared only under the module's lock. Atomic (relaxed)
     *         because the epoll adapter's attach reads and sets it with no lock while a detach
     *         may clear it under one; every other ordering comes from the lock or the kernel's
     *         registration. */
    std::atomic<bool> attached{false};
    /** @brief The next source in the lwIP adapter's list: the receiving link pays for its own
     *         entry, so the module holds no table. Written under the module's lock; the driver
     *         reads it with none, under the source's firing mark. An empty member, taking no
     *         space, in a build without that adapter (`%tr::net::kListedWaitSources`). */
    [[no_unique_address]] std::conditional_t<kListedWaitSources, std::atomic<wait_source_t*>,
                                             unlisted_t> next{};
};

/**
 * @brief The node's one wait module (ADR-0087 §3): every socket wait on the node, with no
 *        timeout.
 *
 * Thread safety: @ref attach, @ref detach, @ref want_write and @ref stop may be called from any
 * thread, and from inside a callback. A @ref detach from another thread waits for that
 * source's callback if it is in flight, and for nothing else, so once it returns the source's
 * callback never runs again. It must not be called holding a lock that the detached source's
 * own callback takes (see the file's "A detach waits for its own source only"). @ref run is
 * called by one thread at a time.
 */
class wait_module_t {
   public:
    /** @brief Opens the wait and its wake; @ref ok reports whether both opened. Draws no
     *         memory: each attached source carries its own entry. */
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
     * @retval false The platform refused the registration, or, on the poll adapter, the socket
     *         is numbered past `FD_SETSIZE`; nothing was attached, and nobody already attached
     *         was evicted. Neither adapter draws memory, so neither attach nor a dispatch pass
     *         fails for want of it.
     */
    [[nodiscard]] bool attach(wait_source_t& s) noexcept;

    /**
     * @brief Stop waiting on @p s . A no-op for a detached source.
     *
     * From another thread it waits for @p s 's own callback, if it is in flight, and for no
     * other; from inside a callback it returns at once, and the rest of that batch is
     * redelivered next pass.
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
    /** @brief Run @p op under the module's lock, or straight away once the module is stopped
     *         and no thread is in @ref run (a stopped module dispatches nothing again). The
     *         driver holds the lock only between callbacks, never across one. */
    template <class Op>
    void locked(Op&& op) noexcept {
        // Sequentially consistent, as are run()'s and stop()'s stores and run()'s stop check:
        // the driver stores its id then reads the stop, and this reads the stop then the id.
        // With acquire and release alone both could read the other's old value, so a caller
        // would see "stopped, no driver" and skip the lock while a driver that missed the stop
        // still dispatches.
        const bool stopped = stopping_.load();
        if (stopped && driver_.load() == detail::unowned_thread_id()) {
            op();
            return;
        }
        const std::lock_guard lock(m_);
        op();
    }
    /**
     * @brief The driver's one fire: mark @p s firing, then run @p fire (which may read @p s
     *        and calls its callback) unless a detach was made since the batch's generation
     *        @p seen .
     * @return False when one was, and the rest of the batch is left for the next pass.
     *
     * The mark is in the module's own memory, never in the source, and it is stored before the
     * generation is re-read, as a detach bumps the generation before it reads the mark (all
     * sequentially consistent): either this sees the detach and never touches @p s , or the
     * detach sees the mark and waits for this one callback. A detach that waits is woken by a
     * notify, sent only while one is waiting.
     */
    template <class Fire>
    bool fire_marked(wait_source_t& s, std::uint32_t seen, Fire&& fire) noexcept {
        firing_.store(&s);
        const bool live = gen_.load() == seen;
        if (live) fire();
        firing_.store(nullptr);
        if (waiters_.load() != 0) park_wake(firing_);
        return live;
    }
    /** @brief After a detach of @p s : block until its callback is not running, unless the
     *         caller is the driver, whose own callback this is. A blocking wait, never a spin,
     *         so a caller above the driver's priority on one core does not starve it; parked
     *         through `%tr::park_wait`, as `std::atomic::wait` costs an MCU libstdc++'s 2 KB
     *         waiter table (#2507). */
    void await_quiet(const wait_source_t& s) noexcept {
        if (driver_.load() == detail::this_thread_id()) return;
        waiters_.fetch_add(1);
        for (const wait_source_t* f = firing_.load(); f == &s; f = firing_.load())
            park_wait(firing_, f, std::memory_order_seq_cst);
        waiters_.fetch_sub(1);
    }
    /** @brief Wake a blocked @ref run (the stop and, on the poll adapter, a set change). */
    void ring() noexcept;

    /** @brief The adapter's two descriptors: the `epoll` set and its `eventfd`, or the poll
     *         adapter's loopback doorbell socket twice (its sending and receiving end). */
    int fd_[2] = {-1, -1};
    /** @brief Bumped by every detach: a batch taken before a bump may name a source that is
     *         gone, so it is dropped. */
    std::atomic<std::uint32_t> gen_{0};
    /** @brief The source whose callback the driver is running, or about to: what a detach
     *         from another thread waits on, so it waits for that source alone. Kept in the
     *         module, never in the source, so the driver never writes to a source a finished
     *         detach may have freed. Null between callbacks. */
    std::atomic<const wait_source_t*> firing_{nullptr};
    /** @brief Detaches blocked waiting for a callback: the driver notifies only while one is. */
    std::atomic<std::uint32_t> waiters_{0};
    std::atomic<bool> stopping_{false}; /**< @brief Set by @ref stop. */
    /** @brief The thread inside @ref run, or none. */
    std::atomic<detail::thread_id_t> driver_{detail::unowned_thread_id()};
    tr::mutex_t m_; /**< @brief Serializes attach and detach; the poll adapter's driver holds it
                         while it rebuilds its set, never across a callback. */
    /** @brief The poll adapter's list of attached sources, linked through their own
     *         @ref wait_source_t::next (null on epoll). Under `m_`. */
    wait_source_t* head_ = nullptr;
};

}  // namespace tr::net
