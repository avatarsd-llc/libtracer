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
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
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
    int flags = 0; /**< @brief `MSG_DONTWAIT` for a nonblocking write, which ends `STALLED`
                               the first time the socket is full (#2144), else 0. */

    /** @brief Reads the clock only for a bounded write: an unbounded (@p bound_ms 0) or
     *         nonblocking one costs none (#2144). */
    send_deadline_t(std::uint32_t bound_ms, bool nonblock)
        : bounded(bound_ms != 0 && !nonblock), flags(static_cast<int>(nonblock) * MSG_DONTWAIT) {
        if (bounded) at = std::chrono::steady_clock::now() + std::chrono::milliseconds(bound_ms);
    }

    /** @brief True once the record may no longer be worked on. */
    [[nodiscard]] bool expired() const { return bounded && std::chrono::steady_clock::now() >= at; }
};

/** @brief Process-wide malformed-call tally (#948) — see @ref write_fault_stats_t. Word-wide,
 *         as every drop counter is (#1697): a 64-bit atomic is a libatomic call on rv32. */
std::atomic<std::size_t> g_malformed_calls{0};

/** @brief The errno of the most recent malformed-call fault (#948). */
std::atomic<int> g_last_malformed_errno{0};

/**
 * @brief The ONE write-fault policy of the one full-write loop (#903 / #948 / #838).
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
    if constexpr (tr::graph::kFaultInjection) {
        return tr::detail::write_fault_inject_hook == nullptr
                   ? 0
                   : tr::detail::write_fault_inject_hook();
    } else {
        return 0;  // the seam is compiled out (#1719): the real syscall always runs
    }
}

/** @brief One `sendmsg(2)` attempt through the #948 injection seam. */
ssize_t send_gather(int fd, const msghdr* msg, int flags) {
    if (const int fake = injected_write_errno(); fake != 0) {
        errno = fake;
        return -1;
    }
    // One entry with no address and no ancillary data goes out as send(2): the kernel's
    // sendmsg path first copies the msghdr and imports the iovec from user memory, a cost a
    // small record pays in full.
    if (msg->msg_iovlen == 1 && msg->msg_name == nullptr && msg->msg_controllen == 0)
        return ::send(fd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, MSG_NOSIGNAL | flags);
    return ::sendmsg(fd, msg, MSG_NOSIGNAL | flags);
}

}  // namespace

bool parse_ipv4(std::string_view host, ::in_addr& out) noexcept {
    char text[INET_ADDRSTRLEN];
    if (host.size() >= sizeof(text)) return false;
    std::copy_n(host.data(), host.size(), text);  // an empty view may carry a null data()
    text[host.size()] = '\0';
    return ::inet_pton(AF_INET, text, &out) == 1;
}

write_fault_stats_t write_fault_stats() noexcept {
    return {.malformed_calls = g_malformed_calls.load(std::memory_order_relaxed),
            .last_errno = g_last_malformed_errno.load(std::memory_order_relaxed)};
}

posix_endpoint_t::~posix_endpoint_t() { stop_and_join(); }

void* posix_endpoint_t::thread_entry(void* self) {
    static_cast<posix_endpoint_t*>(self)->body_();
    return nullptr;
}

void posix_endpoint_t::start(thread_body_t body, std::size_t stack_size) {
    body_ = body;

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
    egress_.close(peer_);  // every waiting frame and lane back to the link's store
}

write_result_t stream_endpoint_t::write_all(int fd, std::span<const std::byte> bytes,
                                            std::uint32_t bound_ms) {
    const ::iovec v{const_cast<std::byte*>(bytes.data()), bytes.size()};
    // An empty record is a complete one, with no syscall.
    return write_all_iov(fd, std::span<const ::iovec>(&v, bytes.empty() ? 0 : 1), bound_ms, false);
}

write_result_t stream_endpoint_t::write_all_iov(int fd, std::span<const ::iovec> vec,
                                                std::uint32_t bound_ms, bool nonblock) {
    if (fd < 0) return {write_outcome_t::FAILED, 0};
    const send_deadline_t dl(bound_ms, nonblock);
    std::size_t moved = 0;  // bytes of this record on the socket
    bool retry_spent = false;
    std::size_t i = 0;     // the first entry not yet fully written
    std::size_t skip = 0;  // bytes of entry i already written
    while (i < vec.size()) {
        // Bounded per record, not per syscall (#838): the deadline covers this loop and
        // therefore the caller's whole writer turn.
        if (dl.expired()) return {write_outcome_t::STALLED, moved};
        msghdr msg{};
        // sendmsg does not modify the iovec array; the cast is the C API's missing const,
        // not a licence to consume the caller's gather (#932). A write that stopped INSIDE
        // entry i finishes that entry alone from a local view of its rest, then re-gathers
        // from the next boundary: the gather stays read-only, so a fan-out needs no per-peer
        // copy.
        ::iovec rest{static_cast<std::byte*>(vec[i].iov_base) + skip, vec[i].iov_len - skip};
        msg.msg_iov = const_cast<::iovec*>(vec.data() + i);
        msg.msg_iovlen = vec.size() - i;
        if (skip != 0) {
            msg.msg_iov = &rest;
            msg.msg_iovlen = 1;
        }
        const ssize_t n = send_gather(fd, &msg, dl.flags);
        if (n <= 0) {
            const write_fault_t fault = classify_write_fault(n);
            if (fault == write_fault_t::RESUME) continue;
            if (fault == write_fault_t::STALLED) {
                if (dl.flags != 0) return {write_outcome_t::STALLED, moved};  // full: never wait
                continue;  // re-attempt until the deadline
            }
            if (fault == write_fault_t::PEER_GONE)
                return {write_outcome_t::FAILED, moved};  // dead socket → drop the rest
            if (!retry_malformed_call(retry_spent)) return {write_outcome_t::FAILED, moved};
            continue;  // our call, not the peer: the bytes are still deliverable
        }
        retry_spent = false;
        moved += static_cast<std::size_t>(n);
        // Advance past every fully-written entry — the stream may stop at any byte boundary.
        skip += static_cast<std::size_t>(n);
        while (i < vec.size() && skip >= vec[i].iov_len) {
            skip -= vec[i].iov_len;
            ++i;
        }
    }
    return {write_outcome_t::COMPLETE, moved};
}

namespace {
/** @brief A write result as one egress turn: a stalled write holds the frame, a failed one
 *         abandons it. */
egress_step_t step_of(const write_result_t& r) noexcept {
    return {r.moved, r.outcome == write_outcome_t::STALLED, r.outcome == write_outcome_t::FAILED};
}

/**
 * @brief Write the session name of @p peer, `p<slot>~<gen>`, into @p out; empty when it
 *        does not fit.
 *
 * The ONE spelling both the accept-side stamp and @ref slot_server_t::peer_name use, so the
 * name a frame is tagged with and the name the slot holds cannot disagree. The tenancy
 * generation is part of it: a reused slot gets a fresh name, so a route another node still
 * holds for the previous session names nobody, and a delivery along it is refused.
 */
std::string_view format_session_name(peer_handle_t peer, std::span<char> out) noexcept {
    if (out.size() < 2) return {};
    char* const last = out.data() + out.size();
    out[0] = 'p';
    auto r = std::to_chars(out.data() + 1, last, peer.index);
    if (r.ec != std::errc{} || r.ptr == last) return {};
    *r.ptr = kTenancySeparator;
    r = std::to_chars(r.ptr + 1, last, peer.generation);
    if (r.ec != std::errc{}) return {};
    return std::string_view(out.data(), static_cast<std::size_t>(r.ptr - out.data()));
}
}  // namespace

egress_step_t stream_endpoint_t::write_gather(int fd, std::span<const ::iovec> vec,
                                              std::uint32_t bound_ms, bool nonblock) {
    return step_of(write_all_iov(fd, vec, bound_ms, nonblock));
}

egress_step_t stream_endpoint_t::write_frame(int fd, const egress_frame_t& f, std::size_t off,
                                             std::uint32_t bound_ms, bool nonblock) {
    // One gathered write from byte off: the head the frame copied, then a kept value's
    // links straight from the published block (RFC-0028 §6.9).
    std::array<std::span<const std::byte>, kMaxInlineIov> inline_spans;
    std::array<::iovec, kMaxInlineIov> inline_vec;
    iov_table_t<std::span<const std::byte>> spans(inline_spans, *io_src_);
    iov_table_t<::iovec> table(inline_vec, *io_src_);
    std::span<const std::byte>* const sp = spans.acquire(f.span_count());
    ::iovec* const vec = table.acquire(f.span_count());
    // Refused before a byte moved: a shed frame, never a desync (off == 0), or a condemned
    // peer when the frame was already part-written.
    if (sp == nullptr || vec == nullptr) return {0, false, true};
    const std::size_t n = f.spans_from(off, sp);
    for (std::size_t k = 0; k < n; ++k)
        vec[k] = ::iovec{const_cast<std::byte*>(sp[k].data()), sp[k].size()};
    return write_gather(fd, std::span<const ::iovec>(vec, n), bound_ms, nonblock);
}

std::size_t stream_endpoint_t::retained_parts(std::span<std::span<const std::byte>> parts,
                                              std::span<const std::span<const std::byte>> head,
                                              const graph::value_t& value) noexcept {
    if (head.size() + value.link_count() > parts.size()) return parts.size() + 1;
    std::size_t i = 0;
    for (const std::span<const std::byte>& h : head) parts[i++] = h;
    for (const view::view_t& l : value.links()) parts[i++] = l.bytes();
    return i;
}

egress_frame_t* stream_endpoint_t::frame_of(std::span<const ::iovec> rec,
                                            const graph::value_t* value) const {
    // The value's links end the gather, one entry per non-empty link: the frame copies what
    // comes before them and keeps the value itself.
    std::size_t tail = 0;
    if (value != nullptr)
        for (const view::view_t& l : value->links()) tail += l.bytes().empty() ? 0 : 1;
    const std::span<const ::iovec> vec = rec.first(rec.size() - tail);
    std::size_t head = 0;
    for (const ::iovec& v : vec) head += v.iov_len;
    return egress_frame_t::make(
        *io_src_, head,
        [&](std::span<std::byte> out) {
            std::size_t at = 0;
            for (const ::iovec& v : vec) {
                if (v.iov_len != 0) std::memcpy(out.data() + at, v.iov_base, v.iov_len);
                at += v.iov_len;
            }
            return at;
        },
        value);
}

std::size_t stream_endpoint_t::close_frozen_one(std::uint64_t now, std::uint64_t deadline) {
    const bool moved = tx_moved_.exchange(false, std::memory_order_relaxed);
    if (!judge_frozen(!moved && peer_.wants_write(), live_since_, now, deadline)) return 0;
    // Inside the writer role, so the fd it shuts down is still the peer's; a role somebody
    // holds is a write in progress, and the next check asks again.
    if (!egress_.condemn_held(peer_,
                              [this] { condemn_fd(conn_fd_.load(std::memory_order_acquire)); }))
        return 0;
    live_since_ = now;
    frozen_closed_.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

void stream_endpoint_t::note_egress(const egress_result_t& r) noexcept {
    if (r.shed != 0) dropped_tx_.fetch_add(r.shed, std::memory_order_relaxed);
    if (r.stalled != 0) stalled_tx_.fetch_add(r.stalled, std::memory_order_relaxed);
}

void stream_endpoint_t::condemn_fd(int fd) noexcept {
    // One teardown path, with its departure notification (the close_peer discipline): the
    // fd stays owned by whoever opened it.
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

int stream_endpoint_t::poll_io(int fd) {
    pollfd pfd{
        .fd = fd,
        .events = static_cast<short>(POLLIN | (POLLOUT * static_cast<int>(peer_.wants_write()))),
        .revents = 0};
    const int pr = ::poll(&pfd, 1, 100);
    if (pr <= 0) return pr;
    // The recv thread owns the fd, so the turn names it directly; it never waits.
    if ((pfd.revents & POLLOUT) != 0)
        note_egress(egress_.flush(
            peer_,
            [&](const egress_frame_t& f, std::size_t off) {
                return note_moved(tx_moved_, write_frame(fd, f, off, 0, true));
            },
            [fd] { condemn_fd(fd); }));
    return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0 ? 1 : 0;
}

void stream_endpoint_t::teardown_peer(int fd) {
    // shutdown first so a write in flight returns; the egress close then waits out the
    // writer role and releases every frame, so no sender reads conn_fd_ after this: only
    // then is the fd reset and closed (never a write to a closed or reused fd).
    ::shutdown(fd, SHUT_RDWR);
    egress_.close(peer_);
    conn_fd_.store(-1, std::memory_order_relaxed);
    ::close(fd);
}

void stream_endpoint_t::shut_down_stream() {
    stop_and_join();
    // Only the joined thread ever closed this fd, so nothing else can close it under us.
    const int fd = conn_fd_.load(std::memory_order_relaxed);
    if (fd >= 0) teardown_peer(fd);
}

void stream_endpoint_t::run_accept_loop(int listen_fd, function_ref_t<bool(int)> on_accept,
                                        function_ref_t<void(int)> serve_peer) {
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
        tx_moved_.store(true, std::memory_order_relaxed);  // a new peer starts alive (#2198)
        egress_.open(peer_);  // release: a sender that claims the role then sees the fd

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
    for (const mem::poly_ptr_t<session_base_t>& s : slots_) {
        const int fd = s->fd.exchange(-1, std::memory_order_relaxed);
        if (fd >= 0) ::close(fd);
        egress_.close(s->egress);  // its waiting frames and lanes back to the link's store
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
    for (const mem::poly_ptr_t<session_base_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && s->name[0] != '\0') visit(s->name_view());
}

std::string_view slot_server_t::peer_name(peer_handle_t peer, std::span<char> scratch) const {
    // The name a slot is stamped with at accept IS `p<slot>~<gen>` of its handle, so the
    // inverse is that same formatting and needs neither peers_m_ nor the slot vector. That
    // is what makes this callable per frame from the delivery callback at the cost the free
    // string_view tag used to have.
    if (!peer.valid()) return {};
    return format_session_name(peer, scratch);
}

std::string_view slot_server_t::peer_subject(peer_handle_t peer, std::span<char> scratch) const {
    // The SUBJECT and the peer NAME coincide for this kind, and deliberately so: the session
    // token `p<slot>~<gen>` is the only per-writer discriminator a stream server has until an
    // authentication layer mints a real one (ADR-0045). What does NOT coincide is their
    // AVAILABILITY to the ROUTING PLANE — `peer_name` is an ADDRESSING answer the plane
    // reaches through the `bus_link_t` facet, which a FLAT server does not expose (and which
    // a `kBusLinks = false` build does not even carry, #1438), while this is on `transport_t`
    // and answers at either setting of `peer_named`. That difference IS ADR-0082's decouple,
    // expressed in one override. The formatting is shared rather than duplicated because the
    // two coincide in VALUE.
    return peer_name(peer, scratch);
}

/**
 * @brief Resolve `p<slot>~<gen>` to that slot's directed endpoint — the facade is
 *        SLOT-scoped, not session-scoped (#1153).
 *
 * The returned facade is one object per slot for the link's life. The NAME is
 * session-scoped (it carries the tenancy generation, see accept_peer), so a fresh resolve
 * never reaches the next session; but a caller holding the POINTER across the named
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
    // The exact compare IS the tenancy check: a name carrying a departed session's
    // generation, or none at all, equals no open slot's name and is refused.
    const std::lock_guard lock(peers_m_);
    for (const mem::poly_ptr_t<session_base_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && s->name_view() == peer)
            return s->peer_endpoint;
    return nullptr;
}

transport_t* slot_server_t::peer_link_of(peer_handle_t peer) {
    // `p<slot>~<gen>` names slot `slot` in tenancy `gen` (see `peer_name`), so the slot IS
    // the lookup and the handle compare IS `peer_link`'s tenancy check: a departed session's
    // handle equals no open slot's, without the name compare across every slot.
    if (!peer.valid()) return nullptr;
    const std::lock_guard lock(peers_m_);
    if (peer.index >= slots_.size()) return nullptr;
    const session_base_t& s = *slots_[peer.index];
    return s.open.load(std::memory_order_relaxed) && s.handle == peer ? s.peer_endpoint : nullptr;
}

bool slot_server_t::close_peer(std::string_view peer) {
    // Shutdown-only under peers_m_; the poll thread's next pass observes the close and runs
    // the IDENTICAL remote-FIN teardown — no duplicate logic, and no off-thread touch of the
    // poll-thread-only buffers teardown_slot's on_slot_reset hook clears. The fd cannot be
    // closed under us: teardown un-names the slot under this same lock before it closes it.
    const std::lock_guard plock(peers_m_);
    for (const mem::poly_ptr_t<session_base_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed) || s->name_view() != peer) continue;
        condemn_fd(s->fd.load(std::memory_order_relaxed));
        return true;
    }
    return false;
}

std::size_t slot_server_t::close_frozen_peers(std::uint64_t now, std::uint64_t deadline) {
    // Under peers_m_, the close_peer discipline: teardown un-names a slot under this lock
    // before it closes the fd, so a named slot's fd is still its own here.
    std::size_t closed = 0;
    const std::lock_guard lock(peers_m_);
    for (const mem::poly_ptr_t<session_base_t>& s : slots_) {
        // Acquire pairs with accept_peer's release of the fd: a slot whose fd is seen
        // published also shows its fresh progress flags, never a reused slot's stale ones.
        const int fd = s->fd.load(std::memory_order_acquire);
        if (fd < 0 || s->name[0] == '\0') continue;
        const bool tx = s->tx_moved.exchange(false, std::memory_order_relaxed);
        const bool rx = s->rx_moved.exchange(false, std::memory_order_relaxed);
        const bool stuck =
            (!tx && s->egress.wants_write()) || (!rx && s->rx_owed.load(std::memory_order_relaxed));
        if (!judge_frozen(stuck, s->live_since, now, deadline)) continue;
        condemn_fd(fd);  // the poll thread's next pass runs the remote-FIN teardown
        s->live_since = now;
        ++closed;
    }
    frozen_closed_.fetch_add(closed, std::memory_order_relaxed);
    return closed;
}

void slot_server_t::send_one(session_base_t& s, function_ref_t<egress_frame_t*()> frame,
                             std::span<const ::iovec> rec, const graph::delivery_lane_t& lane) {
    // The fd is read INSIDE the writer role: accept opens the egress only after publishing
    // it, and teardown closes the egress (waiting the role out) before resetting it.
    const auto fd = [&s] { return s.fd.load(std::memory_order_relaxed); };
    const egress_result_t r = egress_.send(
        s.egress, lane, frame,
        [&] { return note_moved(s.tx_moved, write_gather(fd(), rec, 0, true)); },
        [&](const egress_frame_t& f, std::size_t off) {
            return note_moved(s.tx_moved, write_frame(fd(), f, off, 0, true));
        },
        [&] { condemn_fd(fd()); });
    note_egress(r);
}

void slot_server_t::broadcast(std::span<const ::iovec> rec, const graph::value_t* value,
                              const graph::delivery_lane_t& lane) {
    // ONE shared frame for the whole fan-out, built only if some peer has to wait for it
    // (ADR-0087 §2). peers_m_ keeps the slot table still; every write under it is
    // nonblocking, so the hold is a few syscalls and never a stalled peer's wait.
    egress_once_t once([&] { return frame_of(rec, value); });
    const std::lock_guard plock(peers_m_);
    for (const mem::poly_ptr_t<session_base_t>& s : slots_)
        if (s->open.load(std::memory_order_acquire)) send_one(*s, once, rec, lane);
}

void slot_server_t::send_to(session_base_t& s, std::span<const ::iovec> rec,
                            const graph::value_t* value, const graph::delivery_lane_t& lane) {
    egress_once_t once([&] { return frame_of(rec, value); });
    send_one(s, once, rec, lane);  // a departed session's egress is closed: a counted shed
}

void slot_server_t::flush_peer(session_base_t& s) {
    const auto fd = [&s] { return s.fd.load(std::memory_order_relaxed); };
    const auto write = [&](const egress_frame_t& f, std::size_t off) {
        return note_moved(s.tx_moved, write_frame(fd(), f, off, 0, true));
    };
    const auto condemn = [&] { condemn_fd(fd()); };
    note_egress(egress_.flush(s.egress, write, condemn));
}

void slot_server_t::accept_peer(mem::block_array_t<pollfd>& pfds,
                                mem::block_array_t<session_base_t*>& pslots) {
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
            // max_peers_ is the RESOLVED cap, never 0, so there is no "0 means uncapped" arm
            // left to test: a newcomer past it is refused and no admitted peer is evicted.
            if (slots_.size() >= max_peers_) {
                ::close(fd);  // clean refusal at the deployment cap, not a hung SYN
                return;
            }
            // A refused session, or a refused place for it in the poll set (the listen socket
            // plus every slot, #2156), refuses the peer exactly as the cap does: a clean close,
            // nothing half-built in the table, and no admitted peer the poll loop cannot see.
            // The set is grown here, before the slot exists, so a refusal never evicts anyone.
            mem::poly_ptr_t<session_base_t> fresh = make_session();
            if (!pfds.reserve(slots_.size() + 2) || !pslots.reserve(slots_.size() + 1) || !fresh ||
                !slots_.push_back(std::move(fresh))) {
                ::close(fd);
                return;
            }
            slot = slots_.back().get();
            idx = slots_.size() - 1;
        }
        // Mint this tenancy's HANDLE (#1294), under the lock and from the slot index. The
        // generation never repeats and never lands on 0 (which is reserved for "no peer"), so
        // a handle minted against the session that used to hold this slot can never be
        // mistaken for its successor's.
        if (++slot->gen_seq == 0) slot->gen_seq = 1;
        slot->handle = peer_handle_t{static_cast<std::uint32_t>(idx), slot->gen_seq};
        // The routable NAME is the handle spelled out — `p<slot>~<gen>`, legal by
        // construction (ADR-0073 §2). It carries the generation, so a reused slot gets a
        // FRESH name: a route another node still holds for the previous session names
        // nobody, its delivery is refused, and that refusal reaps the stale edge.
        // kPeerNameChars holds the widest index and generation: never truncated.
        const std::string_view named =
            format_session_name(slot->handle, std::span<char>(slot->name, kPeerNameChars - 1));
        slot->name[named.size()] = '\0';
    }
    // The bounded send (#838), armed here rather than in either derived on_accept. Every
    // egress write on a server is nonblocking (#2144), the WS 101 included; the bound stays
    // armed for any blocking write a derived server makes.
    set_snd_timeout(fd);
    // Socket options and the slot's protocol buffers, then its handshake stance: a raw
    // stream peer is open the moment it is accepted, a WS peer only past its 101.
    const bool opens_now = on_accept(*slot, fd);
    // A new peer starts alive (#2198): the next liveness check stamps its record.
    slot->tx_moved.store(true, std::memory_order_relaxed);
    slot->rx_moved.store(true, std::memory_order_relaxed);
    slot->rx_owed.store(mid_frame(*slot), std::memory_order_relaxed);
    // Publish the fd FIRST, then open the session: "open ⇒ fd valid" is the invariant (#891).
    // A sender reads the fd only inside the session's egress role, and the role cannot be
    // claimed until the egress opens — a release a claim's acquire pairs with — so no sender
    // can see the slot half-published, and none waits on the accept to finish.
    // Release: the liveness check's acquire of the fd then sees the flags stored above (#2198).
    slot->fd.store(fd, std::memory_order_release);
    // The seam that makes the order testable: the fd is published, the slot not yet open. A
    // broadcast that lands here must skip the slot rather than write to fd -1 (`tcp_test`,
    // the accept-publish race).
    on_slot_publishing();
    if (opens_now) open_session(*slot);
    // Arrival seam (#1223), fired LAST and with no transport lock held — the mirror image of
    // teardown_slot's departure seam, in every respect including the `open` condition: only a
    // session that reached `open` can flow frames, so only one gets an identity. A WS slot is
    // NOT open here (it opens at its 101), and announces itself from that site instead.
    if (opens_now) publish_peer_up(*slot);
}

void slot_server_t::open_session(session_base_t& s, std::span<const std::byte> first) {
    // The first frame is on the wire before any sender can claim the role, so every send
    // goes after it; and `open` is stored before a byte of it is written, so a peer that has
    // read it finds the session open. A first frame with no block closes the session.
    const ::iovec v{const_cast<std::byte*>(first.data()), first.size()};
    egress_frame_t* const f = first.empty() ? nullptr : frame_of(std::span<const ::iovec>(&v, 1));
    if (f == nullptr && !first.empty()) condemn_fd(s.fd.load(std::memory_order_relaxed));
    egress_.open(s.egress, f);
    s.open.store(true, std::memory_order_release);
    flush_peer(s);  // writes the first frame now; with none held it takes the role and leaves
}

void slot_server_t::publish_peer_up(const session_base_t& s) {
    // `bus_mode()`, not `peer_named_`: a build with no bus module has no per-session identity
    // to anchor, so the arrival seam folds away entirely there (#375 deliverable 3).
    if (!bus_mode()) return;
    char name[kPeerNameChars];
    peer_handle_t handle;
    {
        // The name is peers_m_-guarded state (enumerate_peers / peer_link read it off this
        // thread), so COPY it out under the lock and notify outside: the notifier re-enters
        // the routing plane, and holding a transport lock across that inverts the documented
        // order (`transport_vertex_t::ctl_m_` → router → `graph_t::map_mutex_`).
        const std::lock_guard lock(peers_m_);
        std::memcpy(name, s.name, sizeof(name));
        handle = s.handle;
    }
    // Through the arrival HOOK, not `notify_peer_up` directly (#1438): the notifier is the
    // bus facet's, and this tier is no longer a `bus_link_t`. `bus_slot_server_t` overrides
    // the hook to fire it; the flat arm's inherited no-op is unreachable behind the
    // `bus_mode()` guard above.
    if (name[0] != '\0') announce_peer_up(handle, name);
}

void slot_server_t::service_peer(session_base_t& s) {
    const int fd = s.fd.load(std::memory_order_relaxed);
    if (fd < 0) return;
    // The read and the framing are the derived server's: a length-prefix framer feed, or
    // the WS handshake and then frame ingress. Either may tear this slot down in the hook.
    s.rx_moved.store(true, std::memory_order_relaxed);
    on_readable(s, fd);
    // What the peer still owes after the read, for the liveness check (#2198).
    s.rx_owed.store(mid_frame(s), std::memory_order_relaxed);
}

void slot_server_t::teardown_slot(session_base_t& s) {
    char departed[kPeerNameChars];
    peer_handle_t departed_handle;
    {
        // Stop peer_link/enumerate resolution FIRST, so no new sender targets the dying
        // slot by name. Keep the name: it identifies the departed session to the eviction
        // seam below.
        const std::lock_guard lock(peers_m_);
        std::memcpy(departed, s.name, sizeof(departed));
        s.name[0] = '\0';
        // RETIRE the handle with the name (#1294): the seam's contract is "valid until
        // depart", so nothing this slot delivers after this point may carry the departed
        // session's identity. `gen_seq` survives, so the next accept mints a fresh one.
        departed_handle = s.handle;
        s.handle = peer_handle_t{};
    }
    // The stream teardown-under-role invariant, per slot: clear `open`, shut the fd down so
    // a write in flight returns, close the egress (which waits its writer out and releases
    // every waiting frame), and only THEN reset and close the fd — so no sender ever writes
    // to a closed or reused descriptor. A stalled peer's frames and strikes die with its
    // session and are never inherited by whoever next claims this slot (#838).
    const bool was_open = s.open.exchange(false, std::memory_order_acq_rel);
    const int fd = s.fd.load(std::memory_order_relaxed);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    egress_.close(s.egress);
    s.fd.store(-1, std::memory_order_relaxed);
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
    if (was_open && departed[0] != '\0') {
        if (bus_mode())
            announce_peer_down(departed_handle, departed);
        else if (!any_open_session())
            notify_down();
    }
}

void slot_server_t::shut_down_slots() {
    // The poll thread first: it is the only other thread that tears a slot down or reads the
    // listen socket, so after the join both are this call's alone.
    stop_and_join();
    for (const mem::poly_ptr_t<session_base_t>& s : slots_) teardown_slot(*s);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
}

bool slot_server_t::any_open_session() const {
    // peers_m_ guards the slot VECTOR against the cross-thread readers (enumerate_peers /
    // peer_link); the `open` loads are relaxed for the same reason they are everywhere else
    // — the lock, not the memory order, is what orders them.
    const std::lock_guard lock(peers_m_);
    for (const mem::poly_ptr_t<session_base_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed)) return true;
    return false;
}

void slot_server_t::run() {
    // ONE poll pass multiplexes the listen socket and every live peer — no per-peer thread
    // (the MCU-shaped choice, #362), bounded to 100 ms so the loop stays
    // shutdown-responsive (the posix_endpoint_t idiom).
    //
    // The poll set grows to exactly fit the slot table, never ahead of it (#2156): sizing it
    // to the admission cap up front charged every idle listener the whole cap (two 100-entry
    // tables).
    // accept_peer grows it BEFORE a new slot is admitted and refuses the newcomer when that
    // growth is refused, so every slot always has a place here and no admitted peer goes
    // unpolled. Slots are recycled, never freed, and a cleared set keeps its block.
    mem::block_array_t<pollfd> pfds(slots_.source());
    mem::block_array_t<session_base_t*> pslots(slots_.source());
    pollfd listen_only{listen_fd_, POLLIN, 0};
    while (!stop_.load(std::memory_order_relaxed)) {
        pfds.clear();
        pslots.clear();
        if (pfds.push_back(listen_only)) {
            const std::lock_guard lock(peers_m_);
            for (const mem::poly_ptr_t<session_base_t>& s : slots_) {
                const int fd = s->fd.load(std::memory_order_relaxed);
                if (fd < 0) continue;
                // A peer with a held frame also waits for writability (#2144).
                const auto ev = static_cast<short>(
                    POLLIN | (POLLOUT * static_cast<int>(s->egress.wants_write())));
                if (!pfds.push_back(pollfd{fd, ev, 0})) break;
                if (!pslots.push_back(s.get())) {
                    pfds.pop_back();
                    break;
                }
            }
        }
        pollfd* const set = pfds.empty() ? &listen_only : pfds.data();
        const std::size_t set_n = pfds.empty() ? 1 : pfds.size();
        const int pr = ::poll(set, static_cast<nfds_t>(set_n), 100);
        if (pr < 0) continue;  // transient error → re-check stop_
        if (stop_.load(std::memory_order_relaxed)) break;
        // Peers first (their events are bound to this pass's fd list), then the accept
        // (which may add a slot). Writability resumes a held frame; a pass that ran out its
        // quantum is no event, so it strikes nobody (#2144).
        for (std::size_t i = 1; i < set_n; ++i) {
            if ((set[i].revents & POLLOUT) != 0) flush_peer(*pslots[i - 1]);
            if ((set[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) service_peer(*pslots[i - 1]);
        }
        if ((set[0].revents & POLLIN) != 0) accept_peer(pfds, pslots);
    }
}

}  // namespace tr::net
