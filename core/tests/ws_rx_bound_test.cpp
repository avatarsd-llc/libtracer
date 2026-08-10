/**
 * @file
 * @brief #872 — the WS DATA path is bounded by the injected seam: a peer may not name the
 *        receiver's memory budget, and every refusal is COUNTED.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The WS transports used to take neither a `mem_backend_t*` nor a `max_frame`, unlike every
 * other framed transport. Inbound bytes accumulated in a plain `std::vector` with no size
 * check while `decode_frame` decoded the full announced 64-bit length and simply waited for
 * that many bytes — so an unauthenticated peer chose how much memory the node would hold,
 * and on the `-fno-exceptions` profile the failed growth is a peer-triggered `abort()`.
 *
 * ## The instrument, and why it is not an inference
 *
 * Every case drives the REAL RX entry point — a raw socket through the real RFC 6455 opening
 * handshake, the `transport_alloc_softfail_test` idiom — never a decoder call. The bound
 * itself is MEASURED, not argued: @ref peak_rss_bytes reads `getrusage(RUSAGE_SELF).ru_maxrss`,
 * the kernel's own high-water mark for this process, around a window in which the test tries
 * to push @ref kAttackBytes at the server. A receiver that follows the declared length moves
 * that watermark by tens of MiB; a receiver that refuses the frame off its HEADER cannot. The
 * two measured cases therefore run FIRST in `main`, before anything else in this TU can lift
 * the watermark and make a later comparison read zero growth vacuously.
 *
 * `ru_maxrss` is monotone and page-granular, which is exactly right here: it cannot be
 * "reset" by a free, so it answers "did this buffer ever exist", not "does it exist now".
 *
 * ## Coverage
 *  - an over-cap declared 64-bit length is refused off the header — RSS-measured (server);
 *  - a legal slow-loris UNDER the cap is not torn down and completes — RSS-measured;
 *  - the honored cap is the CONFIGURED one, not the built-in default (a frame legal under
 *    the 16 MiB default is refused under a 4 KiB `max_frame`, and accepted without it);
 *  - fragments summing past the cap are refused, so the CONT route is not a way around it;
 *  - a bounded backend's exhaustion is `dropped_rx` backpressure, not a dead connection;
 *  - the effective cap is `min(max_frame, backend.max_segment_size())` — the bound comes
 *    from the two injected resources, never a literal;
 *  - the DIAL half gets the identical treatment against a hostile SERVER.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
namespace ws = tr::net::ws;

using tr::testing::check;

// --- the memory instrument --------------------------------------------------

/**
 * @brief This process's PEAK resident set in bytes (`getrusage`'s `ru_maxrss`, KiB on Linux).
 *
 * A high-water mark maintained by the kernel, so it survives the free that a torn-down
 * connection performs — which is the property that makes it an honest answer to "was this
 * buffer ever allocated?" rather than "is it allocated now?".
 */
[[nodiscard]] std::size_t peak_rss_bytes() {
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::size_t>(ru.ru_maxrss) * 1024u;
}

/** @brief How many bytes a case tries to push behind an over-cap declared length. */
constexpr std::size_t kAttackBytes = 64u * 1024u * 1024u;

/** @brief The peak-RSS growth a bounded receiver may show across an attack window.
 *
 * Generous by three orders of magnitude relative to what an UNBOUNDED receiver shows
 * (@ref kAttackBytes), and far above the few pages a torn-down connection can move: the
 * assertion is about the ORDER of the answer, so it needs no tuning to stay meaningful. */
constexpr std::size_t kRssSlack = 16u * 1024u * 1024u;

// --- raw-socket helpers (the transport_alloc_softfail_test idiom) ------------

/** @brief Connect a raw TCP client to `127.0.0.1:port`; -1 on failure. */
int tcp_connect(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/**
 * @brief Write every byte of @p b to @p fd; false once the peer is gone.
 *
 * `MSG_NOSIGNAL` is load-bearing, not hygiene: half these cases exist to make the server
 * CLOSE mid-write, and a default `send` onto a closed socket raises `SIGPIPE`, which would
 * kill the test process instead of failing an assertion.
 */
bool write_bytes(int fd, std::span<const std::byte> b) {
    std::size_t off = 0;
    while (off < b.size()) {
        const ssize_t n = ::send(fd, b.data() + off, b.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

/** @brief Read until @p done or the budget expires; `*closed` is set on a peer FIN/RST. */
template <typename Done>
std::vector<std::byte> read_until(int fd, Done done, std::chrono::milliseconds budget,
                                  bool* closed = nullptr) {
    std::vector<std::byte> buf;
    std::array<std::byte, 4096> chunk;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done(buf) && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, 25) <= 0) continue;
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) {
            if (closed != nullptr) *closed = true;
            break;
        }
        buf.insert(buf.end(), chunk.data(), chunk.data() + n);
    }
    return buf;
}

/** @brief The fixed masking key every client frame this TU builds uses (RFC 6455 §5.3). */
constexpr std::array<std::uint8_t, 4> kMask{0x37, 0xFA, 0x21, 0x3D};

/** @brief Mask one payload byte at stream offset @p i under @ref kMask. */
[[nodiscard]] std::byte mask_at(std::byte b, std::size_t i) {
    return static_cast<std::byte>(std::to_integer<std::uint8_t>(b) ^ kMask[i % 4]);
}

/**
 * @brief A masked client→server frame HEADER that DECLARES @p declared_len payload bytes.
 *
 * The declaration is deliberately decoupled from what the caller then writes — that gap is
 * the whole attack: a peer announces a length it never intends to deliver and lets the
 * receiver hold the difference.
 */
std::vector<std::byte> masked_client_header(ws::opcode_t op, std::uint64_t declared_len,
                                            bool fin = true) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>((fin ? 0x80u : 0x00u) | static_cast<std::uint8_t>(op)));
    if (declared_len < 126) {
        out.push_back(static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(declared_len)));
    } else if (declared_len <= 0xFFFFu) {
        out.push_back(static_cast<std::byte>(0x80u | 126u));
        out.push_back(static_cast<std::byte>((declared_len >> 8) & 0xFFu));
        out.push_back(static_cast<std::byte>(declared_len & 0xFFu));
    } else {
        out.push_back(static_cast<std::byte>(0x80u | 127u));
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<std::byte>((declared_len >> (i * 8)) & 0xFFu));
    }
    for (std::uint8_t m : kMask) out.push_back(static_cast<std::byte>(m));
    return out;
}

/** @brief A complete masked client→server frame carrying exactly @p payload. */
std::vector<std::byte> masked_client_frame(ws::opcode_t op, std::span<const std::byte> payload,
                                           bool fin = true) {
    std::vector<std::byte> out = masked_client_header(op, payload.size(), fin);
    for (std::size_t i = 0; i < payload.size(); ++i) out.push_back(mask_at(payload[i], i));
    return out;
}

/** @brief The RFC 6455 opening-handshake request this TU's raw clients send. */
constexpr std::string_view kUpgradeRequest =
    "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

/** @brief Drive the opening handshake on a raw client fd; true once the 101 is read. */
bool raw_handshake(int cfd) {
    if (!write_bytes(cfd, std::span<const std::byte>(
                              reinterpret_cast<const std::byte*>(kUpgradeRequest.data()),
                              kUpgradeRequest.size())))
        return false;
    const auto resp = read_until(
        cfd,
        [](const std::vector<std::byte>& b) {
            return std::string_view(reinterpret_cast<const char*>(b.data()), b.size())
                       .find("\r\n\r\n") != std::string_view::npos;
        },
        2s);
    return std::string_view(reinterpret_cast<const char*>(resp.data()), resp.size())
               .find("101 Switching Protocols") != std::string_view::npos;
}

/** @brief How many peers @p server currently reports as OPEN (handshaken, named). */
[[nodiscard]] std::size_t open_peers(const tr::net::transport_ws_server& server) {
    std::size_t n = 0;
    server.enumerate_peers([&n](std::string_view) { ++n; });
    return n;
}

/** @brief Block until @p server counts at least @p want OPEN peers. */
[[nodiscard]] bool wait_for_peers(const tr::net::transport_ws_server& server, std::size_t want,
                                  std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        std::size_t n = 0;
        server.enumerate_peers([&n](std::string_view) { ++n; });
        if (n >= want) return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

/** @brief Block until @p probe answers true, or @p budget expires. */
template <typename Probe>
[[nodiscard]] bool wait_until(Probe probe, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (probe()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return probe();
}

/**
 * @brief A collecting span sink (bound by address via `set_receiver(F&)`).
 *
 * Deliveries land on the transport's own recv thread while the test thread reads, so the
 * vector is guarded — this is a cross-thread observation, not a local one.
 */
struct frame_sink_t {
    mutable std::mutex m;                       /**< @brief Guards @ref frames. */
    std::vector<std::vector<std::byte>> frames; /**< @brief One entry per delivered message. */

    /** @brief The receiver callable. */
    void operator()(std::span<const std::byte> f) {
        const std::lock_guard lock(m);
        frames.emplace_back(f.begin(), f.end());
    }
    /** @brief How many messages have been delivered so far. */
    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(m);
        return frames.size();
    }
    /** @brief A copy of the first delivered message (empty if none). */
    [[nodiscard]] std::vector<std::byte> first() const {
        const std::lock_guard lock(m);
        return frames.empty() ? std::vector<std::byte>{} : frames.front();
    }
};

/** @brief The per-connection receive cap the attack cases configure (64 KiB). */
constexpr std::size_t kSmallCap = 64u * 1024u;

// ---------------------------------------------------------------------------
// The two RSS-MEASURED cases. These run first — see the file header.
// ---------------------------------------------------------------------------

/**
 * @brief An over-cap declared length is refused off the HEADER, and the receiver's memory
 *        never follows it.
 *
 * The peer announces a 1 TiB payload in the 64-bit length field and then writes as much of
 * it as the kernel will take. A receiver that trusts the declaration buffers every byte it
 * is given; this one must fail the connection at the header, so the process high-water mark
 * cannot move by anything like what was offered.
 */
void test_oversize_declared_length_is_refused_and_bounded() {
    std::printf("ws server — a 1 TiB declared length is refused off the header (#872):\n");
    tr::net::transport_ws_server server(0, &tr::mem::heap_backend(), /*max_frame=*/kSmallCap);
    check(server.ok(), "server bound");
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");
    check(wait_for_peers(server, 1, 2s), "the server registered the peer as open");

    const std::size_t rss_before = peak_rss_bytes();

    // 1 TiB: three orders past the configured cap and past any plausible heap, so the only
    // way the watermark moves is a receiver that started collecting toward it.
    const std::vector<std::byte> header =
        masked_client_header(ws::opcode_t::BINARY, std::uint64_t{1} << 40);
    check(write_bytes(cfd, header), "the oversize header was written");

    // Offer the body. A bounded receiver has already closed, so this stops after a chunk or
    // two; an unbounded one takes all of it.
    const std::vector<std::byte> body(64u * 1024u, std::byte{0x5A});
    std::size_t pushed = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (pushed < kAttackBytes && std::chrono::steady_clock::now() < deadline) {
        if (!write_bytes(cfd, body)) break;
        pushed += body.size();
    }

    const std::size_t rss_after = peak_rss_bytes();
    const std::size_t growth = rss_after > rss_before ? rss_after - rss_before : 0;
    std::printf("        offered %zu KiB; peak RSS moved %zu KiB\n", pushed / 1024, growth / 1024);

    check(growth < kRssSlack, "the process peak RSS did not follow the declared length");
    check(wait_until([&] { return server.malformed_rx() == 1; }, 2s),
          "malformed_rx ticked exactly once");
    check(server.dropped_rx() == 0, "and nothing was counted as backpressure");
    bool closed = false;
    (void)read_until(cfd, [](const std::vector<std::byte>& b) { return !b.empty(); }, 2s, &closed);
    check(closed, "the server failed the connection");
    check(wait_until([&] { return open_peers(server) == 0; }, 2s), "and recycled the peer's slot");
    ::close(cfd);
}

/**
 * @brief A slow-loris frame UNDER the cap is NOT torn down — it is held, bounded by the cap,
 *        and delivered when the peer finishes it.
 *
 * The complement that keeps the case above from passing for the wrong reason: the fix must
 * bound ingress by the cap, not refuse every incomplete frame. The peer declares exactly the
 * configured cap, dribbles half of it, pauses past several poll rounds, then completes it.
 */
void test_partial_frame_under_the_cap_is_held_then_delivered() {
    std::printf("ws server — a slow-loris frame at the cap is held, bounded, and delivered:\n");
    // The sink is declared BEFORE the transport, so it OUTLIVES it: a transport's recv
    // thread is joined by its destructor, and a sink destroyed first would be delivered
    // into during that window.
    frame_sink_t sink;
    tr::net::transport_ws_server server(0, &tr::mem::heap_backend(), /*max_frame=*/kSmallCap);
    server.set_receiver(sink);
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");
    check(wait_for_peers(server, 1, 2s), "the server registered the peer as open");

    const std::size_t rss_before = peak_rss_bytes();

    std::vector<std::byte> payload(kSmallCap);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i * 7);
    check(write_bytes(cfd, masked_client_header(ws::opcode_t::BINARY, payload.size())),
          "the at-the-cap header was written");

    std::vector<std::byte> first_half;
    for (std::size_t i = 0; i < payload.size() / 2; ++i)
        first_half.push_back(mask_at(payload[i], i));
    check(write_bytes(cfd, first_half), "half the body was written");

    // Several poll rounds (the server's poll bound is 100 ms) with nothing arriving.
    std::this_thread::sleep_for(400ms);
    check(server.malformed_rx() == 0, "a legal partial frame is NOT malformed");
    check(open_peers(server) == 1, "and the connection is still open");

    std::vector<std::byte> second_half;
    for (std::size_t i = payload.size() / 2; i < payload.size(); ++i)
        second_half.push_back(mask_at(payload[i], i));
    check(write_bytes(cfd, second_half), "the rest of the body was written");

    check(wait_until([&] { return sink.count() != 0; }, 3s), "the completed frame arrived");
    check(sink.first() == payload, "with exactly the bytes the peer sent");

    const std::size_t growth = peak_rss_bytes() - rss_before;
    check(growth < kRssSlack, "and holding it cost the cap, not the declared 64-bit space");
    ::close(cfd);
}

// ---------------------------------------------------------------------------
// The cap is the CONFIGURED one
// ---------------------------------------------------------------------------

/**
 * @brief The honored bound comes from `max_frame`, not from a literal: one 8 KiB frame is
 *        refused by a 4 KiB-capped server and accepted by an uncapped one.
 *
 * Both servers see byte-identical input, so the only thing that can explain the two answers
 * is the constructor argument — which is what `:settings max_frame` feeds through
 * `conn_settings_t` (`builtin_transport_ws.cpp`).
 */
void test_the_cap_is_the_configured_one() {
    std::printf("ws server — the refused length tracks `max_frame`, not a built-in number:\n");
    std::vector<std::byte> payload(8192);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i);
    const std::vector<std::byte> frame = masked_client_frame(ws::opcode_t::BINARY, payload);

    {
        // The sink is declared BEFORE the transport, so it OUTLIVES it: a transport's recv
        // thread is joined by its destructor, and a sink destroyed first would be delivered
        // into during that window.
        frame_sink_t sink;
        tr::net::transport_ws_server tight(0, &tr::mem::heap_backend(), /*max_frame=*/4096);
        tight.set_receiver(sink);
        const int cfd = tcp_connect(tight.local_port());
        check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken (4 KiB cap)");
        check(write_bytes(cfd, frame), "the 8 KiB frame was written");
        check(wait_until([&] { return tight.malformed_rx() == 1; }, 2s),
              "the 4 KiB-capped server refused it");
        check(sink.count() == 0, "and delivered nothing");
        ::close(cfd);
    }
    {
        frame_sink_t sink;
        tr::net::transport_ws_server wide(0);  // the default cap (kMaxFrame, 16 MiB)
        wide.set_receiver(sink);
        const int cfd = tcp_connect(wide.local_port());
        check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken (default cap)");
        check(write_bytes(cfd, frame), "the same 8 KiB frame was written");
        check(wait_until([&] { return sink.count() != 0; }, 3s),
              "the default-cap server delivered it");
        check(sink.first() == payload, "byte-for-byte");
        check(wide.malformed_rx() == 0, "and counted nothing malformed");
        ::close(cfd);
    }
}

/**
 * @brief Fragments summing past the cap are refused — the CONT route is not a way around it.
 *
 * Each fragment here is legal on its own (well under the cap), so only a bound applied to
 * the REASSEMBLED total can catch this. Without it a peer walks past any cap 125 bytes at a
 * time and the rope grows without limit.
 */
void test_fragments_cannot_walk_past_the_cap() {
    std::printf("ws server — fragments summing past the cap fail the connection:\n");
    constexpr std::size_t kCap = 4096;
    // The sink is declared BEFORE the transport, so it OUTLIVES it: a transport's recv
    // thread is joined by its destructor, and a sink destroyed first would be delivered
    // into during that window.
    frame_sink_t sink;
    tr::net::transport_ws_server server(0, &tr::mem::heap_backend(), /*max_frame=*/kCap);
    server.set_receiver(sink);
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");

    const std::vector<std::byte> chunk(kCap / 2, std::byte{0x2C});
    check(write_bytes(cfd, masked_client_frame(ws::opcode_t::BINARY, chunk, /*fin=*/false)),
          "fragment 1 (half the cap) written");
    check(write_bytes(cfd, masked_client_frame(ws::opcode_t::CONT, chunk, /*fin=*/false)),
          "fragment 2 (the cap is now exactly reached) written");
    std::this_thread::sleep_for(200ms);
    check(server.malformed_rx() == 0 && sink.count() == 0,
          "neither fragment is refused on its own");

    check(write_bytes(cfd, masked_client_frame(ws::opcode_t::CONT, chunk, /*fin=*/true)),
          "fragment 3 (past the cap) written");
    check(wait_until([&] { return server.malformed_rx() == 1; }, 2s),
          "the over-cap TOTAL is malformed");
    check(sink.count() == 0, "and no message was delivered");
    bool closed = false;
    (void)read_until(cfd, [](const std::vector<std::byte>& b) { return !b.empty(); }, 2s, &closed);
    check(closed, "the connection was failed");
    ::close(cfd);
}

// ---------------------------------------------------------------------------
// The injected backend: it is where the bytes come from, and where the bound comes from
// ---------------------------------------------------------------------------

/**
 * @brief The effective cap is `min(max_frame, backend.max_segment_size())` — read off the
 *        two injected resources, never restated as a number.
 */
void test_effective_cap_is_the_min_of_the_two_seams() {
    std::printf("ws — the effective cap is min(max_frame, backend capacity):\n");
    std::vector<std::byte> slab(64u * 1024u);
    tr::mem::pool_t pool(slab, /*slot_payload=*/4096);

    const tr::net::transport_ws_server pooled(0, &pool, /*max_frame=*/1u << 20);
    check(pooled.effective_max_frame() == pool.max_segment_size(),
          "a pool narrower than max_frame bounds the cap");

    const tr::net::transport_ws_server tight(0, &tr::mem::heap_backend(), /*max_frame=*/1024);
    check(tight.effective_max_frame() == 1024, "a max_frame narrower than the backend wins");

    const tr::net::transport_ws_server plain(0);
    check(plain.effective_max_frame() == tr::net::transport_ws_server::kMaxFrame,
          "and the default is the shared kMaxFrame, one home with tcp/quic/webtransport");
}

/**
 * @brief A refusal from the injected backend is BACKPRESSURE: the message is shed, the
 *        counter ticks, and the connection survives to carry the next one.
 *
 * The pool hands out four 4 KiB slots. A fragmented message whose links outlast the pool
 * makes `over_bytes` answer `nullopt` mid-reassembly — the ADR-0042 §2 refusal channel. That
 * must not look like a protocol violation, and must not cost the link.
 */
void test_backend_exhaustion_is_counted_backpressure() {
    std::printf("ws server — RX-backend exhaustion sheds the message, keeps the link:\n");
    // Four slots of 4 KiB each: the cap is the slot payload, so a five-fragment message of
    // 512-byte pieces stays legal on total length while outrunning the slot count.
    std::vector<std::byte> slab(4u * (4096u + 512u));
    tr::mem::pool_t pool(slab, /*slot_payload=*/4096);
    // Read the slot count ONCE, here, while this is the only thread that can touch the pool:
    // `pool_t` declares "single-threaded reclamation" and from the line below the recv thread
    // owns it, so the loop bound must not re-query it.
    const std::size_t slots = pool.capacity();
    check(slots >= 4, "the pool carved at least four slots");

    // Both the sink and the POOL are declared before the transport, so both outlive it:
    // the recv thread allocates from `pool` and delivers to `sink` right up until the
    // destructor joins it.
    frame_sink_t sink;
    tr::net::transport_ws_server server(0, &pool);
    server.set_receiver(sink);
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");

    const std::vector<std::byte> piece(512, std::byte{0x11});
    check(write_bytes(cfd, masked_client_frame(ws::opcode_t::BINARY, piece, /*fin=*/false)),
          "fragment 1 written");
    for (std::size_t i = 1; i < slots + 1; ++i)
        check(write_bytes(cfd, masked_client_frame(ws::opcode_t::CONT, piece, /*fin=*/false)),
              "another fragment written");

    check(wait_until([&] { return server.dropped_rx() >= 1; }, 3s),
          "the exhausted pool showed up as dropped_rx");
    check(server.malformed_rx() == 0, "and NOT as a protocol violation");

    // The link is still usable: a PING must still be answered.
    check(write_bytes(cfd, masked_client_frame(ws::opcode_t::PING,
                                               std::span<const std::byte>(piece.data(), 4))),
          "a PING was written after the drop");
    const auto pong =
        read_until(cfd, [](const std::vector<std::byte>& b) { return b.size() >= 6; }, 2s);
    const auto dec = ws::decode_frame(pong);
    check(dec.has_value() && dec->first.op == ws::opcode_t::PONG,
          "the connection survived the exhaustion and answered it");
    ::close(cfd);
}

// ---------------------------------------------------------------------------
// The DIAL half — a server we chose to dial is no more trusted than one that dialled us
// ---------------------------------------------------------------------------

/** @brief Accept one client, complete the server side of the RFC 6455 handshake, then send
 *         a frame header declaring @p declared_len and dribble body bytes at it. */
void hostile_ws_server(int lfd, std::uint64_t declared_len) {
    const int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) return;
    const auto req = read_until(
        cfd,
        [](const std::vector<std::byte>& b) {
            return std::string_view(reinterpret_cast<const char*>(b.data()), b.size())
                       .find("\r\n\r\n") != std::string_view::npos;
        },
        2s);
    const std::string_view text(reinterpret_cast<const char*>(req.data()), req.size());
    const std::size_t kpos = text.find("Sec-WebSocket-Key: ");
    if (kpos == std::string_view::npos) {
        ::close(cfd);
        return;
    }
    const std::size_t vstart = kpos + std::string_view("Sec-WebSocket-Key: ").size();
    const std::string key(text.substr(vstart, text.find("\r\n", vstart) - vstart));

    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    resp += ws::accept_key(key);
    resp += "\r\n\r\n";
    (void)write_bytes(cfd, std::span<const std::byte>(
                               reinterpret_cast<const std::byte*>(resp.data()), resp.size()));

    // No pause here. This used to sleep 100 ms so the 101 landed in its own TCP segment,
    // because the client's handshake dropped whatever followed the CRLFCRLF in the same
    // `recv` and the case would then pass or fail on segment boundaries rather than on the
    // bound it tests. That defect is fixed (#1020 — the handshake hands the pipelined bytes
    // to the recv loop), so the mask is gone and this case now runs against BOTH segment
    // layouts: the frame header is free to coalesce with the 101.

    // A SERVER frame is unmasked (RFC 6455 §5.1): FIN|BINARY, the 64-bit length marker,
    // then the declared length — and then nothing like that many bytes.
    std::vector<std::byte> header;
    header.push_back(static_cast<std::byte>(0x80u | 0x2u));
    header.push_back(static_cast<std::byte>(127u));
    for (int i = 7; i >= 0; --i)
        header.push_back(static_cast<std::byte>((declared_len >> (i * 8)) & 0xFFu));
    if (write_bytes(cfd, header)) {
        const std::vector<std::byte> body(64u * 1024u, std::byte{0x33});
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline)
            if (!write_bytes(cfd, body)) break;
    }
    ::close(cfd);
}

/** @brief The client refuses an over-cap declared length from the server it dialled. */
void test_client_refuses_an_oversize_server_frame() {
    std::printf("ws client — a hostile server's 1 TiB frame is refused and counted:\n");
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    check(lfd >= 0, "raw listener created");
    const int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    check(::bind(lfd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "listener bound");
    check(::listen(lfd, 1) == 0, "listener listening");
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&bound), &blen);

    std::thread hostile([lfd] { hostile_ws_server(lfd, std::uint64_t{1} << 40); });

    tr::net::transport_ws_client client("127.0.0.1", ntohs(bound.sin_port),
                                        &tr::mem::heap_backend(), /*max_frame=*/kSmallCap);
    check(client.ok(), "the client completed its opening handshake");
    check(wait_until([&] { return client.malformed_rx() == 1; }, 5s),
          "the client counted the over-cap frame as malformed");
    check(client.dropped_rx() == 0, "and counted nothing as backpressure");

    hostile.join();
    ::close(lfd);
}

}  // namespace

int main() {
    std::printf("#872 — WS RX is bounded by the injected backend + max_frame seam\n");
    // The two RSS-measured cases run FIRST: `ru_maxrss` is a process-wide high-water mark,
    // so anything that allocated before them would mask the growth they exist to detect.
    test_oversize_declared_length_is_refused_and_bounded();
    test_partial_frame_under_the_cap_is_held_then_delivered();
    test_the_cap_is_the_configured_one();
    test_fragments_cannot_walk_past_the_cap();
    test_effective_cap_is_the_min_of_the_two_seams();
    test_backend_exhaustion_is_counted_backpressure();
    test_client_refuses_an_oversize_server_frame();
    return tr::testing::summary("ws_rx_bound");
}
