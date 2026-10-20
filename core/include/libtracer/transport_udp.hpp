/**
 * @file
 * @brief A UDP socket transport: one datagram carries one whole frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_udp — a real UDP socket transport_t (M5). The first transport that
 * actually crosses the kernel network stack; the bridge / router / graph above
 * it are unchanged. One datagram carries one whole frame (no stream reassembly),
 * so it pairs with the flat decoder. POSIX sockets; a receive thread drains the
 * socket into the registered receiver. Datagram payloads are bounded by the UDP
 * limit (~65507 bytes) — large TLVs need a streaming transport (future).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

#include "libtracer/config.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/posix_endpoint.hpp"
#include "libtracer/transport.hpp"

/** @brief 1 in a sanitized build (ASan, TSan or MSan), whose stack frames are larger; 0 otherwise.
 *         Sizes `udp_transport_t::kRecvStackHeadroom` (#2142). */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LIBTRACER_UDP_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define LIBTRACER_UDP_SANITIZED 1
#endif
#endif
#ifndef LIBTRACER_UDP_SANITIZED
#define LIBTRACER_UDP_SANITIZED 0
#endif

namespace tr::net {

/**
 * @brief Suggested module name for a DIAL `udp` connection (ADR-0073 §4).
 *
 * A *suggestion*, never a registration: the library auto-registers no module names — the
 * application passes this (or any name it prefers) to
 * `transport_vertex_t::register_module("udp-client", "udp", conn_role_t::DIAL)`.
 */
inline constexpr std::string_view kUdpClientSuggestedModule = "udp-client";

/** @brief Suggested module name for a LISTEN `udp` connection (see
 *         `kUdpClientSuggestedModule`; the application registers it, or any name it
 *         prefers). */
inline constexpr std::string_view kUdpServerSuggestedModule = "udp-server";

/**
 * @brief `udp_transport_t`'s knobs as one aggregate (#1593), after the local port and peer.
 */
struct udp_config_t {
    /**
     * @brief The link's memory (@ref link_memory_t). `rx`: when a rope receiver is installed,
     *        each datagram is recvfrom'd straight into a fresh segment drawn from it, sized
     *        `min(kMaxDatagram, rx->max_segment_size())` — so the backend BOUNDS the datagram a
     *        node accepts. Exhaustion is backpressure — the datagram is dropped and
     *        `dropped_rx` ticks, never an OOM.
     */
    link_memory_t memory{};
    /**
     * @brief The universal `:settings max_frame` receive cap, bytes — the largest datagram
     *        accepted (0 → `udp_transport_t::kDefaultMaxFrame`, one Ethernet MTU; values
     *        above `udp_transport_t::kMaxDatagram` are inert). A longer datagram is refused
     *        (`malformed_rx` ticks), provided the backend can furnish `max_frame + 1` bytes
     *        (#1074). It also sizes the RX segment, so an owning link holds about
     *        `max_frame` bytes at rest, not 64 KiB (#2132), and a borrowed-span link whose
     *        cap fits `%graph::kUdpStackScratchBytes` holds none (#2142).
     */
    std::size_t max_frame = 0;
    /** @brief Recv-thread stack size in bytes, 0 = platform default. The receive loop keeps
     *         `%graph::kUdpStackScratchBytes + 1` bytes on it (#2142), so a nonzero value
     *         below `udp_transport_t::kMinRecvStack` is refused at construction
     *         (`udp_transport_t::recv_stack_refused`), never clamped. */
    std::size_t recv_stack = 0;
};

/**
 * @brief A single-peer UDP datagram @ref transport_t (one datagram = one frame).
 *
 * Binds a local UDP socket and sends to one peer; a listener-mode instance learns its peer
 * from the first inbound datagram's source address. Supports the owning rope-receiver seam
 * (ADR-0042 §2): each datagram is received straight into a refcounted segment from a
 * host-injected `mem_backend_t`, which also bounds the datagram size a node accepts.
 *
 * The ingress bound gains its second half: the universal `:settings max_frame` key
 * (@ref effective_max_frame), the same key `tcp_transport_t`, the `ws` transports, `quic`
 * and `webtransport` accept. A datagram longer than the configured cap is refused and
 * counted in @ref malformed_rx instead of being delivered, and the RX segment is drawn at
 * the cap rather than at @ref kMaxDatagram. Unset, the cap is @ref kDefaultMaxFrame. That refusal
 * is unconditional on the borrowed-span path; on the owning path it holds while the injected
 * backend can furnish `max_frame + 1` bytes — a backend bounded tighter than the cap truncates the
 * datagram before the cap is ever consulted (#1074). On the borrowed-span path a datagram
 * longer than the smaller of the cap and the backend's `max_segment_size()` is refused whole
 * (#1783). That path receives on the receive thread's stack when the cap fits
 * `%graph::kUdpStackScratchBytes` (#2142), so the link holds nothing at rest; otherwise, and on
 * the owning path, the link holds ONE segment of `max_frame + 1` bytes from `memory.rx`.
 *
 * The span a borrowed-span sink is handed is that receive buffer: the receive thread's stack
 * array or this link's own segment, writable for the duration of the call. An owner that
 * decrypts in place, as the Noise link does (RFC-0033 §5.8), may write it; nothing else reads it
 * after the callback returns.
 */
class udp_transport_t final : public transport_t, private posix_endpoint_t {
   public:
    /** @brief The largest datagram one frame can occupy — the RX segment size a view
     *         receiver's frames are allocated at (the UDP payload bound, one datagram
     *         = one frame = one segment, ADR-0042 §2). It is also the ceiling on
     *         @ref effective_max_frame — a datagram cannot be larger than this, so a
     *         configured `max_frame` above it is inert rather than loosening. */
    static constexpr std::size_t kMaxDatagram = 65536;

    /** @brief The receive cap of a link whose `max_frame` is unset (#2132): 1,472 bytes, the
     *         payload of one 1,500-byte Ethernet MTU after the 20-byte IPv4 and 8-byte UDP
     *         headers. A longer datagram needs IP fragmentation, which lwIP's default
     *         configuration does not reassemble, so an MCU peer never receives one anyway.
     *         The RX segment is drawn one byte past it. A link that carries
     *         wider datagrams sets `max_frame`, up to @ref kMaxDatagram. */
    static constexpr std::size_t kDefaultMaxFrame = 1472;

    /** @brief The stack the receive thread keeps beside its stack scratch, bytes (#2142): its
     *         own frames, the thread's TLS (glibc carves it from the same stack) and the
     *         borrowed sink's dispatch, which runs on that thread. A margin this check
     *         requires, not a measured bound of any one sink's depth.
     *
     * 8 KiB in an ordinary build. A sanitized build (ASan, TSan or MSan) inflates every frame
     * and the TLS block, so it keeps 64 KiB: an ASan+UBSan Debug build of the receive loop
     * was measured to need about 28 KiB in all, where 8 KiB of headroom overflowed. */
    static constexpr std::size_t kRecvStackHeadroom = LIBTRACER_UDP_SANITIZED ? 65536 : 8192;

    /** @brief The smallest nonzero `udp_config_t::recv_stack` this build accepts (#2142): the
     *         stack scratch, one byte past it and @ref kRecvStackHeadroom. `0` in a build
     *         with no stack scratch (`%graph::kUdpStackScratchBytes` 0), where any size is
     *         accepted. */
    static constexpr std::size_t kMinRecvStack =
        graph::kUdpStackScratchBytes == 0 ? 0
                                          : graph::kUdpStackScratchBytes + 1 + kRecvStackHeadroom;

    /**
     * @brief Bind a local UDP socket on @p bind_port and target @p peer_host : @p peer_port.
     *
     * @p bind_port 0 = ephemeral (see @ref local_port). @p peer_host is an IPv4 dotted-quad
     * (e.g. "127.0.0.1"). Listener mode: with an unresolved peer (@p peer_host empty or
     * @p peer_port 0) the transport LEARNS its peer from each inbound datagram's source
     * address — the single-peer UDP-server shape that lets a config-created LISTEN-module
     * connection (#83) reply to a dialing client whose ephemeral port is unknowable in
     * advance; until the first datagram arrives, @ref send is a no-op.
     *
     * @param config The link's knobs (@ref udp_config_t): memory (the RX seam that bounds the
     *        datagram), receive cap, recv-thread stack.
     */
    udp_transport_t(std::uint16_t bind_port, std::string_view peer_host, std::uint16_t peer_port,
                    const udp_config_t& config = {});
    ~udp_transport_t();

    /** @brief See @ref transport_t::shut_down. */
    void shut_down() override;

    udp_transport_t(const udp_transport_t&) = delete;
    udp_transport_t& operator=(const udp_transport_t&) = delete;

    void send(std::span<const std::byte> frame) override;                 // sendto(peer)
    void send(std::span<const std::span<const std::byte>> iov) override;  // sendmsg(iovec)

    /**
     * @brief Send @p iov as one datagram to @p address, whatever the link's own peer is.
     *
     * The addressed form of @ref send, for an owner that keeps its own peer address: the Noise
     * link answers a first message at that datagram's source and moves its session's address
     * only on an authenticated message (RFC-0033 §5.4), so it never sends to the peer this
     * link learned from the last datagram. The same best-effort contract as `send`: a
     * datagram with no socket, no address or no gather store is dropped and counted in
     * @ref dropped_tx.
     *
     * @param address A packed endpoint, as @ref inbound_source and @ref peer_address give it;
     *                0 is no address.
     */
    void send_to(std::uint64_t address, std::span<const std::span<const std::byte>> iov);

    /**
     * @brief The source of the datagram being delivered, packed as `(ip << 16) | port`.
     * @warning Valid only inside a receive callback, on the receive thread, like
     *          @ref transport_t::inbound_peer.
     */
    [[nodiscard]] std::uint64_t inbound_source() const noexcept { return source_; }

    /** @brief The peer @ref send targets, packed as @ref inbound_source packs it: the
     *         configured one, or in listener mode the last datagram's source; 0 = none yet. */
    [[nodiscard]] std::uint64_t peer_address() const noexcept { return peer(); }

    /** @brief True — this transport honors @ref set_rope_receiver (ADR-0042 §2):
     *         one datagram = one frame = one refcounted segment from the injected
     *         backend, handed up owning; span-only sinks keep the borrowed path. */
    [[nodiscard]] bool delivers_ropes() const override { return true; }

    /** @brief True iff the socket bound successfully. False too when @ref recv_stack_refused. */
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }
    /** @brief True iff the constructor refused `udp_config_t::recv_stack` (#2142): nonzero and
     *         below @ref kMinRecvStack, so the receive loop's stack scratch would not fit.
     *         No socket is opened and no thread started; @ref ok is false. */
    [[nodiscard]] bool recv_stack_refused() const noexcept { return recv_stack_refused_; }
    /** @brief The bound local port (resolves an ephemeral @c bind_port of 0). */
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }
    /** @brief Datagrams dropped because the RX backend was exhausted (backpressure,
     *         ADR-0039 §4 / ADR-0042 §2) — never an OOM. */
    [[nodiscard]] std::uint64_t dropped_rx() const noexcept {
        return dropped_rx_.load(std::memory_order_relaxed);
    }

    /** @brief Datagrams refused for exceeding @ref effective_max_frame — the peer's fault,
     *         not this node's resources (the tcp/ws counter vocabulary: over-cap is
     *         `malformed_rx`, exhaustion is @ref dropped_rx). UDP is connectionless, so
     *         nothing is torn down; the next datagram is served normally. */
    [[nodiscard]] std::uint64_t malformed_rx() const noexcept {
        return malformed_rx_.load(std::memory_order_relaxed);
    }

    /** @brief Datagrams shed on the way OUT (#932): no peer learned or configured yet,
     *         no socket, or a refused gather store — each one used to be a bare return. */
    [[nodiscard]] std::uint64_t dropped_tx() const noexcept {
        return dropped_tx_.load(std::memory_order_relaxed);
    }

    /** @brief The interface-level snapshot (#932) — what a generic `transport_t*` reads. */
    [[nodiscard]] transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx(), malformed_rx(), dropped_tx()};
    }

    /** @brief The largest datagram accepted: `min(max_frame, kMaxDatagram)`, with 0 meaning
     *         @ref kDefaultMaxFrame. The RX *segment* is additionally bounded by the injected
     *         backend's `max_segment_size()`, which never widens this. */
    [[nodiscard]] std::size_t effective_max_frame() const noexcept { return max_frame_; }

   private:
    void run();  // receive thread

    // The peer endpoint packed as (ip << 16) | port (ip network order, port host
    // order), atomic because listener mode updates it from the recv thread while
    // send() reads it from callers' threads. 0 = no peer yet (send is a no-op).
    [[nodiscard]] std::uint64_t peer() const noexcept {
        return peer_.load(std::memory_order_relaxed);
    }

    int fd_ = -1;
    std::uint16_t bound_port_ = 0;
    // 64-bit because the packed (ip, port) is 48 bits read whole — stored and loaded, never an
    // RMW. On rv32 each access is an `__atomic_*_8` call, so send() pays one per datagram
    // there (#1697).
    std::atomic<std::uint64_t> peer_{0};
    bool learn_peer_ = false;          // constructed peer-less => adopt each datagram's source
    bool recv_stack_refused_ = false;  // recv_stack below kMinRecvStack (#2142)
    /** @brief The in-flight datagram's source (@ref inbound_source). Written and read on the
     *         receive thread only, so it needs no atomic. */
    std::uint64_t source_ = 0;

    // RX segment source for view delivery (ADR-0042 §2) + backend-exhaustion
    // drop counter (backpressure, never OOM).
    mem::mem_backend_t* backend_;
    std::size_t max_frame_ = kDefaultMaxFrame;  // accepted-datagram cap (:settings; 0 => default)
    // Drop counters: word-wide, not 64-bit (core/STYLE.md §Introspection clause 5, #1697) —
    // a 64-bit atomic is a libatomic call on every rv32, the ESP32-C6 included. The 64-bit
    // accessors widen on read; a 32-bit target wraps after 2^32.
    std::atomic<std::size_t> dropped_rx_{0};
    std::atomic<std::size_t> malformed_rx_{0};
    std::atomic<std::size_t> dropped_tx_{0};
};

}  // namespace tr::net
