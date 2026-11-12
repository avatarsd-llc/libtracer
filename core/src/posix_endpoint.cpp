/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/posix_endpoint.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <utility>

#include "libtracer/iov_table.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/static_thread.hpp"

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
    PEER_GONE,      /**< @brief This socket is dead — drop the rest silently (#66 lifecycle). */
    STALLED,        /**< @brief The socket is full: the peer is not taking bytes right now
                                (#838). Healthy call, healthy socket, slow or absent peer. */
    MALFORMED_CALL, /**< @brief The arguments were rejected — OUR defect, not a disconnect. */
};

/** @brief Process-wide malformed-call tally (#948) — see @ref write_fault_stats_t. Word-wide,
 *         as every drop counter is (#1697): a 64-bit atomic is a libatomic call on rv32. */
std::atomic<std::size_t> g_malformed_calls{0};

/** @brief The errno of the most recent malformed-call fault (#948). */
std::atomic<int> g_last_malformed_errno{0};

/**
 * @brief The ONE write-fault policy of the one full-write loop (#903 / #948 / #838).
 *
 * Three outcomes, not two — conflating the last with the first is #948, where one
 * unimplemented flag on one platform's `sendmsg` made every data frame vanish while the
 * connection stayed up, looking exactly like a peer that never asked for anything. Every
 * write is nonblocking (#2252), so no write sleeps and none is interrupted by a signal: an
 * `EINTR` has nothing to resume and is an ordinary malformed call.
 *
 * - **PEER_GONE.** An errno that says this fd is no longer a usable connection (the peer
 *   went away, the route died, the fd is not an open socket), plus the `n == 0` a stream
 *   write of a non-empty buffer must never return. Nothing is deliverable on it: drop the
 *   rest silently, link-down is #66 lifecycle. `EBADF`/`ENOTSOCK` sit here deliberately —
 *   a recycled fd is indistinguishable from a vanished peer, and a teardown race must not
 *   fabricate a defect report in the counter below.
 * - **STALLED.** `EAGAIN`/`EWOULDBLOCK`: the socket is full, because the peer's receive
 *   window is. Nothing is wrong with the call OR the socket — the PEER is not taking bytes
 *   right now. It used to land in MALFORMED_CALL below, which would both mis-file a stalled
 *   peer as a libtracer defect and abandon the record after one re-attempt; it is its own
 *   arm so the egress keeps the frame and its offset, resumes it on writability, and
 *   attributes the stall to the peer that earned it.
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

/**
 * @brief The fallback adapter's state: the private module and the one thread that drives it,
 *        in one block drawn from the link's stack plane. It records that plane, so the link
 *        holds only the pointer.
 */
struct posix_endpoint_t::fallback_t {
    /** @brief Opens the private module; the block came from @p s . */
    explicit fallback_t(mem::block_source_t& s) noexcept : src(&s) {}
    mem::block_source_t* src; /**< @brief Where this block (and the thread's stack) came from. */
    wait_module_t module;     /**< @brief The private module. */
    static_thread_t thread;   /**< @brief Its driver. */
};

posix_endpoint_t::~posix_endpoint_t() {
    stop_and_join();
    if (fallback_ != nullptr) mem::drop_in(*fallback_->src, fallback_);
}

void posix_endpoint_t::thread_entry(void* fallback) {
    static_cast<fallback_t*>(fallback)->module.run();
}

bool posix_endpoint_t::start(std::size_t stack_size, mem::block_source_t& src) {
    if (node_ != nullptr) {
        wait_.store(node_, std::memory_order_release);
        return true;
    }
    // The fallback adapter: a private module, whose poll-adapter tables come from the same
    // stack plane, and one thread of the link's own to drive it.
    fallback_ = mem::make_in<fallback_t>(src, src);
    if (fallback_ == nullptr || !fallback_->module.ok()) return false;
    wait_.store(&fallback_->module, std::memory_order_release);
    if (fallback_->thread.start(&posix_endpoint_t::thread_entry, fallback_, stack_size, "tr_wait",
                                src))
        return true;
    wait_.store(nullptr, std::memory_order_release);
    return false;
}

void posix_endpoint_t::stop_and_join() {
    assert(!on_callback_thread() && "a link's destructor runs off every callback thread");
    (void)stop_for_shut_down();
}

bool posix_endpoint_t::stop_for_shut_down() {
    const bool deferred = on_callback_thread();
    // Before any descriptor is read: a callback's release_fd that runs after this keeps its fd.
    if (deferred) shut_.store(true, std::memory_order_seq_cst);
    wait_module_t* const w = wait_.load(std::memory_order_acquire);
    if (w == nullptr) return deferred;
    // A private module's driver goes first: once it is joined no callback can run, so the
    // detach takes no lock and waits on nobody, and a teardown made under the caller's lock
    // (a transport vertex's control plane) cannot cross a callback that takes that lock. Not
    // joined on a deferred teardown: the thread may be this one, or be inside a callback that
    // waits on this one. Once its callback returns its run() sees the stop and the thread
    // ends; the destructor joins it.
    if (fallback_ != nullptr) {
        fallback_->module.stop();
        if (!deferred) fallback_->thread.join();
    }
    w->detach(src_);
    return deferred;
}

void posix_endpoint_t::release_fd(std::atomic<int>& slot, int fd) noexcept {
    // Cleared, THEN the mark read, against stop_for_shut_down's mark-then-read (both
    // seq_cst): a teardown that read this fd sees it kept open, and one that read -1 found
    // it gone, so no shutdown lands on a closed descriptor.
    slot.store(-1, std::memory_order_seq_cst);
    if (!shut_.load(std::memory_order_seq_cst)) {
        ::close(fd);
        return;
    }
    slot.store(fd, std::memory_order_relaxed);  // the destructor closes it
}

void posix_endpoint_t::release_listen(int& fd) noexcept {
    if (fd < 0) return;
    if (shut_.load(std::memory_order_relaxed)) {
        (void)::shutdown(fd, SHUT_RDWR);
        return;
    }
    ::close(fd);
    fd = -1;
}

stream_endpoint_t::~stream_endpoint_t() {
    // The derived destructor already ran stop_and_join (its first act, per the
    // posix_endpoint_t teardown invariant), so nothing races this exchange.
    const int leftover = conn_fd_.exchange(-1, std::memory_order_relaxed);
    if (leftover >= 0) ::close(leftover);
    egress_.close(peer_);  // every waiting frame and lane back to the link's store
}

write_result_t stream_endpoint_t::write_all(int fd, std::span<const std::byte> bytes) {
    const ::iovec v{const_cast<std::byte*>(bytes.data()), bytes.size()};
    // An empty record is a complete one, with no syscall.
    return write_all_iov(fd, std::span<const ::iovec>(&v, bytes.empty() ? 0 : 1));
}

write_result_t stream_endpoint_t::write_all_iov(int fd, std::span<const ::iovec> vec) {
    if (fd < 0) return {write_outcome_t::FAILED, 0};
    std::size_t moved = 0;  // bytes of this record on the socket
    bool retry_spent = false;
    std::size_t i = 0;     // the first entry not yet fully written
    std::size_t skip = 0;  // bytes of entry i already written
    while (i < vec.size()) {
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
        // Nonblocking (#2252): a full socket refuses the call at once, so no caller waits on
        // a peer that stopped reading.
        const ssize_t n = send_gather(fd, &msg, MSG_DONTWAIT);
        if (n <= 0) {
            const write_fault_t fault = classify_write_fault(n);
            if (fault == write_fault_t::STALLED) return {write_outcome_t::STALLED, moved};
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

egress_step_t stream_endpoint_t::write_gather(int fd, std::span<const ::iovec> vec) {
    return step_of(write_all_iov(fd, vec));
}

egress_step_t stream_endpoint_t::write_frame(int fd, const egress_frame_t& f, std::size_t off) {
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
    return write_gather(fd, std::span<const ::iovec>(vec, n));
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

std::uint64_t stream_endpoint_t::sweep_one(std::uint64_t now, std::uint64_t deadline) {
    sweep_t sw{.now = now, .deadline = deadline};
    // The slot server's rule (#2812): each direction is owed and moved on its own. What is
    // owed is read FIRST, with acquire: each writer marks its progress before it publishes
    // what it still owes (release), so a turn that lands between the reads is seen as moved
    // whenever its debt is seen, and never closes a live peer.
    const bool tx_owed = peer_.wants_write();
    const bool rx_owed = rx_owed_.load(std::memory_order_acquire);
    const bool tx = tx_moved_.exchange(false, std::memory_order_relaxed);
    const bool rx = rx_moved_.exchange(false, std::memory_order_relaxed);
    if (!sw.frozen(tx_owed || rx_owed, (!tx && tx_owed) || (!rx && rx_owed), live_since_))
        return sw.next;
    // Inside the writer role, so the fd it shuts down is still the peer's; a role somebody
    // holds is a write in progress, and the check one window on asks again.
    if (!egress_.condemn_held(
            peer_, [this] { condemn_fd(conn_fd_.load(std::memory_order_acquire)); },
            !rx && rx_owed))
        return now + (now - deadline) + 1;
    live_since_ = now;
    frozen_closed_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void stream_endpoint_t::note_egress(const egress_result_t& r) noexcept {
    if (r.shed != 0) drops_.count_dropped_tx(r.shed);
    if (r.stalled != 0) stalled_tx_.fetch_add(r.stalled, std::memory_order_relaxed);
}

void stream_endpoint_t::condemn_fd(int fd) noexcept {
    // One teardown path, with its departure notification (the close_peer discipline): the
    // fd stays owned by whoever opened it.
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

void stream_endpoint_t::flush_one(int fd) {
    // The driving thread owns the fd's lifecycle, so the turn names it directly; it never
    // waits.
    note_egress(egress_.flush(
        peer_,
        [&](const egress_frame_t& f, std::size_t off) {
            return note_moved(tx_moved_, write_frame(fd, f, off));
        },
        [fd] { condemn_fd(fd); }));
    disarm_flushed(src_, peer_);
}

void stream_endpoint_t::disarm_flushed(wait_source_t& s, const egress_peer_t& peer) {
    // Interest follows the held frame, armed here too: a session's first frame is flushed
    // before any writability event, and one the socket cut short would otherwise wait for
    // the next send (#2781). Then look again: a sender that held a frame after the first
    // look armed it, and a disarm here may have landed after that arm.
    const bool held = peer.wants_write();
    waiter().want_write(s, held);
    if (peer.wants_write()) waiter().want_write(s, true);
}

void stream_endpoint_t::depart() {
    waiter().detach(src_);
    src_.write.store(false, std::memory_order_relaxed);  // a next peer starts unarmed
    teardown_peer(src_.fd.load(std::memory_order_relaxed));
}

void stream_endpoint_t::teardown_peer(int fd) {
    // shutdown first so a write in flight returns; the egress close then waits out the
    // writer role and releases every frame, so no sender reads conn_fd_ after this: only
    // then is the fd reset and closed (never a write to a closed or reused fd).
    ::shutdown(fd, SHUT_RDWR);
    egress_.close(peer_);
    rx_owed_.store(false, std::memory_order_relaxed);  // a gone peer owes nothing (#2812)
    release_fd(conn_fd_, fd);
}

void stream_endpoint_t::shut_down_stream() {
    // Detached first, so no callback can tear the peer down under this one. Deferred, a
    // callback may still run: the fd is read after the link is marked shut, so it stays open.
    (void)stop_for_shut_down();
    const int fd = conn_fd_.load(std::memory_order_seq_cst);
    if (fd >= 0) teardown_peer(fd);
}

void stream_endpoint_t::accept_one(int listen_fd, function_ref_t<bool(int)> on_accept,
                                   wait_source_t::fire_t fire) {
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) return;
    if (!on_accept(fd)) {  // per-peer setup / handshake failed → reject
        ::close(fd);
        return;
    }
    conn_fd_.store(fd, std::memory_order_relaxed);
    tx_moved_.store(true, std::memory_order_relaxed);  // a new peer starts alive (#2198)
    rx_moved_.store(true, std::memory_order_relaxed);
    // The listener's source carries the connection now: nobody else is accepted until the
    // peer departs and `relisten` puts it back. The egress opens only once the source is on
    // the connection, so no sender can arm write interest on the listening socket.
    // A refused registration refuses the peer and puts the listener back, so the link keeps
    // accepting rather than going deaf.
    const wait_source_t::fire_t on_listen = src_.fire;
    waiter().detach(src_);
    if (!attach(src_, fd, fire)) {
        teardown_peer(fd);
        relisten(listen_fd, on_listen);
        return;
    }
    egress_.open(peer_);  // release: a sender that claims the role then sees the fd
}

void stream_endpoint_t::relisten(int listen_fd, wait_source_t::fire_t fire) {
    (void)attach(src_, listen_fd, fire);
}

// ---------------------------------------------------------------------------
// slot_server_t — the multi-peer slot/poll machinery both stream servers share
// (#871). One home for the shape transport_tcp.cpp and transport_ws.cpp used to
// restate line-for-line; only framing and handshake stay in the derived TUs.
// ---------------------------------------------------------------------------

slot_server_t::~slot_server_t() {
    // The derived destructor's first act was stop_and_join, so the link is off the wait
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
        close_listen();
        return false;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        bound_port_ = ntohs(bound.sin_port);
    return true;
}

void slot_server_t::close_listen() noexcept {
    ::close(listen_fd_);
    listen_fd_ = -1;
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
            return &endpoint_of(*s);
    return nullptr;
}

transport_t* slot_server_t::peer_link_of(peer_handle_t peer) {
    // `p<slot>~<gen>` names slot `slot` in tenancy `gen` (see `peer_name`), so the slot IS
    // the lookup and the handle compare IS `peer_link`'s tenancy check: a departed session's
    // handle equals no open slot's, without the name compare across every slot.
    if (!peer.valid()) return nullptr;
    const std::lock_guard lock(peers_m_);
    if (peer.index >= slots_.size()) return nullptr;
    session_base_t& s = *slots_[peer.index];
    return s.open.load(std::memory_order_relaxed) && s.handle == peer ? &endpoint_of(s) : nullptr;
}

bool slot_server_t::close_peer(std::string_view peer) {
    // Shutdown-only under peers_m_; the driving thread's next pass observes the close and runs
    // the IDENTICAL remote-FIN teardown — no duplicate logic, and no off-thread touch of the
    // driving-thread-only buffers teardown_slot's on_slot_reset hook clears. The fd cannot be
    // closed under us: teardown un-names the slot under this same lock before it closes it.
    const std::lock_guard plock(peers_m_);
    for (const mem::poly_ptr_t<session_base_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed) || s->name_view() != peer) continue;
        condemn_fd(s->fd.load(std::memory_order_relaxed));
        return true;
    }
    return false;
}

std::uint64_t slot_server_t::sweep(std::uint64_t now, std::uint64_t deadline) {
    // Under peers_m_, the close_peer discipline: teardown un-names a slot under this lock
    // before it closes the fd, so a named slot's fd is still its own here.
    sweep_t sw{.now = now, .deadline = deadline};
    std::size_t closed = 0;
    const std::lock_guard lock(peers_m_);
    for (const mem::poly_ptr_t<session_base_t>& s : slots_) {
        // Acquire pairs with accept_peer's release of the fd: a slot whose fd is seen
        // published also shows its fresh progress flags, never a reused slot's stale ones.
        const int fd = s->fd.load(std::memory_order_acquire);
        if (fd < 0 || s->name[0] == '\0') continue;
        // Owed first, with acquire (the order sweep_one explains, #2812): progress marked
        // before a debt was published is then never missed.
        const bool tx_owed = s->egress.wants_write();
        const bool rx_owed = s->rx_owed.load(std::memory_order_acquire);
        const bool tx = s->tx_moved.exchange(false, std::memory_order_relaxed);
        const bool rx = s->rx_moved.exchange(false, std::memory_order_relaxed);
        if (!sw.frozen(tx_owed || rx_owed, (!tx && tx_owed) || (!rx && rx_owed), s->live_since))
            continue;
        condemn_fd(fd);  // the driving thread's next pass runs the remote-FIN teardown
        s->live_since = now;
        ++closed;
    }
    frozen_closed_.fetch_add(closed, std::memory_order_relaxed);
    return sw.next;
}

void slot_server_t::send_one(session_base_t& s, function_ref_t<egress_frame_t*()> frame,
                             std::span<const ::iovec> rec, const graph::delivery_lane_t& lane) {
    // The fd is read INSIDE the writer role: accept opens the egress only after publishing
    // it, and teardown closes the egress (waiting the role out) before resetting it.
    const auto fd = [&s] { return s.fd.load(std::memory_order_relaxed); };
    // The arm runs inside the session's writer role, so its fd cannot be closed under it.
    const egress_result_t r = egress_.send(
        s.egress, lane, frame, [&] { return note_moved(s.tx_moved, write_gather(fd(), rec)); },
        [&](const egress_frame_t& f, std::size_t off) {
            return note_moved(s.tx_moved, write_frame(fd(), f, off));
        },
        [&] { condemn_fd(fd()); }, [&] { arm_write(s); });
    note_egress(r);
}

void slot_server_t::broadcast(std::span<const ::iovec> rec, const graph::value_t* value,
                              const graph::delivery_lane_t& lane) {
    // ONE shared frame for the whole fan-out, built only if some peer has to wait for it
    // (ADR-0087 §2). No lock (#2263): the chain only ever grows and its slots are never freed,
    // so a loaded slot stays walkable, and the egress role, not peers_m_, is what keeps a
    // departing session's fd its own while a write is in it.
    egress_once_t once([&] { return frame_of(rec, value); });
    for (session_base_t* s = first_.load(std::memory_order_acquire); s != nullptr;
         s = s->next.load(std::memory_order_acquire))
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
        return note_moved(s.tx_moved, write_frame(fd(), f, off));
    };
    const auto condemn = [&] { condemn_fd(fd()); };
    note_egress(egress_.flush(s.egress, write, condemn));
    disarm_flushed(s, s.egress);
}

void slot_server_t::accept_peer() {
    sockaddr_in remote{};
    socklen_t rlen = sizeof(remote);
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&remote), &rlen);
    if (fd < 0) return;

    session_base_t* slot = nullptr;
    {
        const std::lock_guard lock(peers_m_);
        // A shut link accepts nothing. Read under the lock a shut link's slot walk takes, so a
        // session either lands before the walk reaches its index, or finds the link shut.
        if (shut()) {
            ::close(fd);
            return;
        }
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
            // A refused session refuses the peer exactly as the cap does: a clean close and
            // nothing half-built in the table. The fd was never published, so its close never
            // waits.
            mem::poly_ptr_t<session_base_t> fresh;
            if (slots_.size() >= max_peers_ || !(fresh = make_session()) ||
                !slots_.push_back(std::move(fresh))) {
                ::close(fd);  // a clean refusal, not a hung SYN
                return;
            }
            slot = slots_.back().get();
            idx = slots_.size() - 1;
            // Its wait callback and context, the same for every tenancy of the slot: the
            // context every callback casts back, and the server its facade reaches.
            slot->fire = &slot_server_t::on_session;
            slot->ctx = static_cast<posix_endpoint_t*>(this);
            // Link the slot at the HEAD of the broadcast's chain (#2263): this thread is its
            // only writer, and the release makes a walker that loads the slot see it
            // constructed. Its egress is still closed, so it takes nothing yet.
            slot->next.store(first_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            first_.store(slot, std::memory_order_release);
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
    if constexpr (graph::kFaultInjection) {
        if (detail::slot_taken_hook != nullptr) detail::slot_taken_hook();
    }
    // Socket options and the slot's protocol buffers, then its handshake stance: a raw
    // stream peer is open the moment it is accepted, a WS peer only past its 101.
    const bool opens_now = on_accept(*slot, fd);
    // A new peer starts alive (#2198): the next liveness check stamps its record.
    slot->tx_moved.store(true, std::memory_order_relaxed);
    slot->rx_moved.store(true, std::memory_order_relaxed);
    // Relaxed: the fd's release below publishes all three, and the check acquires the fd.
    slot->rx_owed.store(mid_frame(*slot), std::memory_order_relaxed);
    // Publish the fd FIRST, then open the session: "open ⇒ fd valid" is the invariant (#891).
    // A sender reads the fd only inside the session's egress role, and the role cannot be
    // claimed until the egress opens — a release a claim's acquire pairs with — so no sender
    // can see the slot half-published, and none waits on the accept to finish.
    // Release: the liveness check's acquire of the fd then sees the flags stored above (#2198).
    slot->write.store(false, std::memory_order_relaxed);
    slot->fd.store(fd, std::memory_order_release);
    // On the wait before it opens, so a first frame the socket holds can arm writability. A
    // refused registration refuses the peer, as the cap does. A slot taken before a deferred
    // teardown walked the table may be attached after it: on a shut link the session is torn
    // down the same way. That read comes after the attach (seq_cst, against the mark the
    // teardown stores before its walk), so either it sees the link shut or the walk's detach
    // follows the attach.
    if (!waiter().attach(*slot) || shut_seq()) {
        teardown_slot(*slot);
        return;
    }
    // The seam that makes the order testable: the fd is published, the slot not yet open. A
    // broadcast that lands here must skip the slot rather than write to fd -1 (`tcp_test`,
    // the accept-publish race).
    on_slot_publishing();
    // Arrival seam (#1223), fired LAST and with no transport lock held — the mirror image of
    // teardown_slot's departure seam, in every respect including the `open` condition: only a
    // session that reached `open` can flow frames, so only one gets an identity. A WS slot is
    // NOT open here (it opens at its 101), and announces itself from that site instead.
    if (opens_now) {
        open_session(*slot);
        publish_peer_up(*slot);
    }
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
    // Release: published after rx_moved, so a check that sees the debt sees the progress.
    s.rx_owed.store(mid_frame(s), std::memory_order_release);
}

void slot_server_t::teardown_slot(session_base_t& s) {
    // Off the wait first: no callback for this slot runs after, so a shut-down walk never
    // tears down a session under its own callback, and this is that walk's only detach.
    if (wait_module_t* const w = waiting()) w->detach(s);
    char departed[kPeerNameChars];
    peer_handle_t departed_handle;
    bool was_open = false;
    bool open_left = false;
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
        // Clear `open` and ask whether any session is still open in ONE critical section, so
        // of concurrent teardowns (a shut-down walk and a peer's own departure) exactly one
        // closes the last open session; the departing slot is already closed, so it never
        // counts itself (#889). The lock also orders the table read after every growth.
        was_open = s.open.exchange(false, std::memory_order_acq_rel);
        for (const mem::poly_ptr_t<session_base_t>& o : slots_)
            open_left |= o->open.load(std::memory_order_acquire);
    }
    // The stream teardown-under-role invariant, per slot: shut the fd down so a write in
    // flight returns, close the egress (which waits its writer out and releases every
    // waiting frame), and only THEN reset and close the fd — so no sender ever writes to a
    // closed or reused descriptor. A stalled peer's frames and strikes die with its session
    // and are never inherited by whoever next claims this slot (#838).
    if constexpr (graph::kFaultInjection) {
        if (detail::slot_teardown_hook != nullptr) detail::slot_teardown_hook();
    }
    const int fd = s.fd.load(std::memory_order_seq_cst);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    egress_.close(s.egress);  // once per open
    if (fd >= 0) release_fd(s.fd, fd);
    // A shut link's session buffers live until its destructor: a callback of the link may
    // still be using them.
    if (!shut()) on_slot_reset(s);
    // Departure seam (RFC-0009 §D.5): fired LAST, with no transport lock held — the
    // notifier re-enters the routing plane. Only a session that reached `open` can have
    // flowed frames (subscriptions), and only then.
    //
    // WHICH seam is the constructed mode's answer, not "is a peer sink installed" (#889):
    // peer-named mode evicts exactly the departed peer's edges under its own name, while a
    // FLAT link has ONE routing identity for every peer it carries — the registered child
    // NAME — so its only seam is the whole link. Firing that on a mid-life close evicts the
    // SURVIVING peers' edges too, which is why it waits for the last open session to go.
    if (was_open && departed[0] != '\0') {
        if (bus_mode())
            announce_peer_down(departed_handle, departed);
        else if (!open_left)
            notify_down();
    }
}

void slot_server_t::shut_down_slots(bool tear_down) {
    // The fallback thread and the listener first, for the reason posix_endpoint_t gives:
    // `slots_` grows only in the listener's callback, so the table is then still, except
    // against an accept already past its shut check on a deferred teardown (see
    // accept_peer). Then ONE walk: each slot off the wait, and torn down for a shut_down.
    (void)stop_for_shut_down();
    for (std::size_t i = 0; session_base_t* const s = slot_at(i); ++i)
        if (tear_down)
            teardown_slot(*s);  // its first act is the slot's detach
        else if (wait_module_t* const w = waiting())
            w->detach(*s);
    release_listen(listen_fd_);
}

void slot_server_t::shut_down() { shut_down_slots(true); }

void slot_server_t::stop_and_join() {
    assert(!on_callback_thread() && "a link's destructor runs off every callback thread");
    shut_down_slots(false);
}

slot_server_t::session_base_t* slot_server_t::slot_at(std::size_t i) {
    // Under the lock the accept grows the table under: a growth never meets this read, and a
    // session, never removed once added, stays valid after it.
    const std::lock_guard lock(peers_m_);
    return i < slots_.size() ? slots_[i].get() : nullptr;
}

bool slot_server_t::serve(std::size_t stack_size, mem::block_source_t& stack_src) {
    // ONE wait multiplexes the listen socket and every live peer, with no per-peer thread
    // (#362) and no timeout (#2147).
    return start(stack_size, stack_src) && attach(src_, listen_fd_, &slot_server_t::on_listen);
}

void slot_server_t::on_listen(void* ctx, wait_source_t& self, std::uint8_t ready) {
    (void)self;
    (void)ready;
    static_cast<slot_server_t*>(static_cast<posix_endpoint_t*>(ctx))->accept_peer();
}

void slot_server_t::on_session(void* ctx, wait_source_t& self, std::uint8_t ready) {
    auto* const server = static_cast<slot_server_t*>(static_cast<posix_endpoint_t*>(ctx));
    auto& s = static_cast<session_base_t&>(self);
    // Writability resumes a held frame (#2144); then the read, which may tear the slot down.
    if (writable(ready)) server->flush_peer(s);
    if (readable(ready)) server->service_peer(s);
}

}  // namespace tr::net
