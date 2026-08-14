/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_tcp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <memory>
#include <mutex>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/iov_table.hpp"
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
 *        stack array; only an unusually large gather falls back to the shared
 *        NOTHROW overflow store (`%iov_table.hpp`).  `ok` is false when the
 *        record exceeds @p cap — the peer would reject it as malformed — or
 *        when that store is exhausted, which is peer-reachable (a rope's link
 *        count is chosen by the sending peer) and answered by DROPPING the
 *        frame, never by truncating it (#848).  The prefix lives inside the
 *        struct, so the assembled iovec stays valid for the struct's (local)
 *        lifetime.
 *
 *        MEASURED (`bench_transport_iov`): the fallback fires at exactly **17
 *        caller spans** — `inline_vec` holds `kMaxInlineIov + 1` because the
 *        length prefix takes slot 0 — costing one allocation of ~288 B.
 *        `bench_forward_heap`'s `allocs=0` gate cannot see it: that bench drives
 *        a stub link which never assembles an iovec.  Headroom against
 *        `kFwdMaxIov` (9) is **8 regions**, and a rope source may split any
 *        region further.
 */
struct prefixed_iov_t {
    static constexpr std::size_t kMaxInlineIov = tr::net::kMaxInlineIov;
    std::array<std::byte, kPrefixBytes> prefix;
    std::array<::iovec, kMaxInlineIov + 1> inline_vec;
    iov_table_t<::iovec> table{inline_vec};
    ::iovec* vec = nullptr;
    std::size_t n = 0;
    bool ok = false;

    prefixed_iov_t(std::span<const std::span<const std::byte>> iov, std::size_t cap) {
        std::size_t total = 0;
        for (const std::span<const std::byte>& s : iov) total += s.size();
        if (total > cap) return;
        tr::detail::store_le(prefix, static_cast<std::uint32_t>(total));
        vec = table.acquire(iov.size() + 1);
        if (vec == nullptr) return;  // overflow store exhausted => ok stays false => DROP
        vec[0] = ::iovec{prefix.data(), prefix.size()};
        n = 1;
        for (const std::span<const std::byte>& s : iov) {
            if (s.empty()) continue;  // writev rejects nothing, but skip no-op entries
            vec[n++] = ::iovec{const_cast<std::byte*>(s.data()), s.size()};
        }
        ok = true;
    }

    /** @brief The assembled record as the read-only gather `write_all_iov` takes (#932). */
    [[nodiscard]] std::span<const ::iovec> span() const noexcept {
        return std::span<const ::iovec>(vec, n);
    }
};

}  // namespace

tcp_transport_t::tcp_transport_t(const std::string& peer_host, std::uint16_t peer_port,
                                 mem::mem_backend_t* backend, std::size_t max_frame,
                                 std::size_t recv_stack, bool defer_recv)
    : backend_(backend), recv_stack_(recv_stack) {
    max_frame_ = length_prefix_framer::configured_cap(max_frame);  // tighten-only (#1035)
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
    came_up_ = true;  // the ok() fact (#1059): the dial succeeded; liveness is link_up()
    // Two-phase bring-up (#1045). Spawning the recv thread HERE is the historical shape and
    // stays the default, but it makes the base's "install the sinks before frames flow"
    // contract unsatisfiable: this thread can decode and deliver a frame the peer pushed on
    // connect before the caller's next statement installs a sink, and the empty slot drops it
    // silently — no dropped_rx(), no malformed_rx(). `defer_recv` hands that ordering back to
    // the owner: the socket is up and its bytes are left on it until `start_receiving()`.
    //
    // Qualified deliberately (the transport_ws_client spelling): dispatching a virtual from a
    // constructor would reach THIS class's override anyway (the derived part does not exist
    // yet), so spelling it out is the honest form — a subclass cannot substitute its own
    // bring-up here, and nothing in this call should look as if it could.
    if (!defer_recv) tcp_transport_t::start_receiving();
}

void tcp_transport_t::start_receiving() {
    // A LISTEN link's accept loop IS its one `start`, spawned in its constructor. Without
    // this arm an owner arming every link it wires would spawn a SECOND thread onto an
    // already-accepted peer's fd — two `serve` loops splitting one stream, over a `body_`
    // the second `start` reassigns while the first is running it.
    if (listen_) return;
    const int fd = conn_fd_.load(std::memory_order_relaxed);
    if (fd < 0) return;  // the dial failed: there is no socket to serve
    // One-shot: `posix_endpoint_t::start` may be called at most once per endpoint, and this
    // is reachable both from the one-phase constructor and from an owner that calls it
    // unconditionally on every link it wires.
    if (recv_started_.exchange(true, std::memory_order_relaxed)) return;
    start(
        [this, fd] {
            serve(fd);
            teardown_peer(fd);  // reset-under-write_m_ then close (stream_endpoint_t)
            // Departure seam (RFC-0009 §D extended): the one connection died under us —
            // not a local stop — so report the link down (no locks held here).
            if (!stop_.load(std::memory_order_relaxed)) notify_down();
        },
        recv_stack_);
}

tcp_transport_t::tcp_transport_t(std::uint16_t bind_port, mem::mem_backend_t* backend,
                                 std::size_t max_frame, std::size_t recv_stack)
    : listen_(true), backend_(backend) {
    max_frame_ = length_prefix_framer::configured_cap(max_frame);  // tighten-only (#1035)
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

    start([this] { run_listen(); }, recv_stack);
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
    const prefixed_iov_t rec(iov, kMaxFrame);
    // Oversize for the cap, or a refused gather store: shed it, but COUNT it (#932) —
    // an egress drop used to be a bare return no observer could see.
    if (!rec.ok) {
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Hold write_m_ across the whole write so (a) the recv thread cannot close and
    // reset conn_fd_ underneath us, and (b) two senders can never interleave their
    // length-prefixed records on the stream; read the fd inside the lock to pair
    // with the teardown.
    const std::lock_guard lock(write_m_);
    const int fd = conn_fd_.load(std::memory_order_relaxed);
    if (fd < 0) {  // no live peer (still dialing, or torn down) => a counted drop
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    write_all_iov(fd, rec.span());
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
        const std::size_t len = tr::detail::load_le<std::uint32_t>(prefix);

        // The framing rules (empty record, over the protocol cap ⇒ malformed,
        // undeliverable ⇒ backpressure drain) live in length_prefix_framer — one
        // home shared with the chunk-fed transports (quic/webtransport). Only the
        // byte source differs: this pull-mode loop reads the body straight off
        // the socket into the accepted segment (ADR-0042 §2/§4 — no library
        // buffer, no copy; feeding recv chunks through feed() would add one).
        using kind_t = length_prefix_framer::prefix_decision_t::kind_t;
        auto dec = length_prefix_framer::on_prefix(*backend_, max_frame_, len);
        if (dec.kind == kind_t::EMPTY) continue;  // an empty record carries no TLV — a no-op
        if (dec.kind == kind_t::MALFORMED) {
            // Beyond the protocol cap (corrupt/hostile): count it and tear the
            // connection down — a desynced stream can't re-frame.
            malformed_rx_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (dec.kind == kind_t::DROP) {
            // Undeliverable (exhausted backend, or a frame larger than any segment
            // it produces) is backpressure: drain the frame off the stream (framing
            // sync survives), drop it, tick the counter — never an OOM, and never a
            // disconnect for a peer that stayed inside the protocol cap (#932).
            dropped_rx_.fetch_add(1, std::memory_order_relaxed);
            if (!drain(fd, len)) return;
            continue;
        }

        view::segment_ptr_t seg = std::move(dec.seg);
        if (!read_exact(fd, seg->bytes.data(), len)) return;

        // Tier select lives in the slot (receiver_slot.hpp): the rope sink gets the
        // frame OWNING, narrowed by aggregate init (over().subview() would copy the
        // handle, leaving a 2nd ref live across the callback, #845); span sink borrows.
        rx_.deliver(view::view_t{std::move(seg), 0, len});
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
// transport_tcp_server — the multi-peer listener.  The slot/poll machinery is
// slot_server_t's (posix_endpoint.hpp, shared with transport_ws_server since
// #871); what lives here is the raw length-prefix stream framing.
// ---------------------------------------------------------------------------

/**
 * @brief One peer slot: the protocol-agnostic half (fd/open/name/endpoint,
 *        and the threading rule that governs them) is slot_server_t's; this
 *        adds the per-stream framer and the directed facade object.
 *
 * The framer is poll-thread-only, like every protocol buffer a slot carries.
 */
struct transport_tcp_server::session_t : slot_server_t::session_base_t {
    length_prefix_framer framer; /**< @brief Per-stream u32-LE frame reassembly. */
    peer_endpoint_t endpoint;    /**< @brief The directed facade `peer_link` returns. */
};

transport_tcp_server::transport_tcp_server(std::uint16_t bind_port, mem::mem_backend_t* backend,
                                           std::size_t max_frame, std::size_t max_peers,
                                           bool peer_named, std::size_t recv_stack)
    : slot_server_t(max_peers, peer_named), backend_(backend) {
    max_frame_ = length_prefix_framer::configured_cap(max_frame);  // tighten-only (#1035)
    if (!bind_listen(bind_port)) return;
    start([this] { run(); }, recv_stack);
}

transport_tcp_server::~transport_tcp_server() {
    // FIRST: the run() thread dispatches this class's variance points, so it must be
    // joined while the derived object is still whole.  ~slot_server_t closes the listen
    // socket and sweeps the slot fds after this body.
    stop_and_join();
}

std::unique_ptr<slot_server_t::session_base_t> transport_tcp_server::make_session() {
    auto slot = std::make_unique<session_t>();
    slot->endpoint.owner_ = this;
    slot->endpoint.slot_ = slot.get();
    slot->peer_endpoint = &slot->endpoint;
    return slot;
}

bool transport_tcp_server::on_accept(session_base_t& s, int fd) {
    set_nodelay(fd);
    static_cast<session_t&>(s).framer.reset();
    // No handshake phase: the peer is open the moment it is accepted.
    return true;
}

void transport_tcp_server::on_slot_publishing() {
    if (detail::tcp_peer_publishing_hook != nullptr) detail::tcp_peer_publishing_hook();
}

void transport_tcp_server::on_slot_reset(session_base_t& s) {
    static_cast<session_t&>(s).framer.reset();
}

void transport_tcp_server::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void transport_tcp_server::send(std::span<const std::span<const std::byte>> iov) {
    // Build the record ONCE, then hand the gather to the shared fan-out, which writes
    // it to every peer under the header lock order (peers_m_ → write_m_) — the gather
    // is read-only, so no per-peer copy is taken (#932).
    const prefixed_iov_t rec(iov, tcp_transport_t::kMaxFrame);
    if (!rec.ok) {
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);  // shed, and counted (#932)
        return;
    }
    broadcast_iov(rec.span());
}

void transport_tcp_server::peer_endpoint_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void transport_tcp_server::peer_endpoint_t::send(std::span<const std::span<const std::byte>> iov) {
    if (owner_ == nullptr || slot_ == nullptr) return;
    // The single-fd twin of the broadcast override: one gathered record, one peer.
    const prefixed_iov_t rec(iov, tcp_transport_t::kMaxFrame);
    if (!rec.ok) {
        owner_->dropped_tx_.fetch_add(1, std::memory_order_relaxed);  // shed, counted (#932)
        return;
    }
    const std::lock_guard lock(owner_->write_m_);
    if (!slot_->open.load(std::memory_order_relaxed)) {  // departed ⇒ a counted drop
        owner_->dropped_tx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    write_all_iov(slot_->fd.load(std::memory_order_relaxed), rec.span());
}

void transport_tcp_server::on_readable(session_base_t& base, const std::byte* data,
                                       std::size_t len) {
    session_t& s = static_cast<session_t&>(base);
    // Feed the chunk through the slot's reassembler; each completed frame is
    // delivered inline.  Tier select per frame: the constructed MODE picks the
    // slot (#889) — peer-named servers deliver tagged with the sending peer's
    // name (the ADR-0044 bus precedence, what the router wires), flat servers
    // deliver point-to-point under the link's own registered name.  Reading the
    // stored flag, not `peer_rx_.has_any()`: a mode is a wiring-time fact, not a
    // per-frame consequence of which sink someone happened to install — and this
    // reads a member instead of taking the slot's mutex once per frame.
    const auto res = s.framer.feed(
        *backend_, max_frame_, data, len,
        [this, &s](view::segment_ptr_t seg, std::size_t flen) {
            // aggregate init — no handle copy (#845)
            view::view_t frame{std::move(seg), 0, flen};
            if (peer_named_)
                peer_rx_.deliver(s.name, std::move(frame));
            else
                rx_.deliver(std::move(frame));
        },
        // At decision time, so a frame delivered later in this same chunk cannot reach
        // an observer before the drop that preceded it is counted (#1255).
        [this] { dropped_rx_.fetch_add(1, std::memory_order_relaxed); });
    if (res.malformed) {
        // Malformed (corrupt/hostile) or undeliverable: a desynced stream
        // cannot be re-framed — count it and tear this one peer down.
        malformed_rx_.fetch_add(1, std::memory_order_relaxed);
        teardown_slot(s);
    }
}

}  // namespace tr::net
