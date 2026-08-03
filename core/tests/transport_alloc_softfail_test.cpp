/**
 * @file
 * @brief #848 — the transport EGRESS framing path never aborts on a tight heap: a refusing
 *        allocator must produce a DROPPED FRAME AND A LIVE NODE, never `abort()`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ## Why this TU owns `operator new` rather than leaning on `probe_fail_hook`
 *
 * `tr::detail::probe_fail_hook` (`%mem_heap.hpp`) is consulted only inside `probe_bytes` /
 * `probe_hook_ok` — i.e. only by code that ALREADY went through a `try_*` seam. An
 * **unguarded** `std::vector::reserve`/`resize`/`push_back` never touches it, so a test built
 * on that hook alone passes vacuously on the broken code at exactly the sites that matter.
 * These tests therefore fail the REAL allocator: the TU overrides the global
 * `operator new`/`delete` family with a fail-the-k-th-allocation injector, which is the one
 * thing every allocation on the path goes through. The hook seam is exercised too (
 * `iov_table_t::acquire` honours it), so the cheaper injection point stays live and tested.
 *
 * The arming state is `thread_local`, so only the test thread's allocations are affected —
 * the transports' recv threads run untouched, and the test can drive a live socket pair while
 * its own `send()` call sees an exhausted heap.
 *
 * A host build has exceptions ON, so an unguarded allocation failure surfaces here as an
 * escaping `std::bad_alloc`. That is the SAME failure the shipping `-fno-exceptions` MCU
 * profile turns into `abort()`: an escaping `bad_alloc` at injection point k is a mechanical
 * proof that allocation k on that path is unguarded.
 *
 * Coverage — one behavioural case per transport, plus the RFC 6455 §5.5 control-frame rules
 * that make the PONG reply unfailable rather than droppable:
 *   - ws server `send(span)` / `send(iov)` / the directed peer facade (A2, A3, B1, B2);
 *   - ws client `send(span)` — the one nothrow twin (A5);
 *   - tcp `send(iov)` and the multi-peer broadcast scratch;
 *   - udp `send(iov)`;
 *   - can `encode_advertise_header` (A1 — the allocation is DELETED, not guarded);
 *   - the oversized / non-final control frame fails the connection instead of being echoed.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/can.hpp"
#include "libtracer/iov_table.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/transport_can.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"

// --- the fail-the-k-th-allocation injector (this TU owns the override) -------

namespace {

thread_local bool t_armed = false;      /**< @brief Count/fail only on the arming thread. */
thread_local std::size_t t_allocs = 0;  /**< @brief Allocations seen since arming. */
thread_local std::size_t t_fail_at = 0; /**< @brief Fail this allocation index; 0 = never. */

/** @brief `malloc`, but counted and (when armed at index @ref t_fail_at) refused. */
void* counted_alloc(std::size_t size, std::size_t align) {
    if (t_armed) {
        ++t_allocs;
        if (t_fail_at != 0 && t_allocs == t_fail_at) return nullptr;
    }
    const std::size_t n = size != 0 ? size : 1;
    if (align <= alignof(std::max_align_t)) return std::malloc(n);
    // aligned_alloc requires a size that is a whole multiple of the alignment.
    return std::aligned_alloc(align, ((n + align - 1) / align) * align);
}

}  // namespace

void* operator new(std::size_t size) {
    void* p = counted_alloc(size, alignof(std::max_align_t));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    void* p = counted_alloc(size, alignof(std::max_align_t));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return counted_alloc(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_alloc(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t a) {
    void* p = counted_alloc(size, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size, std::align_val_t a) {
    void* p = counted_alloc(size, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_alloc(size, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_alloc(size, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

using namespace std::chrono_literals;
namespace ws = tr::net::ws;
namespace can = tr::net::can;

int g_failures = 0;

/** @brief Record one assertion result. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief One armed window: allocation @p fail_at on THIS thread returns null / throws. */
struct arm_t {
    explicit arm_t(std::size_t fail_at) noexcept {
        t_allocs = 0;
        t_fail_at = fail_at;
        t_armed = true;
    }
    ~arm_t() {
        t_armed = false;
        t_fail_at = 0;
    }
    arm_t(const arm_t&) = delete;
    arm_t& operator=(const arm_t&) = delete;
};

/**
 * @brief Run @p op with allocation @p k refused; report whether the failure ESCAPED.
 *
 * An escape is the defect: on the MCU profile the same failure is `abort()`.
 */
template <class F>
bool escapes_at(std::size_t k, F&& op) {
    try {
        const arm_t arm(k);
        op();
    } catch (const std::bad_alloc&) {
        return true;
    }
    return false;
}

/** @brief Run @p op with the allocator counting but never refusing; return the count. */
template <class F>
std::size_t count_allocs(F&& op) {
    const arm_t arm(0);
    op();
    return t_allocs;
}

/** @brief Sweep injection points 1..@p n and report how many ESCAPED. */
template <class F>
std::size_t escape_sweep(std::size_t n, F&& op) {
    std::size_t escaped = 0;
    for (std::size_t k = 1; k <= n; ++k)
        if (escapes_at(k, op)) ++escaped;
    return escaped;
}

/** @brief `tr::detail::probe_fail_hook` set for the guard's lifetime, cleared after. */
struct hook_guard_t {
    explicit hook_guard_t(bool (*hook)(std::size_t) noexcept) noexcept {
        tr::detail::probe_fail_hook = hook;
    }
    ~hook_guard_t() { tr::detail::probe_fail_hook = nullptr; }
    hook_guard_t(const hook_guard_t&) = delete;
    hook_guard_t& operator=(const hook_guard_t&) = delete;
};

/** @brief A frame sink a transport receiver pushes into (count + last bytes). */
struct sink_t {
    mutable std::mutex m;
    std::size_t count = 0;
    std::vector<std::byte> last;

    void push(std::span<const std::byte> f) {
        const std::lock_guard lock(m);
        ++count;
        last.assign(f.begin(), f.end());
    }
    [[nodiscard]] std::size_t n() const {
        const std::lock_guard lock(m);
        return count;
    }
    [[nodiscard]] std::vector<std::byte> bytes() const {
        const std::lock_guard lock(m);
        return last;
    }
    [[nodiscard]] bool wait_for(std::size_t want, std::chrono::milliseconds budget) const {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (n() >= want) return true;
            std::this_thread::sleep_for(5ms);
        }
        return n() >= want;
    }
};

// --- raw-socket helpers (the ws_transport_test idiom) -----------------------

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

/** @brief Write every byte of @p b to @p fd. */
void write_bytes(int fd, std::span<const std::byte> b) {
    std::size_t off = 0;
    while (off < b.size()) {
        const ssize_t n = ::send(fd, b.data() + off, b.size() - off, 0);
        if (n <= 0) return;
        off += static_cast<std::size_t>(n);
    }
}

/** @brief Read until @p done or the budget expires; `*closed` is set on a clean peer FIN. */
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
            // poll() said readable, so this is a real end-of-connection: a clean FIN (0) or
            // an RST (-1), which is what a close with unread bytes still queued produces.
            if (closed != nullptr) *closed = true;
            break;
        }
        buf.insert(buf.end(), chunk.data(), chunk.data() + n);
    }
    return buf;
}

/** @brief A MASKED client→server frame (RFC 6455 §5.3), 7-bit or 16-bit length. */
std::vector<std::byte> masked_client_frame(ws::opcode_t op, std::span<const std::byte> payload,
                                           bool fin = true) {
    static constexpr std::array<std::uint8_t, 4> mask{0x37, 0xFA, 0x21, 0x3D};
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>((fin ? 0x80u : 0x00u) | static_cast<std::uint8_t>(op)));
    const std::size_t len = payload.size();
    if (len < 126) {
        out.push_back(static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(len)));
    } else {
        out.push_back(static_cast<std::byte>(0x80u | 126u));
        out.push_back(static_cast<std::byte>((len >> 8) & 0xFFu));
        out.push_back(static_cast<std::byte>(len & 0xFFu));
    }
    for (std::uint8_t m : mask) out.push_back(static_cast<std::byte>(m));
    for (std::size_t i = 0; i < len; ++i)
        out.push_back(
            static_cast<std::byte>(std::to_integer<std::uint8_t>(payload[i]) ^ mask[i % 4]));
    return out;
}

/** @brief Drive the RFC 6455 opening handshake on a raw client fd; true on 101. */
bool raw_handshake(int cfd) {
    static constexpr std::string_view kUpgrade =
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    write_bytes(cfd, std::span<const std::byte>(reinterpret_cast<const std::byte*>(kUpgrade.data()),
                                                kUpgrade.size()));
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

/**
 * @brief Block until @p server counts at least @p want OPEN peers.
 *
 * `raw_handshake` returning is NOT enough to make a subsequent `server.send` observable.
 * `service_peer` writes the 101 response and only THEN stores `open = true`, so between
 * those two points the client has read "101 Switching Protocols" while the broadcast still
 * skips the slot — the send goes to nobody and the frame never arrives. `enumerate_peers`
 * visits exactly the open, named slots, which is the same predicate `send` broadcasts over,
 * so it is the precise barrier. (The peer's routable `p<slot>` name is stamped at accept
 * regardless of the server's `peer_named` setting, so this works on a default server too.)
 */
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

/** @brief > `kMaxInlineIov` (16) + the header entry, so the egress table must overflow. */
constexpr std::size_t kWideSpans = 24;

/** @brief The wire size of a `kWideSpans`-byte server frame: a 2-byte header (len < 126). */
constexpr std::size_t kWideFrameBytes = kWideSpans + 2;

/** @brief A gather of @p n one-byte spans over @p storage — a deliberately wide iov. */
std::vector<std::span<const std::byte>> wide_gather(const std::vector<std::byte>& storage,
                                                    std::size_t n) {
    std::vector<std::span<const std::byte>> iov;
    iov.reserve(n);
    for (std::size_t i = 0; i < n; ++i) iov.emplace_back(storage.data() + i, 1);
    return iov;
}

// ---------------------------------------------------------------------------
// WS server (A2, A3, B1, B2)
// ---------------------------------------------------------------------------

/**
 * @brief A2/A3: the single-span server send now rides the gathered path, so it allocates
 *        NOTHING — it used to build a whole `std::vector` frame on every send.
 */
void test_ws_server_send_span_is_allocation_free() {
    std::printf("ws server send(span) — allocation-free (A2/A3):\n");
    tr::net::transport_ws_server server(0);
    check(server.ok(), "server bound");
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");
    check(wait_for_peers(server, 1, 2s), "the server registered the peer as open");

    std::array<std::byte, 8> payload{};
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i + 1);

    std::size_t allocs = 0;
    const bool escaped = escapes_at(1, [&] {
        server.send(std::span<const std::byte>(payload));
        allocs = t_allocs;
    });
    check(!escaped, "send(span) with the FIRST allocation refused does not throw");
    check(allocs == 0, "send(span) performs ZERO heap allocations");

    const auto got =
        read_until(cfd, [](const std::vector<std::byte>& b) { return b.size() >= 10; }, 2s);
    const auto dec = ws::decode_frame(got);
    check(dec.has_value() && dec->first.payload.size() == payload.size() &&
              std::memcmp(dec->first.payload.data(), payload.data(), payload.size()) == 0,
          "the peer received the frame intact");
    ::close(cfd);
}

/**
 * @brief B1/B2: an overflowing gather — whose entry count is the SENDING peer's choice —
 *        drops the frame and leaves the node live. It used to `resize()` a throwing vector.
 */
void test_ws_server_send_iov_overflow_drops() {
    std::printf("ws server send(iov) — overflow gather drops, node lives (B1/B2):\n");
    tr::net::transport_ws_server server(0);
    check(server.ok(), "server bound");
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");
    check(wait_for_peers(server, 1, 2s), "the server registered the peer as open");

    const std::vector<std::byte> storage(kWideSpans, std::byte{0xAB});
    const auto iov = wide_gather(storage, kWideSpans);
    const auto send_wide = [&] { server.send(std::span<const std::span<const std::byte>>(iov)); };

    // How many allocations this send makes bounds the sweep: injecting past the last one
    // would merely let the send succeed and make the "nothing arrived" assertion vacuous.
    const std::size_t n_allocs = count_allocs(send_wide);
    check(n_allocs >= 1, "the wide gather really does reach the overflow store");
    const auto baseline = read_until(
        cfd, [](const std::vector<std::byte>& b) { return b.size() >= kWideFrameBytes; }, 2s);
    check(baseline.size() == kWideFrameBytes, "the unrefused baseline frame arrived whole");

    // EVERY injection point on the send path, not just the first.
    const std::size_t escaped = escape_sweep(n_allocs, send_wide);
    check(escaped == 0, "no allocation failure escapes send(iov) at any injection point");

    // The probe_fail_hook seam iov_table_t::acquire honours reaches the same drop leg.
    {
        const hook_guard_t hook([](std::size_t) noexcept { return false; });
        send_wide();
    }
    const auto nothing =
        read_until(cfd, [](const std::vector<std::byte>& b) { return !b.empty(); }, 300ms);
    check(nothing.empty(), "every refused frame was DROPPED, not truncated onto the wire");

    // ... and the node is still live: the next send, unrefused, arrives whole.
    send_wide();
    const auto got = read_until(
        cfd, [](const std::vector<std::byte>& b) { return b.size() >= kWideFrameBytes; }, 2s);
    const auto dec = ws::decode_frame(got);
    check(dec.has_value() && dec->first.payload.size() == kWideSpans,
          "the node is LIVE: the next gather arrives whole");
    ::close(cfd);
}

/** @brief The directed per-peer facade takes the same overflow store, and the same answer. */
void test_ws_peer_endpoint_send_iov_overflow_drops() {
    std::printf("ws peer_endpoint send(iov) — overflow gather drops, node lives (A3/B1):\n");
    tr::net::transport_ws_server server(0, /*max_peers=*/0, /*peer_named=*/true);
    check(server.ok(), "peer-named server bound");
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");

    tr::net::transport_t* peer = nullptr;
    for (int i = 0; i < 200 && peer == nullptr; ++i) {
        peer = server.peer_link("p0");
        if (peer == nullptr) std::this_thread::sleep_for(10ms);
    }
    check(peer != nullptr, "the peer resolved by name");
    if (peer == nullptr) {
        ::close(cfd);
        return;
    }

    const std::vector<std::byte> storage(kWideSpans, std::byte{0xCD});
    const auto iov = wide_gather(storage, kWideSpans);
    const auto send_wide = [&] { peer->send(std::span<const std::span<const std::byte>>(iov)); };

    const std::size_t n_allocs = count_allocs(send_wide);
    check(n_allocs >= 1, "the wide gather really does reach the overflow store");
    const auto baseline = read_until(
        cfd, [](const std::vector<std::byte>& b) { return b.size() >= kWideFrameBytes; }, 2s);
    check(baseline.size() == kWideFrameBytes, "the unrefused baseline frame arrived whole");

    const std::size_t escaped = escape_sweep(n_allocs, send_wide);
    check(escaped == 0, "no allocation failure escapes the directed send(iov)");
    const auto nothing =
        read_until(cfd, [](const std::vector<std::byte>& b) { return !b.empty(); }, 300ms);
    check(nothing.empty(), "every refused directed frame was DROPPED");

    send_wide();
    const auto got = read_until(
        cfd, [](const std::vector<std::byte>& b) { return b.size() >= kWideFrameBytes; }, 2s);
    const auto dec = ws::decode_frame(got);
    check(dec.has_value() && dec->first.payload.size() == kWideSpans,
          "the node is LIVE: the next directed gather arrives whole");
    ::close(cfd);
}

// ---------------------------------------------------------------------------
// WS client (A5 — the one surviving nothrow twin)
// ---------------------------------------------------------------------------

/**
 * @brief A5: the masked client frame is the ONE WS egress buffer that survives (a client
 *        frame's wire bytes are not the caller's bytes), so it is the nothrow twin over a
 *        reused buffer — exhaustion DROPS the frame, and the link keeps working after.
 */
void test_ws_client_send_drops_then_recovers() {
    std::printf("ws client send(span) — nothrow twin drops, link recovers (A5):\n");
    sink_t at_server;
    auto srv_rx = [&](std::span<const std::byte> f) { at_server.push(f); };
    tr::net::transport_ws_server server(0);
    check(server.ok(), "server bound");
    server.set_receiver(srv_rx);

    const std::vector<std::byte> payload(64, std::byte{0x5A});

    // How many allocations a COLD send makes bounds the sweep below. Measured on a client of
    // its own: `tx_buf_` keeps its block between sends, so a count taken from an already-warm
    // client would be 0 and every injection point would be vacuous.
    std::size_t n_allocs = 0;
    {
        tr::net::transport_ws_client probe("127.0.0.1", server.local_port());
        check(probe.ok(), "ws probe client handshaken");
        n_allocs = count_allocs([&] { probe.send(payload); });
        check(at_server.wait_for(1, 2s), "the unrefused baseline frame arrived");
    }
    // The whole point of moving this path off `tr::detail::try_reserve`: that helper probed
    // with a nothrow allocation and then ran a THROWING `std::vector::reserve` behind it, so a
    // cold encode had TWO allocation points and only the first was refusable — refusing the
    // second crossed `try_reserve`'s own `noexcept` and called std::terminate (an abort no
    // test can catch, and a bare abort() under -fno-exceptions). One allocation here means
    // there is no unguardable second step left to refuse.
    check(n_allocs == 1,
          "a cold send makes exactly ONE refusable allocation (no unguardable second step)");

    // EVERY injection point, each on a client whose tx_buf_ is still COLD.
    for (std::size_t k = 1; k <= n_allocs; ++k) {
        tr::net::transport_ws_client client("127.0.0.1", server.local_port());
        check(client.ok(), "ws client handshaken");
        const bool escaped = escapes_at(k, [&] { client.send(payload); });
        check(!escaped, "send with the frame-buffer growth refused does not throw");
    }
    std::this_thread::sleep_for(150ms);
    check(at_server.n() == 1, "every refused frame was DROPPED (only the baseline arrived)");

    tr::net::transport_ws_client client("127.0.0.1", server.local_port());
    check(client.ok(), "ws client handshaken");
    client.send(payload);
    check(at_server.wait_for(2, 2s), "the node is LIVE: the next send arrives");
    check(at_server.bytes() == payload, "and its bytes are intact");
}

// ---------------------------------------------------------------------------
// TCP (transport_tcp.cpp:78 and :358 — the sites the issue's inventory missed)
// ---------------------------------------------------------------------------

/** @brief The identical overflow gather over TCP: a wide iov drops, then the link recovers. */
void test_tcp_send_iov_overflow_drops() {
    std::printf("tcp send(iov) — overflow gather drops, node lives:\n");
    sink_t at_listener;
    auto listener_rx = [&](std::span<const std::byte> f) { at_listener.push(f); };
    tr::net::tcp_transport_t listener(std::uint16_t{0});
    check(listener.ok(), "listener bound");
    listener.set_receiver(listener_rx);
    tr::net::tcp_transport_t dialer("127.0.0.1", listener.local_port());
    check(dialer.ok(), "dialer connected");

    const std::vector<std::byte> storage(kWideSpans, std::byte{0x11});
    const auto iov = wide_gather(storage, kWideSpans);
    const auto send_wide = [&] { dialer.send(std::span<const std::span<const std::byte>>(iov)); };

    const std::size_t n_allocs = count_allocs(send_wide);
    check(n_allocs >= 1, "the wide gather really does reach the overflow store");
    check(at_listener.wait_for(1, 2s), "the unrefused baseline record arrived");

    const std::size_t escaped = escape_sweep(n_allocs, send_wide);
    check(escaped == 0, "no allocation failure escapes tcp send(iov)");
    std::this_thread::sleep_for(200ms);
    check(at_listener.n() == 1, "every refused record was DROPPED, not truncated");

    send_wide();
    check(at_listener.wait_for(2, 2s), "the node is LIVE: the next record arrives");
    check(at_listener.bytes().size() == kWideSpans, "and it is the whole gather");
}

/** @brief The multi-peer broadcast's per-peer scratch table takes the same treatment. */
void test_tcp_server_broadcast_scratch_drops() {
    std::printf("tcp server broadcast — scratch gather drops, node lives:\n");
    sink_t at_peer;
    auto peer_rx = [&](std::span<const std::byte> f) { at_peer.push(f); };
    tr::net::transport_tcp_server server(0);
    check(server.ok(), "tcp server bound");
    tr::net::tcp_transport_t peer("127.0.0.1", server.local_port());
    check(peer.ok(), "peer connected");
    peer.set_receiver(peer_rx);
    std::this_thread::sleep_for(150ms);  // let the accept complete

    const std::vector<std::byte> storage(kWideSpans, std::byte{0x22});
    const auto iov = wide_gather(storage, kWideSpans);
    const auto send_wide = [&] { server.send(std::span<const std::span<const std::byte>>(iov)); };

    const std::size_t n_allocs = count_allocs(send_wide);
    check(n_allocs >= 2, "both the record table AND the per-peer scratch reached the store");
    check(at_peer.wait_for(1, 2s), "the unrefused baseline broadcast arrived");

    const std::size_t escaped = escape_sweep(n_allocs, send_wide);
    check(escaped == 0, "no allocation failure escapes the tcp broadcast");
    std::this_thread::sleep_for(200ms);
    check(at_peer.n() == 1, "every refused broadcast was DROPPED");

    send_wide();
    check(at_peer.wait_for(2, 2s), "the node is LIVE: the next broadcast arrives");
}

// ---------------------------------------------------------------------------
// UDP (transport_udp.cpp:103)
// ---------------------------------------------------------------------------

/** @brief The datagram gather's overflow store: refuse it and the datagram drops. */
void test_udp_send_iov_overflow_drops() {
    std::printf("udp send(iov) — overflow gather drops, node lives:\n");
    sink_t at_b;
    auto b_rx = [&](std::span<const std::byte> f) { at_b.push(f); };
    tr::net::udp_transport_t a(47140, "127.0.0.1", 47141);
    tr::net::udp_transport_t b(47141, "127.0.0.1", 47140);
    check(a.ok() && b.ok(), "udp pair bound");
    b.set_receiver(b_rx);

    const std::vector<std::byte> storage(kWideSpans, std::byte{0x33});
    const auto iov = wide_gather(storage, kWideSpans);
    const auto send_wide = [&] { a.send(std::span<const std::span<const std::byte>>(iov)); };

    const std::size_t n_allocs = count_allocs(send_wide);
    check(n_allocs >= 1, "the wide gather really does reach the overflow store");
    check(at_b.wait_for(1, 2s), "the unrefused baseline datagram arrived");

    const std::size_t escaped = escape_sweep(n_allocs, send_wide);
    check(escaped == 0, "no allocation failure escapes udp send(iov)");
    std::this_thread::sleep_for(200ms);
    check(at_b.n() == 1, "every refused datagram was DROPPED, not truncated");

    send_wide();
    check(at_b.wait_for(2, 2s), "the node is LIVE: the next datagram arrives");
    check(at_b.bytes().size() == kWideSpans, "and it is the whole gather");
}

// ---------------------------------------------------------------------------
// CAN (A1 — the allocation is DELETED, not made failable)
// ---------------------------------------------------------------------------

/** @brief A `can_link_t` that just records what was written. */
class recording_link_t final : public tr::net::can_link_t {
   public:
    void write_raw(const tr::net::can_frame_data_t& f) override {
        const std::lock_guard lock(m_);
        frames_.push_back(f);
    }
    void on_receive(rx_fn_t rx) override { rx_ = std::move(rx); }

    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(m_);
        return frames_.size();
    }
    /** @brief The concatenated data bytes of every recorded frame, in order. */
    [[nodiscard]] std::vector<std::byte> stream() const {
        const std::lock_guard lock(m_);
        std::vector<std::byte> out;
        for (const tr::net::can_frame_data_t& f : frames_)
            for (std::size_t i = 0; i < f.len; ++i) out.push_back(f.data[i]);
        return out;
    }

   private:
    mutable std::mutex m_;
    std::vector<tr::net::can_frame_data_t> frames_;
    rx_fn_t rx_;
};

/**
 * @brief A1: the advertise header encoder is on the STACK — under a refusing allocator it
 *        neither throws nor allocates, and it still produces the exact same 18 bytes.
 *
 * `emit_advertise` is unconditional in `send_impl`, so this used to be a `std::vector` on
 * EVERY CAN send. It is deleted, not guarded: there is no drop leg left to test.
 */
void test_can_advertise_header_is_allocation_free() {
    std::printf("can encode_advertise_header — allocation-free, byte-identical (A1):\n");
    can::advertise_t adv;
    adv.can_id = 0x1234;
    adv.group = true;
    adv.group_total_len = 4096;
    adv.slice_count = 7;
    adv.target = 42;
    adv.path = "node/sensor/temperature";

    const std::vector<std::byte> whole = can::encode_advertise(adv);  // unarmed reference
    check(whole.size() == can::kAdvertiseHeaderSize + adv.path.size(), "reference frame encoded");

    std::array<std::byte, can::kAdvertiseHeaderSize> header{};
    bool ok = false;
    std::size_t allocs = 0;
    const bool escaped = escapes_at(1, [&] {
        ok = can::encode_advertise_header(header, adv);
        allocs = t_allocs;
    });
    check(!escaped, "encode_advertise_header with the first allocation refused does not throw");
    check(allocs == 0, "encode_advertise_header performs ZERO heap allocations");
    check(ok && std::memcmp(header.data(), whole.data(), header.size()) == 0,
          "and the 18 header bytes are identical to the contiguous encoder's");
}

/** @brief The documented `kAdvertiseMaxPathLen` bound is now ENFORCED, not just written down:
 *         an over-long path encodes nothing and emits nothing (it used to truncate silently). */
void test_can_over_long_path_refused() {
    std::printf("can advertise — an over-long path is refused, not silently truncated:\n");
    can::advertise_t adv;
    adv.can_id = 0x7;
    adv.path = std::string(can::kAdvertiseMaxPathLen + 1, 'x');
    std::array<std::byte, can::kAdvertiseHeaderSize> header{};
    check(!can::encode_advertise_header(header, adv), "encode_advertise_header refuses it");
    check(can::encode_advertise(adv).empty(), "encode_advertise returns no bytes");

    auto link = std::make_unique<recording_link_t>();
    recording_link_t* raw = link.get();
    tr::net::transport_can_config_t cfg;
    cfg.node = 3;
    cfg.path = adv.path;  // the hello at join carries it
    const tr::net::transport_can can_tx(std::move(link), cfg);
    check(raw->count() == 0, "the join hello emits NOTHING rather than an undecodable frame");
}

/** @brief The advertise a real CAN send emits is byte-identical to the contiguous encoder's,
 *         proving the two-source stack walk did not change the wire. */
void test_can_emit_advertise_wire_identical() {
    std::printf("can emit_advertise — the sliced stack walk matches the contiguous encoder:\n");
    auto link = std::make_unique<recording_link_t>();
    recording_link_t* raw = link.get();
    tr::net::transport_can_config_t cfg;
    cfg.node = 5;
    cfg.path = "node5/telemetry";
    const tr::net::transport_can can_tx(std::move(link), cfg);

    can::advertise_t hello;
    hello.can_id = can::encode_can_id({cfg.version, cfg.node, 0});
    hello.group = false;
    hello.group_total_len = 0;
    hello.slice_count = 0;
    hello.path = cfg.path;
    const std::vector<std::byte> expect = can::encode_advertise(hello);
    check(raw->stream() == expect, "the emitted hello byte stream equals encode_advertise(hello)");
}

// ---------------------------------------------------------------------------
// RFC 6455 §5.5 — the PONG is unfailable because the control frame is bounded
// ---------------------------------------------------------------------------

/** @brief A legal (<= 125 B) PING is answered with a PONG carrying exactly those bytes —
 *         built on the stack, so there is no allocation to fail. */
void test_ws_ping_at_the_bound_is_answered() {
    std::printf("ws server — a 125-byte PING is answered with an identical PONG:\n");
    tr::net::transport_ws_server server(0);
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");

    std::vector<std::byte> payload(ws::kMaxControlPayload);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i);
    write_bytes(cfd, masked_client_frame(ws::opcode_t::PING, payload));

    const auto got = read_until(
        cfd, [](const std::vector<std::byte>& b) { return b.size() >= ws::kMaxControlPayload + 2; },
        2s);
    const auto dec = ws::decode_frame(got);
    check(dec.has_value() && dec->first.op == ws::opcode_t::PONG && dec->first.payload == payload,
          "the PONG echoes the 125-byte payload exactly");
    ::close(cfd);
}

/** @brief An OVERSIZED control frame fails the connection (RFC 6455 §5.5/§7.1.7) instead of
 *         being echoed — which is what removed both the abort and the amplification. */
void test_ws_oversized_ping_fails_the_connection() {
    std::printf("ws server — an oversized PING fails the connection, it is NOT echoed:\n");
    tr::net::transport_ws_server server(0);
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");

    const std::vector<std::byte> payload(4096, std::byte{0x77});
    write_bytes(cfd, masked_client_frame(ws::opcode_t::PING, payload));

    bool closed = false;
    const auto got =
        read_until(cfd, [](const std::vector<std::byte>& b) { return !b.empty(); }, 2s, &closed);
    check(got.empty(), "no PONG (and so no 4 KiB reflection) came back");
    check(closed, "the server failed the connection");
    ::close(cfd);
}

/** @brief A FRAGMENTED control frame is equally illegal (RFC 6455 §5.5). */
void test_ws_fragmented_control_fails_the_connection() {
    std::printf("ws server — a non-final control frame fails the connection:\n");
    tr::net::transport_ws_server server(0);
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0 && raw_handshake(cfd), "raw client handshaken");

    const std::vector<std::byte> payload(8, std::byte{0x01});
    write_bytes(cfd, masked_client_frame(ws::opcode_t::PING, payload, /*fin=*/false));

    bool closed = false;
    const auto got =
        read_until(cfd, [](const std::vector<std::byte>& b) { return !b.empty(); }, 2s, &closed);
    check(got.empty() && closed, "no PONG, and the connection was failed");
    ::close(cfd);
}

/** @brief The stack control encoders emit exactly what the contiguous encoders do — the split
 *         changed where the bytes are built, never what they are. */
void test_control_encoders_are_byte_identical() {
    std::printf("ws control encoders — stack form == contiguous form:\n");
    std::vector<std::byte> payload(ws::kMaxControlPayload);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i * 3);

    std::array<std::byte, ws::kMaxServerControlFrame> srv{};
    const std::size_t slen = ws::encode_server_control(srv, ws::opcode_t::PONG, payload);
    const std::vector<std::byte> srv_ref = ws::encode_frame(ws::opcode_t::PONG, payload);
    check(slen == srv_ref.size() && std::memcmp(srv.data(), srv_ref.data(), slen) == 0,
          "encode_server_control == encode_frame");

    std::array<std::byte, ws::kMaxClientControlFrame> cli{};
    const std::size_t clen =
        ws::encode_client_control(cli, ws::opcode_t::PONG, payload, 0xDEADBEEFu);
    const std::vector<std::byte> cli_ref =
        ws::encode_client_frame(ws::opcode_t::PONG, payload, 0xDEADBEEFu);
    check(clen == cli_ref.size() && std::memcmp(cli.data(), cli_ref.data(), clen) == 0,
          "encode_client_control == encode_client_frame");

    // Over the bound both refuse rather than write past the buffer.
    const std::vector<std::byte> too_big(ws::kMaxControlPayload + 1, std::byte{0});
    check(ws::encode_server_control(srv, ws::opcode_t::PONG, too_big) == 0 &&
              ws::encode_client_control(cli, ws::opcode_t::PONG, too_big, 1) == 0,
          "both refuse a payload over the §5.5 bound");
}

/** @brief The nothrow client-frame twin: identical bytes to the throwing form, and it
 *         soft-fails (rather than throwing) when the buffer cannot grow. */
void test_try_encode_client_frame() {
    std::printf("ws try_encode_client_frame — same bytes, soft-fails on exhaustion:\n");
    const std::vector<std::byte> payload(300, std::byte{0x9C});  // the 16-bit length encoding
    const std::vector<std::byte> ref =
        ws::encode_client_frame(ws::opcode_t::BINARY, payload, 0x01020304u);

    tr::mem::block_array_t<std::byte> out(tr::mem::heap_source());
    const std::size_t n =
        ws::try_encode_client_frame(out, ws::opcode_t::BINARY, payload, 0x01020304u);
    check(n == ref.size(), "the nothrow twin succeeds on a healthy heap");
    check(n == ref.size() && std::memcmp(out.data(), ref.data(), n) == 0,
          "and produces exactly the throwing form's bytes");

    // A cold encode must have exactly ONE refusable allocation. The previous
    // `std::vector` + `tr::detail::try_reserve` form had two — a nothrow probe and the
    // throwing `reserve` behind it — and refusing the second escaped `try_reserve`'s
    // `noexcept` into std::terminate. A k=1-only test could never see that; the sweep can.
    tr::mem::block_array_t<std::byte> probe(tr::mem::heap_source());
    const std::size_t n_allocs = count_allocs([&] {
        (void)ws::try_encode_client_frame(probe, ws::opcode_t::BINARY, payload, 0x01020304u);
    });
    check(n_allocs == 1, "a cold encode makes exactly ONE refusable allocation");

    std::size_t escaped = 0;
    std::size_t wrote_despite_refusal = 0;
    for (std::size_t k = 1; k <= n_allocs; ++k) {
        tr::mem::block_array_t<std::byte> fresh(tr::mem::heap_source());
        std::size_t got = 1;
        if (escapes_at(k, [&] {
                got =
                    ws::try_encode_client_frame(fresh, ws::opcode_t::BINARY, payload, 0x01020304u);
            }))
            ++escaped;
        if (got != 0) ++wrote_despite_refusal;
    }
    check(escaped == 0, "no injection point escapes — every refusal is a soft failure");
    check(wrote_despite_refusal == 0, "and a refused encode reports 0 bytes: drop the frame");
}

}  // namespace

int main() {
    std::printf("#848 — transport egress framing soft-fail\n");
    test_ws_server_send_span_is_allocation_free();
    test_ws_server_send_iov_overflow_drops();
    test_ws_peer_endpoint_send_iov_overflow_drops();
    test_ws_client_send_drops_then_recovers();
    test_tcp_send_iov_overflow_drops();
    test_tcp_server_broadcast_scratch_drops();
    test_udp_send_iov_overflow_drops();
    test_can_advertise_header_is_allocation_free();
    test_can_over_long_path_refused();
    test_can_emit_advertise_wire_identical();
    test_ws_ping_at_the_bound_is_answered();
    test_ws_oversized_ping_fails_the_connection();
    test_ws_fragmented_control_fails_the_connection();
    test_control_encoders_are_byte_identical();
    test_try_encode_client_frame();
    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
