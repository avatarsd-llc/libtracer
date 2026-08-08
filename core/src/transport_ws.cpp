/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_ws.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/iov_table.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/ws.hpp"

namespace tr::net {

namespace {

/** @brief What feeding one data frame to @ref ws_assembler_t produced. */
enum class assemble_status_t : std::uint8_t {
    PENDING,  /**< @brief Mid-message (or a stray CONT) — nothing to deliver yet. */
    COMPLETE, /**< @brief @ref assemble_result_t::message holds the finished message. */
    DROPPED,  /**< @brief The RX backend refused a fragment — the message is shed
                          (backpressure); the connection lives and re-syncs at the next
                          BINARY. Count it as `dropped_rx`. */
    OVERSIZE, /**< @brief The reassembled message grew past the receive cap — FAIL the
                          connection (§7.1.7) and count it as `malformed_rx`. */
};

/** @brief One @ref ws_assembler_t::on_data outcome: a status, and (on `COMPLETE`) the rope. */
struct assemble_result_t {
    assemble_status_t status = assemble_status_t::PENDING; /**< @brief Which outcome. */
    tr::view::rope_t message; /**< @brief The finished message — meaningful only on `COMPLETE`. */
};

/**
 * @brief RFC 6455 fragmented-message reassembly as ROPE CHAINING (ADR-0053 §5): each data fragment
 *        becomes one owning link (the copy out of the reused connection buffer is the legitimate
 *        substrate-boundary copy; chaining the fragments is pointer-linking, never a memcpy).
 *
 * One assembler per connection,
 * used only on its recv thread.
 */
struct ws_assembler_t {
    tr::view::rope_t partial;
    bool assembling = false;

    void reset() {
        partial = tr::view::rope_t{};
        assembling = false;
    }

    /**
     * @brief Feed one data frame (BINARY or CONT).
     *
     * Returns the completed message as a
     * rope when this frame finishes one, `PENDING` otherwise. A BINARY that
     * arrives mid-assembly is an RFC 6455 protocol error: the stale assembly is
     * dropped and the new message starts. A stray CONT (no assembly open) is
     * dropped. An allocation failure drops the whole message (backpressure).
     *
     * @param backend Where each fragment's owning link is drawn from (ADR-0042 §2) — the
     *                transport's injected seam, never the global heap, so a bounded host
     *                bounds reassembly by its own pool.
     * @param cap     The effective receive cap the REASSEMBLED total may not exceed. The
     *                per-frame header check bounds one fragment; without this one a peer
     *                walks past the cap 125 bytes at a time across a million CONTs (#872).
     */
    assemble_result_t on_data(ws::opcode_t op, bool fin, std::span<const std::byte> payload,
                              mem::mem_backend_t& backend, std::size_t cap) {
        if (op == ws::opcode_t::BINARY && assembling) reset();   // protocol error
        if (op == ws::opcode_t::CONT && !assembling) return {};  // stray

        // Checked BEFORE the fragment is copied, and written as a subtraction because
        // `total_length() + payload.size()` can wrap. `total_length() <= cap` holds by
        // induction (every earlier fragment passed this same test), so the difference is
        // well-defined.
        if (payload.size() > cap - partial.total_length()) {
            reset();
            return {assemble_status_t::OVERSIZE, {}};
        }

        const std::optional<tr::view::view_t> link = tr::view::over_bytes(payload, backend);
        if (!link) {  // backend refused => drop the message (backpressure)
            reset();
            return {assemble_status_t::DROPPED, {}};
        }
        partial.append(*link);
        assembling = true;
        if (!fin) return {};
        assemble_result_t done{assemble_status_t::COMPLETE, std::move(partial)};
        reset();
        return done;
    }
};

}  // namespace

namespace {

/**
 * @brief Case-insensitive search for an HTTP header and return its trimmed value.
 *
 * `request` is the raw header block; `name` is lowercase (e.g. "sec-websocket-key").
 */
std::string header_value(std::string_view request, std::string_view name) {
    std::string lower(request);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t pos = 0;
    while ((pos = lower.find(name, pos)) != std::string::npos) {
        // Must sit at the start of a header line (start of buffer or after a newline).
        if (pos != 0 && request[pos - 1] != '\n') {
            pos += name.size();
            continue;
        }
        std::size_t colon = request.find(':', pos + name.size());
        if (colon == std::string_view::npos) return {};
        std::size_t eol = request.find("\r\n", colon);
        if (eol == std::string_view::npos) eol = request.size();
        std::string_view val = request.substr(colon + 1, eol - colon - 1);
        std::size_t b = val.find_first_not_of(" \t");
        if (b == std::string_view::npos) return {};
        std::size_t e = val.find_last_not_of(" \t");
        return std::string(val.substr(b, e - b + 1));
    }
    return {};
}

/** @brief Inline iovec capacity for a scatter-gather send before the overflow store:
 *         a FWD forward/reply gathers only a few spans, so the common broadcast
 *         never allocates (the shared `%iov_table.hpp` bound, as tcp/udp). */
constexpr std::size_t kMaxServerIov = kMaxInlineIov;

/**
 * @brief Build the scatter-gather iovec for one server→client frame: entry 0 is the
 *        pre-encoded frame @p header (its first @p header_len bytes), the payload
 *        @p iov spans follow (empty spans skipped).
 *
 * Server frames are UNMASKED (RFC 6455 §5.1), so the payload bytes ride straight to
 * the wire with no copy. The entry storage comes from @p table — the caller's stack array
 * while the span count fits, a NOTHROW overflow block when it does not; @p table and
 * @p header must outlive the write, since the returned iovec aliases them.
 *
 * @param table      The entry store (its inline array is the no-allocation fast path).
 * @param header     The already-encoded frame header buffer (its bytes are aliased).
 * @param header_len The number of valid header bytes in @p header.
 * @param iov        The payload spans to gather after the header.
 * @retval {nullptr, 0} The overflow store refused — the caller DROPS the frame. The entry
 *                      count is `link count x region count`, chosen by the SENDING peer
 *                      (`fwd_router.cpp`'s rope arm), so this is peer-reachable; a
 *                      truncated frame on the wire would be worse than none (#848).
 * @return `{vec, count}` — the assembled iovec array and its entry count.
 */
std::pair<::iovec*, std::size_t> build_server_iov(
    iov_table_t<::iovec>& table, std::array<std::byte, ws::kMaxServerFrameHeader>& header,
    std::size_t header_len, std::span<const std::span<const std::byte>> iov) {
    ::iovec* vec = table.acquire(iov.size() + 1);
    if (vec == nullptr) return {nullptr, 0};
    vec[0] = ::iovec{header.data(), header_len};
    std::size_t n = 1;
    for (const std::span<const std::byte>& s : iov) {
        if (s.empty()) continue;  // an empty span is a no-op gather entry
        vec[n++] = ::iovec{const_cast<std::byte*>(s.data()), s.size()};
    }
    return {vec, n};
}

}  // namespace

/**
 * @brief One peer slot: the connection state of a single inbound WebSocket client.
 *
 * Slots are recycled in place across connections (the header's bounded-steady-state
 * contract) and never freed before the server, so the @ref endpoint facade
 * `peer_link` hands out stays pointer-valid for the server's lifetime. Threading, ONE
 * rule for every half of the lifecycle: @ref fd / @ref open are atomics MUTATED only
 * under `write_m_` — accept publishes them, the 101 flips `open`, teardown resets them
 * — and read by senders under that same lock, so a sender never sees a half-published
 * slot; the destructor's closing sweep is the single mutation outside it, and runs
 * after the recv thread is joined. @ref name is guarded by `peers_m_` (cross-thread
 * reads from `peer_link`/`enumerate_peers`, which read @ref open too and never touch
 * the fd); the buffers and the assembler are recv-thread-only. Every access to the two
 * atomics is `relaxed`: the lock, not the memory order, is what orders them.
 */
struct transport_ws_server::session_t {
    std::atomic<int> fd{-1};       /**< @brief The peer socket; -1 ⇒ free slot. */
    std::atomic<bool> open{false}; /**< @brief True once the 101 handshake completed. */
    /** @brief The peer's routable NAME, `p<slot>` — a pure function of the slot index
     *         (ADR-0073 §2), stamped at accept and moved out by teardown (the eviction
     *         seam), so a reused slot gets the SAME name back. A legal path segment,
     *         which `<ip>:<port>` (two reserved characters) never was — that name was
     *         enumerable but unaddressable and tainted every accumulated return route
     *         (#426). It identifies a SESSION, not a device: a reconnecting peer may
     *         land in a different slot. Device-stable identity is a named link
     *         (RFC-0014). */
    std::string name;
    /** @brief The remote `<ip>:<port>` — DIAGNOSTIC only, never a name and never in the
     *         graph (#584 owns any future per-peer facet). Refreshed per accept. */
    std::string endpoint_str;
    std::string hs_buf;         /**< @brief HTTP Upgrade request accumulation. */
    std::vector<std::byte> buf; /**< @brief Stream bytes → frame reassembly. */
    ws_assembler_t assembler;   /**< @brief RFC 6455 fragment reassembly. */
    peer_endpoint_t endpoint;   /**< @brief The directed facade `peer_link` returns. */
};

transport_ws_server::transport_ws_server(std::uint16_t bind_port, mem::mem_backend_t* backend,
                                         std::size_t max_frame, std::size_t max_peers,
                                         bool peer_named, std::size_t recv_stack)
    : max_peers_(max_peers), peer_named_(peer_named), backend_(backend) {
    if (max_frame != 0) max_frame_ = max_frame;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bind_port);
    // SOMAXCONN: the OS's own accept-queue bound — admission is per-connection in
    // accept_peer (the max_peers deployment cap), never a synthetic backlog of 1.
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

    start([this] { run(); }, recv_stack);
}

transport_ws_server::~transport_ws_server() {
    stop_and_join();  // FIRST: the run() thread touches the fds closed below
    if (listen_fd_ >= 0) ::close(listen_fd_);
    for (const std::unique_ptr<session_t>& s : slots_) {  // thread joined — nothing races
        const int fd = s->fd.exchange(-1, std::memory_order_relaxed);
        if (fd >= 0) ::close(fd);
    }
}

void transport_ws_server::send(std::span<const std::byte> frame) {
    // One span, same wire bytes — the gathered path is the ONE implementation (the
    // tcp_transport_t::send idiom). A server frame is UNMASKED (RFC 6455 §5.1), so the
    // caller's payload rides to the wire untouched behind a stack-built header: this
    // removes the per-send `std::vector` (a peer-reachable abort() under -fno-exceptions,
    // #848) AND a full payload memcpy, at the same one writev per peer.
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void transport_ws_server::send(std::span<const std::span<const std::byte>> iov) {
    // Encode the ONE server frame header for the whole gathered payload, then fan
    // [header, span0, span1, ...] to every open peer as a single gathered write —
    // no flatten, no re-copy (server frames are UNMASKED, RFC 6455 §5.1). Lock
    // order per the header contract: peers_m_ (slot list stable) → write_m_ (the
    // stream write-serialization invariant, now covering every peer fd).
    std::size_t total = 0;
    for (const std::span<const std::byte>& s : iov) total += s.size();
    std::array<std::byte, ws::kMaxServerFrameHeader> header{};
    const std::size_t hlen = ws::encode_frame_header(header, ws::opcode_t::BINARY, total);

    std::array<::iovec, kMaxServerIov + 1> inline_vec;
    iov_table_t<::iovec> table(inline_vec);
    const auto [pristine, n] = build_server_iov(table, header, hlen, iov);
    if (pristine == nullptr) return;  // gather store exhausted => drop the frame

    // write_all_iov CONSUMES its iovec array (advances base/len on partial writes),
    // so each peer must write from a fresh COPY of the pristine gather — otherwise
    // peer 2+ would writev a consumed/zeroed array. peer_endpoint_t::send is
    // single-fd and needs no such copy.
    std::array<::iovec, kMaxServerIov + 1> scratch_inline;
    iov_table_t<::iovec> scratch_table(scratch_inline);
    ::iovec* scratch = scratch_table.acquire(n);
    if (scratch == nullptr) return;  // same store, same answer: drop, never truncate

    const std::lock_guard plock(peers_m_);
    const std::lock_guard wlock(write_m_);
    for (const std::unique_ptr<session_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed)) continue;
        std::copy_n(pristine, n, scratch);
        write_all_iov(s->fd.load(std::memory_order_relaxed), scratch, n);
    }
}

void transport_ws_server::peer_endpoint_t::send(std::span<const std::byte> frame) {
    // One span through the gathered path — the same delegation as the broadcast override
    // (#848): no per-send vector, no payload copy.
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void transport_ws_server::peer_endpoint_t::send(std::span<const std::span<const std::byte>> iov) {
    if (owner_ == nullptr || slot_ == nullptr) return;
    // The single-fd twin of the broadcast override: one gathered [header, spans...]
    // write, server frame UNMASKED (RFC 6455 §5.1), no flatten copy.
    std::size_t total = 0;
    for (const std::span<const std::byte>& s : iov) total += s.size();
    std::array<std::byte, ws::kMaxServerFrameHeader> header{};
    const std::size_t hlen = ws::encode_frame_header(header, ws::opcode_t::BINARY, total);

    std::array<::iovec, kMaxServerIov + 1> inline_vec;
    iov_table_t<::iovec> table(inline_vec);
    const auto [vec, n] = build_server_iov(table, header, hlen, iov);
    if (vec == nullptr) return;  // gather store exhausted => drop the frame

    // Single consumer, so no pristine copy is needed (unlike the broadcast).
    const std::lock_guard lock(owner_->write_m_);
    if (!slot_->open.load(std::memory_order_relaxed)) return;  // departed ⇒ no-op
    write_all_iov(slot_->fd.load(std::memory_order_relaxed), vec, n);
}

void transport_ws_server::enumerate_peers(const peer_visitor_t& visit) const {
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && !s->name.empty()) visit(s->name);
}

transport_t* transport_ws_server::peer_link(std::string_view peer) {
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && s->name == peer) return &s->endpoint;
    return nullptr;
}

bool transport_ws_server::close_peer(std::string_view peer) {
    // Find the named open peer under the same lock order senders use
    // (peers_m_ → write_m_), then ::shutdown its socket. We deliberately do NOT
    // call teardown_slot here: it clears the recv-thread-only buffers/assembler,
    // so it must run only on the recv thread. The shutdown wakes that thread's
    // next poll, ::recv returns 0, and the IDENTICAL remote-FIN path recycles the
    // slot — no duplicate teardown logic, no off-thread buffer touch.
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

void transport_ws_server::accept_peer() {
    sockaddr_in remote{};
    socklen_t rlen = sizeof(remote);
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&remote), &rlen);
    if (fd < 0) return;

    session_t* slot = nullptr;
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
            slots_.push_back(std::make_unique<session_t>());
            slot = slots_.back().get();
            slot->endpoint.owner_ = this;
            slot->endpoint.slot_ = slot;
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
    slot->hs_buf.clear();
    slot->buf.clear();
    slot->assembler.reset();
    // The accept-side half of the slot rule (#891): `open`/`fd` are mutated only under
    // write_m_ — the lock teardown_slot resets them under, and the lock `service_peer` holds
    // when it flips `open` true past the 101.  A WS slot is published NOT-open (its 101 has
    // not been written yet), so the hold is what keeps the pair one step for a broadcast
    // holding write_m_, not the order of the two stores.  Accept is cold; relaxed stores
    // because the lock is what orders them.
    {
        const std::lock_guard lock(write_m_);
        slot->open.store(false, std::memory_order_relaxed);
        slot->fd.store(fd, std::memory_order_relaxed);
    }
}

void transport_ws_server::service_peer(session_t& s) {
    const int fd = s.fd.load(std::memory_order_relaxed);
    if (fd < 0) return;
    std::array<std::byte, 4096> chunk;
    const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (n <= 0) {  // peer closed the TCP connection, or error
        teardown_slot(s);
        return;
    }

    if (!s.open.load(std::memory_order_relaxed)) {
        // Opening handshake: accumulate the HTTP Upgrade request until CRLFCRLF.
        s.hs_buf.append(reinterpret_cast<const char*>(chunk.data()), static_cast<std::size_t>(n));
        if (s.hs_buf.size() > 16384) {  // runaway request guard
            teardown_slot(s);
            return;
        }
        const std::size_t hdr_end = s.hs_buf.find("\r\n\r\n");
        if (hdr_end == std::string::npos) return;  // keep accumulating

        const std::string key =
            header_value(std::string_view(s.hs_buf.data(), hdr_end + 4), "sec-websocket-key");
        if (key.empty()) {
            teardown_slot(s);
            return;
        }
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
        resp += ws::accept_key(key);
        resp += "\r\n\r\n";
        std::vector<std::byte> bytes(resp.size());
        for (std::size_t i = 0; i < resp.size(); ++i) bytes[i] = static_cast<std::byte>(resp[i]);
        {
            const std::lock_guard lock(write_m_);
            write_all(fd, bytes);
            // Publish the slot as OPEN while still holding the write lock, so the
            // "101 is on the wire" and "senders may use this slot" transitions are one
            // step. Storing it after the lock is released opens a window in which the peer
            // has already read `101 Switching Protocols` — and so believes the connection
            // is up — while every send still skips the slot as not-open and the frame goes
            // nowhere. A sender can only reach the fd through this same lock, so taking the
            // store inside it means anyone who could observe the response also observes the
            // slot as open. (`open` must NOT move ahead of the response write instead: a
            // concurrent send would then be free to put a BINARY frame on the wire in front
            // of the handshake reply.)  Relaxed: the lock is what orders this store.
            s.open.store(true, std::memory_order_relaxed);
        }
        // The seam that makes the line above testable: here the response is out AND the slot
        // is published, so a send that lands in this instant must reach the peer. Move the
        // store below this point and the same probe reads an empty socket — that is the
        // regression this hook exists to redden (`ws_transport_test`, the handshake race).
        if (detail::ws_peer_published_hook != nullptr) detail::ws_peer_published_hook();
        // Bytes pipelined past the header are the start of the frame stream —
        // the old one-peer server dropped them; carry them over.
        const auto* rest = reinterpret_cast<const std::byte*>(s.hs_buf.data()) + hdr_end + 4;
        s.buf.assign(rest, rest + (s.hs_buf.size() - hdr_end - 4));
        s.hs_buf.clear();
        s.hs_buf.shrink_to_fit();
        if (!drain_frames(s)) teardown_slot(s);
        return;
    }

    s.buf.insert(s.buf.end(), chunk.data(), chunk.data() + n);
    if (!drain_frames(s)) teardown_slot(s);
}

bool transport_ws_server::drain_frames(session_t& s) {
    // Drain every complete frame currently buffered; leftover partial bytes stay
    // for the next read. Returns false when the peer sent CLOSE — or broke RFC 6455,
    // which fails the connection through the exact same teardown (§7.1.7).
    //
    // The cap resolves from the two INJECTED resources on every pass (one virtual call, the
    // tcp_transport_t::serve idiom) rather than being cached: it is what stops `s.buf` from
    // following a declared 64-bit length, so it must be the live answer, not a snapshot.
    const std::size_t cap = length_prefix_framer::effective_cap(*backend_, max_frame_);
    while (true) {
        ws::decode_result_t decoded = ws::decode_frame_checked(s.buf, cap);
        if (decoded.status == ws::decode_status_t::NEED_MORE) return true;
        if (decoded.status == ws::decode_status_t::PROTOCOL_ERROR) {
            // An over-cap declared length or a §5.5 control breach: a stream we refuse to
            // keep reading. Count it, then tear down through the one teardown path.
            malformed_rx_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ws::frame_t frame = std::move(decoded.frame);
        s.buf.erase(s.buf.begin(), s.buf.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));

        switch (frame.op) {
            case ws::opcode_t::BINARY:
            case ws::opcode_t::CONT: {
                // Peer-named slot first (the ADR-0044 bus precedence — the router's
                // wiring), flat transport_t slot as the point-to-point fallback.
                // Tier select per frame (receiver_slot.hpp), so a sink installed
                // mid-stream takes effect on the next data frame. Unfragmented
                // fast path on the span tier: borrowed payload, no owning copy.
                const bool peer_named = peer_rx_.has_any();
                const bool want_rope = peer_named ? peer_rx_.has_rope() : rx_.has_rope();
                if (frame.op == ws::opcode_t::BINARY && frame.fin && !s.assembler.assembling &&
                    !want_rope) {
                    const std::span<const std::byte> payload(frame.payload);
                    if (peer_named)
                        peer_rx_.deliver_borrowed(s.name, payload);
                    else
                        rx_.deliver_borrowed(payload);
                    break;
                }
                auto msg = s.assembler.on_data(frame.op, frame.fin, frame.payload, *backend_, cap);
                if (msg.status == assemble_status_t::DROPPED) {
                    // Backpressure: the message is shed, the connection lives.
                    dropped_rx_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (msg.status == assemble_status_t::OVERSIZE) {
                    // Fragments summing past the cap — the same answer a single over-cap
                    // frame gets, so the fragmented route is not a way around the bound.
                    malformed_rx_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                if (msg.status != assemble_status_t::COMPLETE) break;  // mid-message
                // The reassembled message IS a rope — one owning link per
                // fragment (ADR-0053 §5): the rope sink takes it as-is; only
                // a span-only sink pays the one materialize (in the slot).
                if (peer_named)
                    peer_rx_.deliver_rope(s.name, std::move(msg.message));
                else
                    rx_.deliver_rope(std::move(msg.message));
                break;
            }
            case ws::opcode_t::PING: {
                // Built on the STACK: decode_frame_checked has already enforced RFC 6455
                // §5.5, so the payload is <= 125 bytes and the reply cannot fail to be
                // built. There is deliberately no drop policy here — an unanswered PING
                // entitles the peer to fail the connection (§5.5.2/§5.5.3), so a heap blip
                // must not be able to cost the link (#848).
                std::array<std::byte, ws::kMaxServerControlFrame> pong{};
                const std::size_t plen =
                    ws::encode_server_control(pong, ws::opcode_t::PONG, frame.payload);
                const std::lock_guard lock(write_m_);
                write_all(s.fd.load(std::memory_order_relaxed),
                          std::span<const std::byte>(pong.data(), plen));
                break;
            }
            case ws::opcode_t::CLOSE:
                return false;  // tear this one connection down
            default:
                break;  // TEXT / PONG: ignored
        }
    }
}

void transport_ws_server::teardown_slot(session_t& s) {
    std::string departed;
    {
        // Stop peer_link/enumerate resolution FIRST, so no new sender targets
        // the dying slot by name. Keep the name: it identifies the departed
        // session to the eviction seam below.
        const std::lock_guard lock(peers_m_);
        departed = std::move(s.name);
        s.name.clear();
    }
    int fd;
    bool was_open;
    {
        // The stream teardown-under-write-lock invariant, per slot: reset the fd
        // and the open flag under write_m_ BEFORE ::close, so an in-flight send
        // either finished against the still-open fd or observes the reset.  The
        // mirror image of the accept-side publish — same lock, same relaxed stores.
        const std::lock_guard lock(write_m_);
        was_open = s.open.load(std::memory_order_relaxed);
        s.open.store(false, std::memory_order_relaxed);
        fd = s.fd.exchange(-1, std::memory_order_relaxed);
    }
    if (fd >= 0) ::close(fd);
    s.buf.clear();
    s.buf.shrink_to_fit();
    s.hs_buf.clear();
    s.assembler.reset();
    // Departure seam (RFC-0009 §D extended to peer departure): only a session that
    // completed its handshake can have flowed frames (subscriptions), and only then.
    // Fired LAST, with no transport lock held — the notifier re-enters the routing
    // plane (router → graph locks). Peer-named mode reports the peer's own name;
    // flat mode reports the whole link down.
    if (was_open && !departed.empty()) {
        if (peer_rx_.has_any())
            notify_peer_down(departed);
        else
            notify_down();
    }
}

void transport_ws_server::run() {
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

// ---------------------------------------------------------------------------
// transport_ws_client — the dial-out half.
// ---------------------------------------------------------------------------

transport_ws_client::transport_ws_client(const std::string& host, std::uint16_t port,
                                         mem::mem_backend_t* backend, std::size_t max_frame,
                                         std::size_t recv_stack, bool defer_recv)
    : backend_(backend), recv_stack_(recv_stack) {
    if (max_frame != 0) max_frame_ = max_frame;
    // Seed the per-frame masking-key stream with something that varies between
    // connections (steady_clock + this address). Not crypto-strong — RFC 6455
    // masking exists for proxy/cache safety, not to defend against a peer.
    std::uint64_t seed =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    seed ^= reinterpret_cast<std::uintptr_t>(this);
    mask_state_.store(seed, std::memory_order_relaxed);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &peer.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
        ::close(fd);
        return;
    }

    // Bytes the server pipelined behind its 101 — carried out of the handshake and into
    // the recv loop's buffer, never re-read from the socket (they are already off it).
    std::vector<std::byte> pipelined;
    if (!handshake(fd, host, port, pipelined)) {
        ::close(fd);
        return;
    }

    conn_fd_.store(fd, std::memory_order_relaxed);
    connected_ = true;
    pipelined_ = std::move(pipelined);
    // Two-phase bring-up (#1025). Spawning the recv thread HERE is the historical shape and
    // stays the default, but it makes the base's "install the sinks before frames flow"
    // contract unsatisfiable: this thread can decode and deliver a message the server pushed
    // on connect before the caller's next statement installs a sink, and the empty slot drops
    // it silently. `defer_recv` hands that ordering back to the owner — the socket is up and
    // the pipelined bytes are parked, but nothing is read until `start_receiving()`.
    //
    // Qualified deliberately: dispatching a virtual from a constructor would reach THIS
    // class's override anyway (the derived part does not exist yet), so spelling it out is
    // the honest form — a subclass cannot substitute its own bring-up here, and nothing in
    // this call should look as if it could.
    if (!defer_recv) transport_ws_client::start_receiving();
}

void transport_ws_client::start_receiving() {
    if (!connected_) return;  // handshake failed: there is nothing to serve
    // One-shot: `posix_endpoint_t::start` may be called at most once per endpoint, and this
    // is reachable both from the constructor (one-phase) and from an owner that calls it
    // unconditionally on every link it wires.
    if (recv_started_.exchange(true, std::memory_order_relaxed)) return;
    const int fd = conn_fd_.load(std::memory_order_relaxed);
    start([this, fd, pre = std::move(pipelined_)]() mutable { serve(fd, std::move(pre)); },
          recv_stack_);
}

transport_ws_client::~transport_ws_client() {
    stop_and_join();  // FIRST: serve() touches conn_fd_
    // A leftover fd (never-spawned thread — handshake failed — leaves conn_fd_
    // at -1 anyway) is closed by ~stream_endpoint_t after this body.
}

std::uint32_t transport_ws_client::next_mask_key() {
    // SplitMix64 step → a varied (non-crypto) 32-bit masking key per frame.
    std::uint64_t z = mask_state_.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed) +
                      0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<std::uint32_t>(z);
}

void transport_ws_client::send(std::span<const std::byte> frame) {
    // One serialized MASKED record under write_m_ (the stream_endpoint_t
    // write-serialization invariant). A client frame MUST be masked (RFC 6455 §5.1), so
    // unlike every server-side send this one cannot gather the caller's bytes by reference
    // and genuinely needs a buffer — the single WS egress site that keeps an allocation.
    // It is the NOTHROW twin over a REUSED buffer (#848): steady state allocates nothing,
    // and exhaustion drops the frame instead of aborting under -fno-exceptions.
    //
    // tx_buf_ is guarded by write_m_, the same lock that serializes the write, so the
    // encode happens inside it rather than through send_all_locked.
    const std::lock_guard lock(write_m_);
    const std::size_t n =
        ws::try_encode_client_frame(tx_buf_, ws::opcode_t::BINARY, frame, next_mask_key());
    if (n == 0) return;  // frame buffer exhausted => drop the frame
    write_all(conn_fd_.load(std::memory_order_relaxed),
              std::span<const std::byte>(tx_buf_.data(), n));
}

bool transport_ws_client::handshake(int fd, const std::string& host, std::uint16_t port,
                                    std::vector<std::byte>& pipelined) {
    pipelined.clear();
    // A fresh 16-byte nonce (RFC 6455 §4.1) base64'd into Sec-WebSocket-Key.
    std::array<std::byte, 16> nonce{};
    std::uint64_t s = mask_state_.load(std::memory_order_relaxed) ^ 0xD1B54A32D192ED03ull;
    for (auto& b : nonce) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        b = static_cast<std::byte>((s >> 56) & 0xFFu);
    }
    const std::string key = ws::base64(nonce);

    std::string req = "GET / HTTP/1.1\r\nHost: ";
    req += host;
    req += ':';
    req += std::to_string(port);
    req +=
        "\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: ";
    req += key;
    req += "\r\nSec-WebSocket-Version: 13\r\n\r\n";

    std::vector<std::byte> bytes(req.size());
    for (std::size_t i = 0; i < req.size(); ++i) bytes[i] = static_cast<std::byte>(req[i]);
    {
        const std::lock_guard lock(write_m_);
        write_all(fd, bytes);
    }

    // Read the response header block (CRLFCRLF), bounded so a stuck peer cannot
    // hang construction forever.
    std::string resp;
    std::array<char, 1024> chunk;
    std::size_t hdr_end = std::string::npos;
    while (hdr_end == std::string::npos) {
        if (stop_.load(std::memory_order_relaxed)) return false;
        const int pr = poll_readable(fd);  // one bounded 100 ms readability wait
        if (pr < 0) return false;
        if (pr == 0) continue;  // timeout → re-check stop_
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) return false;  // peer closed / error
        resp.append(chunk.data(), static_cast<std::size_t>(n));
        hdr_end = resp.find("\r\n\r\n");
        // Runaway-response guard: it bounds the HEADER scan only. Once CRLFCRLF is in hand
        // everything past it is frame bytes, and a large frame pipelined behind the 101
        // must not be read as a header block that never ends.
        if (hdr_end == std::string::npos && resp.size() > 16384) return false;
    }

    // Validate against the HEADER BLOCK alone: the tail may be arbitrary frame bytes, and
    // neither the `101` search nor the header scan may take a match out of them.
    const std::string_view header(resp.data(), hdr_end + 4);
    if (header.find("101") == std::string_view::npos) return false;
    const std::string accept = header_value(header, "sec-websocket-accept");
    if (accept.empty() || accept != ws::accept_key(key)) return false;

    // Bytes pipelined past the header are the start of the frame stream — they are already
    // off the socket, so `serve` can never read them again. Carry them over, exactly as
    // `transport_ws_server::service_peer` does on the accept side.
    const auto* rest = reinterpret_cast<const std::byte*>(resp.data()) + hdr_end + 4;
    pipelined.assign(rest, rest + (resp.size() - hdr_end - 4));
    return true;
}

void transport_ws_client::serve(int fd, std::vector<std::byte> pipelined) {
    ws_assembler_t asm_state;  // per-connection fragment assembly (recv thread only)
    std::vector<std::byte> buf = std::move(pipelined);
    std::array<std::byte, 4096> chunk;
    // The ingress bound, resolved from the injected backend and `max_frame` exactly as the
    // server resolves it — a dialled peer gets no more credit than an accepted one.
    const std::size_t cap = length_prefix_framer::effective_cap(*backend_, max_frame_);

    while (true) {
        // Drain BEFORE touching the socket: the loop starts with whatever the server
        // pipelined behind its 101 already in `buf`, and a peer that pushes its state on
        // connect and then goes quiet must still get that message delivered — a
        // poll-first loop would sit on a complete frame until the peer spoke again.
        // Leftover partial bytes stay for the next read.
        while (true) {
            ws::decode_result_t decoded = ws::decode_frame_checked(buf, cap);
            if (decoded.status == ws::decode_status_t::NEED_MORE) break;
            // RFC 6455 §7.1.7: a protocol violation FAILS the connection — the same
            // teardown a peer CLOSE takes, not an unbounded wait for legal bytes. That
            // now covers a declared length past the receive cap, which is what stops the
            // server we dialled from naming our memory budget (#872).
            if (decoded.status == ws::decode_status_t::PROTOCOL_ERROR) {
                malformed_rx_.fetch_add(1, std::memory_order_relaxed);
                goto teardown;
            }
            ws::frame_t frame = std::move(decoded.frame);
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));

            switch (frame.op) {
                case ws::opcode_t::BINARY:
                case ws::opcode_t::CONT: {
                    // Sink snapshot/tier select lives in the slot (receiver_slot.hpp);
                    // rx_.has_rope() is the per-frame tier query, so a sink installed
                    // mid-stream takes effect on the next data frame.
                    // Unfragmented fast path on the span tier: the borrowed
                    // payload is delivered directly, no owning copy (as before).
                    if (frame.op == ws::opcode_t::BINARY && frame.fin && !asm_state.assembling &&
                        !rx_.has_rope()) {
                        rx_.deliver_borrowed(std::span<const std::byte>(frame.payload));
                        break;
                    }
                    auto msg =
                        asm_state.on_data(frame.op, frame.fin, frame.payload, *backend_, cap);
                    if (msg.status == assemble_status_t::DROPPED) {
                        dropped_rx_.fetch_add(1, std::memory_order_relaxed);
                        break;  // backpressure: shed the message, keep the connection
                    }
                    if (msg.status == assemble_status_t::OVERSIZE) {
                        malformed_rx_.fetch_add(1, std::memory_order_relaxed);
                        goto teardown;  // fragments summing past the cap fail the connection
                    }
                    if (msg.status != assemble_status_t::COMPLETE) break;  // mid-message
                    // The reassembled message IS a rope — one owning link per
                    // fragment (ADR-0053 §5): the rope sink takes it as-is; only
                    // a span-only sink pays the one materialize (in the slot).
                    rx_.deliver_rope(std::move(msg.message));
                    break;
                }
                case ws::opcode_t::PING: {
                    // A client PONG must be masked, just like client data frames — and,
                    // like the server's, it is built on the STACK: §5.5 was enforced at
                    // decode, so the payload is <= 125 bytes and the reply cannot fail
                    // to be built (#848).
                    std::array<std::byte, ws::kMaxClientControlFrame> pong{};
                    const std::size_t plen = ws::encode_client_control(
                        pong, ws::opcode_t::PONG, frame.payload, next_mask_key());
                    const std::lock_guard lock(write_m_);
                    write_all(fd, std::span<const std::byte>(pong.data(), plen));
                    break;
                }
                case ws::opcode_t::CLOSE:
                    goto teardown;  // peer asked to close
                default:
                    break;  // TEXT / PONG: ignored
            }
        }

        if (stop_.load(std::memory_order_relaxed)) break;
        const int pr = poll_readable(fd);  // one bounded 100 ms readability wait
        if (pr < 0) break;
        if (pr == 0) continue;  // timeout → re-check stop_

        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) break;  // peer closed / error
        buf.insert(buf.end(), chunk.data(), chunk.data() + n);
    }

teardown:
    teardown_peer(fd);  // reset-under-write_m_ then close (stream_endpoint_t)
    // Departure seam (RFC-0009 §D extended): the one connection died under us — not a
    // local stop — so report the link down (after teardown, no locks held).
    if (!stop_.load(std::memory_order_relaxed)) notify_down();
}

}  // namespace tr::net
