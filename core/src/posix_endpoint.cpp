/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/posix_endpoint.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <utility>

namespace tr::net {

namespace {
/** @brief SIGPIPE would otherwise kill the process if the peer vanishes
 *         mid-write (platforms without the flag fall back to 0). */
#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif

/**
 * @brief How a failed `send(2)`/`sendmsg(2)` attempt is classified (#903 / #948).
 */
enum class write_fault_t : std::uint8_t {
    RESUME,         /**< @brief EINTR on a healthy connection — resume where the write stopped. */
    PEER_GONE,      /**< @brief This socket is dead — drop the rest silently (#66 lifecycle). */
    MALFORMED_CALL, /**< @brief The arguments were rejected — OUR defect, not a disconnect. */
};

/** @brief Process-wide malformed-call tally (#948) — see @ref write_fault_stats_t. */
std::atomic<std::uint64_t> g_malformed_calls{0};

/** @brief The errno of the most recent malformed-call fault (#948). */
std::atomic<int> g_last_malformed_errno{0};

/**
 * @brief The ONE write-fault policy both full-write helpers share (#903 / #948).
 *
 * Three outcomes, not two — conflating the last with the middle is #948, where one
 * unimplemented flag on one platform's `sendmsg` made every data frame vanish while the
 * connection stayed up, looking exactly like a peer that never asked for anything:
 *
 * - **RESUME.** A blocking `send`/`sendmsg` that a signal interrupts BEFORE any byte moved
 *   fails with EINTR — the connection is untouched and healthy, so the write must resume
 *   where it stopped. Abandoning it would leave a partial frame on a live framed stream and
 *   desync the peer's framing permanently (every later byte parses under the wrong length).
 * - **PEER_GONE.** An errno that says this fd is no longer a usable connection (the peer
 *   went away, the route died, the fd is not an open socket), plus the `n == 0` a stream
 *   write of a non-empty buffer must never return. Nothing is deliverable on it: drop the
 *   rest silently, link-down is #66 lifecycle. `EBADF`/`ENOTSOCK` sit here deliberately —
 *   a recycled fd is indistinguishable from a vanished peer, and a teardown race must not
 *   fabricate a defect report in the counter below.
 * - **MALFORMED_CALL.** Everything else: the kernel rejected the ARGUMENTS (`EOPNOTSUPP`,
 *   `EINVAL`, …) or ran short of buffers (`ENOBUFS`). The socket is fine and the bytes are
 *   still deliverable, so this must never be read as a disconnect — it is counted, its errno
 *   is recorded, and the write is re-attempted rather than silently truncated.
 *
 * @param n The `send(2)` / `sendmsg(2)` return value (must be `<= 0`).
 */
write_fault_t classify_write_fault(ssize_t n) {
    // errno is meaningful only for a negative return.
    if (n == 0) return write_fault_t::PEER_GONE;
    switch (errno) {
        case EINTR:
            return write_fault_t::RESUME;
        case EPIPE:
        case ECONNRESET:
        case ECONNABORTED:
        case ENOTCONN:
        case ESHUTDOWN:
        case ETIMEDOUT:
        case EHOSTDOWN:
        case EHOSTUNREACH:
        case ENETDOWN:
        case ENETRESET:
        case ENETUNREACH:
        case EBADF:
        case ENOTSOCK:
            return write_fault_t::PEER_GONE;
        default:
            return write_fault_t::MALFORMED_CALL;
    }
}

/**
 * @brief Count one malformed-call fault and answer whether to re-attempt the write (#948).
 *
 * The re-attempt allowance is ONE per stretch of progress, and that is a proof rather than a
 * tunable: a call malformed in its arguments is deterministic, so a second identical result
 * PROVES the defect is real (count it, abandon) while a first-and-only failure was a
 * transient rejection the frame can still survive. Zero re-attempts would silently truncate a
 * framed stream exactly as #903 did; an unbounded retry would spin forever on the deterministic
 * case. @p spent is cleared by every byte of progress, so the loop still terminates — each
 * re-attempt is either the last or is followed by at least one byte written, and a frame is
 * finite.
 *
 * @param spent Whether this write has already used its re-attempt (updated in place).
 * @return true to re-attempt the write, false to abandon the rest.
 */
bool retry_malformed_call(bool& spent) {
    g_malformed_calls.fetch_add(1, std::memory_order_relaxed);
    g_last_malformed_errno.store(errno, std::memory_order_relaxed);
    if (spent) return false;
    spent = true;
    return true;
}

/** @brief The errno the #948 injection seam wants this attempt to fail with, or 0. */
int injected_write_errno() {
    return tr::detail::write_fault_inject_hook == nullptr ? 0
                                                          : tr::detail::write_fault_inject_hook();
}

/** @brief One `send(2)` attempt through the #948 injection seam. */
ssize_t send_bytes(int fd, const void* buf, std::size_t len) {
    if (const int fake = injected_write_errno(); fake != 0) {
        errno = fake;
        return -1;
    }
    return ::send(fd, buf, len, MSG_NOSIGNAL);
}

/** @brief One `sendmsg(2)` attempt through the #948 injection seam. */
ssize_t send_gather(int fd, const msghdr* msg) {
    if (const int fake = injected_write_errno(); fake != 0) {
        errno = fake;
        return -1;
    }
    return ::sendmsg(fd, msg, MSG_NOSIGNAL);
}
}  // namespace

write_fault_stats_t write_fault_stats() noexcept {
    return {.malformed_calls = g_malformed_calls.load(std::memory_order_relaxed),
            .last_errno = g_last_malformed_errno.load(std::memory_order_relaxed)};
}

posix_endpoint_t::~posix_endpoint_t() { stop_and_join(); }

void* posix_endpoint_t::thread_entry(void* self) {
    static_cast<posix_endpoint_t*>(self)->body_();
    return nullptr;
}

void posix_endpoint_t::start(std::function<void()> body, std::size_t stack_size) {
    body_ = std::move(body);

    pthread_attr_t attr;
    ::pthread_attr_init(&attr);
    if (stack_size != 0) {
        // A hint below the platform floor makes setstacksize return EINVAL and
        // leaves the attr's default stacksize in place — we fall back to the
        // default stack rather than fail the spawn.
        (void)::pthread_attr_setstacksize(&attr, stack_size);
    }

    // Error-code return, not a throw: see start()'s header contract (a std::thread
    // spawn failure would std::abort under -fno-exceptions).
    started_ = (::pthread_create(&thread_, &attr, &posix_endpoint_t::thread_entry, this) == 0);
    ::pthread_attr_destroy(&attr);
}

void posix_endpoint_t::stop_and_join() {
    stop_.store(true, std::memory_order_relaxed);
    if (started_) {
        ::pthread_join(thread_, nullptr);
        started_ = false;
    }
}

void posix_endpoint_t::set_rcv_timeout(int fd) {
    timeval tv{.tv_sec = 0, .tv_usec = 100000};  // 100 ms
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int posix_endpoint_t::poll_readable(int fd) {
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    return ::poll(&pfd, 1, 100);
}

int posix_endpoint_t::poll_accept(int listen_fd) {
    if (poll_readable(listen_fd) <= 0) return -1;  // timeout / error → caller re-checks stop_
    return ::accept(listen_fd, nullptr, nullptr);
}

stream_endpoint_t::~stream_endpoint_t() {
    // The derived destructor already ran stop_and_join (its first act, per the
    // posix_endpoint_t teardown invariant), so nothing races this exchange.
    const int leftover = conn_fd_.exchange(-1, std::memory_order_relaxed);
    if (leftover >= 0) ::close(leftover);
}

void stream_endpoint_t::write_all(int fd, std::span<const std::byte> bytes) {
    if (fd < 0) return;
    std::size_t off = 0;
    bool retry_spent = false;
    while (off < bytes.size()) {
        const ssize_t n = send_bytes(fd, bytes.data() + off, bytes.size() - off);
        if (n <= 0) {
            const write_fault_t fault = classify_write_fault(n);
            if (fault == write_fault_t::RESUME) continue;
            if (fault == write_fault_t::PEER_GONE) return;  // dead socket → drop the rest
            if (!retry_malformed_call(retry_spent)) return;
            continue;  // our call, not the peer: the bytes are still deliverable
        }
        off += static_cast<std::size_t>(n);
        retry_spent = false;
    }
}

void stream_endpoint_t::write_all_iov(int fd, ::iovec* vec, std::size_t count) {
    if (fd < 0) return;
    bool retry_spent = false;
    while (count > 0) {
        msghdr msg{};
        msg.msg_iov = vec;
        msg.msg_iovlen = count;
        const ssize_t n = send_gather(fd, &msg);
        if (n <= 0) {
            const write_fault_t fault = classify_write_fault(n);
            if (fault == write_fault_t::RESUME) continue;
            if (fault == write_fault_t::PEER_GONE) return;  // dead socket → drop the rest
            if (!retry_malformed_call(retry_spent)) return;
            continue;  // our call, not the peer: the bytes are still deliverable
        }
        retry_spent = false;
        std::size_t done = static_cast<std::size_t>(n);
        // Advance past every fully-written entry, then trim the one the write
        // stopped inside — the stream may stop at any byte boundary.
        while (count > 0 && done >= vec->iov_len) {
            done -= vec->iov_len;
            ++vec;
            --count;
        }
        if (count > 0 && done > 0) {
            vec->iov_base = static_cast<std::byte*>(vec->iov_base) + done;
            vec->iov_len -= done;
        }
    }
}

void stream_endpoint_t::send_all_locked(std::span<const std::byte> bytes) {
    // Hold write_m_ across the whole write so the recv thread cannot close and
    // reset conn_fd_ underneath us; read the fd inside the lock to pair with
    // teardown_peer.
    const std::lock_guard lock(write_m_);
    write_all(conn_fd_.load(std::memory_order_relaxed), bytes);
}

void stream_endpoint_t::teardown_peer(int fd) {
    // Reset under write_m_ BEFORE ::close so a concurrent send() never writes
    // to (or reads) a closed/reused fd.
    {
        const std::lock_guard lock(write_m_);
        conn_fd_.store(-1, std::memory_order_relaxed);
    }
    ::close(fd);
}

void stream_endpoint_t::run_accept_loop(int listen_fd, const std::function<bool(int)>& on_accept,
                                        const std::function<void(int)>& serve_peer) {
    while (!stop_.load(std::memory_order_relaxed)) {
        // One poll-100ms-recheck accept pass: timeout / error / no connection
        // → re-check stop_ and try again.
        const int fd = poll_accept(listen_fd);
        if (fd < 0) continue;
        if (!on_accept(fd)) {  // per-peer setup / handshake failed → reject
            ::close(fd);
            continue;
        }
        conn_fd_.store(fd, std::memory_order_relaxed);

        serve_peer(fd);

        teardown_peer(fd);  // then re-accept the next peer
    }
}

}  // namespace tr::net
