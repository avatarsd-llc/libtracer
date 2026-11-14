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
#include <atomic>
#include <cstring>
#include <mutex>
#include <span>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/frame_ingress.hpp"
#include "libtracer/iov_table.hpp"
#include "libtracer/length_prefix_framer.hpp"

namespace tr::net {

namespace {

/** @brief The u32-LE length prefix (transport framing) — the framer's, shared verbatim. */
constexpr std::size_t kPrefixBytes = length_prefix_framer_t::kPrefixBytes;

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
    iov_table_t<::iovec> table;
    ::iovec* vec = nullptr;
    std::size_t n = 0;
    bool ok = false;

    /**
     * @brief Assemble the record, overflowing into the SENDING LINK's egress store @p src
     *        (`transport_t::egress_source`, #873 family 1) rather than the process heap.
     */
    prefixed_iov_t(std::span<const std::span<const std::byte>> iov, std::size_t cap,
                   mem::block_source_t& src)
        : table(inline_vec, src) {
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

/**
 * @brief The u32-LE length prefix, as frame ingress calls it (`frame_ingress.hpp`): one set
 *        of rules for the dialled, the one-peer listening and the multi-peer server link.
 *
 * A length above `max_frame` is corrupt or hostile, and a desynced stream cannot re-frame,
 * so it closes the stream before anything is drawn and is counted as malformed. An empty
 * record carries no TLV and is skipped. A frame the receive source refused (exhausted, or
 * larger than any segment it produces) is backpressure: it was read past, and is dropped and
 * counted, never a disconnect for a peer that stayed inside the cap (#932). Every other frame
 * goes to @p Deliver as the one owning view over its exact segment, with no copy.
 */
template <class Deliver>
struct prefix_framing_t {
    std::size_t max_frame; /**< @brief The link's receive cap. */
    drop_tally_t<>& drops; /**< @brief The link's shed-frame counters. */
    Deliver deliver;       /**< @brief Hands one frame up. */

    /** @brief Judge the prefix bytes read so far. */
    [[nodiscard]] ingress_header_t header(std::span<const std::byte> have) const {
        if (have.size() < kPrefixBytes) return {ingress_header_t::kind_t::MORE, kPrefixBytes, 0};
        const std::size_t len = tr::detail::load_le<std::uint32_t>(have.first(kPrefixBytes));
        if (len > max_frame) {
            drops.count_malformed_rx();
            return {ingress_header_t::kind_t::CLOSE, 0, 0};
        }
        return {ingress_header_t::kind_t::BODY, kPrefixBytes, len};
    }

    /** @brief One whole frame, in its exact segment: empty for an empty record (skipped) or
     *         a refused draw (shed and counted). */
    [[nodiscard]] bool body(tr::view::rx_block_t& blk, std::size_t len) {
        if (!blk.seg) {
            drops.count_dropped_rx(static_cast<std::size_t>(len != 0));
            return true;
        }
        deliver(blk.take(len));
        return true;
    }
};

/** @brief A @ref prefix_framing_t over @p deliver. */
template <class Deliver>
prefix_framing_t<Deliver> prefix_framing(std::size_t max_frame, drop_tally_t<>& drops,
                                         Deliver deliver) {
    return {max_frame, drops, std::move(deliver)};
}

}  // namespace

tcp_transport_t::tcp_transport_t(std::string_view peer_host, std::uint16_t peer_port,
                                 const tcp_config_t& config)
    : stream_endpoint_t(config.memory.io_or_default(), true, config.wait),
      backend_(config.memory.rx),
      recv_stack_(config.recv_stack),
      stack_src_(&config.memory.stack_or_default()) {
    const std::size_t max_frame = config.max_frame;
    max_frame_ = length_prefix_framer_t::configured_cap(max_frame);  // tighten-only (#1035)
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(peer_port);
    if (!parse_ipv4(peer_host, peer.sin_addr) ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
        ::close(fd);
        return;
    }
    set_nodelay(fd);
    conn_fd_.store(fd, std::memory_order_relaxed);
    came_up_ = true;  // the ok() fact (#1059): the dial succeeded; liveness is link_up()
    // Two-phase bring-up (#1045): the socket goes on the wait in `start_receiving()`, never
    // here. A wait begun here could decode a frame the peer pushed on connect before the
    // owner installs a sink, and the empty slot would drop it with no counter moving.
}

bool tcp_transport_t::start_receiving() {
    // A LISTEN link began waiting in its constructor: its one `start` is spent.
    if (listen_) return true;
    const int fd = conn_fd_.load(std::memory_order_relaxed);
    if (fd < 0) return true;  // the dial failed: there is no socket to serve, and ok() says so
    // One-shot: `posix_endpoint_t::start` may be called at most once per endpoint, and this
    // is reachable both from the one-phase constructor and from an owner that calls it
    // unconditionally on every link it wires.
    if (recv_started_.exchange(true, std::memory_order_relaxed)) return true;
    // Write interest is armed for the first turn only when a send held a frame before now, so
    // the driving thread resumes it; otherwise the driver never writes until a send holds.
    if (start(recv_stack_, *stack_src_) &&
        attach(src_, fd, &tcp_transport_t::on_conn, peer_.wants_write()))
        return true;
    // A refused wait (#2288) is the connection dying before its first byte: closed, and
    // ok() false. The owner hears it from the return, not from a link-down fired under
    // whatever lock it holds while it wires the link.
    teardown_peer(fd);
    came_up_ = false;
    return false;
}

tcp_transport_t::tcp_transport_t(std::uint16_t bind_port, const tcp_config_t& config)
    // A listener: the peer stays closed until an accept opens it.
    : stream_endpoint_t(config.memory.io_or_default(), false, config.wait),
      listen_(true),
      backend_(config.memory.rx) {
    const std::size_t max_frame = config.max_frame;
    const std::size_t recv_stack = config.recv_stack;
    max_frame_ = length_prefix_framer_t::configured_cap(max_frame);  // tighten-only (#1035)
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

    // A refused wait (#2288) leaves the listener inert: ok() false.
    if (!start(recv_stack, config.memory.stack_or_default()) ||
        !attach(src_, listen_fd_, &tcp_transport_t::on_listen)) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

tcp_transport_t::~tcp_transport_t() {
    stop_and_join();  // FIRST: a callback touches the fds released below
    // A leftover peer fd (one never put on the wait) is closed by
    // ~stream_endpoint_t after this body; the listen socket is ours.
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void tcp_transport_t::shut_down() {
    shut_down_stream();
    // A callback in flight may still read the listen socket: it is only shut down here, and
    // the destructor closes it and frees its port.
    release_listen(listen_fd_);
}

void tcp_transport_t::send(std::span<const std::byte> frame) {
    // One span, same wire bytes (an empty frame is a prefix-only record either
    // way) — the gathered path is the one implementation.
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void tcp_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    send_record(iov, nullptr, {});
}

void tcp_transport_t::send(std::span<const std::span<const std::byte>> head,
                           const graph::value_t& value, const graph::delivery_lane_t& lane) {
    // The record's spans on the stack: a delivery's head is two or three spans and a value one
    // or two links, so an unusually wide one takes the base's general lowering (the copy path)
    // rather than growing a table here.
    std::array<std::span<const std::byte>, kMaxInlineIov> parts;
    const std::size_t n = retained_parts(parts, head, value);
    if (n > parts.size()) {
        transport_t::send(head, value, lane);
        return;
    }
    send_record(std::span<const std::span<const std::byte>>(parts.data(), n), &value, lane);
}

void tcp_transport_t::send_record(std::span<const std::span<const std::byte>> parts,
                                  const graph::value_t* value, const graph::delivery_lane_t& lane) {
    const prefixed_iov_t rec(parts, kMaxFrame, egress_source());
    // Oversize for the cap, or a refused gather store: shed it, but COUNT it (#932) —
    // an egress drop used to be a bare return no observer could see.
    if (!rec.ok) {
        drops_.count_dropped_tx();
        return;
    }
    // One egress (ADR-0087 §2): a publisher that finds the peer idle writes its record
    // straight from the caller's gather; one that finds a write in flight leaves ONE shared
    // frame in its vertex's ring and returns, never waiting on another publisher's write
    // (#1619). The frame copies the prefix and the head — header-sized — and keeps one
    // reference to the value. The writer never waits (#2252): what the socket did not take is
    // held and resumed on writability.
    egress_once_t once([&] { return frame_of(rec.span(), value); });
    egress_send(once, [&](int fd) { return write_gather(fd, rec.span()); }, lane);
}

void tcp_transport_t::on_conn(void* ctx, wait_source_t& self, std::uint8_t ready) {
    (void)self;
    auto& link = *static_cast<tcp_transport_t*>(static_cast<posix_endpoint_t*>(ctx));
    auto framing = prefix_framing(link.max_frame_, link.drops_, [&link](view::view_t frame) {
        link.rx_.deliver(std::move(frame));
    });
    if (link.peer_turn(
            ready, [&](int fd) { return link.ingress_.read(fd, *link.backend_, framing); },
            [&link] { return link.ingress_.mid_frame(); }))
        return;
    // Departure seam (RFC-0009 §D extended): the one connection died under us, so report the
    // link down, after the teardown and with no lock held. A listener waits for the next peer.
    link.depart();
    link.notify_down();
    if (link.listen_) link.relisten(link.listen_fd_, &tcp_transport_t::on_listen);
}

void tcp_transport_t::on_listen(void* ctx, wait_source_t& self, std::uint8_t ready) {
    (void)self;
    (void)ready;
    auto& link = *static_cast<tcp_transport_t*>(static_cast<posix_endpoint_t*>(ctx));
    // The one-peer accept/serve/teardown shape is stream_endpoint_t's; only the socket
    // options and the fresh ingress are this link's.
    link.accept_one(
        link.listen_fd_,
        [&link](int fd) {
            set_nodelay(fd);
            link.ingress_.reset();
            return true;
        },
        &tcp_transport_t::on_conn);
}

// ---------------------------------------------------------------------------
// tcp_server_transport_t — the multi-peer listener.  The slot/poll machinery is
// slot_server_t's (posix_endpoint.hpp, shared with ws_server_transport_t since
// #871); what lives here is the raw length-prefix stream framing.
// ---------------------------------------------------------------------------

/**
 * @brief One peer slot: the protocol-agnostic half (fd/open/name/endpoint,
 *        and the threading rule that governs them) is slot_server_t's; this
 *        adds the frame-ingress state and the directed facade object.
 *
 * The ingress state is the driving thread's alone, like every protocol buffer a slot carries.
 */
struct tcp_server_transport_t::session_t : slot_server_t::session_base_t {
    /** @brief Frame ingress (ADR-0087 §1, #2141): no stream buffer, the body lands in its
     *         segment. */
    frame_ingress_t<kPrefixBytes> ingress;
    peer_endpoint_t endpoint; /**< @brief The directed facade `peer_link` returns. */
};

tcp_server_transport_t::tcp_server_transport_t(std::uint16_t bind_port,
                                               const tcp_server_config_t& config)
    : stream_server_base_t(config.max_peers, config.peer_named, config.memory.io_or_default(),
                           config.memory.state_or_default(), config.wait),
      backend_(config.memory.rx) {
    max_frame_ = length_prefix_framer_t::configured_cap(config.max_frame);  // tighten-only (#1035)
    if (!bind_listen(bind_port)) return;
    // A refused poll-loop spawn (#2288) leaves the listener inert: ok() false.
    if (!serve(config.recv_stack, config.memory.stack_or_default())) close_listen();
}

tcp_server_transport_t::~tcp_server_transport_t() {
    // FIRST: the driving thread dispatches this class's variance points, so the link must
    // be off the wait (and a fallback thread joined) while the derived object is still whole.
    // ~slot_server_t closes the listen socket and sweeps the slot fds after this body.
    stop_and_join();
}

mem::poly_ptr_t<slot_server_t::session_base_t> tcp_server_transport_t::make_session() {
    mem::poly_ptr_t<session_t> slot = mem::make_poly<session_t>(slots_.source());
    if (!slot) return nullptr;
    slot->endpoint.slot_ = slot.get();
    return slot;
}

transport_t& tcp_server_transport_t::endpoint_of(session_base_t& s) noexcept {
    return static_cast<session_t&>(s).endpoint;
}

transport_drop_stats_t tcp_server_transport_t::peer_endpoint_t::drop_stats() const noexcept {
    return slot_ == nullptr ? transport_drop_stats_t{} : server_of(*slot_).drop_stats();
}

bool tcp_server_transport_t::on_accept(session_base_t& s, int fd) {
    set_nodelay(fd);
    static_cast<session_t&>(s).ingress.reset();
    // No handshake phase: the peer is open the moment it is accepted.
    return true;
}

void tcp_server_transport_t::on_slot_publishing() {
    if constexpr (graph::kFaultInjection) {
        if (detail::tcp_peer_publishing_hook != nullptr) detail::tcp_peer_publishing_hook();
    }
}

void tcp_server_transport_t::on_slot_reset(session_base_t& s) {
    static_cast<session_t&>(s).ingress.reset();
}

bool tcp_server_transport_t::mid_frame(const session_base_t& s) const {
    return static_cast<const session_t&>(s).ingress.mid_frame();
}

void tcp_server_transport_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void tcp_server_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    send_record(nullptr, iov, nullptr, {});
}

void tcp_server_transport_t::send(std::span<const std::span<const std::byte>> head,
                                  const graph::value_t& value, const graph::delivery_lane_t& lane) {
    send_retained(nullptr, head, value, lane);
}

void tcp_server_transport_t::send_retained(session_base_t* to,
                                           std::span<const std::span<const std::byte>> head,
                                           const graph::value_t& value,
                                           const graph::delivery_lane_t& lane) {
    std::array<std::span<const std::byte>, kMaxInlineIov> parts;
    const std::size_t n = retained_parts(parts, head, value);
    if (n > parts.size()) {  // too wide for the stack table: the base's copy path
        if (to == nullptr)
            transport_t::send(head, value, lane);
        else
            static_cast<session_t*>(to)->endpoint.transport_t::send(head, value, lane);
        return;
    }
    send_record(to, std::span<const std::span<const std::byte>>(parts.data(), n), &value, lane);
}

void tcp_server_transport_t::send_record(session_base_t* to,
                                         std::span<const std::span<const std::byte>> parts,
                                         const graph::value_t* value,
                                         const graph::delivery_lane_t& lane) {
    // Build the record ONCE, then hand the gather to the fan-out or to the one session: every
    // idle peer writes from it (read-only, #932), and a peer that has to wait gets one shared
    // frame in that vertex's ring.
    const prefixed_iov_t rec(parts, tcp_transport_t::kMaxFrame, egress_source());
    if (!rec.ok) {
        drops_.count_dropped_tx();  // shed, and counted (#932)
        return;
    }
    if (to == nullptr)
        broadcast(rec.span(), value, lane);
    else
        send_to(*to, rec.span(), value, lane);  // a departed session's egress is closed: shed
}

void tcp_server_transport_t::peer_endpoint_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void tcp_server_transport_t::peer_endpoint_t::send(
    std::span<const std::span<const std::byte>> iov) {
    if (slot_ != nullptr)
        static_cast<tcp_server_transport_t&>(server_of(*slot_))
            .send_record(slot_, iov, nullptr, {});
}

void tcp_server_transport_t::peer_endpoint_t::send(std::span<const std::span<const std::byte>> head,
                                                   const graph::value_t& value,
                                                   const graph::delivery_lane_t& lane) {
    if (slot_ != nullptr)
        static_cast<tcp_server_transport_t&>(server_of(*slot_))
            .send_retained(slot_, head, value, lane);
}

void tcp_server_transport_t::on_readable(session_base_t& base, int fd) {
    session_t& s = static_cast<session_t&>(base);
    // Tier select per frame: the constructed MODE picks the slot (#889), and `bus_mode()` is
    // that member conjoined with whether this BUILD carries a bus module at all (#375
    // deliverable 3), so the per-frame path reads a member and gains no dispatch (#1438). The
    // FLAT tier's WHO (#375 Part 2) is one 8-byte store, cleared once the frame is handed up,
    // so the field names a session only while its frame is being delivered (#1915).
    auto framing = prefix_framing(max_frame_, drops_, [&](view::view_t f) {
        delivering_ = s.handle;
        if (bus_mode())
            deliver_to_peer(s.handle, std::move(f));
        else
            rx_.deliver(std::move(f));
        delivering_ = {};
    });
    // Frame ingress reads each prefix first and each body straight into its exact segment
    // (ADR-0087 §1); false is a peer that closed, a read error, or a desynced stream.
    if (!s.ingress.read(fd, *backend_, framing)) teardown_slot(s);
}

}  // namespace tr::net
