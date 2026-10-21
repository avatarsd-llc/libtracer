/**
 * @file
 * @brief The per-connection RAM census — how many heap bytes ONE established
 *        connection costs each transport, measured with a counting allocator
 *        (the bench_forward_heap `counting_resource_t` pattern, hoisted to the
 *        transport plane).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RAM is a HARD CONSTRAINT on this project, and until now no instrument priced a
 * CONNECTION. bench_forward_heap prices a forward hop and its own header warns its
 * `allocs=0` "says nothing about the real wire" (it drives a stub link); the
 * cortex-m0 sentinel (`core/tests/footprint/sentinel_node.cpp`) deliberately links
 * NO transports. This bench closes that gap from the other side: it stands up a real
 * server transport on the loopback (or `vcan0`), drives real peers at it with RAW
 * client sockets — so every counted byte belongs to the transport under test and
 * none to a second libtracer instance — and reads the LIVE (usable-size) heap
 * balance at four points:
 *
 *   T0  server constructed, receive thread up, quiesced   (the per-LINK baseline)
 *   TK  K peers established and verified                  (the per-CONNECTION cost)
 *   T16 each peer pushed one 16 KiB frame, all delivered   (what a peer KEEPS after it)
 *   THW each peer pushed one large frame                  (the buffer HIGH-WATER)
 *   TD  every peer disconnected and the departure observed (what teardown RETURNS)
 *   TR  the same K peers reconnected                      (the slot-RECYCLE claim)
 *
 * `T16 - TK` is `retained@16384`: the bytes a peer still holds once one 16 KiB frame has
 * passed, above what it held idle. Unlike the high-water columns it is a steady balance,
 * so it can be pinned (#2171); the #2136 goal is 0, a peer that drops back to its idle base.
 *
 * The `<kind>-remote-sub` arms price a REMOTE SUBSCRIPTION (#2171): a node (graph, router
 * and one server link of that kind, every source counted) and one raw peer that has already
 * subscribed once, so the link's first-subscription draws are outside the window. The peer
 * then sends K `:subscribers[]` appends, one per vertex, and the arm reads the live balance
 * and the block count before and after: `per_sub` is the bytes, `sub_blocks` the blocks one more
 * remote subscription holds.
 *
 * `TD - T0` is the interesting one: the tcp/ws servers document slots as
 * "recycled in place, never destroyed while the server lives", so a departure is
 * expected to return ~nothing and a reconnect to cost ~nothing. That is a CONTROL
 * arm here, not an assumption.
 *
 * Two measurement rules this file obeys, because a number that breaks them is
 * worthless: (1) every arm runs INTERLEAVED with the others, `--reps` times, and
 * the report is a MEDIAN with min/max, so an overlap check is possible; (2) a
 * NULL arm (two snapshots with nothing but the quiesce between them) must read
 * exactly 0 — it is printed with the rest so the reader can confirm the
 * instrument is not itself allocating inside the window.
 *
 * Since ADR-0083 the host default root is the slab pool, which takes whole 256 KiB slabs
 * from the platform heap, so a counting `operator new` alone cannot see a connection. Every
 * arm therefore hands its link a `link_memory_t` whose sources are counting wrappers over
 * the default sub-pools (`counting_source_t`, #2134): the link draws from where it would by
 * default, each block is charged at its slab class, and the slab draws underneath are not
 * counted. What still goes through `operator new` is counted at its usable size.
 *
 * The counting override is process-global and NOT thread-scoped (the receive
 * threads allocate on it, which is exactly what we want to see), so the harness
 * itself must not allocate inside a window: every harness container is reserved
 * before the first arm, and everything inside a window uses stack buffers and
 * raw syscalls.
 *
 * @warning What this does NOT count: pthread STACKS. Every socket transport
 * spawns one receive thread via `pthread_create`, whose stack is `mmap`ed, not
 * `malloc`ed — invisible to any `operator new` counter. On glibc that is 8 MiB of
 * virtual address space per transport (a few resident pages); on ESP-IDF the same
 * pthread maps to a FreeRTOS task whose stack IS heap
 * (`CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`, 12288 B in the full_node example).
 * On an MCU that term dominates everything this bench measures, so read the
 * thread count per transport alongside the byte counts.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if __has_include(<malloc.h>)
#include <malloc.h>
#define BENCH_HAS_USABLE_SIZE 1
#endif

#include "libtracer/can.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/mem_source_backend.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport_can.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"

// --- the counting allocator override (all variants) --------------------------

namespace {

/** @brief A whole advertise frame in a `std::vector` — the test-side form of
 *         `can::encode_advertise` (#1780 moved it into a caller's `bytes_t`); empty when
 *         the advertise is unencodable. */
[[maybe_unused]] std::vector<std::byte> advertise_vec(const tr::net::can::advertise_t& a) {
    tr::mem::bytes_t b(tr::mem::heap_source());
    if (!tr::net::can::encode_advertise(b, a)) return {};
    return std::vector<std::byte>(b.begin(), b.end());
}

/** @brief Live usable-size balance while armed — the steady-state heap the process holds. */
std::atomic<long long> g_live{0};
/** @brief High-water mark of @ref g_live — catches TRANSIENT per-frame buffers. */
std::atomic<long long> g_peak{0};
/** @brief Blocks allocated / freed while armed. */
std::atomic<long long> g_allocs{0};
std::atomic<long long> g_frees{0};
/** @brief Counting is on. */
std::atomic<bool> g_armed{false};
/** @brief Set while a @ref counting_source_t forwards a draw: the slabs the default root
 *         takes from the platform heap then are not a connection's bytes, and a block the
 *         source already counted must not be counted twice. */
thread_local bool t_in_source = false;

void bump_peak(long long now) {
    long long seen = g_peak.load(std::memory_order_relaxed);
    while (now > seen && !g_peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
}

void* counted_alloc(std::size_t size) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (g_armed.load(std::memory_order_relaxed) && !t_in_source && p != nullptr) {
        g_allocs.fetch_add(1, std::memory_order_relaxed);
#ifdef BENCH_HAS_USABLE_SIZE
        const long long now = g_live.fetch_add(static_cast<long long>(malloc_usable_size(p)),
                                               std::memory_order_relaxed) +
                              static_cast<long long>(malloc_usable_size(p));
        bump_peak(now);
#endif
    }
    return p;
}

void counted_free(void* p) {
    if (p == nullptr) return;
    if (g_armed.load(std::memory_order_relaxed) && !t_in_source) {
        g_frees.fetch_add(1, std::memory_order_relaxed);
#ifdef BENCH_HAS_USABLE_SIZE
        g_live.fetch_sub(static_cast<long long>(malloc_usable_size(p)), std::memory_order_relaxed);
#endif
    }
    std::free(p);
}

/**
 * @brief The bytes the default slab root charges for a @p bytes block at @p align: the
 *        smallest row of `config_t::kSizeClasses` that holds it, as `slab_pool_t::class_of`
 *        picks it. An over-aligned request is padded to @p align and takes only a row that
 *        is a multiple of it. A request past the last row passes straight through to the
 *        platform heap and is charged as asked.
 */
long long charged(std::size_t bytes, std::size_t align) {
    const bool over = align > alignof(std::max_align_t);
    if (over) bytes = (bytes + align - 1) / align * align;
    for (const std::size_t row : tr::graph::config_t::kSizeClasses)
        if (row >= bytes && (!over || row % align == 0)) return static_cast<long long>(row);
    return static_cast<long long>(bytes);
}

/**
 * @brief A @ref tr::mem::block_source_t that forwards to a default sub-pool and counts what
 *        it hands out into the same live balance as the `operator new` counter (#2134).
 *
 * Since ADR-0083 the host default root is the slab pool: a link's state, egress and receive
 * draws reach the platform heap only as whole 256 KiB slabs, so the `operator new` counter
 * alone read 0 per connection, or one slab. Each arm hands its transport this wrapper over
 * the very sub-pool the link would take by default, so the link draws from the same place
 * and the bench sees every block, charged at its slab class.
 */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Count the draws served by @p inner, which must outlive this source. */
    explicit counting_source_t(tr::mem::block_source_t& inner) noexcept
        : block_source_t("counting"), inner_(&inner) {}

    /** @brief A block from the wrapped sub-pool, added to the live balance when armed. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        t_in_source = true;
        void* p = inner_->try_alloc(bytes, align);
        t_in_source = false;
        if (p != nullptr && g_armed.load(std::memory_order_relaxed)) {
            g_allocs.fetch_add(1, std::memory_order_relaxed);
            const long long c = charged(bytes, align);
            bump_peak(g_live.fetch_add(c, std::memory_order_relaxed) + c);
        }
        return p;
    }

    /** @brief Return a block to the wrapped sub-pool, taken off the live balance when armed. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        if (p != nullptr && g_armed.load(std::memory_order_relaxed)) {
            g_frees.fetch_add(1, std::memory_order_relaxed);
            g_live.fetch_sub(charged(bytes, align), std::memory_order_relaxed);
        }
        t_in_source = true;
        inner_->release(p, bytes, align);
        t_in_source = false;
    }

   private:
    tr::mem::block_source_t* inner_; /**< @brief The default sub-pool the draws come from. */
};

/** @brief The net sub-pool, counted: every link's state and egress store in this bench. */
counting_source_t g_net{tr::mem::net_source()};
/** @brief The receive backend over @ref g_net — what `net_backend()` is, counted. */
tr::mem::source_backend_t g_net_rx{g_net};
/** @brief The value sub-pool, counted: what `heap_backend()` draws its segments from. */
counting_source_t g_values{tr::mem::value_source()};
/** @brief The UDP rope arm's "default heap backend", counted. */
tr::mem::source_backend_t g_values_rx{g_values};
/** @brief The link memory every arm injects: the defaults, through the counters. */
const tr::net::link_memory_t kCounted{.rx = &g_net_rx, .io = &g_net, .state = &g_net};
/** @brief The default root, counted: the source the remote-subscription arms' graph draws from. */
counting_source_t g_graph{tr::mem::default_root()};

}  // namespace

void* operator new(std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new(std::size_t size, std::align_val_t) { return operator new(size); }
void* operator new(std::size_t size, std::align_val_t, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new[](std::size_t size, std::align_val_t) { return operator new(size); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }

// --- harness -----------------------------------------------------------------

namespace {

using namespace std::chrono_literals;

/** @brief One arm's five readings for one repetition, in bytes relative to T0. */
struct sample_t {
    long long per_conn = 0;   /**< @brief (TK - T0) / K — heap per established connection. */
    long long high_water = 0; /**< @brief (peak during THW - T0) / K — per-conn buffer peak. */
    long long retained = 0;   /**< @brief (THW steady - T0) / K — buffer capacity kept after. */
    long long after_td = 0;   /**< @brief (TD - T0) / K — what a departure did NOT return. */
    long long recycle = 0;    /**< @brief (TR - TD) / K — cost of reconnecting the same K. */
    long long link_base = 0;  /**< @brief T0 relative to the pre-construction balance. */
    long long retained_16k = -1; /**< @brief (T16 - TK) / K; -1 where the arm has no T16. */
};

long long live() { return g_live.load(std::memory_order_relaxed); }
/** @brief Blocks drawn and not yet returned while armed. */
long long blocks() {
    return g_allocs.load(std::memory_order_relaxed) - g_frees.load(std::memory_order_relaxed);
}
void reset_peak() { g_peak.store(live(), std::memory_order_relaxed); }
long long peak() { return g_peak.load(std::memory_order_relaxed); }

/** @brief The settle the receive threads need before a balance is stable. */
void quiesce() { std::this_thread::sleep_for(250ms); }

/** @brief Connect a raw TCP client socket to 127.0.0.1:@p port (no heap). */
int dial_tcp(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/** @brief Write @p n bytes from @p p, resuming partial writes. */
bool write_all_fd(int fd, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    while (n != 0) {
        const ssize_t w = ::send(fd, b, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        b += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

/** @brief Write @p len bytes of @p data, or @p len zero bytes when @p data is null. */
bool write_body(int fd, const std::byte* data, std::size_t len) {
    if (data != nullptr) return write_all_fd(fd, data, len);
    static std::array<std::uint8_t, 4096> zero{};  // static: no allocation inside a window
    std::size_t left = len;
    while (left != 0) {
        const std::size_t chunk = std::min(left, zero.size());
        if (!write_all_fd(fd, zero.data(), chunk)) return false;
        left -= chunk;
    }
    return true;
}

/** @brief Send one u32-LE length-prefixed record of @p len bytes (@ref write_body). */
bool send_tcp_frame(int fd, std::size_t len, const std::byte* data = nullptr) {
    std::array<std::uint8_t, 4> pfx{};
    pfx[0] = static_cast<std::uint8_t>(len & 0xFFu);
    pfx[1] = static_cast<std::uint8_t>((len >> 8) & 0xFFu);
    pfx[2] = static_cast<std::uint8_t>((len >> 16) & 0xFFu);
    pfx[3] = static_cast<std::uint8_t>((len >> 24) & 0xFFu);
    return write_all_fd(fd, pfx.data(), pfx.size()) && write_body(fd, data, len);
}

/** @brief Run the RFC 6455 client opening handshake on an already-connected @p fd. */
bool ws_upgrade(int fd) {
    static constexpr std::string_view kReq =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!write_all_fd(fd, kReq.data(), kReq.size())) return false;
    std::array<char, 512> resp{};
    std::size_t got = 0;
    while (got + 1 < resp.size()) {
        const ssize_t n = ::recv(fd, resp.data() + got, resp.size() - got - 1, 0);
        if (n <= 0) return false;
        got += static_cast<std::size_t>(n);
        if (std::string_view(resp.data(), got).find("\r\n\r\n") != std::string_view::npos) break;
    }
    return std::string_view(resp.data(), got).find("101") != std::string_view::npos;
}

/** @brief Send one MASKED client→server BINARY WebSocket frame of @p len bytes
 *         (@ref write_body). */
bool send_ws_frame(int fd, std::size_t len, const std::byte* data = nullptr) {
    std::array<std::uint8_t, 14> hdr{};
    std::size_t h = 0;
    hdr[h++] = 0x82;  // FIN | BINARY
    if (len < 126) {
        hdr[h++] = static_cast<std::uint8_t>(0x80u | len);
    } else if (len <= 0xFFFF) {
        hdr[h++] = 0x80u | 126u;
        hdr[h++] = static_cast<std::uint8_t>((len >> 8) & 0xFFu);
        hdr[h++] = static_cast<std::uint8_t>(len & 0xFFu);
    } else {
        hdr[h++] = 0x80u | 127u;
        for (int i = 7; i >= 0; --i)
            hdr[h++] =
                static_cast<std::uint8_t>((static_cast<std::uint64_t>(len) >> (i * 8)) & 0xFFu);
    }
    for (int i = 0; i < 4; ++i) hdr[h++] = 0;  // all-zero mask key: payload passes through
    return write_all_fd(fd, hdr.data(), h) && write_body(fd, data, len);
}

/** @brief Count the peers a bus link currently enumerates (the visitor captures one pointer,
 *         so its std::function stays inside libstdc++'s small-object buffer — no allocation). */
std::size_t peer_count(tr::net::bus_link_t& bus) {
    std::size_t n = 0;
    std::size_t* np = &n;
    bus.enumerate_peers([np](std::string_view) { ++*np; });
    return n;
}

/** @brief Poll until @p bus enumerates @p want peers, or the deadline passes. */
bool wait_peers(tr::net::bus_link_t& bus, std::size_t want) {
    for (int i = 0; i < 200; ++i) {
        if (peer_count(bus) == want) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

/** @brief Steps whose pushed frames were not all delivered: each fails the run. */
std::size_t g_undelivered = 0;

/** @brief A peer-named link's frames delivered so far: what @ref wait_delivered polls. */
void count_delivery(void* ctx, tr::net::peer_handle_t, tr::view::rope_t) {
    static_cast<std::atomic<std::size_t>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief Poll until @p seen reaches @p want deliveries, or the deadline passes: a balance
 *        read before the receive threads have handed every pushed frame up would charge
 *        a frame still in flight to the step (#2171 review).
 */
bool wait_delivered(const std::atomic<std::size_t>& seen, std::size_t want) {
    for (int i = 0; i < 200; ++i) {
        if (seen.load(std::memory_order_relaxed) >= want) return true;
        std::this_thread::sleep_for(10ms);
    }
    std::fprintf(stderr, "error: %zu of %zu pushed frames delivered\n",
                 seen.load(std::memory_order_relaxed), want);
    ++g_undelivered;
    return false;
}

// --- the arms ----------------------------------------------------------------

/** @brief Largest peer count an arm may request (the fd table is a fixed stack array). */
constexpr std::size_t kMaxPeers = 64;
/** @brief The frame `retained@16384` pushes: the #2136 "one 16 KiB frame". */
constexpr std::size_t kRetainedFrame = 16384;

/**
 * @brief Drive one server transport through T0 / TK / THW / TD / TR with @p k raw peers.
 *
 * @tparam Server  The transport under test (must expose local_port() and bus()).
 * @param make     Constructs the server under test (the object whose RAM is measured).
 * @param dial     Establishes one raw client connection, returning its fd (-1 on failure).
 * @param push     Sends one @p big -byte frame on an established client fd.
 */
template <typename Server, typename Make, typename Dial, typename Push>
sample_t run_stream_arm(std::size_t k, std::size_t big, Make make, Dial dial, Push push) {
    sample_t out{};
    std::array<int, kMaxPeers> fds{};
    fds.fill(-1);
    k = std::min(k, kMaxPeers);

    const long long pre = live();
    std::unique_ptr<Server> srv(make());
    tr::net::bus_link_t* bus = srv->bus();
    // Installed before any peer dials (the set_receiver contract), so every frame a step
    // pushes is counted as it is handed up, and released on return as an unread one is.
    std::atomic<std::size_t> delivered{0};
    srv->set_peer_rope_receiver(&count_delivery, &delivered);
    quiesce();
    const long long t0 = live();
    out.link_base = t0 - pre;

    // --- TK: k peers established -------------------------------------------
    for (std::size_t i = 0; i < k; ++i) fds[i] = dial(srv->local_port());
    if (bus != nullptr && !wait_peers(*bus, k)) std::fprintf(stderr, "warn: peers != %zu\n", k);
    quiesce();
    const long long tk = live();
    out.per_conn = (tk - t0) / static_cast<long long>(k);

    // --- T16: one 16 KiB frame per peer, and what each keeps once it has passed ---------
    for (std::size_t i = 0; i < k; ++i) push(fds[i], kRetainedFrame);
    (void)wait_delivered(delivered, k);
    quiesce();
    out.retained_16k = (live() - tk) / static_cast<long long>(k);

    // --- THW: one big frame per peer ---------------------------------------
    reset_peak();
    for (std::size_t i = 0; i < k; ++i) push(fds[i], big);
    (void)wait_delivered(delivered, 2 * k);
    quiesce();
    const long long thw_peak = peak();
    const long long thw = live();
    out.high_water = (thw_peak - t0) / static_cast<long long>(k);
    out.retained = (thw - t0) / static_cast<long long>(k);

    // --- TD: every peer departs --------------------------------------------
    for (std::size_t i = 0; i < k; ++i)
        if (fds[i] >= 0) ::close(fds[i]);
    if (bus != nullptr && !wait_peers(*bus, 0)) std::fprintf(stderr, "warn: peers != 0\n");
    quiesce();
    const long long td = live();
    out.after_td = (td - t0) / static_cast<long long>(k);

    // --- TR: the same k peers reconnect (the slot-recycle control) ----------
    for (std::size_t i = 0; i < k; ++i) fds[i] = dial(srv->local_port());
    if (bus != nullptr && !wait_peers(*bus, k)) std::fprintf(stderr, "warn: re-peers != %zu\n", k);
    quiesce();
    out.recycle = (live() - td) / static_cast<long long>(k);

    for (std::size_t i = 0; i < k; ++i)
        if (fds[i] >= 0) ::close(fds[i]);
    if (bus != nullptr) (void)wait_peers(*bus, 0);
    quiesce();
    return out;
}

sample_t arm_tcp(std::size_t k, std::size_t big) {
    return run_stream_arm<tr::net::tcp_server_transport_t>(
        k, big,
        [] {
            return new tr::net::tcp_server_transport_t(0, {.memory = kCounted, .peer_named = true});
        },
        [](std::uint16_t p) { return dial_tcp(p); },
        [](int fd, std::size_t n) { (void)send_tcp_frame(fd, n); });
}

/** @brief Connect a raw client to 127.0.0.1:@p port and complete the WebSocket upgrade. */
int dial_ws(std::uint16_t port) {
    const int fd = dial_tcp(port);
    if (fd >= 0 && !ws_upgrade(fd)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

sample_t arm_ws(std::size_t k, std::size_t big) {
    return run_stream_arm<tr::net::ws_server_transport_t>(
        k, big,
        [] {
            return new tr::net::ws_server_transport_t(0, {.memory = kCounted, .peer_named = true});
        },
        [](std::uint16_t p) { return dial_ws(p); },
        [](int fd, std::size_t n) { (void)send_ws_frame(fd, n); });
}

// --- the remote-subscription arms (#2171) --------------------------------------

/** @brief Subscriptions one remote-subscription arm adds inside its window. */
constexpr std::size_t kRemoteSubs = 8;

/** @brief One remote-subscription reading, per subscription. */
struct sub_sample_t {
    long long per_sub = 0; /**< @brief Live bytes one more remote subscription holds. */
    long long blocks = 0;  /**< @brief Blocks it holds. */
    bool ok = false;       /**< @brief Every subscription in the window was admitted. */
};

/** @brief Append the TLV @p type over @p body to @p out. */
void emit(std::vector<std::byte>& out, tr::wire::type_t type, std::span<const std::byte> body,
          bool structured) {
    tr::wire::emit_tlv(out, type, structured ? tr::wire::opt_t{.pl = true} : tr::wire::opt_t{},
                       body);
}

/** @brief A PATH TLV over @p segs. */
std::vector<std::byte> path_tlv(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    emit(out, tr::wire::type_t::PATH, body, false);
    return out;
}

/**
 * @brief The frame a remote peer subscribes with: `FWD{WRITE}` to `/<vertex>:subscribers[]`
 *        carrying a SUBSCRIBER whose target is the peer's own `/sink/<vertex>`.
 */
std::vector<std::byte> subscribe_frame(std::string_view vertex) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    emit(body, tr::wire::type_t::VALUE, std::span<const std::byte>(&op, 1), false);
    const std::vector<std::byte> dst = path_tlv({vertex});
    body.insert(body.end(), dst.begin(), dst.end());
    std::vector<std::byte> field;
    tr::wire::emit_name(field, "subscribers");
    const std::byte element{1};  // index_mode ELEMENT, no index: the `[]` append
    emit(field, tr::wire::type_t::VALUE, std::span<const std::byte>(&element, 1), false);
    emit(body, tr::wire::type_t::FIELD, field, true);
    // A non-empty src: an empty one asks for no reply, and a subscribe without a route home
    // is refused (RFC-0004 Amendment 2).
    const std::vector<std::byte> src = path_tlv({"c"});
    body.insert(body.end(), src.begin(), src.end());
    emit(body, tr::wire::type_t::SUBSCRIBER, path_tlv({"sink", vertex}), true);
    std::vector<std::byte> frame;
    emit(frame, tr::wire::type_t::FWD, body, true);
    return frame;
}

/**
 * @brief Price one remote subscription over a link of one kind.
 *
 * @param make Constructs the link under test, over the counted link memory.
 * @param port Its bound port.
 * @param dial Establishes the raw peer, returning its fd (-1 on failure).
 * @param push Sends one frame's bytes on that fd.
 */
template <typename Make, typename Port, typename Dial, typename Push>
sub_sample_t run_remote_sub_arm(Make make, Port port, Dial dial, Push push) {
    sub_sample_t out{};
    // Every frame and vertex name is built here, before the window opens.
    std::array<std::string, kRemoteSubs + 1> names;
    std::array<std::vector<std::byte>, kRemoteSubs + 1> frames;
    for (std::size_t i = 0; i <= kRemoteSubs; ++i) {
        names[i] = "s" + std::to_string(i);
        frames[i] = subscribe_frame(names[i]);
    }
    tr::graph::graph_t g(g_graph);
    tr::net::fwd_router_t router(g);
    auto link = make();
    if (!router.attach_link("in", *link)) return out;
    std::vector<tr::graph::vertex_handle_t> v;
    v.reserve(kRemoteSubs + 1);
    for (std::size_t i = 0; i <= kRemoteSubs; ++i)
        v.push_back(g.register_vertex(tr::graph::path_t(("/" + names[i]).c_str()),
                                      tr::graph::role_t::STORED_VALUE));
    const int fd = dial(port(*link));
    if (fd < 0) return out;
    // The first subscription over this link takes whatever the link draws once; it is
    // outside the window, which prices the next one.
    push(fd, frames[0]);
    quiesce();
    const long long t0 = live();
    const long long b0 = blocks();
    for (std::size_t i = 1; i <= kRemoteSubs; ++i) push(fd, frames[i]);
    quiesce();
    out.per_sub = (live() - t0) / static_cast<long long>(kRemoteSubs);
    out.blocks = (blocks() - b0) / static_cast<long long>(kRemoteSubs);
    out.ok = true;
    for (std::size_t i = 0; i <= kRemoteSubs; ++i) out.ok = out.ok && g.has_subscribers(v[i]);
    if (!out.ok) std::fprintf(stderr, "warn: a remote subscription was not admitted\n");
    ::close(fd);
    quiesce();
    return out;
}

sub_sample_t arm_remote_sub_tcp() {
    return run_remote_sub_arm(
        [] {
            return std::make_unique<tr::net::tcp_server_transport_t>(
                0, tr::net::tcp_server_config_t{.memory = kCounted, .peer_named = true});
        },
        [](tr::net::tcp_server_transport_t& t) { return t.local_port(); },
        [](std::uint16_t p) { return dial_tcp(p); },
        [](int fd, const std::vector<std::byte>& f) {
            (void)send_tcp_frame(fd, f.size(), f.data());
        });
}

sub_sample_t arm_remote_sub_ws() {
    return run_remote_sub_arm(
        [] {
            return std::make_unique<tr::net::ws_server_transport_t>(
                0, tr::net::ws_server_config_t{.memory = kCounted, .peer_named = true});
        },
        [](tr::net::ws_server_transport_t& t) { return t.local_port(); },
        [](std::uint16_t p) { return dial_ws(p); },
        [](int fd, const std::vector<std::byte>& f) {
            (void)send_ws_frame(fd, f.size(), f.data());
        });
}

/** @brief A UDP socket connected to 127.0.0.1:@p port: the listener learns it as its peer. */
int dial_udp(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

sub_sample_t arm_remote_sub_udp() {
    return run_remote_sub_arm(
        [] {
            return std::make_unique<tr::net::udp_transport_t>(
                0, std::string{}, 0, tr::net::udp_config_t{.memory = kCounted});
        },
        [](tr::net::udp_transport_t& t) { return t.local_port(); },
        [](std::uint16_t p) { return dial_udp(p); },
        [](int fd, const std::vector<std::byte>& f) { (void)::send(fd, f.data(), f.size(), 0); });
}

/** @brief Which receive tier / memory seam a UDP arm wires up. */
enum class udp_mode_t {
    SPAN, /**< @brief No receiver installed — the borrowed-span path (lazy 64 KiB scratch). */
    ROPE_HEAP, /**< @brief Rope receiver over the default heap backend (ADR-0042 owning path). */
    ROPE_POOL, /**< @brief Rope receiver over a bounded pool — the injection bound under test. */
};

/** @brief A sink that drops every frame — the receiver exists only to select the tier. */
void drop_rope(void*, tr::view::rope_t) {}

/** @brief The bounded MCU-shaped slab an injected pool draws from (static: no heap). */
alignas(std::max_align_t) std::array<std::byte, 8192> g_udp_slab{};

/**
 * @brief UDP has no accept: a listener LEARNS its single peer from the first datagram.
 *        The arm therefore prices "peer learned" and "one max datagram received".
 */
sample_t arm_udp(std::size_t big, udp_mode_t mode) {
    sample_t out{};
    // The pool lives over a static slab, so constructing it costs no heap and the
    // link_base reading below stays a pure transport measurement.
    tr::mem::pool_t pool(g_udp_slab, 1536);
    tr::mem::mem_backend_t* backend =
        mode == udp_mode_t::ROPE_POOL ? static_cast<tr::mem::mem_backend_t*>(&pool) : &g_values_rx;
    tr::net::link_memory_t memory = kCounted;
    memory.rx = backend;
    const long long pre = live();
    auto srv = std::make_unique<tr::net::udp_transport_t>(0, std::string{}, 0,
                                                          tr::net::udp_config_t{.memory = memory});
    if (mode != udp_mode_t::SPAN) srv->set_rope_receiver(&drop_rope, nullptr);
    quiesce();
    const long long t0 = live();
    out.link_base = t0 - pre;

    const int cfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(srv->local_port());
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static std::array<std::uint8_t, 16> small{};
    (void)::sendto(cfd, small.data(), small.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    quiesce();
    out.per_conn = live() - t0;

    reset_peak();
    static std::array<std::uint8_t, 65000> jumbo{};
    (void)::sendto(cfd, jumbo.data(), std::min(big, jumbo.size()), 0,
                   reinterpret_cast<sockaddr*>(&a), sizeof(a));
    quiesce();
    out.high_water = peak() - t0;
    out.retained = live() - t0;

    ::close(cfd);
    quiesce();
    out.after_td = live() - t0;
    out.recycle = 0;
    return out;
}

/**
 * @brief CAN is a BUS: a "connection" is a peer heard on the wire. The arm writes
 *        `k` hello advertises (slice_count == 0) from distinct node ids onto vcan0
 *        with a raw CAN socket, so the transport under test learns k peers with no
 *        second libtracer instance in the process.
 */
sample_t arm_can(std::size_t k, const char* ifname, bool with_group) {
    sample_t out{};
    const int raw = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (raw < 0) return out;
    ifreq ifr{};
    std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (::ioctl(raw, SIOCGIFINDEX, &ifr) < 0) {
        ::close(raw);
        return out;
    }
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(raw, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(raw);
        return out;
    }

    // Pre-encode every peer's hello OUTSIDE the measured window (encode_advertise
    // returns a std::vector — it must not allocate inside an armed window).
    std::vector<std::vector<std::byte>> hellos;
    hellos.reserve(k);
    std::vector<std::uint32_t> ctrl_ids;
    ctrl_ids.reserve(k);
    for (std::size_t i = 0; i < k; ++i) {
        const auto node = static_cast<std::uint16_t>(100 + i);
        tr::net::can::advertise_t a{};
        a.can_id = tr::net::can::encode_can_id({0, node, tr::net::kCanFirstDataEndpoint});
        // slice_count == 0 is the pure presence hello (learn_advertise returns early);
        // a non-zero count is a REAL group manifest, which also lands a `learned_`
        // binding — the term the hello-only arm cannot see.
        a.slice_count = with_group ? 1 : 0;
        a.group_total_len = with_group ? 8 : 0;
        if (with_group) a.path = "/sensor/temp";
        a.target = tr::net::can::kCanBroadcastNode;
        hellos.push_back(advertise_vec(a));
        ctrl_ids.push_back(tr::net::can::encode_can_id({0, node, tr::net::kCanControlEndpoint}));
    }

    const long long pre = live();
    tr::net::transport_can_config_t cfg{};
    cfg.node = 1;
    cfg.reasm_src = &g_net;  // the peer table and RX tables: the net sub-pool, counted
    auto link = tr::mem::make_poly<tr::net::socketcan_link_t>(g_net, ifname, 0);
    if (!link->ok()) {
        ::close(raw);
        return out;
    }
    auto srv = std::make_unique<tr::net::can_transport_t>(std::move(link), cfg);
    tr::net::bus_link_t* bus = srv->bus();
    quiesce();
    const long long t0 = live();
    out.link_base = t0 - pre;

    // Each hello is longer than one classic CAN data field, so it rides as a
    // multi-frame control stream — exactly how a real node announces itself.
    auto emit = [&](std::size_t i) {
        const std::vector<std::byte>& bytes = hellos[i];
        for (std::size_t off = 0; off < bytes.size(); off += 8) {
            can_frame f{};
            f.can_id = ctrl_ids[i] | CAN_EFF_FLAG;
            f.can_dlc = static_cast<std::uint8_t>(std::min<std::size_t>(8, bytes.size() - off));
            std::memcpy(f.data, bytes.data() + off, f.can_dlc);
            (void)::write(raw, &f, sizeof(f));
        }
    };
    for (std::size_t i = 0; i < k; ++i) emit(i);
    if (!wait_peers(*bus, k)) std::fprintf(stderr, "warn: can peers != %zu\n", k);
    quiesce();
    out.per_conn = (live() - t0) / static_cast<long long>(k);

    // A CAN peer's buffer high-water is its control-stream accumulator: re-emit
    // each hello so the per-node `control_` vector is exercised again.
    reset_peak();
    for (std::size_t i = 0; i < k; ++i) emit(i);
    quiesce();
    out.high_water = (peak() - t0) / static_cast<long long>(k);
    out.retained = (live() - t0) / static_cast<long long>(k);

    // A bus peer "departs" by falling silent past peer_ttl (3 s default).
    std::this_thread::sleep_for(3500ms);
    (void)peer_count(*bus);
    quiesce();
    out.after_td = (live() - t0) / static_cast<long long>(k);

    for (std::size_t i = 0; i < k; ++i) emit(i);
    (void)wait_peers(*bus, k);
    quiesce();
    out.recycle = (live() - t0) / static_cast<long long>(k) - out.after_td;

    ::close(raw);
    return out;
}

/** @brief The NULL arm: two snapshots with only the quiesce between them. Must read 0. */
long long arm_null() {
    const long long a = live();
    quiesce();
    return live() - a;
}

// --- reporting ---------------------------------------------------------------

long long median(std::vector<long long> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void report(const char* name, const char* metric, std::vector<long long>& v) {
    if (v.empty()) return;
    auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    std::printf("RESULT arm=%-12s metric=%-11s n=%zu median=%lld min=%lld max=%lld\n", name, metric,
                v.size(), median(v), *lo, *hi);
}

struct series_t {
    std::vector<long long> per_conn, high_water, retained, after_td, recycle, link_base,
        retained_16k;
    void reserve(std::size_t n) {
        retained_16k.reserve(n);
        per_conn.reserve(n);
        high_water.reserve(n);
        retained.reserve(n);
        after_td.reserve(n);
        recycle.reserve(n);
        link_base.reserve(n);
    }
    void push(const sample_t& s) {
        per_conn.push_back(s.per_conn);
        high_water.push_back(s.high_water);
        retained.push_back(s.retained);
        after_td.push_back(s.after_td);
        recycle.push_back(s.recycle);
        link_base.push_back(s.link_base);
        if (s.retained_16k >= 0) retained_16k.push_back(s.retained_16k);
    }
    void emit(const char* name) {
        report(name, "link_base", link_base);
        report(name, "per_conn", per_conn);
        report(name, "hw_peak", high_water);
        report(name, "hw_retained", retained);
        report(name, "after_td", after_td);
        report(name, "recycle", recycle);
        report(name, "retained@16384", retained_16k);
    }
};

/** @brief The remote-subscription readings of one link kind, over repetitions. */
struct sub_series_t {
    std::vector<long long> per_sub, blocks;
    std::size_t failed = 0;
    void reserve(std::size_t n) {
        per_sub.reserve(n);
        blocks.reserve(n);
    }
    void push(const sub_sample_t& s) {
        if (!s.ok) {
            ++failed;
            return;
        }
        per_sub.push_back(s.per_sub);
        blocks.push_back(s.blocks);
    }
    void emit(const char* name) {
        if (failed != 0)
            std::fprintf(stderr, "error: %s: %zu repetition(s) refused a subscription\n", name,
                         failed);
        report(name, "per_sub", per_sub);
        report(name, "sub_blocks", blocks);
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::size_t reps = 9;
    std::size_t k = 8;
    std::size_t big = 64 * 1024;
    const char* canif = "vcan0";
    bool do_can = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a.starts_with("--reps="))
            reps = std::strtoul(argv[i] + 7, nullptr, 10);
        else if (a.starts_with("--peers="))
            k = std::strtoul(argv[i] + 8, nullptr, 10);
        else if (a.starts_with("--frame="))
            big = std::strtoul(argv[i] + 8, nullptr, 10);
        else if (a.starts_with("--canif="))
            canif = argv[i] + 8;
        else if (a == "--no-can")
            do_can = false;
    }

    std::printf("# bench_conn_ram peers=%zu frame=%zu reps=%zu\n", k, big, reps);
    std::printf(
        "# sizeof: tcp_client=%zu tcp_server=%zu udp=%zu ws_server=%zu ws_client=%zu can=%zu\n",
        sizeof(tr::net::tcp_transport_t), sizeof(tr::net::tcp_server_transport_t),
        sizeof(tr::net::udp_transport_t), sizeof(tr::net::ws_server_transport_t),
        sizeof(tr::net::ws_client_transport_t), sizeof(tr::net::can_transport_t));
    std::printf("# sizeof: framer=%zu socketcan_link=%zu can_cfg=%zu\n",
                sizeof(tr::net::length_prefix_framer_t), sizeof(tr::net::socketcan_link_t),
                sizeof(tr::net::transport_can_config_t));
    std::fflush(stdout);

    series_t tcp, ws, udp_span, udp_rope, udp_pool, can, can_grp;
    sub_series_t sub_tcp, sub_ws, sub_udp;
    sub_tcp.reserve(reps);
    sub_ws.reserve(reps);
    sub_udp.reserve(reps);
    std::vector<long long> nulls;
    tcp.reserve(reps);
    ws.reserve(reps);
    udp_span.reserve(reps);
    udp_rope.reserve(reps);
    udp_pool.reserve(reps);
    can.reserve(reps);
    can_grp.reserve(reps);
    nulls.reserve(reps);

    g_armed.store(true, std::memory_order_seq_cst);
    for (std::size_t r = 0; r < reps; ++r) {
        // INTERLEAVED: one repetition of every arm before the next repetition.
        nulls.push_back(arm_null());
        tcp.push(arm_tcp(k, big));
        udp_span.push(arm_udp(big, udp_mode_t::SPAN));
        ws.push(arm_ws(k, big));
        udp_rope.push(arm_udp(big, udp_mode_t::ROPE_HEAP));
        if (do_can) can.push(arm_can(k, canif, false));
        udp_pool.push(arm_udp(big, udp_mode_t::ROPE_POOL));
        if (do_can) can_grp.push(arm_can(k, canif, true));
        sub_tcp.push(arm_remote_sub_tcp());
        sub_ws.push(arm_remote_sub_ws());
        sub_udp.push(arm_remote_sub_udp());
    }
    g_armed.store(false, std::memory_order_seq_cst);

    report("null", "drift", nulls);
    tcp.emit("tcp-server");
    ws.emit("ws-server");
    udp_span.emit("udp-span");
    udp_rope.emit("udp-rope-heap");
    udp_pool.emit("udp-rope-pool");
    if (do_can) can.emit("can-bus");
    if (do_can) can_grp.emit("can-bus-grp");
    sub_tcp.emit("tcp-remote-sub");
    sub_ws.emit("ws-remote-sub");
    sub_udp.emit("udp-remote-sub");
    std::printf("# allocs=%lld frees=%lld residual_live=%lld\n",
                g_allocs.load(std::memory_order_relaxed), g_frees.load(std::memory_order_relaxed),
                g_live.load(std::memory_order_relaxed));
    return sub_tcp.failed + sub_ws.failed + sub_udp.failed + g_undelivered == 0 ? 0 : 1;
}
