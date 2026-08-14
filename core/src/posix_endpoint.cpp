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
#include <chrono>
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
    STALLED,        /**< @brief The socket's bounded send expired: the peer is not taking
                                bytes (#838). Healthy call, healthy socket, absent peer. */
    MALFORMED_CALL, /**< @brief The arguments were rejected — OUR defect, not a disconnect. */
};

/**
 * @brief The deadline that bounds ONE record's write — and, with it, the write-mutex hold
 *        the caller is inside (#838).
 *
 * A per-syscall `SO_SNDTIMEO` alone does not bound a RECORD: a stream write may need
 * several syscalls, and a peer that accepts one byte per timeout would restart the clock
 * forever. So the bound is a monotonic deadline taken once at the top of the record and
 * consulted before every attempt. `bound_ms == 0` reproduces the pre-#838 unbounded
 * behaviour exactly, for sockets that carry no send timeout at all.
 */
struct send_deadline_t {
    std::chrono::steady_clock::time_point at{}; /**< @brief When the record is abandoned. */
    bool bounded = false;                       /**< @brief False ⇒ no deadline at all. */

    explicit send_deadline_t(std::uint32_t bound_ms)
        : at(std::chrono::steady_clock::now() + std::chrono::milliseconds(bound_ms)),
          bounded(bound_ms != 0) {}

    /** @brief True once the record may no longer be worked on. */
    [[nodiscard]] bool expired() const { return bounded && std::chrono::steady_clock::now() >= at; }
};

/** @brief Process-wide malformed-call tally (#948) — see @ref write_fault_stats_t. */
std::atomic<std::uint64_t> g_malformed_calls{0};

/** @brief The errno of the most recent malformed-call fault (#948). */
std::atomic<int> g_last_malformed_errno{0};

/**
 * @brief The ONE write-fault policy both full-write helpers share (#903 / #948 / #838).
 *
 * Four outcomes, not two — conflating the last with the middle is #948, where one
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
 * - **STALLED.** `EAGAIN`/`EWOULDBLOCK`: the socket's `SO_SNDTIMEO` (#838, armed by
 *   @ref posix_endpoint_t::set_snd_timeout) expired with nothing written, because the
 *   peer's receive window is full. Nothing is wrong with the call OR the socket — the PEER
 *   is not taking bytes. It used to land in MALFORMED_CALL below, which would both mis-file
 *   a stalled peer as a libtracer defect and abandon the record after one re-attempt; it is
 *   its own arm precisely so the record can be retried until the caller's deadline and then
 *   attributed to the peer that earned it.
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
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return write_fault_t::STALLED;
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

/**
 * @brief The plain full-write loop, carrying the record's deadline and its
 *        already-on-the-wire fact (#838).
 *
 * @param fd       The destination socket.
 * @param bytes    The remaining bytes of the record.
 * @param dl       The record's deadline (see @ref send_deadline_t).
 * @param progress Whether EARLIER bytes of this record already reached the socket — what
 *                 makes an abandoned write a framing desync rather than a clean miss.
 */
write_result_t write_all_bounded(int fd, std::span<const std::byte> bytes,
                                 const send_deadline_t& dl, bool progress) {
    std::size_t off = 0;
    bool retry_spent = false;
    while (off < bytes.size()) {
        // The deadline is checked BEFORE the attempt, so a record whose budget is already
        // spent costs no further syscall — and the caller's write-mutex hold ends here.
        if (dl.expired()) return {write_outcome_t::STALLED, progress};
        const ssize_t n = send_bytes(fd, bytes.data() + off, bytes.size() - off);
        if (n <= 0) {
            const write_fault_t fault = classify_write_fault(n);
            if (fault == write_fault_t::RESUME) continue;
            if (fault == write_fault_t::STALLED) continue;  // re-attempt until the deadline
            if (fault == write_fault_t::PEER_GONE)
                return {write_outcome_t::FAILED, progress};  // dead socket → drop the rest
            if (!retry_malformed_call(retry_spent)) return {write_outcome_t::FAILED, progress};
            continue;  // our call, not the peer: the bytes are still deliverable
        }
        off += static_cast<std::size_t>(n);
        progress = true;
        retry_spent = false;
    }
    return {write_outcome_t::COMPLETE, false};  // whole record on the wire ⇒ nothing partial
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

void posix_endpoint_t::set_snd_timeout(int fd) {
    // The same 100 ms quantum as the receive side, and for the same reason: it is what
    // makes a blocked syscall return so the loop above it can re-check the world — here
    // the record deadline (#838), there stop_.
    timeval tv{.tv_sec = 0, .tv_usec = static_cast<suseconds_t>(kBoundedWaitMs) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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

write_result_t stream_endpoint_t::write_all(int fd, std::span<const std::byte> bytes,
                                            std::uint32_t bound_ms) {
    if (fd < 0) return {write_outcome_t::FAILED, false};
    return write_all_bounded(fd, bytes, send_deadline_t(bound_ms), /*progress=*/false);
}

write_result_t stream_endpoint_t::write_all_iov(int fd, std::span<const ::iovec> vec,
                                                std::uint32_t bound_ms) {
    if (fd < 0) return {write_outcome_t::FAILED, false};
    const send_deadline_t dl(bound_ms);
    bool progress = false;  // has ANY byte of this record reached the socket
    bool retry_spent = false;
    std::size_t i = 0;  // the first entry not yet fully written
    while (i < vec.size()) {
        // Bounded per record, not per syscall (#838): the deadline covers this loop, the
        // nested write_all below, and therefore the caller's whole write_m_ hold.
        if (dl.expired()) return {write_outcome_t::STALLED, progress};
        msghdr msg{};
        // sendmsg does not modify the iovec array; the cast is the C API's
        // missing const, not a licence to consume the caller's gather (#932).
        msg.msg_iov = const_cast<::iovec*>(vec.data() + i);
        msg.msg_iovlen = vec.size() - i;
        const ssize_t n = send_gather(fd, &msg);
        if (n <= 0) {
            const write_fault_t fault = classify_write_fault(n);
            if (fault == write_fault_t::RESUME) continue;
            if (fault == write_fault_t::STALLED) continue;  // re-attempt until the deadline
            if (fault == write_fault_t::PEER_GONE)
                return {write_outcome_t::FAILED, progress};  // dead socket → drop the rest
            if (!retry_malformed_call(retry_spent)) return {write_outcome_t::FAILED, progress};
            continue;  // our call, not the peer: the bytes are still deliverable
        }
        retry_spent = false;
        progress = true;
        std::size_t done = static_cast<std::size_t>(n);
        // Advance past every fully-written entry — the stream may stop at any
        // byte boundary.
        while (i < vec.size() && done >= vec[i].iov_len) {
            done -= vec[i].iov_len;
            ++i;
        }
        if (i < vec.size() && done > 0) {
            // The write stopped INSIDE entry i. Finish that entry with the plain
            // writer instead of trimming the caller's array in place, then
            // re-gather from the next entry boundary: the gather stays read-only,
            // so a fan-out needs no per-peer copy. This is the rare slow path — a
            // complete gathered write never lands here.
            const write_result_t tail = write_all_bounded(
                fd,
                std::span<const std::byte>(static_cast<const std::byte*>(vec[i].iov_base) + done,
                                           vec[i].iov_len - done),
                dl, /*progress=*/true);
            if (tail.outcome != write_outcome_t::COMPLETE) return tail;
            ++i;
        }
    }
    return {write_outcome_t::COMPLETE, false};
}

bool stream_endpoint_t::note_write_result(const write_result_t& r, int fd, std::uint8_t& streak) {
    if (r.outcome != write_outcome_t::STALLED) {
        // Any record that COMPLETES clears the streak — the trichotomy's middle arm: a peer
        // that misses one frame and takes the next was riding out a burst, not broken.
        // A FAILED write is #66 lifecycle (the socket is already dead), so it neither
        // counts a stall nor accrues one.
        if (r.outcome == write_outcome_t::COMPLETE) streak = 0;
        return false;
    }
    stalled_tx_.fetch_add(1, std::memory_order_relaxed);
    if (streak < kMaxConsecutiveStalls) ++streak;
    // A half-written record has desynced this stream's framing permanently, so it condemns
    // the session at once, bypassing the streak (#837's short-write rule). Otherwise the
    // streak decides.
    if (r.partial || streak >= kMaxConsecutiveStalls) {
        // shutdown, never close: the fd stays owned by whoever opened it, every later write
        // on it fails immediately instead of waiting out another bound, and the readable-
        // at-EOF this raises is what the recv/poll thread turns into the ORDINARY remote-
        // departure teardown — one teardown path, with its departure notification, rather
        // than a second one invented here (the slot_server_t::close_peer discipline).
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        streak = 0;  // the verdict is spent; the session is on its way out
    }
    return true;  // the frame was shed — the caller counts it in its own dropped_tx_
}

void stream_endpoint_t::send_all_locked(std::span<const std::byte> bytes) {
    // Hold write_m_ across the whole write so the recv thread cannot close and
    // reset conn_fd_ underneath us; read the fd inside the lock to pair with
    // teardown_peer.
    const std::lock_guard lock(write_m_);
    const int fd = conn_fd_.load(std::memory_order_relaxed);
    // One peer in this "round", so the whole liveness window is this record's bound (#838).
    const write_result_t r = write_all(fd, bytes, derive_send_bound_ms(liveness_window_ms_, 1));
    (void)note_write_result(r, fd, tx_stall_streak_);
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

/**
 * @brief Resolve `p<slot>` to that slot's directed endpoint — SLOT-scoped, not
 *        session-scoped (#1153).
 *
 * The returned facade is one object per slot for the link's life, and the name is a
 * pure function of the slot index (see accept_peer), so BOTH halves of a resolution
 * survive the session that produced it: a caller holding this pointer across the named
 * peer's departure sends into whichever session next claims the slot, and
 * `peer_endpoint_t::send`'s `open` check cannot catch it — that check is true for
 * whoever occupies the slot at send time, which is precisely the stranger. Hence the
 * resolve-per-use contract on @ref bus_link_t::peer_link.
 *
 * No production caller is exposed (all re-resolve per send), so this remains an API
 * hazard rather than a live defect. The ESP plane's equivalent hazard was closed
 * independently under #1013 via a per-resolution handle that stamps the slot and the
 * generation it resolved against, so a send compares that generation instead of
 * re-reading the slot's — "both server planes at once" was not how it played out.
 * Whether the POSIX plane adopts the same structural fix is an open maintainer call;
 * #1013's resolution is the precedent to point at, not a plan already decided.
 */
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

std::size_t slot_server_t::broadcast_iov(std::span<const ::iovec> rec) {
    // No per-peer copy and no scratch store: write_all_iov reads the gather
    // without consuming it (#932), so every peer writes from the same record.
    const std::lock_guard plock(peers_m_);
    const std::lock_guard wlock(write_m_);
    // Count the round FIRST: the per-peer bound is the liveness window divided by the peers
    // this round writes to, so the WHOLE fan-out — every peer stalled — still releases both
    // locks inside one window (#838). The divisor is the round's own size, a fact in hand,
    // not a guessed cap.
    std::size_t open_peers = 0;
    for (const std::unique_ptr<session_base_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed)) ++open_peers;
    const std::uint32_t bound = derive_send_bound_ms(liveness_window_ms_, open_peers);

    std::size_t shed = 0;
    for (const std::unique_ptr<session_base_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed)) continue;
        const int fd = s->fd.load(std::memory_order_relaxed);
        const write_result_t r = write_all_iov(fd, rec, bound);
        // A stalled peer strikes only ITSELF: the peers behind it in this round still get
        // the record, and only its own session is condemned once it is provably broken.
        if (note_write_result(r, fd, s->tx_stall_streak)) ++shed;
    }
    return shed;
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
    // The bounded send, armed here rather than in either derived on_accept: an accepted
    // peer's socket is a shared-machinery fact, and #838 is a defect of the SHARED write
    // path — both stream servers write through broadcast_iov and their directed facades, so
    // both peer planes get the bound from one site (and a new stream server inherits it).
    set_snd_timeout(fd);
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
    // Arrival seam (#1223), fired LAST and with no transport lock held — the mirror image of
    // teardown_slot's departure seam, in every respect including the `open` condition: only a
    // session that reached `open` can flow frames, so only one gets an identity. A WS slot is
    // NOT open here (it opens at its 101), and announces itself from that site instead.
    if (opens_now) publish_peer_up(*slot);
}

void slot_server_t::publish_peer_up(const session_base_t& s) {
    if (!peer_named_) return;
    std::string name;
    {
        // The name is peers_m_-guarded state (enumerate_peers / peer_link read it off this
        // thread), so COPY it out under the lock and notify outside: the notifier re-enters
        // the routing plane, and holding a transport lock across that inverts the documented
        // order (`transport_vertex_t::ctl_m_` → router → `graph_t::map_mutex_`).
        const std::lock_guard lock(peers_m_);
        name = s.name;
    }
    if (!name.empty()) notify_peer_up(name);
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
        // Under the same lock the senders mutate it: a stalled peer's strikes die with its
        // session and are never inherited by whoever next claims this slot (#838).
        s.tx_stall_streak = 0;
    }
    if (fd >= 0) ::close(fd);
    on_slot_reset(s);
    // Departure seam (RFC-0009 §D.5): fired LAST, with no transport lock held — the
    // notifier re-enters the routing plane. Only a session that reached `open` can have
    // flowed frames (subscriptions), and only then.
    //
    // WHICH seam is the constructed mode's answer, not "is a peer sink installed" (#889):
    // peer-named mode evicts exactly the departed peer's edges under its own name, while a
    // FLAT link has ONE routing identity for every peer it carries — the registered child
    // NAME — so its only seam is the whole link. Firing that on a mid-life close evicts the
    // SURVIVING peers' edges too, which is why it waits for the last open session to go.
    // The check runs on the poll thread with both transport locks released: `open` is
    // mutated only here and in accept_peer, both on this thread, so no session can appear
    // or vanish between the answer and the notification.
    if (was_open && !departed.empty()) {
        if (peer_named_)
            notify_peer_down(departed);
        else if (!any_open_session())
            notify_down();
    }
}

bool slot_server_t::any_open_session() const {
    // peers_m_ guards the slot VECTOR against the cross-thread readers (enumerate_peers /
    // peer_link); the `open` loads are relaxed for the same reason they are everywhere else
    // — the lock, not the memory order, is what orders them.
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_base_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed)) return true;
    return false;
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
