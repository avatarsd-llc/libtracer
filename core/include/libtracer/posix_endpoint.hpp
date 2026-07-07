/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * posix_endpoint — the recv-thread/endpoint scaffold every POSIX socket
 * transport shares (udp / tcp / ws server / ws client). One home for the
 * stop-flag + receive-thread lifecycle and the 100 ms poll/timeout idioms that
 * let a blocking socket loop notice a clean shutdown; the transports keep what
 * is genuinely theirs (fds, receivers, counters, write mutexes, framing).
 */
#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace tr::net {

/**
 * @brief The shared recv-thread scaffold of the POSIX socket transports.
 *
 * A protected base (inherited privately by the concrete transports) owning the
 * `stop_` flag and the receive thread, plus the socket-timeout/poll idioms that
 * make a blocking loop shutdown-responsive: every blocking wait is bounded to
 * 100 ms (SO_RCVTIMEO or ::poll), after which the loop re-checks @ref stop_.
 *
 * **Teardown invariant (derived destructors):** call @ref stop_and_join
 * FIRST, before releasing ANY resource the thread body touches (sockets,
 * receivers, buffers) — the thread may be mid-loop until the join returns.
 *
 * **Teardown-under-write-lock invariant (derived recv loops):** a transport
 * whose recv thread closes a peer fd that a concurrent `send()` may use MUST
 * reset the fd atomic under the transport's own write mutex before
 * `::close()`, and `send()` must read the fd inside that same mutex — so a
 * sender never writes to (or reads) a closed/reused fd. The fd and the write
 * mutex stay in the derived class; the discipline is documented here because
 * every stream transport built on this base must follow it.
 */
class posix_endpoint_t {
   protected:
    /** @brief Constructs with no thread running and @ref stop_ clear. */
    posix_endpoint_t() = default;

    /**
     * @brief Joins a still-running thread as a last resort.
     *
     * Derived destructors must have called @ref stop_and_join already (see the
     * teardown invariant above) — by the time this runs, derived members the
     * thread touches are gone. The defensive join only covers a derived class
     * that never spawned a thread or already joined it (both no-ops).
     */
    ~posix_endpoint_t();

    posix_endpoint_t(const posix_endpoint_t&) = delete;
    posix_endpoint_t& operator=(const posix_endpoint_t&) = delete;

    /**
     * @brief Spawn the receive thread running @p body.
     *
     * Call at most once, from the derived constructor, after the socket is up
     * and every resource @p body touches is initialized. @p body must poll
     * @ref stop_ (directly or via the bounded waits below) and return promptly
     * once it is set.
     *
     * @param body The thread body (the transport's accept/recv loop).
     */
    void start(std::function<void()> body);

    /**
     * @brief Request shutdown and join the receive thread (idempotent).
     *
     * Sets @ref stop_ and joins the thread if one is running. MUST be the
     * FIRST act of every derived destructor — only after it returns may the
     * destructor release the resources the thread body touches.
     */
    void stop_and_join();

    /**
     * @brief Arm the 100 ms receive timeout (SO_RCVTIMEO) on @p fd.
     *
     * The idiom that keeps a blocking `recv`/`recvfrom` loop shutdown-
     * responsive: each blocked read wakes within 100 ms so the loop can
     * re-check @ref stop_ and resume (or exit) — one home for the constant.
     *
     * @param fd The socket to arm.
     */
    static void set_rcv_timeout(int fd);

    /**
     * @brief One bounded readability wait: `::poll(POLLIN, 100 ms)` on @p fd.
     *
     * The poll-flavored twin of @ref set_rcv_timeout for loops that wait
     * before reading. Returns the raw `::poll` result — `> 0` readable,
     * `0` timeout (re-check @ref stop_ and continue), `< 0` error.
     *
     * @param fd The socket to wait on.
     * @return The `::poll` return value.
     */
    static int poll_readable(int fd);

    /**
     * @brief One iteration of the poll-100ms-recheck accept loop.
     *
     * Waits up to 100 ms for @p listen_fd to become readable, then accepts.
     * Returns the accepted fd, or -1 on timeout / poll error / accept failure
     * — the caller's loop simply continues, re-checking @ref stop_ each pass.
     *
     * @param listen_fd The bound+listening socket.
     * @return The accepted connection fd, or -1 when there is none this pass.
     */
    static int poll_accept(int listen_fd);

    /**
     * @brief The shutdown flag every blocking loop polls (set by
     *        @ref stop_and_join; read with relaxed order — it is a flag, not
     *        a synchronizer; the join provides the ordering).
     */
    std::atomic<bool> stop_{false};

   private:
    std::thread thread_; /**< @brief The receive thread (joined by stop_and_join). */
};

}  // namespace tr::net
