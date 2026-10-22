/**
 * @file
 * @brief The per-link base RAM ratchet (#2156): what an IDLE stream server draws from its
 *        injected memory, before any peer connects, pinned so growth fails the build's tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_conn_ram`'s `link_base` row priced this, but it ran on one host in warn mode and,
 * once the transports drew from the allocation seam (#1780), it counted `operator new` only,
 * which the default slab root never reaches. The base grew from 464 B to 2608 B while it
 * could not see the draws: a dead 8-slot send ring inherited by every server (#1629), and a
 * poll set sized up front to the whole admission cap (#1780). This test counts the link's
 * own seam instead (`link_memory_t::io` and `state`), on every core-ci leg, so the same
 * growth now fails a test rather than a warning nobody reads.
 *
 * The pins are RATCHETS with no headroom, like `kMaxVertexBytes64`: LOWER one in the commit
 * that shrinks the link; RAISING one is a reviewed decision that names the bytes it buys.
 * They are 64-bit numbers (block sizes plus the 8-entry first block of a `block_array_t`);
 * on another pointer width the test reports and does not gate.
 *
 * One more arm (#2140) counts a WS peer across one 16 KiB message: its connected draw is
 * pinned like the base, and it must hold no more after the message than before it, on every
 * pointer width.
 *
 * And one (#2267) counts a WS CLIENT's sends at 64 B, 1 KiB, 4 KiB and 16 KiB: each one draws
 * exactly one block, the masked frame (so its payload is masked once, into that block), and
 * the link holds no more after the 16 KiB send than before the first. It used to keep a
 * `tx_buf_` at its high water for life.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/egress.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_ws.hpp"
#include "libtracer/ws.hpp"

namespace {

using namespace std::chrono_literals;

int g_failures = 0;

/** @brief Record one check, printing it either way. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief A forwarding source over the platform heap that keeps the live byte balance. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : block_source_t("link_base_ram") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* const p = tr::mem::heap_source().try_alloc(bytes, align);
        if (p != nullptr) {
            live.fetch_add(bytes, std::memory_order_relaxed);
            drawn.fetch_add(bytes, std::memory_order_relaxed);
            draws.fetch_add(1, std::memory_order_relaxed);
        }
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        live.fetch_sub(bytes, std::memory_order_relaxed);
        tr::mem::heap_source().release(p, bytes, align);
    }
    std::atomic<std::size_t> live{0};  /**< @brief Bytes drawn and not yet released. */
    std::atomic<std::size_t> drawn{0}; /**< @brief Bytes drawn in all. */
    std::atomic<std::size_t> draws{0}; /**< @brief Blocks drawn in all. */
};

/** @brief Pinned idle draw of a stream server on a 64-bit target: the poll set's first block
 *         (8 `pollfd`s for the listen socket). Was 1992 B before #2156. */
constexpr std::size_t kServerBaseBytes64 = 64;

/**
 * @brief Construct one idle server on a counting seam and compare its draw to the pin.
 *
 * @tparam Server The stream server under test (`tcp_server_transport_t`, `ws_server_transport_t`).
 * @tparam Config Its config aggregate.
 */
template <class Server, class Config>
void run_arm(const char* name) {
    counting_source_t src;
    std::size_t base = 0;
    {
        Config cfg{};
        cfg.memory.io = &src;
        cfg.memory.state = &src;
        Server server(0, cfg);
        check(server.ok(), "server bound");
        // The poll thread draws its set on its first pass: wait for that draw (the pin on a
        // 64-bit target, any draw elsewhere) rather than for a fixed time.
        const std::size_t want = sizeof(void*) == 8 ? kServerBaseBytes64 : 1;
        for (int i = 0; i < 1000 && src.live.load(std::memory_order_relaxed) < want; ++i)
            std::this_thread::sleep_for(2ms);
        base = src.live.load(std::memory_order_relaxed);
    }
    std::printf("%s: idle draw %zu B (sizeof %zu B)\n", name, base, sizeof(Server));
    if constexpr (sizeof(void*) == 8) {
        check(base == kServerBaseBytes64, "idle draw equals the 64-bit pin");
    }
    check(src.live.load(std::memory_order_relaxed) == 0, "teardown returned every byte");
}

/**
 * @brief Pinned draw of one connected, idle WS peer on a 64-bit target (#2140): its session
 *        (with frame ingress's 14-byte next-header slot and the shared egress slot, #2144),
 *        its slot entries and its poll entry. Frame ingress put the session 16 B over the
 *        stream-buffer design it replaced; the egress slot, net of the per-session write
 *        state it retired, adds 16 B more; the liveness record (#2198) 8 B more (its three
 *        progress flags sit in padding).
 *
 * The session's `peer_endpoint_t` is a `transport_t`, whose receiver slot holds a
 * `std::mutex`: 40 B on x86_64 glibc, 48 B on aarch64 glibc. So the pin is 728 B plus that
 * mutex, exact on each target: 768 B on x86_64, 776 B on aarch64. Every other byte is
 * pointers, sizes and block counts, the same on any 64-bit target.
 */
constexpr std::size_t kWsPeerBytes64 = 728 + sizeof(std::mutex);

/** @brief A receive backend over the platform heap that adds to @p src's live balance. */
class counting_rx_t final : public tr::mem::mem_backend_t {
   public:
    explicit counting_rx_t(counting_source_t& src) noexcept
        : mem_backend_t("link_base_ram_rx"), src_(src) {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return src_.try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        src_.release(p, bytes, align);
    }

   private:
    counting_source_t& src_; /**< @brief Where the bytes are drawn and counted. */
};

/** @brief Counts what a link hands up, and keeps none of it. */
struct drop_sink_t {
    std::atomic<std::size_t> frames{0}; /**< @brief Messages handed up. */
    void operator()(std::span<const std::byte> /*f*/) { frames.fetch_add(1); }
    void operator()(tr::view::rope_t /*r*/) { frames.fetch_add(1); }
};

/** @brief Write all of @p b to @p fd. */
bool write_all(int fd, std::span<const std::byte> b) {
    while (!b.empty()) {
        const ssize_t n = ::send(fd, b.data(), b.size(), MSG_NOSIGNAL);
        if (n <= 0) return false;
        b = b.subspan(static_cast<std::size_t>(n));
    }
    return true;
}

/**
 * @brief #2140: a WS peer that sent one 16 KiB message and then went quiet holds nothing
 *        more than it did before it: the body was received into a segment drawn at its exact
 *        length and handed up with it, not into a per-peer buffer that keeps its high water.
 */
void run_idle_peer_after_16k() {
    counting_source_t src;
    counting_rx_t rx(src);
    std::size_t before = 0;
    std::size_t after = 0;
    std::size_t frames = 0;
    {
        drop_sink_t sink;
        tr::net::ws_server_config_t cfg{};
        cfg.memory.rx = &rx;
        cfg.memory.io = &src;
        cfg.memory.state = &src;
        tr::net::ws_server_transport_t server(0, cfg);
        if (server.delivers_ropes()) {
            server.set_rope_receiver(sink);
        } else {
            server.set_receiver(sink);
        }
        server.start_receiving();
        check(server.ok(), "ws server bound");

        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server.local_port());
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const bool dialled = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        constexpr std::string_view kReq =
            "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        bool upgraded = dialled && write_all(fd, std::as_bytes(std::span(kReq)));
        std::vector<char> resp(512);
        pollfd p{fd, POLLIN, 0};
        upgraded = upgraded && ::poll(&p, 1, 3000) == 1 &&
                   ::recv(fd, resp.data(), resp.size(), 0) > 12 &&
                   std::string_view(resp.data(), 12) == "HTTP/1.1 101";
        check(upgraded, "the peer completed the opening handshake");
        // Settle: the session is set up; its draws so far are the per-peer cost.
        std::this_thread::sleep_for(50ms);
        before = src.live.load(std::memory_order_relaxed);

        // One masked 16 KiB BINARY message (a zero key leaves the body as sent).
        constexpr std::size_t kBody = 16u * 1024u;
        std::vector<std::byte> frame{
            std::byte{0x82}, std::byte{0x80 | 126}, std::byte{kBody >> 8}, std::byte{kBody & 0xFF},
            std::byte{0},    std::byte{0},          std::byte{0},          std::byte{0}};
        frame.resize(frame.size() + kBody, std::byte{0x5A});
        check(write_all(fd, frame), "the peer sent one 16 KiB message");
        for (int i = 0; i < 1500 && sink.frames.load() == 0; ++i) std::this_thread::sleep_for(2ms);
        frames = sink.frames.load();
        std::this_thread::sleep_for(50ms);
        after = src.live.load(std::memory_order_relaxed);
        ::close(fd);
    }
    std::printf("ws-server idle peer: %zu B before, %zu B after one 16 KiB message\n", before,
                after);
    if constexpr (sizeof(void*) == 8) {
        check(before == kWsPeerBytes64, "the connected peer's draw equals the 64-bit pin");
    }
    check(frames == 1, "the message was handed up");
    check(after == before, "the idle peer holds 0 B more after it");
    check(src.live.load(std::memory_order_relaxed) == 0, "teardown returned every byte");
}

/**
 * @brief #2267: a WS client's send draws one block, its masked frame, from the link's egress
 *        source and returns it when the frame is written, at 64 B, 1 KiB, 4 KiB and 16 KiB.
 *
 * The payload is masked straight into that block, so the bytes a send writes are the frame's
 * bytes once. The client keeps no buffer between sends: after the 16 KiB send it holds what
 * it held before the first.
 */
void run_ws_client_sends() {
    counting_source_t src;
    std::size_t before = 0;
    std::size_t after = 0;
    {
        drop_sink_t sink;
        tr::net::ws_server_transport_t server(0);
        if (server.delivers_ropes()) {
            server.set_rope_receiver(sink);
        } else {
            server.set_receiver(sink);
        }
        server.start_receiving();
        check(server.ok(), "ws server bound");
        tr::net::ws_client_config_t cfg{};
        cfg.memory.io = &src;
        tr::net::ws_client_transport_t client("127.0.0.1", server.local_port(), cfg);
        check(client.ok(), "ws client handshaken");
        std::this_thread::sleep_for(50ms);
        before = src.live.load(std::memory_order_relaxed);

        std::size_t sent = 0;
        for (const std::size_t len :
             {std::size_t{64}, std::size_t{1024}, std::size_t{4096}, std::size_t{16384}}) {
            const std::vector<std::byte> payload(len, std::byte{0x5A});
            const std::size_t n0 = src.draws.load(std::memory_order_relaxed);
            const std::size_t b0 = src.drawn.load(std::memory_order_relaxed);
            client.send(payload);
            ++sent;
            for (int i = 0; i < 1500 && sink.frames.load() < sent; ++i)
                std::this_thread::sleep_for(2ms);
            const std::size_t draws = src.draws.load(std::memory_order_relaxed) - n0;
            const std::size_t bytes = src.drawn.load(std::memory_order_relaxed) - b0;
            const std::size_t frame =
                sizeof(tr::net::egress_frame_t) + tr::net::ws::detail::client_frame_bytes(len);
            std::printf("ws-client send %zu B: %zu draw(s), %zu B drawn (one frame: %zu B)\n", len,
                        draws, bytes, frame);
            check(sink.frames.load() == sent, "the message reached the server");
            check(draws == 1 && bytes == frame, "the send drew exactly one masked frame");
            check(src.live.load(std::memory_order_relaxed) == before,
                  "and returned it once written");
        }
        std::this_thread::sleep_for(50ms);
        after = src.live.load(std::memory_order_relaxed);
    }
    std::printf("ws-client: %zu B before, %zu B after a 16 KiB send\n", before, after);
    check(after == before, "the client holds 0 B more after a 16 KiB send");
    check(src.live.load(std::memory_order_relaxed) == 0, "teardown returned every byte");
}

}  // namespace

int main() {
    std::printf("link_base_ram_test (#2156):\n");
    run_arm<tr::net::tcp_server_transport_t, tr::net::tcp_server_config_t>("tcp-server");
    run_arm<tr::net::ws_server_transport_t, tr::net::ws_server_config_t>("ws-server");
    run_idle_peer_after_16k();
    run_ws_client_sends();
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all checks passed\n");
    return EXIT_SUCCESS;
}
