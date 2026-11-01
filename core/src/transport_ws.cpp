/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_ws.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "libtracer/frame_ingress.hpp"
#include "libtracer/iov_table.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/ws.hpp"

namespace tr::net {

namespace {

/** @brief What chaining one data fragment into @ref ws_assembler_t produced. */
enum class assemble_status_t : std::uint8_t {
    PENDING,  /**< @brief Mid-message (or a stray CONT) — nothing to deliver yet. */
    COMPLETE, /**< @brief @ref assemble_result_t::message holds the finished message. */
    OVERSIZE, /**< @brief The message's charged cost grew past the receive cap — FAIL the
                          connection (§7.1.7) and count it as `malformed_rx`. */
    REFUSED,  /**< @brief A store refused the chain: the message is shed and counted as
                          `dropped_rx`, the connection lives (receiver pays). */
};

/** @brief One @ref ws_assembler_t::on_piece outcome: a status, and (on `COMPLETE`) the rope. */
struct assemble_result_t {
    assemble_status_t status = assemble_status_t::PENDING; /**< @brief Which outcome. */
    tr::view::rope_t message; /**< @brief The finished message — meaningful only on `COMPLETE`. */
};

/**
 * @brief RFC 6455 fragmented-message reassembly as ROPE CHAINING (ADR-0053 §5): each data
 *        fragment is one owning link and chaining them is pointer-linking, never a memcpy.
 *
 * Only a fragmented message needs it: either end hands an unfragmented frame up as the one
 * view over the segment it was received into (ADR-0087 §1). One assembler per connection,
 * used only on its receive thread.
 *
 * The links are held in a chain drawn from the receiving link's own rx source, not the
 * process heap, and the receive cap bounds what a message COSTS, not its payload alone:
 * each fragment is charged its bytes plus @ref kFragmentCost, the segment it was received
 * into and its chain slot. Without that charge a message of empty or one-byte fragments
 * held a segment and a link per fragment while its payload total stayed far under the cap.
 */
struct ws_assembler_t {
    /** @brief What a fragment costs beyond its payload: its segment's header, the receive
     *         loan room `alloc_rx` may put in front of the bytes, and its chain slot. */
    static constexpr std::size_t kFragmentCost =
        sizeof(tr::view::segment_t) + tr::mem::kRxLoanBytes + sizeof(tr::view::view_t);

    /** @brief An idle assembler whose chain will be drawn from @p rx, the link's rx source. */
    explicit ws_assembler_t(tr::mem::block_source_t& rx) noexcept : chain(rx) {}

    tr::mem::block_array_t<tr::view::view_t> chain; /**< @brief The message's fragments. */
    std::size_t charged = 0;                        /**< @brief What they cost against the cap. */

    /** @brief True while a message is open: its first fragment is in the chain. */
    [[nodiscard]] bool assembling() const noexcept { return !chain.empty(); }

    /** @brief Drop the message being assembled and hand its chain back to the source. */
    void reset() noexcept {
        chain = tr::mem::block_array_t<tr::view::view_t>(chain.source());
        charged = 0;
    }

    /**
     * @brief Chain one owning data fragment (BINARY or CONT) into the message.
     *
     * Returns the completed message as a rope when this fragment finishes one, `PENDING`
     * otherwise. A BINARY that arrives mid-assembly is an RFC 6455 protocol error: the stale
     * assembly is dropped and the new message starts. A stray CONT (no assembly open) is
     * dropped.
     *
     * @param piece The fragment's bytes, owned.
     * @param cap   The effective receive cap the message's CHARGED cost may not exceed. The
     *              per-frame header check bounds one fragment; without this one a peer walks
     *              past the cap a few bytes at a time across a million CONTs (#872).
     */
    assemble_result_t on_piece(ws::opcode_t op, bool fin, tr::view::view_t piece, std::size_t cap) {
        if (op == ws::opcode_t::BINARY)
            reset();
        else if (!assembling())
            return {};  // stray
        // Written as a subtraction because `charged + cost` can wrap. `charged <= cap` holds
        // by induction (every earlier fragment passed this same test), so the difference is
        // well-defined; the header check holds `piece.length` to the cap, so `cost` cannot
        // wrap either.
        const std::size_t cost = piece.length + kFragmentCost;
        if (cost > cap - charged) {
            reset();
            return {assemble_status_t::OVERSIZE, {}};
        }
        if (!chain.push_back(std::move(piece))) {
            reset();
            return {assemble_status_t::REFUSED, {}};
        }
        charged += cost;
        if (!fin) return {};
        // The delivered rope holds the links itself, in a chain from the same rx source
        // (the receiver pays); reserving them first makes a refusal backpressure instead
        // of an abort.
        assemble_result_t done{assemble_status_t::REFUSED, tr::view::rope_t(chain.source())};
        if (done.message.try_reserve(chain.size())) {
            for (tr::view::view_t& link : chain) done.message.append(std::move(link));
            done.status = assemble_status_t::COMPLETE;
        }
        reset();
        return done;
    }
};

/** @brief What the body of the frame being received needs from its header (the length is
 *         frame ingress's own). */
struct ws_rx_frame_t {
    ws::opcode_t op = ws::opcode_t::CONT;   /**< @brief Its opcode. */
    bool fin = false;                       /**< @brief Its FIN bit. */
    bool masked = false;                    /**< @brief Its MASK bit. */
    std::array<std::uint8_t, 4> mask_key{}; /**< @brief Its masking key, when masked. */
};

/**
 * @brief The RFC 6455 receive rules both ends share, as frame ingress calls them
 *        (`frame_ingress.hpp`): the header rules on the way in, then unmasking, fragment
 *        chaining and the opcode's answer on each whole body.
 *
 * What differs between the server and the dial-out client is reached through @p End, the
 * end's own adapter: `cap()` (the live receive cap), `malformed()` and `dropped()` (its
 * counters), `pong(bytes)` (its PONG, masked or not) and `hand_up(message)` (its delivery),
 * plus the `frame` and `assembler` state of the stream being read.
 */
template <class End>
struct ws_framing_t {
    End& end; /**< @brief The receiving end. */

    /** @brief Judge the header bytes read so far; an over-cap length or a protocol breach
     *         closes the stream before anything is drawn. */
    [[nodiscard]] ingress_header_t header(std::span<const std::byte> have) const {
        const ws::header_t h = ws::decode_header(have, end.cap());
        if (h.status == ws::decode_status_t::NEED_MORE)
            return {ingress_header_t::kind_t::MORE, h.size, 0};
        if (h.status == ws::decode_status_t::PROTOCOL_ERROR) {
            // An over-cap length, a §5.5 control breach or a §5.2 reserved opcode: a stream
            // we refuse to keep reading. Count it; the stream is torn down (§7.1.7).
            end.malformed();
            return {ingress_header_t::kind_t::CLOSE, 0, 0};
        }
        end.frame = {h.op, h.fin, h.masked, h.mask_key};
        return {ingress_header_t::kind_t::BODY, h.size, h.len};
    }

    /** @brief One whole body, in its exact segment (empty when the source refused it). */
    [[nodiscard]] bool body(tr::view::rx_block_t& blk, std::size_t len) const {
        if (len != 0 && !blk.seg) {
            // The receiving link's source refused the body (backpressure): the message is
            // shed and counted, the connection lives and re-syncs at the next BINARY. A
            // PING shed here goes unanswered.
            end.dropped();
            if (!ws::is_control_opcode(end.frame.op)) end.assembler.reset();
            return end.frame.op != ws::opcode_t::CLOSE;
        }
        const std::span<std::byte> bytes = blk.seg ? blk.frame(len) : std::span<std::byte>{};
        if (end.frame.masked) ws::detail::xor_mask(bytes, end.frame.mask_key);
        switch (end.frame.op) {
            case ws::opcode_t::BINARY:
            case ws::opcode_t::CONT:
                return data(len != 0 ? blk.take(len) : tr::view::view_t{});
            case ws::opcode_t::PING:
                end.pong(bytes);  // the header rules held it to <= 125 bytes (§5.5)
                return true;
            case ws::opcode_t::CLOSE:
                return false;  // tear this one connection down
            default:
                return true;  // TEXT / PONG: ignored (a RESERVED opcode never gets here: #1060)
        }
    }

    /**
     * @brief One data frame's bytes: an unfragmented message goes up as the one view over its
     *        segment, a fragment is chained by the assembler until its message completes.
     */
    [[nodiscard]] bool data(tr::view::view_t piece) const {
        if (end.frame.op == ws::opcode_t::BINARY && end.frame.fin) {
            end.assembler.reset();  // a BINARY mid-assembly drops the stale message (§5.4)
            end.hand_up(tr::view::rope_t{std::move(piece)});
            return true;
        }
        assemble_result_t r =
            end.assembler.on_piece(end.frame.op, end.frame.fin, std::move(piece), end.cap());
        // Fragments summing past the cap get the answer a single over-cap frame gets, so the
        // fragmented route is not a way around the bound.
        if (r.status == assemble_status_t::OVERSIZE) {
            end.malformed();
            return false;
        }
        // A refused chain is a refused body by another name: shed, counted, and the
        // connection re-syncs at the next BINARY.
        if (r.status == assemble_status_t::REFUSED) end.dropped();
        if (r.status == assemble_status_t::COMPLETE) end.hand_up(std::move(r.message));
        return true;
    }
};

}  // namespace

namespace {

/** @brief ASCII lower-case of @p c (HTTP header names are ASCII, RFC 9110 §5.1). */
constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/**
 * @brief Case-insensitive search for an HTTP header; return its trimmed value as a view
 *        into @p request (empty when absent).
 *
 * `request` is the raw header block; `name` is lowercase (e.g. "sec-websocket-key"). Each
 * header line is matched in place, so nothing is lowered into a copy (#1780).
 */
std::string_view header_value(std::string_view request, std::string_view name) {
    for (std::size_t line = 0; line < request.size();) {
        std::size_t eol = request.find("\r\n", line);
        if (eol == std::string_view::npos) eol = request.size();
        const std::string_view text = request.substr(line, eol - line);
        line = eol + 2;
        if (text.size() <= name.size() || text[name.size()] != ':' ||
            !std::equal(name.begin(), name.end(), text.begin(),
                        [](char want, char got) { return want == ascii_lower(got); }))
            continue;
        const std::string_view val = text.substr(name.size() + 1);
        const std::size_t b = val.find_first_not_of(" \t");
        if (b == std::string_view::npos) return {};
        return val.substr(b, val.find_last_not_of(" \t") - b + 1);
    }
    return {};
}

/** @brief The bytes of @p b as text. */
std::string_view as_text(const mem::bytes_t& b) noexcept {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

/** @brief The bytes of @p s. */
std::span<const std::byte> as_bytes(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/** @brief A fixed text buffer filled by appending — the handshake's request/response, built
 *         on the stack (#1780). @c ok turns false when a piece does not fit. */
template <std::size_t N>
struct text_buf_t {
    std::array<char, N> text{}; /**< @brief The characters. */
    std::size_t len = 0;        /**< @brief How many are used. */
    bool ok = true;             /**< @brief False once a piece did not fit. */

    /** @brief Append @p s (or mark the buffer overflowed). */
    text_buf_t& operator<<(std::string_view s) noexcept {
        if (s.size() > N - len) {
            ok = false;
            return *this;
        }
        std::memcpy(text.data() + len, s.data(), s.size());
        len += s.size();
        return *this;
    }
    /** @brief The text so far, as bytes. */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return as_bytes(std::string_view(text.data(), len));
    }
};

/** @brief The most one handshake read asks for: a browser's Upgrade request fits, and the
 *         request buffer grows by no more than this past what has arrived. */
constexpr std::size_t kHandshakeReadBytes = 1024;

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
 * @brief One peer slot: the WebSocket half of a single inbound client's connection
 *        state.
 *
 * The protocol-agnostic half — fd/open/name/endpoint and the threading rule that
 * governs them — is `slot_server_t::session_base_t`'s; this adds the handshake
 * accumulation, the frame-ingress state (the header being read and the body's segment while
 * it fills), the current frame's header, the fragment reassembler and the directed facade
 * object. All but the facade are recv-thread-only, like every protocol buffer a slot
 * carries. WS reads the shared rule with ONE addition of its own: the slot is
 * published NOT-open and opens only once its `101` is on the wire (`read_handshake` below).
 */
struct ws_server_transport_t::session_t : slot_server_t::session_base_t {
    /** @brief Draw the handshake buffer from @p src — the server's session store (#1780) —
     *         and the fragment chain from @p rx, the server's rx source. */
    session_t(mem::block_source_t& src, mem::block_source_t& rx) noexcept
        : hs_buf(src), assembler(rx) {}
    mem::bytes_t hs_buf; /**< @brief HTTP Upgrade request accumulation. */
    /** @brief Frame ingress (ADR-0087 §1): no stream buffer, the body lands in its segment. */
    frame_ingress_t<ws::kMaxClientFrameHeader> ingress;
    ws_rx_frame_t frame;      /**< @brief The header of the frame being received. */
    ws_assembler_t assembler; /**< @brief RFC 6455 fragment reassembly. */
    peer_endpoint_t endpoint; /**< @brief The directed facade `peer_link` returns. */
};

/**
 * @brief The WS server's end of the shared receive rules (@ref ws_framing_t): one peer's
 *        stream state, this server's cap and counters, an unmasked PONG, and delivery
 *        tagged by the constructed mode.
 */
struct ws_server_transport_t::framing_t {
    ws_server_transport_t& link; /**< @brief The server (its counters, sinks and lock). */
    session_t& s;                /**< @brief The peer the bytes came from. */
    ws_rx_frame_t& frame;        /**< @brief That peer's frame header in progress. */
    ws_assembler_t& assembler;   /**< @brief That peer's fragment reassembly. */

    /** @brief The receive cap, resolved from the two INJECTED resources on every header
     *         (one virtual call, the tcp_transport_t::serve idiom) rather than cached: it is
     *         what stops a declared 64-bit length from naming the receiver's memory, so it
     *         must be the live answer, not a snapshot. */
    [[nodiscard]] std::size_t cap() const {
        return length_prefix_framer_t::effective_cap(*link.backend_, link.max_frame_);
    }
    /** @brief Count a frame that broke the rules. */
    void malformed() const { link.malformed_rx_.fetch_add(1, std::memory_order_relaxed); }
    /** @brief Count a body the source refused. */
    void dropped() const { link.dropped_rx_.fetch_add(1, std::memory_order_relaxed); }

    /** @brief Answer a PING with a PONG built on the STACK: the payload is <= 125 bytes, so
     *         the reply cannot fail to be built (#848). */
    void pong(std::span<const std::byte> payload) const {
        std::array<std::byte, ws::kMaxServerControlFrame> pong{};
        const std::size_t plen = ws::encode_server_control(pong, ws::opcode_t::PONG, payload);
        // Through the session's egress like any other frame (#2144): never a wait on the
        // poll thread, and never interleaved with a frame in flight.
        const ::iovec pv{pong.data(), plen};
        link.send_to(s, std::span<const ::iovec>(&pv, 1));
    }

    /**
     * @brief Hand one whole message up.
     *
     * The constructed MODE picks the slot (#889): peer-named servers deliver tagged with the
     * sending peer's handle (the ADR-0044 bus precedence, what the router wires), flat
     * servers deliver point-to-point under the link's own registered name. `bus_mode()` is
     * that flag conjoined with whether this BUILD carries a bus module at all (#375
     * deliverable 3), and the peer-tier call is the STATIC seam of #1438, so the per-frame
     * path gained no dispatch. A single-link rope reaches a span-only sink borrowed, so the
     * tier needs no choosing here; a fragmented one materializes once, from this server's rx
     * source.
     *
     * The FLAT tier carries no per-frame tag by construction, so the WHO the subject seam
     * needs is stamped here instead (#375 Part 2, ADR-0082): the session's handle, on the
     * poll thread, immediately before the delivery that consumes it, and cleared once it is
     * handed up, so the field names a session only while that session's frame is being
     * delivered (#1915).
     */
    void hand_up(tr::view::rope_t message) const {
        link.delivering_ = s.handle;
        if (link.bus_mode())
            link.deliver_to_peer_rope(s.handle, std::move(message), *link.backend_);
        else
            link.rx_.deliver_rope(std::move(message), *link.backend_);
        link.delivering_ = {};
    }
};

ws_server_transport_t::ws_server_transport_t(std::uint16_t bind_port,
                                             const ws_server_config_t& config)
    : stream_server_base_t(config.max_peers, config.peer_named, config.liveness_window_ms,
                           config.memory.io_or_default(), config.memory.state_or_default()),
      backend_(config.memory.rx) {
    max_frame_ = length_prefix_framer_t::configured_cap(config.max_frame);  // tighten-only (#1035)
    max_handshake_ = handshake_cap(config.max_handshake);                   // tighten-only (#934)
    if (!bind_listen(bind_port)) return;
    // A refused poll-loop spawn (#2288) leaves the listener inert: ok() false.
    if (!start([this] { run(); }, config.recv_stack, config.memory.stack_or_default()))
        close_listen();
}

ws_server_transport_t::~ws_server_transport_t() {
    // FIRST: the run() thread dispatches this class's variance points, so it must be
    // joined while the derived object is still whole. ~slot_server_t closes the listen
    // socket and sweeps the slot fds after this body.
    stop_and_join();
}

void ws_server_transport_t::shut_down() { shut_down_slots(); }

mem::poly_ptr_t<slot_server_t::session_base_t> ws_server_transport_t::make_session() {
    mem::poly_ptr_t<session_t> slot =
        mem::make_poly<session_t>(slots_.source(), slots_.source(), *backend_);
    if (!slot) return nullptr;
    slot->endpoint.owner_ = this;
    slot->endpoint.slot_ = slot.get();
    slot->peer_endpoint = &slot->endpoint;
    return slot;
}

bool ws_server_transport_t::on_accept(session_base_t& base, int fd) {
    (void)fd;  // no per-socket option of its own: WS rides the shared stream setup, which
               // since #838 includes the bounded SO_SNDTIMEO armed in accept_peer

    session_t& s = static_cast<session_t&>(base);
    s.hs_buf.clear();
    s.ingress.reset();
    s.assembler.reset();
    // A WS slot is published NOT-open: its 101 has not been written yet, so nothing may
    // send on it. `on_readable` opens it once the response is out — the handshake
    // variance, localised here and there alone.
    return false;
}

void ws_server_transport_t::on_slot_reset(session_base_t& base) {
    session_t& s = static_cast<session_t&>(base);
    // Return the blocks, not just the sizes (#934). A slot is RECYCLED for the next accept,
    // so a `clear()` alone left whatever capacity the worst pre-auth peer grew the handshake
    // buffer to attached to the slot for the process lifetime — one sweep across every
    // slot was a permanent max_peers x budget tax that no refusal ever gave back.
    s.hs_buf = mem::bytes_t(s.hs_buf.source());
    s.ingress.reset();
    s.assembler.reset();
}

bool ws_server_transport_t::mid_frame(const session_base_t& base) const {
    // A peer owes its opening handshake until the 101 opens the session.
    const session_t& s = static_cast<const session_t&>(base);
    return !s.open.load(std::memory_order_relaxed) || s.ingress.mid_frame() ||
           s.assembler.assembling();
}

void ws_server_transport_t::send(std::span<const std::byte> frame) {
    // One span, same wire bytes — the gathered path is the ONE implementation (the
    // tcp_transport_t::send idiom). A server frame is UNMASKED (RFC 6455 §5.1), so the
    // caller's payload rides to the wire untouched behind a stack-built header: this
    // removes the per-send `std::vector` (a peer-reachable abort() under -fno-exceptions,
    // #848) AND a full payload memcpy, at the same one writev per peer.
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void ws_server_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    send_record(nullptr, iov, nullptr, {});
}

void ws_server_transport_t::send(std::span<const std::span<const std::byte>> head,
                                 const graph::value_t& value, const graph::delivery_lane_t& lane) {
    send_retained(nullptr, head, value, lane);
}

void ws_server_transport_t::send_retained(session_base_t* to,
                                          std::span<const std::span<const std::byte>> head,
                                          const graph::value_t& value,
                                          const graph::delivery_lane_t& lane) {
    std::array<std::span<const std::byte>, kMaxServerIov> parts;
    const std::size_t n = retained_parts(parts, head, value);
    if (n > parts.size()) {  // too wide for the stack table: the base's copy path
        if (to == nullptr)
            transport_t::send(head, value, lane);
        else
            to->peer_endpoint->transport_t::send(head, value, lane);
        return;
    }
    send_record(to, std::span<const std::span<const std::byte>>(parts.data(), n), &value, lane);
}

void ws_server_transport_t::send_record(session_base_t* to,
                                        std::span<const std::span<const std::byte>> parts,
                                        const graph::value_t* value,
                                        const graph::delivery_lane_t& lane) {
    // Encode the ONE server frame header for the whole gathered payload, then hand
    // [header, span0, span1, ...] to every open peer (@p to null) or to the one session as a
    // single gathered write — no flatten, no re-copy (server frames are UNMASKED, RFC 6455
    // §5.1), each peer through its own egress (ADR-0087 §2).
    std::size_t total = 0;
    for (const std::span<const std::byte>& s : parts) total += s.size();
    std::array<std::byte, ws::kMaxServerFrameHeader> header{};
    const std::size_t hlen = ws::encode_frame_header(header, ws::opcode_t::BINARY, total);

    std::array<::iovec, kMaxServerIov + 1> gather_inline;
    iov_table_t<::iovec> gather_table(gather_inline, egress_source());
    const auto [vec, n] = build_server_iov(gather_table, header, hlen, parts);
    if (vec == nullptr) {  // gather store exhausted => drop the frame, and count it (#932)
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Every idle peer writes this ONE gather (read-only, #932), and a peer that has to wait
    // gets one shared frame of it in that vertex's ring; sheds and stalls are counted.
    const std::span<const ::iovec> rec(vec, n);
    if (to == nullptr)
        broadcast(rec, value, lane);
    else
        send_to(*to, rec, value, lane);  // a departed session's egress is closed: shed
}

void ws_server_transport_t::peer_endpoint_t::send(std::span<const std::byte> frame) {
    // One span through the gathered path — the same delegation as the broadcast override
    // (#848): no per-send vector, no payload copy.
    const std::span<const std::byte> one[1] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void ws_server_transport_t::peer_endpoint_t::send(std::span<const std::span<const std::byte>> iov) {
    if (owner_ != nullptr && slot_ != nullptr) owner_->send_record(slot_, iov, nullptr, {});
}

void ws_server_transport_t::peer_endpoint_t::send(std::span<const std::span<const std::byte>> head,
                                                  const graph::value_t& value,
                                                  const graph::delivery_lane_t& lane) {
    if (owner_ != nullptr && slot_ != nullptr) owner_->send_retained(slot_, head, value, lane);
}

void ws_server_transport_t::on_readable(session_base_t& base, int fd) {
    session_t& s = static_cast<session_t&>(base);
    framing_t end{*this, s, s.frame, s.assembler};
    ws_framing_t<framing_t> framing{end};
    // Past the 101, frame ingress reads each header first and each body straight into its
    // exact segment (ADR-0087 §1). Either phase answers false for a peer that closed or broke
    // the protocol, and both tear down through the one teardown.
    const bool keep = s.open.load(std::memory_order_relaxed)
                          ? s.ingress.read(fd, *backend_, framing)
                          : read_handshake(s, fd);
    if (!keep) teardown_slot(s);
}

bool ws_server_transport_t::read_handshake(session_t& s, int fd) {
    // Opening handshake: read the HTTP Upgrade request straight into the slot's request
    // buffer until CRLFCRLF, under the deployment's PRE-AUTH budget. The peer here has
    // completed a TCP connect and nothing else — no ACL, no subscription, no router — so the
    // buffer never grows past the budget: refuse early, never carefully allocate (#934).
    const std::size_t have = s.hs_buf.size();
    if (have == max_handshake_) {
        // The budget is spent and the request has not ended. Count it, then close — the same
        // disposition an over-cap frame declaration gets, and for the same reason (#838's
        // shape, ruled for this path 2026-08-15): backpressure would let the attacker's own
        // stall hold the slot, and a silent drop would leave the attack invisible to every
        // counter.
        malformed_rx_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::size_t room = std::min(max_handshake_ - have, kHandshakeReadBytes);
    if (!s.hs_buf.resize_for_overwrite(have + room)) {  // the session store refused: shed
        dropped_rx_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const ssize_t n = ::recv(fd, s.hs_buf.data() + have, room, 0);
    // Shrinking to what arrived never draws, so it cannot fail.
    (void)s.hs_buf.resize_for_overwrite(have + static_cast<std::size_t>(n > 0 ? n : 0));
    if (n <= 0) return false;  // peer closed the TCP connection, or error
    const std::size_t hdr_end = as_text(s.hs_buf).find("\r\n\r\n");
    if (hdr_end == std::string_view::npos) return true;  // keep accumulating

    const std::string_view key =
        header_value(as_text(s.hs_buf).substr(0, hdr_end + 4), "sec-websocket-key");
    if (key.empty()) return false;
    // Built on the stack: the reply is fixed text plus the 28-character accept value, and
    // the buffer is sized from exactly those pieces, so it cannot come up short.
    static constexpr std::string_view kHead =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    static constexpr std::string_view kTail = "\r\n\r\n";
    text_buf_t<kHead.size() + ws::kAcceptKeyChars + kTail.size()> resp;
    resp << kHead << ws::accept_key(key) << kTail;
    const std::span<const std::byte> bytes = resp.bytes();
    // The reply is the session's FIRST frame: its egress opens with the 101 held on the
    // wire, so no frame can go ahead of it, and `open` is stored before the reply is
    // written, so every send that follows a peer which has read it finds the slot open.
    // The write never waits: a reply the socket cuts short finishes on writability.
    open_session(s, bytes);
    // The seam that makes the line above testable: here the response is out AND the slot
    // is published, so a send that lands in this instant must reach the peer. Move the
    // store below this point and the same probe reads an empty socket — that is the
    // regression this hook exists to redden (`ws_transport_test`, the handshake race).
    if constexpr (graph::kFaultInjection) {
        if (detail::ws_peer_published_hook != nullptr) detail::ws_peer_published_hook();
    }
    // The WS slot's OPEN transition is here, not at accept — so this is where its arrival
    // seam fires (#1223). Before the pipelined frames are fed, so a session that departs inside the
    // very first drain still sees its arrival announced ahead of its departure.
    publish_peer_up(s);
    // Bytes pipelined past the header are the start of the frame stream: frame ingress
    // takes them exactly as if they had come off the socket.
    framing_t end{*this, s, s.frame, s.assembler};
    ws_framing_t<framing_t> framing{end};
    const bool ok = s.ingress.feed(std::span<const std::byte>(s.hs_buf.data() + hdr_end + 4,
                                                              s.hs_buf.size() - hdr_end - 4),
                                   *backend_, framing) >= 0;
    s.hs_buf = mem::bytes_t(s.hs_buf.source());
    return ok;
}

// ---------------------------------------------------------------------------
// ws_client_transport_t — the dial-out half.
// ---------------------------------------------------------------------------

ws_client_transport_t::ws_client_transport_t(std::string_view host, std::uint16_t port,
                                             const ws_client_config_t& config)
    // Every masked frame this link sends is egress store (#1661, #2267): drawn from
    // `memory.io` per send, not from the process heap, and never kept past its write.
    : stream_endpoint_t(config.memory.io != nullptr ? *config.memory.io : mem::net_source()),
      backend_(config.memory.rx),
      pipelined_(config.memory.state_or_default()),
      recv_stack_(config.recv_stack),
      stack_src_(&config.memory.stack_or_default()) {
    mem::block_source_t* const egress_src = config.memory.io;
    const std::size_t max_frame = config.max_frame;
    const std::uint32_t liveness_window_ms = config.liveness_window_ms;
    const std::size_t max_handshake = config.max_handshake;
    // Wire the BASE's egress store to the same one, before anything can send: the gather
    // temporary of `transport_t::send(iov)` and this class's masked frames are then one
    // store, not two. First statement on purpose — `start_receiving()` at the end of this body
    // spawns the recv thread.
    if (egress_src != nullptr) set_egress_source(*egress_src);
    liveness_window_ms_ = liveness_window_ms;                        // the #838 send bound's source
    max_frame_ = length_prefix_framer_t::configured_cap(max_frame);  // tighten-only (#1035)
    // One home for the resolution, shared with the accept side (#934): a dialled server is
    // no more trusted with our pre-auth memory than a peer that dialled us.
    max_handshake_ = ws_server_transport_t::handshake_cap(max_handshake);
    // Seed the per-frame masking-key stream with something that varies between
    // connections: one OS entropy read per dial, mixed with this address (#2305). Not
    // crypto-strong — RFC 6455 masking exists for proxy/cache safety, not to defend against
    // a peer. A failed read (ENOSYS, or EAGAIN early in boot) leaves the address-only mix;
    // there is no clock fallback, because the library reads no clock.
    std::uint64_t seed = 0;
    if (::getrandom(&seed, sizeof(seed), GRND_NONBLOCK) != static_cast<ssize_t>(sizeof(seed)))
        seed = 0;
    seed ^= reinterpret_cast<std::uintptr_t>(this);
    mask_state_.store(seed, std::memory_order_relaxed);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (!parse_ipv4(host, peer.sin_addr) ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
        ::close(fd);
        return;
    }

    // The bounded send (#838), armed before the first byte goes out: without it the
    // handshake request AND every later frame block forever on a peer that stops reading,
    // inside the writer's turn.
    set_snd_timeout(fd);

    // Bytes the server pipelined behind its 101 — carried out of the handshake and into
    // the recv loop's buffer, never re-read from the socket (they are already off it).
    mem::bytes_t pipelined(pipelined_.source());  // connection state: `memory.state`
    if (!handshake(fd, host, port, pipelined)) {
        ::close(fd);
        return;
    }

    conn_fd_.store(fd, std::memory_order_relaxed);
    // The two facts diverge from here (#1059): came_up_ is the ok() answer, written once
    // and never reverting; connected_ is the link_up() liveness the recv thread's
    // teardown clears.
    came_up_ = true;
    connected_.store(true, std::memory_order_relaxed);
    pipelined_ = std::move(pipelined);
    // Two-phase bring-up (#1025): the recv thread is spawned by `start_receiving()`, never
    // here. A thread started here could decode a message the server pushed on connect before
    // the owner installs a sink, and the empty slot would drop it silently. The pipelined
    // bytes stay parked until then.
}

bool ws_client_transport_t::start_receiving() {
    if (!came_up_) return true;  // handshake failed: nothing to serve, and ok() says so
    // One-shot: `posix_endpoint_t::start` may be called at most once per endpoint, and this
    // is reachable both from the constructor (one-phase) and from an owner that calls it
    // unconditionally on every link it wires.
    if (recv_started_.exchange(true, std::memory_order_relaxed)) return true;
    const int fd = conn_fd_.load(std::memory_order_relaxed);
    // `pipelined_` is the recv thread's from here on: `serve` takes it over.
    if (start([this, fd] { serve(fd); }, recv_stack_, *stack_src_)) return true;
    // A refused spawn (#2288) is the connection dying before its first message: closed, and
    // ok() false. The owner hears it from the return, not from a link-down fired under
    // whatever lock it holds while it wires the link.
    teardown_peer(fd);
    connected_.store(false, std::memory_order_relaxed);
    came_up_ = false;
    return false;
}

ws_client_transport_t::~ws_client_transport_t() {
    stop_and_join();  // FIRST: serve() touches conn_fd_
    // A leftover fd (never-spawned thread — handshake failed — leaves conn_fd_
    // at -1 anyway) is closed by ~stream_endpoint_t after this body.
}

void ws_client_transport_t::shut_down() { shut_down_stream(); }

std::uint32_t ws_client_transport_t::next_mask_key() {
    // SplitMix64 step → a varied (non-crypto) 32-bit masking key per frame.
    std::uint64_t z = mask_state_.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed) +
                      0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<std::uint32_t>(z);
}

void ws_client_transport_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[1] = {frame};
    send_masked(one, {});
}

void ws_client_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    send_masked(iov, {});
}

void ws_client_transport_t::send(std::span<const std::span<const std::byte>> head,
                                 const graph::value_t& value, const graph::delivery_lane_t& lane) {
    // The record's spans on the stack, masked as they are gathered (#2145): never flattened
    // first. An unusually wide record takes the base's general lowering (the copy path).
    std::array<std::span<const std::byte>, kMaxInlineIov> parts;
    const std::size_t n = retained_parts(parts, head, value);
    if (n > parts.size()) {
        transport_t::send(head, value, lane);
        return;
    }
    send_masked(std::span<const std::span<const std::byte>>(parts.data(), n), lane);
}

void ws_client_transport_t::send_masked(std::span<const std::span<const std::byte>> frame,
                                        const graph::delivery_lane_t& lane) {
    // One MASKED record per frame, through the link's egress (ADR-0087 §2). A client frame
    // MUST be masked (RFC 6455 §5.1), so unlike every server-side send this one cannot gather
    // the caller's bytes by reference: its wire bytes are written once, gathered and masked in
    // ONE pass (`ws::detail::mask_copy`, #2145), into the one frame `once` draws from the
    // link's own egress source (#2267). The idle writer writes that frame and lets it go; a
    // write the socket holds keeps the SAME frame on the wire, never a second masked copy; a
    // publisher that arrives while a write is in flight shares it. So the link keeps no
    // buffer between sends, and exhaustion drops the frame (#848, #932) instead of aborting
    // under -fno-exceptions. Each writer turn is still bounded by the liveness window (#838).
    const std::uint32_t key = next_mask_key();
    std::size_t len = 0;
    for (const std::span<const std::byte>& part : frame) len += part.size();
    egress_once_t once([&] {
        return egress_frame_t::make(
            *io_src_, ws::detail::client_frame_bytes(len), [&](std::span<std::byte> out) {
                return ws::detail::put_client_frame(out, ws::opcode_t::BINARY, frame, key, true);
            });
    });
    egress_send(
        once,
        [&](int fd, std::uint32_t bound) {
            const egress_frame_t* const f = once();
            // No block for the frame => a counted drop (#932), nothing written.
            if (f == nullptr) return egress_step_t{0, false, true};
            return write_frame(fd, *f, 0, bound, false);
        },
        lane);
}

bool ws_client_transport_t::handshake(int fd, std::string_view host, std::uint16_t port,
                                      mem::bytes_t& pipelined) {
    pipelined.clear();
    // A fresh 16-byte nonce (RFC 6455 §4.1) base64'd into Sec-WebSocket-Key.
    std::array<std::byte, 16> nonce{};
    std::uint64_t s = mask_state_.load(std::memory_order_relaxed) ^ 0xD1B54A32D192ED03ull;
    for (auto& b : nonce) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        b = static_cast<std::byte>((s >> 56) & 0xFFu);
    }
    std::array<char, ws::base64_len(16)> key_text{};
    const std::string_view key(key_text.data(), ws::base64_into(nonce, key_text));

    // Built on the stack (#1780): `host` is an IPv4 literal (it parsed as one above), so the
    // request is fixed text plus at most 15 + 5 + 24 characters; 192 holds the worst case
    // (160) with room, and a request that did not fit is never sent.
    std::array<char, 8> port_text{};
    const std::string_view port_view(
        port_text.data(),
        static_cast<std::size_t>(
            std::to_chars(port_text.data(), port_text.data() + port_text.size(), port).ptr -
            port_text.data()));
    text_buf_t<192> req;
    req << "GET / HTTP/1.1\r\nHost: " << host << ":" << port_view
        << "\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Key: "
        << key << "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!req.ok) return false;
    // Bounded (#838) — this runs in the CONSTRUCTOR, so an unbounded write here hangs the
    // application thread that is wiring the link. No sender can race it: the fd is not
    // published to conn_fd_ until the handshake has succeeded.
    write_all(fd, req.bytes(), derive_send_bound_ms(liveness_window_ms_, 1));

    // Read the response header block (CRLFCRLF), bounded so a stuck peer cannot
    // hang construction forever.
    mem::bytes_t resp(pipelined.source());  // connection state: the link's `memory.state`
    std::array<std::byte, 1024> chunk;
    std::size_t hdr_end = std::string_view::npos;
    while (hdr_end == std::string_view::npos) {
        if (stop_.load(std::memory_order_relaxed)) return false;
        // The PRE-AUTH budget, applied to the READ rather than to the buffer afterwards
        // (#934): ask the socket for only what is left of it, so the accumulation can never
        // pass the budget by even one byte and nothing has to be discarded after the fact.
        // Exhausted with no CRLFCRLF in hand ⇒ the server is not answering a handshake:
        // count it and fail the dial (the count-then-close disposition).
        const std::size_t room = max_handshake_ - resp.size();
        if (room == 0) {
            malformed_rx_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const int pr = poll_readable(fd);  // one bounded 100 ms readability wait
        if (pr < 0) return false;
        if (pr == 0) continue;  // timeout → re-check stop_
        const ssize_t n = ::recv(fd, chunk.data(), std::min(chunk.size(), room), 0);
        if (n <= 0) return false;  // peer closed / error
        if (!resp.append(chunk.data(), static_cast<std::size_t>(n))) return false;  // store refused
        hdr_end = as_text(resp).find("\r\n\r\n");
        // The budget bounds the HEADER scan only. Once CRLFCRLF is in hand everything past
        // it is frame bytes — bounded by `max_frame`, not by this — and a large frame
        // pipelined behind the 101 must not be read as a header block that never ends.
        // Whatever of it did not fit the budget is still on the socket for `serve` to read.
    }

    // Validate against the HEADER BLOCK alone: the tail may be arbitrary frame bytes, and
    // neither the `101` search nor the header scan may take a match out of them.
    const std::string_view header = as_text(resp).substr(0, hdr_end + 4);
    if (header.find("101") == std::string_view::npos) return false;
    const std::string_view accept = header_value(header, "sec-websocket-accept");
    if (accept.empty() || !(ws::accept_key(key) == accept)) return false;

    // Bytes pipelined past the header are the start of the frame stream — they are already
    // off the socket, so `serve` can never read them again. Carry them over, exactly as
    // `ws_server_transport_t::read_handshake` does on the accept side.
    return pipelined.append(resp.data() + hdr_end + 4, resp.size() - hdr_end - 4);
}

/**
 * @brief The dial-out client's end of the shared receive rules (@ref ws_framing_t): the one
 *        connection's stream state, this link's cap and counters, a masked PONG, and
 *        delivery to its one slot.
 */
struct ws_client_transport_t::framing_t {
    ws_client_transport_t& link; /**< @brief The client (its counters, sink and egress). */
    ws_rx_frame_t frame;         /**< @brief The header of the frame being received. */
    ws_assembler_t assembler;    /**< @brief Fragment reassembly. */

    /** @brief The receive cap, resolved from the injected backend and `max_frame` exactly as
     *         the server resolves it: a dialled peer gets no more credit than an accepted
     *         one. */
    [[nodiscard]] std::size_t cap() const {
        return length_prefix_framer_t::effective_cap(*link.backend_, link.max_frame_);
    }
    /** @brief Count a frame that broke the rules (#872, #1060). */
    void malformed() const { link.malformed_rx_.fetch_add(1, std::memory_order_relaxed); }
    /** @brief Count a body the source refused (receiver pays). */
    void dropped() const { link.dropped_rx_.fetch_add(1, std::memory_order_relaxed); }

    /** @brief Answer a PING: a client PONG is masked like client data frames, and built on
     *         the STACK, so the reply cannot fail to be built (#848). */
    void pong(std::span<const std::byte> payload) const {
        std::array<std::byte, ws::kMaxClientControlFrame> pong{};
        const std::size_t plen =
            ws::encode_client_control(pong, ws::opcode_t::PONG, payload, link.next_mask_key());
        // Through the egress like any frame (#2144): never interleaved with a publisher's
        // record, and bounded per turn (#838).
        const ::iovec pv{pong.data(), plen};
        const std::span<const ::iovec> rec(&pv, 1);
        egress_once_t once([&] { return link.frame_of(rec); });
        link.egress_send(once, [&](int wfd, std::uint32_t bound) {
            return link.write_gather(wfd, rec, bound, false);
        });
    }

    /** @brief Hand one whole message up: a single-link rope reaches a span-only sink
     *         borrowed, a fragmented one materializes once, in the slot, from this link's rx
     *         source. */
    void hand_up(tr::view::rope_t message) const {
        link.rx_.deliver_rope(std::move(message), *link.backend_);
    }
};

void ws_client_transport_t::serve(int fd) {
    framing_t end{*this, {}, ws_assembler_t{*backend_}};
    ws_framing_t<framing_t> framing{end};
    // Stack state, not a member: the header being read and the body's segment while it fills
    // live on this receive thread for the life of the connection (ADR-0087 §1).
    // Sized for a masked header too: RFC 6455 forbids a masked server frame, but the shared
    // rules unmask one rather than stall on a header the slot could not hold.
    frame_ingress_t<ws::kMaxClientFrameHeader> ingress;
    // The bytes the server pipelined behind its 101 are the start of the frame stream: a peer
    // that pushes its state on connect and then goes quiet must still get that message
    // delivered, so they are fed before the first wait, then released.
    bool alive = ingress.feed(std::span<const std::byte>(pipelined_.data(), pipelined_.size()),
                              *backend_, framing) >= 0;
    pipelined_ = mem::bytes_t(pipelined_.source());

    // The local stop is checked once per pass, here: a `continue` on a poll timeout comes
    // straight back to it.
    while (alive && !stop_.load(std::memory_order_relaxed)) {
        // One bounded 100 ms wait: readable, and writable too while a frame the socket cut
        // short is held, which resumes it here (#2144).
        const int pr = poll_io(fd);
        if (pr < 0) break;
        if (pr == 0) continue;  // timeout → re-check stop_
        // False for a peer that closed, a read error, a CLOSE, or an RFC 6455 breach
        // (§7.1.7), which is what stops the server we dialled from naming our memory
        // budget (#872).
        alive = ingress.read(fd, *backend_, framing);
    }

    teardown_peer(fd);  // close the egress, then reset and close (stream_endpoint_t)
    // Liveness (#1059): every death of the one connection lands here — peer CLOSE, remote
    // hangup, receive error, RFC 6455 breach, and the local stop — so this is the ONE
    // place link_up() flips false. Cleared BEFORE the departure seam fires, so a notifier
    // that turns around and polls the link reads the state it was just told about.
    connected_.store(false, std::memory_order_relaxed);
    // Departure seam (RFC-0009 §D extended): the one connection died under us — not a
    // local stop — so report the link down (after teardown, no locks held).
    if (!stop_.load(std::memory_order_relaxed)) notify_down();
}

}  // namespace tr::net
