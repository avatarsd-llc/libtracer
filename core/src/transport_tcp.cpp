/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_tcp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/length_prefix_framer.hpp"

namespace tr::net {

namespace {

/** @brief The u32-LE length prefix (transport framing) — the framer's, shared verbatim. */
constexpr std::size_t kPrefixBytes = length_prefix_framer::kPrefixBytes;

/**
 * @brief Frames are small and latency-sensitive (a READ round-trip is two tiny records); Nagle
 *        coalescing would serialize them behind ACKs.
 */
void set_nodelay(int fd) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/**
 * @brief The gathered `[u32-LE prefix, spans...]` record EVERY stream sender
 *        writes — built once, shared by the one-peer transport, the multi-peer
 *        server's broadcast, and the directed peer facade (one copy of the
 *        assembly in flash, not six).  The iovec count is small and bounded (a
 *        FWD forward/reply is ≤ ~6 spans), so the common case uses the fixed
 *        stack array; only an unusually large gather falls back to the heap
 *        vector.  `ok` is false when the record exceeds @p cap — the peer
 *        would reject it as malformed.  The prefix lives inside the struct, so
 *        the assembled iovec stays valid for the struct's (local) lifetime.
 */
struct prefixed_iov_t {
    static constexpr std::size_t kMaxInlineIov = 16;
    std::array<std::byte, kPrefixBytes> prefix;
    std::array<::iovec, kMaxInlineIov + 1> inline_vec;
    std::vector<::iovec> heap_vec;
    ::iovec* vec = nullptr;
    std::size_t n = 0;
    bool ok = false;

    prefixed_iov_t(std::span<const std::span<const std::byte>> iov, std::size_t cap) {
        std::size_t total = 0;
        for (const std::span<const std::byte>& s : iov) total += s.size();
        if (total > cap) return;
        detail::store_le(prefix, static_cast<std::uint32_t>(total));
        vec = inline_vec.data();
        if (iov.size() + 1 > inline_vec.size()) {
            heap_vec.resize(iov.size() + 1);
            vec = heap_vec.data();
        }
        vec[0] = ::iovec{prefix.data(), prefix.size()};
        n = 1;
        for (const std::span<const std::byte>& s : iov) {
            if (s.empty()) continue;  // writev rejects nothing, but skip no-op entries
            vec[n++] = ::iovec{const_cast<std::byte*>(s.data()), s.size()};
        }
        ok = true;
    }
};

}  // namespace

tcp_transport_t::tcp_transport_t(const std::string& peer_host, std::uint16_t peer_port,
                                 mem::mem_backend_t* backend, std::size_t max_frame)
    : backend_(backend) {
    if (max_frame != 0) max_frame_ = max_frame;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(peer_port);
    if (::inet_pton(AF_INET, peer_host.c_str(), &peer.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
        ::close(fd);
        return;
    }
    // A receive timeout lets every blocking read poll stop_ for a clean
    // shutdown (the posix_endpoint_t SO_RCVTIMEO idiom, applied per connection).
    set_rcv_timeout(fd);
    set_nodelay(fd);
    conn_fd_.store(fd, std::memory_order_relaxed);
    start([this, fd] {
        serve(fd);
        teardown_peer(fd);  // reset-under-write_m_ then close (stream_endpoint_t)
        // Departure seam (RFC-0009 §D extended): the one connection died under us —
        // not a local stop — so report the link down (no locks held here).
        if (!stop_.load(std::memory_order_relaxed)) notify_down();
    });
}

tcp_transport_t::tcp_transport_t(std::uint16_t bind_port, mem::mem_backend_t* backend,
                                 std::size_t max_frame)
    : listen_(true), backend_(backend) {
    if (max_frame != 0) max_frame_ = max_frame;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bind_port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
        ::listen(listen_fd_, 1) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        bound_port_ = ntohs(bound.sin_port);

    start([this] { run_listen(); });
}

tcp_transport_t::~tcp_transport_t() {
    stop_and_join();  // FIRST: the thread touches the fds released below
    // A leftover peer fd (never-spawned thread — dial failed) is closed by
    // ~stream_endpoint_t after this body; the listen socket is ours.
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void tcp_transport_t::send(std::span<const std::byte> frame) {
    // One span, same wire bytes (an empty frame is a prefix-only record either
    // way) — the gathered path is the one implementation.
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void tcp_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    // NOT const: write_all_iov consumes the iovec array in place.
    prefixed_iov_t rec(iov, kMaxFrame);
    if (!rec.ok) return;
    // Hold write_m_ across the whole write so (a) the recv thread cannot close and
    // reset conn_fd_ underneath us, and (b) two senders can never interleave their
    // length-prefixed records on the stream; read the fd inside the lock to pair
    // with the teardown.
    const std::lock_guard lock(write_m_);
    write_all_iov(conn_fd_.load(std::memory_order_relaxed), rec.vec, rec.n);
}

bool tcp_transport_t::read_exact(int fd, std::byte* dst, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        if (stop_.load(std::memory_order_relaxed)) return false;
        const ssize_t n = ::recv(fd, dst + off, len - off, 0);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) return false;  // peer closed the connection
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;  // receive timeout → re-check stop_ and resume the partial read
        return false;  // hard error
    }
    return true;
}

bool tcp_transport_t::drain(int fd, std::size_t len) {
    // Backpressure discard: the frame must still leave the stream (the next
    // prefix sits right behind it), so it is read into a stack scratch and dropped.
    std::array<std::byte, 4096> scratch;
    while (len > 0) {
        const std::size_t chunk = len < scratch.size() ? len : scratch.size();
        if (!read_exact(fd, scratch.data(), chunk)) return false;
        len -= chunk;
    }
    return true;
}

void tcp_transport_t::serve(int fd) {
    std::array<std::byte, kPrefixBytes> prefix;
    while (!stop_.load(std::memory_order_relaxed)) {
        // Read the 4-byte length prefix, reassembling it across TCP segment
        // boundaries (read_exact resumes partial reads through receive timeouts).
        if (!read_exact(fd, prefix.data(), prefix.size())) return;
        const std::size_t len = detail::load_le<std::uint32_t>(prefix);

        // The framing rules (effective cap, empty record, oversize ⇒ malformed,
        // alloc failure ⇒ backpressure drain) live in length_prefix_framer — one
        // home shared with the chunk-fed transports (quic/webtransport). Only the
        // byte source differs: this pull-mode loop reads the body straight off
        // the socket into the accepted segment (ADR-0042 §2/§4 — no library
        // buffer, no copy; feeding recv chunks through feed() would add one).
        using kind_t = length_prefix_framer::prefix_decision_t::kind_t;
        auto dec = length_prefix_framer::on_prefix(
            *backend_, length_prefix_framer::effective_cap(*backend_, max_frame_), len);
        if (dec.kind == kind_t::EMPTY) continue;  // an empty record carries no TLV — a no-op
        if (dec.kind == kind_t::MALFORMED) {
            // Malformed (corrupt/hostile) or undeliverable: count it and tear the
            // connection down — a desynced stream can't re-frame.
            malformed_rx_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (dec.kind == kind_t::DROP) {
            // Exhaustion is backpressure: drain the frame off the stream (framing
            // sync survives), drop it, tick the counter — never an OOM.
            dropped_rx_.fetch_add(1, std::memory_order_relaxed);
            if (!drain(fd, len)) return;
            continue;
        }

        view::segment_ptr_t seg = std::move(dec.seg);
        if (!read_exact(fd, seg->bytes.data(), len)) return;

        // Tier select lives in the slot (receiver_slot.hpp): the rope sink gets
        // the frame OWNING (narrowed to the frame length — pin/subview beyond
        // this call); a span-only sink gets the same segment bytes borrowed.
        rx_.deliver(view::view_t::over(std::move(seg)).subview(0, len));
    }
}

void tcp_transport_t::run_listen() {
    // The one-peer accept/serve/teardown shape is stream_endpoint_t's; only
    // the per-peer socket options are TCP's.
    run_accept_loop(
        listen_fd_,
        [this](int fd) {
            set_rcv_timeout(fd);
            set_nodelay(fd);
            return true;
        },
        [this](int fd) {
            serve(fd);
            // Departure seam (RFC-0009 §D extended): serve only returns once the
            // peer's connection is dead (or we are stopping); teardown_peer follows
            // in the accept loop before the next peer can connect, so eviction
            // never races a successor session on this single-peer link.
            if (!stop_.load(std::memory_order_relaxed)) notify_down();
        });
}

// ---------------------------------------------------------------------------
// transport_tcp_server — the multi-peer listener (the transport_ws_server
// slot/poll machinery over raw length-prefix stream framing).
// ---------------------------------------------------------------------------

/**
 * @brief One peer slot.  Slots are never destroyed while the server lives —
 *        recycled in place on departure — so the peer_endpoint_t facade
 *        `peer_link` hands out stays pointer-valid for the server's lifetime.
 *        Threading: @ref fd / @ref open are atomics (senders read them under
 *        `write_m_`); @ref name is guarded by `peers_m_`; the framer is
 *        poll-thread-only.
 */
struct transport_tcp_server::session_t {
    std::atomic<int> fd{-1};       /**< @brief The peer socket; -1 ⇒ free slot. */
    std::atomic<bool> open{false}; /**< @brief True while the connection is live. */
    std::string name;              /**< @brief The peer's name, `<ip>:<port>`. */
    length_prefix_framer framer;   /**< @brief Per-stream u32-LE frame reassembly. */
    peer_endpoint_t endpoint;      /**< @brief The directed facade `peer_link` returns. */
};

transport_tcp_server::transport_tcp_server(std::uint16_t bind_port, mem::mem_backend_t* backend,
                                           std::size_t max_frame, std::size_t max_peers,
                                           bool peer_named)
    : backend_(backend), max_peers_(max_peers), peer_named_(peer_named) {
    if (max_frame != 0) max_frame_ = max_frame;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bind_port);
    // SOMAXCONN: the OS's own accept-queue bound — admission is per-connection
    // in accept_peer (the max_peers deployment cap), never a synthetic backlog.
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
        ::listen(listen_fd_, SOMAXCONN) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        bound_port_ = ntohs(bound.sin_port);

    start([this] { run(); });
}

transport_tcp_server::~transport_tcp_server() {
    stop_and_join();  // FIRST: the run() thread touches the fds closed below
    if (listen_fd_ >= 0) ::close(listen_fd_);
    for (const std::unique_ptr<session_t>& s : slots_) {  // thread joined — nothing races
        const int fd = s->fd.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) ::close(fd);
    }
}

void transport_tcp_server::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void transport_tcp_server::send(std::span<const std::span<const std::byte>> iov) {
    // Build the record ONCE.  write_all_iov CONSUMES its iovec array (advances
    // base/len on partial writes), so each peer writes from a fresh COPY of
    // the pristine gather.  Lock order per the header contract:
    // peers_m_ → write_m_.
    const prefixed_iov_t rec(iov, tcp_transport_t::kMaxFrame);
    if (!rec.ok) return;
    const std::lock_guard plock(peers_m_);
    const std::lock_guard wlock(write_m_);
    std::array<::iovec, prefixed_iov_t::kMaxInlineIov + 1> scratch_inline;
    std::vector<::iovec> scratch_heap;
    ::iovec* scratch = scratch_inline.data();
    if (rec.n > scratch_inline.size()) {
        scratch_heap.resize(rec.n);
        scratch = scratch_heap.data();
    }
    for (const std::unique_ptr<session_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed)) continue;
        std::copy_n(rec.vec, rec.n, scratch);
        write_all_iov(s->fd.load(std::memory_order_relaxed), scratch, rec.n);
    }
}

void transport_tcp_server::peer_endpoint_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void transport_tcp_server::peer_endpoint_t::send(std::span<const std::span<const std::byte>> iov) {
    if (owner_ == nullptr || slot_ == nullptr) return;
    // Single consumer, so the record is written in place (no pristine copy —
    // unlike the broadcast).  NOT const: write_all_iov consumes it.
    prefixed_iov_t rec(iov, tcp_transport_t::kMaxFrame);
    if (!rec.ok) return;
    const std::lock_guard lock(owner_->write_m_);
    if (!slot_->open.load(std::memory_order_relaxed)) return;  // departed ⇒ no-op
    write_all_iov(slot_->fd.load(std::memory_order_relaxed), rec.vec, rec.n);
}

void transport_tcp_server::enumerate_peers(const peer_visitor_t& visit) const {
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && !s->name.empty()) visit(s->name);
}

transport_t* transport_tcp_server::peer_link(std::string_view peer) {
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && s->name == peer) return &s->endpoint;
    return nullptr;
}

bool transport_tcp_server::close_peer(std::string_view peer) {
    // Shutdown-only under the sender lock order (peers_m_ → write_m_); the
    // poll thread's next pass observes the close and runs the IDENTICAL
    // remote-FIN teardown — no duplicate logic, no off-thread framer touch.
    const std::lock_guard plock(peers_m_);
    for (const std::unique_ptr<session_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed) || s->name != peer) continue;
        const std::lock_guard wlock(write_m_);
        const int fd = s->fd.load(std::memory_order_relaxed);
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        return true;
    }
    return false;
}

void transport_tcp_server::accept_peer() {
    sockaddr_in remote{};
    socklen_t rlen = sizeof(remote);
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&remote), &rlen);
    if (fd < 0) return;

    session_t* slot = nullptr;
    {
        const std::lock_guard lock(peers_m_);
        for (const std::unique_ptr<session_t>& s : slots_)
            if (s->fd.load(std::memory_order_relaxed) < 0) {
                slot = s.get();
                break;
            }
        if (slot == nullptr) {
            if (max_peers_ != 0 && slots_.size() >= max_peers_) {
                ::close(fd);  // clean refusal at the deployment cap, not a hung SYN
                return;
            }
            slots_.push_back(std::make_unique<session_t>());
            slot = slots_.back().get();
            slot->endpoint.owner_ = this;
            slot->endpoint.slot_ = slot;
        }
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        slot->name = std::string(ip) + ':' + std::to_string(ntohs(remote.sin_port));
    }
    set_nodelay(fd);
    slot->framer.reset();
    // No handshake phase: the peer is open the moment it is accepted.  Publish
    // the fd LAST — the slot is now live for senders and the poll pass.
    slot->open.store(true, std::memory_order_relaxed);
    slot->fd.store(fd, std::memory_order_release);
}

void transport_tcp_server::service_peer(session_t& s) {
    const int fd = s.fd.load(std::memory_order_relaxed);
    if (fd < 0) return;
    std::array<std::byte, 4096> chunk;
    const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (n <= 0) {  // peer closed the TCP connection, or error
        teardown_slot(s);
        return;
    }

    // Feed the chunk through the slot's reassembler; each completed frame is
    // delivered inline.  Tier select per frame (receiver_slot.hpp): the
    // peer-named slot first (the ADR-0044 bus precedence — the router's
    // wiring), flat transport_t slot as the point-to-point fallback.
    const auto res = s.framer.feed(*backend_, max_frame_, chunk.data(), static_cast<std::size_t>(n),
                                   [this, &s](view::segment_ptr_t seg, std::size_t len) {
                                       view::view_t frame =
                                           view::view_t::over(std::move(seg)).subview(0, len);
                                       if (peer_rx_.has_any())
                                           peer_rx_.deliver(s.name, std::move(frame));
                                       else
                                           rx_.deliver(std::move(frame));
                                   });
    if (res.dropped != 0) dropped_rx_.fetch_add(res.dropped, std::memory_order_relaxed);
    if (res.malformed) {
        // Malformed (corrupt/hostile) or undeliverable: a desynced stream
        // cannot be re-framed — count it and tear this one peer down.
        malformed_rx_.fetch_add(1, std::memory_order_relaxed);
        teardown_slot(s);
    }
}

void transport_tcp_server::teardown_slot(session_t& s) {
    std::string departed;
    {
        // Stop peer_link/enumerate resolution FIRST, so no new sender targets
        // the dying slot by name.  Keep the name: it identifies the departed
        // session to the eviction seam below.
        const std::lock_guard lock(peers_m_);
        departed = std::move(s.name);
        s.name.clear();
    }
    int fd;
    bool was_open;
    {
        // The stream teardown-under-write-lock invariant, per slot: reset the
        // fd and the open flag under write_m_ BEFORE ::close, so an in-flight
        // send either finished against the still-open fd or observes the reset.
        const std::lock_guard lock(write_m_);
        was_open = s.open.load(std::memory_order_relaxed);
        s.open.store(false, std::memory_order_relaxed);
        fd = s.fd.exchange(-1, std::memory_order_acq_rel);
    }
    if (fd >= 0) ::close(fd);
    s.framer.reset();
    // Departure seam (RFC-0009 §D.5): fired LAST, with no transport lock held —
    // the notifier re-enters the routing plane.  Peer-named mode reports the
    // peer's own name; flat mode reports the whole link down.
    if (was_open && !departed.empty()) {
        if (peer_rx_.has_any())
            notify_peer_down(departed);
        else
            notify_down();
    }
}

void transport_tcp_server::run() {
    // ONE poll pass multiplexes the listen socket and every live peer — no
    // per-peer thread (the MCU-shaped choice, #362), bounded to 100 ms so the
    // loop stays shutdown-responsive (the posix_endpoint_t idiom).
    std::vector<pollfd> pfds;
    std::vector<session_t*> pslots;
    while (!stop_.load(std::memory_order_relaxed)) {
        pfds.clear();
        pslots.clear();
        pfds.push_back(pollfd{listen_fd_, POLLIN, 0});
        {
            const std::lock_guard lock(peers_m_);
            for (const std::unique_ptr<session_t>& s : slots_) {
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
        // Peers first (their events are bound to this pass's fd list), then the
        // accept (which may add a slot).
        for (std::size_t i = 1; i < pfds.size(); ++i)
            if ((pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) service_peer(*pslots[i - 1]);
        if ((pfds[0].revents & POLLIN) != 0) accept_peer();
    }
}

}  // namespace tr::net
