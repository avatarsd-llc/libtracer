/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * posix_endpoint — the recv-thread/endpoint scaffold every POSIX socket
 * transport shares (udp / tcp / ws server / ws client). One home for the
 * stop-flag + receive-thread lifecycle and the 100 ms poll/timeout idioms that
 * let a blocking socket loop notice a clean shutdown; the transports keep what
 * is genuinely theirs (fds, receivers, counters, framing). The STREAM
 * transports additionally share the one-peer fd/teardown discipline —
 * stream_endpoint_t below owns that layer (the peer-fd atomic, the write
 * mutex, the teardown-under-write-lock ordering, the one-peer accept loop);
 * udp keeps its datagram shape (one connectionless fd, no peer teardown).
 */
#pragma once

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <span>

/** @brief The POSIX scatter-gather descriptor (`<sys/uio.h>`), forward-declared
 *         so this header need not pull the system socket headers in. */
struct iovec;

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
 * Stream transports (tcp / ws) layer the shared one-peer fd/teardown
 * discipline on top via @ref stream_endpoint_t — the write-serialization and
 * teardown-under-write-lock invariants live there, with the code.
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
     * Spawns via `pthread_create` (not `std::thread`): the constructor of the
     * latter THROWS on failure, which under `-fno-exceptions` (the MCU build)
     * `std::abort`s — a thread-spawn OOM on a starved node would bring the whole
     * process down instead of soft-failing. `pthread_create` returns an error
     * code; a failed spawn leaves the endpoint simply not receiving (no abort).
     *
     * @param body       The thread body (the transport's accept/recv loop).
     * @param stack_size Recv-thread stack size in bytes, or 0 for the platform
     *        default (the ONLY value that preserves prior behavior). A non-zero
     *        hint is applied via `pthread_attr_setstacksize`, honored by glibc
     *        AND the ESP-IDF pthread layer (where it maps to the FreeRTOS task
     *        stack) — the portable knob that lets an integrator right-size this
     *        thread instead of inflating `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`
     *        for every pthread in the system. A hint below the platform floor is
     *        ignored (the default stack is used) rather than failing the spawn.
     */
    void start(std::function<void()> body, std::size_t stack_size = 0);

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
    /**
     * @brief pthread entry trampoline — runs @ref body_ then returns.
     *
     * @param self The owning @ref posix_endpoint_t (the `pthread_create` arg).
     * @return Always nullptr (the thread's exit value is unused).
     */
    static void* thread_entry(void* self);

    std::function<void()> body_; /**< @brief The owned thread body @ref thread_entry runs. */
    pthread_t thread_{};         /**< @brief The receive thread (joined by stop_and_join;
                                            valid only while @ref started_). */
    bool started_ = false;       /**< @brief Whether @ref thread_ holds a joinable thread
                                            (a failed/never-attempted spawn stays false). */
};

/**
 * @brief The one-peer fd/teardown discipline every POSIX STREAM transport
 *        shares (tcp dial+listen, ws server, ws client).
 *
 * Owns the live peer fd (@ref conn_fd_) and the write mutex (@ref write_m_),
 * and is the ONE home of the invariants that keep a concurrent `send()` and
 * the recv thread's connection teardown safe against each other:
 *
 * **Write-serialization invariant:** every write to the peer fd happens with
 * @ref write_m_ held across the WHOLE write — so (a) two senders can never
 * interleave their records on the stream, and (b) the recv thread cannot
 * close and reset the fd underneath an in-flight write. `send()` reads
 * @ref conn_fd_ INSIDE the lock, pairing with the teardown below.
 *
 * **Teardown-under-write-lock invariant:** a recv thread that closes the peer
 * fd MUST reset @ref conn_fd_ to -1 under @ref write_m_ BEFORE `::close()`
 * (@ref teardown_peer) — so a sender never writes to (or reads) a
 * closed/reused fd.
 *
 * The one-peer accept loop shape (poll-100ms-recheck accept → per-peer setup
 * → serve → teardown → re-accept) shared by tcp's LISTEN mode and the ws
 * server lives here too (@ref run_accept_loop). A protected base, inherited
 * privately by the concrete stream transports; udp stays on plain
 * posix_endpoint_t — a datagram socket has no per-peer fd to tear down and
 * its single-syscall sends need no serialization.
 */
class stream_endpoint_t : protected posix_endpoint_t {
   protected:
    /** @brief Constructs with no peer connected (@ref conn_fd_ = -1). */
    stream_endpoint_t() = default;

    /**
     * @brief Closes a leftover peer fd (one the recv thread never tore down).
     *
     * Runs AFTER the derived destructor, whose first act was stop_and_join
     * (the posix_endpoint_t teardown invariant) — so no thread can race this.
     * A normally-torn-down connection already reset @ref conn_fd_ to -1 and
     * this is a no-op; it only catches a never-spawned thread (a failed dial /
     * handshake left the fd parked) so nothing double-closes.
     */
    ~stream_endpoint_t();

    stream_endpoint_t(const stream_endpoint_t&) = delete;
    stream_endpoint_t& operator=(const stream_endpoint_t&) = delete;

    /**
     * @brief Write @p bytes to @p fd completely, resuming partial writes.
     *
     * A stream write may stop anywhere; loops `::send` (MSG_NOSIGNAL — a
     * vanished peer must not SIGPIPE the process) until done. Peer-gone /
     * error drops the rest silently (link-down is #66 lifecycle). The caller
     * holds @ref write_m_ per the write-serialization invariant.
     *
     * @param fd    The destination fd; a negative fd is a no-op.
     * @param bytes The bytes to write.
     */
    static void write_all(int fd, std::span<const std::byte> bytes);

    /**
     * @brief Write the gathered @p vec entries to @p fd completely as ONE record,
     *        resuming partial writes — the zero-copy scatter-gather twin of
     *        @ref write_all.
     *
     * `::sendmsg` (MSG_NOSIGNAL — a vanished peer must not SIGPIPE the process)
     * emits every iovec in one syscall; a stream write may stop anywhere, so the
     * loop advances past fully-written entries and trims a partially-written one
     * and resends. @p vec is CONSUMED (its entries' `iov_base`/`iov_len` are
     * advanced in place) — a caller that fans the same gather to several fds must
     * pass a fresh copy per fd. Peer-gone / error drops the rest silently
     * (link-down is #66 lifecycle). The caller holds @ref write_m_ per the
     * write-serialization invariant.
     *
     * @param fd    The destination fd; a negative fd is a no-op.
     * @param vec   The iovec array to gather (consumed in place).
     * @param count The number of entries in @p vec.
     */
    static void write_all_iov(int fd, ::iovec* vec, std::size_t count);

    /**
     * @brief Write @p bytes to the live peer as one serialized record.
     *
     * The whole write-serialization invariant in one call: takes
     * @ref write_m_, reads @ref conn_fd_ inside the lock, and @ref write_all
     * s the bytes. No-op while no peer is connected.
     *
     * @param bytes One complete encoded record's bytes.
     */
    void send_all_locked(std::span<const std::byte> bytes);

    /**
     * @brief Tear the peer connection down (recv-thread side).
     *
     * The teardown-under-write-lock invariant as code: resets @ref conn_fd_
     * to -1 under @ref write_m_, THEN `::close(fd)` — a concurrent `send()`
     * either finished against the still-open fd or reads -1 and no-ops.
     *
     * @param fd The peer fd the recv loop was serving.
     */
    void teardown_peer(int fd);

    /**
     * @brief The one-peer accept loop (tcp LISTEN / ws server shape).
     *
     * Until @ref stop_: one poll-100ms-recheck accept pass (@ref poll_accept);
     * on a new connection run @p on_accept (per-peer setup — socket options,
     * handshake; return false to reject: the fd is closed and the loop
     * re-accepts), publish the fd to @ref conn_fd_, run @p serve_peer, then
     * @ref teardown_peer and re-accept the next peer.
     *
     * @param listen_fd  The bound+listening socket.
     * @param on_accept  Per-peer setup; false rejects the connection.
     * @param serve_peer The per-connection recv loop; returns on peer
     *                   departure or @ref stop_.
     */
    void run_accept_loop(int listen_fd, const std::function<bool(int)>& on_accept,
                         const std::function<void(int)>& serve_peer);

    std::mutex write_m_;           /**< @brief Serializes writes to @ref conn_fd_ (see the
                                               write-serialization invariant). */
    std::atomic<int> conn_fd_{-1}; /**< @brief The live peer connection (-1 = none). */
};

}  // namespace tr::net
