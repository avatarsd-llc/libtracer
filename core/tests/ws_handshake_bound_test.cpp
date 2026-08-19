/**
 * @file
 * @brief #934 car B — the WS OPENING HANDSHAKE is bounded by a deployment-injected pre-auth
 *        budget: an over-budget request is refused BEFORE it is buffered, counted, and closed.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The frame path was bounded by #872 (`ws_rx_bound_test.cpp`). The HANDSHAKE path was not: the
 * HTTP Upgrade request accumulated into the slot's `std::string` under a hard-coded literal,
 * the check ran AFTER the append (so the overshoot past it was the peer's to choose), the
 * refusal moved no counter, and the grown capacity was never returned to the allocator. Every
 * byte of that is reachable from a bare TCP connect — no ACL, no subscription, no router,
 * nothing authenticated. This is the PRE-AUTH surface, so the ruling is REFUSE EARLY: the
 * budget is injected, tighten-only, checked before the byte that would exceed it is copied,
 * and its breach is counted and closed (#838's disposition shape, ruled for this path
 * 2026-08-15).
 *
 * ## The instrument, and why it is not an inference
 *
 * Every case drives the REAL RX entry point — a raw loopback socket into the real accept/poll
 * path, or a raw hostile listener the library's own client dials — never a helper call. The
 * bound is MEASURED where it can be: @ref peak_rss_bytes reads `getrusage(RUSAGE_SELF).ru_maxrss`,
 * the kernel's own high-water mark for this process, around a window in which a raw peer offers
 * @ref kAttackBytes of header bytes that never terminate. `ru_maxrss` is monotone and
 * page-granular, which is the point: a free cannot reset it, so it answers "did this buffer
 * ever exist", not "does it exist now" — and it is why the two RSS-measured cases run FIRST in
 * `main`, before anything else in this TU can lift the watermark and make a later comparison
 * read zero growth vacuously.
 *
 * ## Coverage
 *  - a never-terminated request offering 64 MiB is refused, counted and closed, and the peak
 *    RSS does not follow it (server) — RSS-measured;
 *  - the DIAL half gets the identical treatment against a hostile SERVER whose header block
 *    never ends — RSS-measured;
 *  - the refusal is COUNTED and the slot RECYCLED, with no `101` ever written;
 *  - the honored budget is the CONFIGURED one, not a literal: a 4 KiB padded request a
 *    default server accepts is refused by a `max_handshake=1024` one, and a request ABOVE
 *    the default is clamped back to it (tighten-only);
 *  - no false positive: a legal request delivered in 64-byte dribbles across many reads still
 *    completes — and the same dribble under a budget SMALLER than the request is refused, so
 *    the budget is a TOTAL-REQUEST one and not a per-read one.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/transport_ws.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;

using tr::testing::check;

// --- the memory instrument --------------------------------------------------

/**
 * @brief This process's PEAK resident set in bytes (`getrusage`'s `ru_maxrss`, KiB on Linux).
 *
 * A high-water mark maintained by the kernel, so it survives the free a torn-down connection
 * performs — the property that makes it an honest answer to "was this buffer ever allocated?"
 * rather than "is it allocated now?".
 */
[[nodiscard]] std::size_t peak_rss_bytes() {
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::size_t>(ru.ru_maxrss) * 1024u;
}

/** @brief How many header bytes a case offers behind a request that never terminates. */
constexpr std::size_t kAttackBytes = 64u * 1024u * 1024u;

/** @brief The peak-RSS growth a bounded receiver may show across an attack window.
 *
 * Generous by three orders of magnitude relative to what an UNBOUNDED accumulator shows
 * (@ref kAttackBytes), and far above the few pages a torn-down connection can move: the
 * assertion is about the ORDER of the answer, so it needs no tuning to stay meaningful. */
constexpr std::size_t kRssSlack = 16u * 1024u * 1024u;

// --- raw-socket helpers (the ws_rx_bound_test.cpp idiom) ---------------------

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
 * `MSG_NOSIGNAL` is load-bearing, not hygiene: every case here exists to make the server
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

/** @brief Write the bytes of @p s to @p fd (the text half of @ref write_bytes). */
bool write_text(int fd, std::string_view s) {
    return write_bytes(
        fd, std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()));
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

/** @brief Drain @p fd until the peer closes it, and answer everything that arrived. */
std::vector<std::byte> read_to_close(int fd, bool* closed) {
    return read_until(fd, [](const std::vector<std::byte>&) { return false; }, 3s, closed);
}

/** @brief True when @p b contains the `101 Switching Protocols` status line. */
[[nodiscard]] bool has_101(const std::vector<std::byte>& b) {
    return std::string_view(reinterpret_cast<const char*>(b.data()), b.size())
               .find("101 Switching Protocols") != std::string_view::npos;
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

/** @brief How many peers @p server currently reports as OPEN (handshaken, named). */
[[nodiscard]] std::size_t open_peers(const tr::net::transport_ws_server& server) {
    std::size_t n = 0;
    server.enumerate_peers([&n](std::string_view) { ++n; });
    return n;
}

/** @brief The legal RFC 6455 opening-handshake request this TU's raw clients send. */
constexpr std::string_view kUpgradeRequest =
    "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

/**
 * @brief The same legal request, padded with one long `X-Pad` header to @p pad extra bytes.
 *
 * Legal HTTP in every respect — `header_value` still finds `sec-websocket-key` at the start
 * of its own line — so the ONLY thing that can refuse it is the size budget.
 */
std::string padded_request(std::size_t pad) {
    std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: ";
    req.append(pad, 'a');
    req +=
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    return req;
}

/**
 * @brief Write @p s in @p step-byte pieces with a pause between them, so the server performs
 *        MANY separate reads none of which is itself over budget.
 *
 * The pause is what makes the case discriminating rather than lucky: without it the kernel
 * coalesces the pieces into one segment and a per-read bound would be indistinguishable from
 * a total-request one.
 */
bool dribble(int fd, std::string_view s, std::size_t step) {
    for (std::size_t off = 0; off < s.size(); off += step) {
        const std::size_t n = std::min(step, s.size() - off);
        if (!write_text(fd, s.substr(off, n))) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The two RSS-MEASURED cases. These run first — see the file header.
// ---------------------------------------------------------------------------

/**
 * @brief A never-terminated Upgrade request is refused, counted and closed, and the node's
 *        memory never follows what the peer offered.
 *
 * The peer completes a TCP connect and then streams header bytes with no CRLFCRLF as fast as
 * the kernel will take them. Nothing about it is authenticated; the node must refuse it at the
 * budget rather than accumulate toward whatever the peer feels like sending.
 */
void test_endless_request_is_refused_and_bounded() {
    std::printf("ws server — a never-terminated Upgrade request is refused at the budget:\n");
    tr::net::transport_ws_server server(0);  // the default 16 KiB pre-auth budget
    check(server.ok(), "server bound");
    const int cfd = tcp_connect(server.local_port());
    check(cfd >= 0, "raw client connected");

    const std::size_t rss_before = peak_rss_bytes();

    // No CRLFCRLF anywhere: the header block never ends, so the accumulation is the peer's
    // to grow unless a budget stops it.
    const std::string block = "X-Pad: " + std::string(65528, 'a') + "\r\n";
    std::size_t pushed = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    check(write_text(cfd, "GET / HTTP/1.1\r\n"), "the request line was written");
    while (pushed < kAttackBytes && std::chrono::steady_clock::now() < deadline) {
        if (!write_text(cfd, block)) break;
        pushed += block.size();
    }

    const std::size_t rss_after = peak_rss_bytes();
    const std::size_t growth = rss_after > rss_before ? rss_after - rss_before : 0;
    std::printf("        offered %zu KiB; peak RSS moved %zu KiB\n", pushed / 1024, growth / 1024);

    check(growth < kRssSlack, "the process peak RSS did not follow what the peer offered");
    check(wait_until([&] { return server.malformed_rx() == 1; }, 2s),
          "malformed_rx ticked exactly once");
    check(server.dropped_rx() == 0, "and nothing was counted as backpressure");
    bool closed = false;
    const auto got = read_to_close(cfd, &closed);
    check(closed, "the server closed the link");
    check(!has_101(got), "and never wrote a 101");
    check(wait_until([&] { return open_peers(server) == 0; }, 2s), "the slot was recycled");
    ::close(cfd);
}

/**
 * @brief Accept one client, complete the request read, then answer with a header block that
 *        never terminates — the DIAL-half mirror of the case above.
 */
void endless_header_server(int lfd) {
    const int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) return;
    (void)read_until(
        cfd,
        [](const std::vector<std::byte>& b) {
            return std::string_view(reinterpret_cast<const char*>(b.data()), b.size())
                       .find("\r\n\r\n") != std::string_view::npos;
        },
        2s);
    // A `101` status line and then padding headers forever: no CRLFCRLF is ever written, so
    // a client that keeps reading until the header block ends never stops.
    (void)write_text(cfd, "HTTP/1.1 101 Switching Protocols\r\n");
    const std::string block = "X-Pad: " + std::string(65528, 'a') + "\r\n";
    std::size_t pushed = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (pushed < kAttackBytes && std::chrono::steady_clock::now() < deadline) {
        if (!write_text(cfd, block)) break;
        pushed += block.size();
    }
    ::close(cfd);
}

/** @brief The DIAL half: a server we chose to dial is no more trusted with our pre-auth
 *         memory than a peer that dialled us. */
void test_client_refuses_an_endless_server_header_block() {
    std::printf("ws client — a hostile server's endless header block is refused and counted:\n");
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

    std::thread hostile([lfd] { endless_header_server(lfd); });

    const std::size_t rss_before = peak_rss_bytes();
    tr::net::transport_ws_client client("127.0.0.1", ntohs(bound.sin_port));
    const std::size_t growth = peak_rss_bytes() - rss_before;

    check(!client.ok(), "the dial failed rather than completing a handshake");
    check(client.malformed_rx() == 1, "the over-budget response header block was counted");
    check(client.effective_max_handshake() == tr::net::transport_ws_server::kMaxHandshakeBytes,
          "on the default budget, the same 16 KiB the accept side defaults to");
    check(growth < kRssSlack, "and the accumulation did not follow the server's offer");

    hostile.join();
    ::close(lfd);
}

// ---------------------------------------------------------------------------
// The budget is the CONFIGURED one, and its breach is counted and closed
// ---------------------------------------------------------------------------

/**
 * @brief The honored budget comes from `max_handshake`, not from a literal: one 4 KiB padded
 *        request is refused by a 1 KiB-budget server and accepted by a default one.
 *
 * Both servers see byte-identical input, so the only thing that can explain the two answers is
 * the constructor argument — which is what the `ws`-private `max_handshake` key feeds through
 * (`builtin_transport_ws.cpp`). The tighten-only clamp is asserted on the accessor, on both
 * roles, so a config-writable key provably cannot RAISE a pre-auth bound.
 */
void test_the_budget_is_the_configured_one() {
    std::printf("ws — the refused request size tracks `max_handshake`, not a built-in number:\n");
    const std::string request = padded_request(4096);

    {
        tr::net::transport_ws_server tight(0, &tr::mem::heap_backend(), /*max_frame=*/0,
                                           /*max_peers=*/0, /*peer_named=*/false,
                                           /*recv_stack=*/std::size_t{0},
                                           /*liveness_window_ms=*/0, /*max_handshake=*/1024);
        check(tight.effective_max_handshake() == 1024, "the tight server honors 1024");
        const int cfd = tcp_connect(tight.local_port());
        check(cfd >= 0, "raw client connected (1 KiB budget)");
        (void)write_text(cfd, request);  // the server may close mid-write; that is the point
        check(wait_until([&] { return tight.malformed_rx() == 1; }, 2s),
              "the 1 KiB-budget server refused the 4 KiB request");
        bool closed = false;
        const auto got = read_to_close(cfd, &closed);
        check(closed && !has_101(got), "closed the link without ever writing a 101");
        check(wait_until([&] { return open_peers(tight) == 0; }, 2s), "and recycled the slot");
        ::close(cfd);
    }
    {
        tr::net::transport_ws_server wide(0);  // the default budget (kMaxHandshakeBytes)
        check(wide.effective_max_handshake() == tr::net::transport_ws_server::kMaxHandshakeBytes,
              "the default server honors kMaxHandshakeBytes");
        const int cfd = tcp_connect(wide.local_port());
        check(cfd >= 0, "raw client connected (default budget)");
        check(write_text(cfd, request), "the same 4 KiB request was written");
        const auto got =
            read_until(cfd, [](const std::vector<std::byte>& b) { return has_101(b); }, 3s);
        check(has_101(got), "the default-budget server completed the handshake");
        check(wide.malformed_rx() == 0, "and counted nothing malformed");
        ::close(cfd);
    }

    // Tighten-only: a request ABOVE the default cannot widen the pre-auth bound. Non-vacuous —
    // without the clamp the accessor would read back the 64 KiB that was asked for.
    const tr::net::transport_ws_server raised(0, &tr::mem::heap_backend(), /*max_frame=*/0,
                                              /*max_peers=*/0, /*peer_named=*/false,
                                              /*recv_stack=*/std::size_t{0},
                                              /*liveness_window_ms=*/0,
                                              /*max_handshake=*/64u * 1024u);
    check(raised.effective_max_handshake() == tr::net::transport_ws_server::kMaxHandshakeBytes,
          "a max_handshake ABOVE the default is clamped — tighten-only, never raise");

    // The DIAL half resolves through the SAME `handshake_cap` home. Port 1 refuses the
    // connect, so this asserts the resolution alone, with no listener to stand up.
    const tr::net::transport_ws_client dial_tight("127.0.0.1", 1, &tr::mem::heap_backend(),
                                                  /*max_frame=*/0, /*recv_stack=*/std::size_t{0},
                                                  /*defer_recv=*/true, /*liveness_window_ms=*/0,
                                                  &tr::mem::heap_source(), /*max_handshake=*/2048);
    check(dial_tight.effective_max_handshake() == 2048, "the dial half honors its own budget");
    const tr::net::transport_ws_client dial_raised(
        "127.0.0.1", 1, &tr::mem::heap_backend(), /*max_frame=*/0, /*recv_stack=*/std::size_t{0},
        /*defer_recv=*/true, /*liveness_window_ms=*/0, &tr::mem::heap_source(),
        /*max_handshake=*/1u << 20);
    check(dial_raised.effective_max_handshake() == tr::net::transport_ws_server::kMaxHandshakeBytes,
          "and clamps a raised one to the same ceiling the accept side does");
}

// ---------------------------------------------------------------------------
// No false positive — and the budget is a TOTAL, not a per-read allowance
// ---------------------------------------------------------------------------

/**
 * @brief A legal request delivered in 64-byte dribbles across many reads still completes; the
 *        same dribble under a budget SMALLER than the whole request is refused.
 *
 * The pair is what makes the claim precise. Every individual read is 64 bytes — far under
 * either budget — so a per-read check would accept both. Only a bound applied to the
 * ACCUMULATED request can tell them apart, and only a bound that does NOT reset per read can
 * still admit the legal one.
 */
void test_a_dribbled_request_is_judged_on_its_total() {
    std::printf("ws server — the budget is the whole request, not one read of it:\n");
    check(kUpgradeRequest.size() > 128 && kUpgradeRequest.size() < 512,
          "the legal request sits between the two budgets this case uses");
    {
        tr::net::transport_ws_server ample(0, &tr::mem::heap_backend(), /*max_frame=*/0,
                                           /*max_peers=*/0, /*peer_named=*/false,
                                           /*recv_stack=*/std::size_t{0},
                                           /*liveness_window_ms=*/0, /*max_handshake=*/512);
        const int cfd = tcp_connect(ample.local_port());
        check(cfd >= 0, "raw client connected (512-byte budget)");
        check(dribble(cfd, kUpgradeRequest, 64),
              "the legal request was dribbled 64 bytes at a"
              " time");
        const auto got =
            read_until(cfd, [](const std::vector<std::byte>& b) { return has_101(b); }, 3s);
        check(has_101(got), "a legal dribbled handshake still completes");
        check(ample.malformed_rx() == 0, "with nothing counted malformed");
        check(wait_until([&] { return open_peers(ample) == 1; }, 2s), "and the peer is open");
        ::close(cfd);
    }
    {
        tr::net::transport_ws_server tiny(0, &tr::mem::heap_backend(), /*max_frame=*/0,
                                          /*max_peers=*/0, /*peer_named=*/false,
                                          /*recv_stack=*/std::size_t{0},
                                          /*liveness_window_ms=*/0, /*max_handshake=*/128);
        const int cfd = tcp_connect(tiny.local_port());
        check(cfd >= 0, "raw client connected (128-byte budget)");
        (void)dribble(cfd, kUpgradeRequest, 64);  // refused partway; the writes then fail
        check(wait_until([&] { return tiny.malformed_rx() == 1; }, 2s),
              "the same 64-byte reads sum past a 128-byte budget and are refused");
        bool closed = false;
        const auto got = read_to_close(cfd, &closed);
        check(closed && !has_101(got), "closed with no 101, exactly as the bulk case");
        ::close(cfd);
    }
}

}  // namespace

int main() {
    std::printf(
        "#934 car B — the WS opening handshake is bounded by an injected pre-auth budget\n");
    // The two RSS-measured cases run FIRST: `ru_maxrss` is a process-wide high-water mark, so
    // anything that allocated before them would mask the growth they exist to detect.
    test_endless_request_is_refused_and_bounded();
    test_client_refuses_an_endless_server_header_block();
    test_the_budget_is_the_configured_one();
    test_a_dribbled_request_is_judged_on_its_total();
    return tr::testing::summary("ws_handshake_bound");
}
