/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/posix_endpoint.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>

#include "libtracer/iov_table.hpp"

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

// ---------------------------------------------------------------------------
// slot_server_t — the multi-peer slot/poll machinery both stream servers share
// (#871). One home for the shape transport_tcp.cpp and transport_ws.cpp used to
// restate line-for-line; only framing and handshake stay in the derived TUs.
// ---------------------------------------------------------------------------

slot_server_t::~slot_server_t() {
    // The derived destructor's first act was stop_and_join, so the poll thread is gone
    // and nothing races either close below.
    if (listen_fd_ >= 0) ::close(listen_fd_);
    for (const std::unique_ptr<session_base_t>& s : slots_) {
        const int fd = s->fd.exchange(-1, std::memory_order_relaxed);
        if (fd >= 0) ::close(fd);
    }
}

bool slot_server_t::bind_listen(std::uint16_t bind_port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bind_port);
    // SOMAXCONN: the OS's own accept-queue bound — admission is per-connection in
    // accept_peer (the max_peers deployment cap), never a synthetic backlog.
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
        ::listen(listen_fd_, SOMAXCONN) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        bound_port_ = ntohs(bound.sin_port);
    return true;
}

void slot_server_t::enumerate_peers(const peer_visitor_t& visit) const {
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_base_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && !s->name.empty()) visit(s->name);
}

transport_t* slot_server_t::peer_link(std::string_view peer) {
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_base_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && s->name == peer) return s->peer_endpoint;
    return nullptr;
}

bool slot_server_t::close_peer(std::string_view peer) {
    // Shutdown-only under the sender lock order (peers_m_ → write_m_); the poll thread's
    // next pass observes the close and runs the IDENTICAL remote-FIN teardown — no
    // duplicate logic, and no off-thread touch of the poll-thread-only buffers
    // teardown_slot's on_slot_reset hook clears.
    const std::lock_guard plock(peers_m_);
    for (const std::unique_ptr<session_base_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed) || s->name != peer) continue;
        const std::lock_guard wlock(write_m_);
        const int fd = s->fd.load(std::memory_order_relaxed);
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        return true;
    }
    return false;
}

void slot_server_t::broadcast_iov(const ::iovec* pristine, std::size_t count) {
    std::array<::iovec, kMaxInlineIov + 1> scratch_inline;
    iov_table_t<::iovec> scratch_table(scratch_inline);
    ::iovec* scratch = scratch_table.acquire(count);
    if (scratch == nullptr) return;  // same store, same answer: drop, never truncate
    const std::lock_guard plock(peers_m_);
    const std::lock_guard wlock(write_m_);
    for (const std::unique_ptr<session_base_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed)) continue;
        std::copy_n(pristine, count, scratch);
        write_all_iov(s->fd.load(std::memory_order_relaxed), scratch, count);
    }
}

void slot_server_t::accept_peer() {
    sockaddr_in remote{};
    socklen_t rlen = sizeof(remote);
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&remote), &rlen);
    if (fd < 0) return;

    session_base_t* slot = nullptr;
    {
        const std::lock_guard lock(peers_m_);
        std::size_t idx = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i]->fd.load(std::memory_order_relaxed) < 0) {
                slot = slots_[i].get();
                idx = i;
                break;
            }
        if (slot == nullptr) {
            if (max_peers_ != 0 && slots_.size() >= max_peers_) {
                ::close(fd);  // clean refusal at the deployment cap, not a hung SYN
                return;
            }
            slots_.push_back(make_session());
            slot = slots_.back().get();
            idx = slots_.size() - 1;
        }
        // The routable NAME is the slot index — `p<slot>`, legal by construction and a
        // pure function of the slot's position (ADR-0073 §2), so a reused slot gets the
        // SAME name back (teardown_slot moved the old string out for the eviction seam).
        slot->name = 'p' + std::to_string(idx);
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        slot->endpoint_str = std::string(ip) + ':' + std::to_string(ntohs(remote.sin_port));
    }
    // Socket options and the slot's protocol buffers, then its handshake stance: a raw
    // stream peer is open the moment it is accepted, a WS peer only past its 101.
    const bool opens_now = on_accept(*slot, fd);
    // Publish the two sender-visible fields under write_m_ — the SAME lock teardown_slot
    // resets them under — so the pair moves as one step for anyone who could act on it: a
    // broadcast holding that lock runs either entirely before this slot goes live or
    // entirely after. Inside the hold the fd goes FIRST, which is what makes "open ⇒ fd
    // valid" an invariant; the reverse (open first, unlocked) let a broadcast read
    // `open == true` next to `fd == -1` and write the frame to a closed descriptor — a
    // dropped frame today and a trap for any future per-fd state (#891). Accept is cold —
    // once per connection, off the delivery path — so the hold costs nothing a sender can
    // measure. The stores are relaxed: the lock, not the memory order, is what orders them
    // — every reader of the fd that is not the poll thread itself (both send overloads,
    // close_peer) holds this same lock.
    {
        const std::lock_guard lock(write_m_);
        slot->fd.store(fd, std::memory_order_relaxed);
        // The seam that makes the line below testable: here the fd is published and the
        // slot is one store from open, with write_m_ still held. A broadcast that lands in
        // this instant must block, and must then find the slot whole. Publish these two
        // stores unlocked in the other order and the same probe writes to fd -1 — that is
        // the regression the derived hook exists to redden (`tcp_test`, the accept-publish
        // race).
        on_slot_publishing();
        slot->open.store(opens_now, std::memory_order_relaxed);
    }
}

void slot_server_t::service_peer(session_base_t& s) {
    const int fd = s.fd.load(std::memory_order_relaxed);
    if (fd < 0) return;
    std::array<std::byte, 4096> chunk;
    const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (n <= 0) {  // peer closed the TCP connection, or error
        teardown_slot(s);
        return;
    }
    // Framing is the derived server's: a length-prefix framer feed, or the WS
    // handshake-then-decode path. Both may tear this slot down from inside the hook.
    on_readable(s, chunk.data(), static_cast<std::size_t>(n));
}

void slot_server_t::teardown_slot(session_base_t& s) {
    std::string departed;
    {
        // Stop peer_link/enumerate resolution FIRST, so no new sender targets the dying
        // slot by name. Keep the name: it identifies the departed session to the eviction
        // seam below.
        const std::lock_guard lock(peers_m_);
        departed = std::move(s.name);
        s.name.clear();
    }
    int fd;
    bool was_open;
    {
        // The stream teardown-under-write-lock invariant, per slot: reset the fd and the
        // open flag under write_m_ BEFORE ::close, so an in-flight send either finished
        // against the still-open fd or observes the reset. The mirror image of the
        // accept-side publish, same lock, same relaxed stores — `open` clears first here,
        // so the pair never reads live-with-no-fd from this side either.
        const std::lock_guard lock(write_m_);
        was_open = s.open.load(std::memory_order_relaxed);
        s.open.store(false, std::memory_order_relaxed);
        fd = s.fd.exchange(-1, std::memory_order_relaxed);
    }
    if (fd >= 0) ::close(fd);
    on_slot_reset(s);
    // Departure seam (RFC-0009 §D.5): fired LAST, with no transport lock held — the
    // notifier re-enters the routing plane. Only a session that reached `open` can have
    // flowed frames (subscriptions), and only then. Peer-named mode reports the peer's own
    // name; flat mode reports the whole link down (#889 owns that coarseness).
    if (was_open && !departed.empty()) {
        if (peer_rx_.has_any())
            notify_peer_down(departed);
        else
            notify_down();
    }
}

void slot_server_t::run() {
    // ONE poll pass multiplexes the listen socket and every live peer — no per-peer thread
    // (the MCU-shaped choice, #362), bounded to 100 ms so the loop stays
    // shutdown-responsive (the posix_endpoint_t idiom).
    std::vector<pollfd> pfds;
    std::vector<session_base_t*> pslots;
    while (!stop_.load(std::memory_order_relaxed)) {
        pfds.clear();
        pslots.clear();
        pfds.push_back(pollfd{listen_fd_, POLLIN, 0});
        {
            const std::lock_guard lock(peers_m_);
            for (const std::unique_ptr<session_base_t>& s : slots_) {
                const int fd = s->fd.load(std::memory_order_relaxed);
                if (fd >= 0) {
                    pfds.push_back(pollfd{fd, POLLIN, 0});
                    pslots.push_back(s.get());
                }
            }
        }
        const int pr = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100);
        if (pr <= 0) continue;  // timeout or transient error → re-check stop_
        if (stop_.load(std::memory_order_relaxed)) break;
        // Peers first (their events are bound to this pass's fd list), then the accept
        // (which may add a slot).
        for (std::size_t i = 1; i < pfds.size(); ++i)
            if ((pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) service_peer(*pslots[i - 1]);
        if ((pfds[0].revents & POLLIN) != 0) accept_peer();
    }
}

}  // namespace tr::net
