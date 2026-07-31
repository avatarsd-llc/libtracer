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
 *   THW each peer pushed one large frame                  (the buffer HIGH-WATER)
 *   TD  every peer disconnected and the departure observed (what teardown RETURNS)
 *   TR  the same K peers reconnected                      (the slot-RECYCLE claim)
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
#include "libtracer/mem_pool.hpp"
#include "libtracer/transport_can.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"

// --- the counting allocator override (all variants) --------------------------

namespace {

/** @brief Live usable-size balance while armed — the steady-state heap the process holds. */
std::atomic<long long> g_live{0};
/** @brief High-water mark of @ref g_live — catches TRANSIENT per-frame buffers. */
std::atomic<long long> g_peak{0};
/** @brief Blocks allocated / freed while armed. */
std::atomic<long long> g_allocs{0};
std::atomic<long long> g_frees{0};
/** @brief Counting is on. */
std::atomic<bool> g_armed{false};

void bump_peak(long long now) {
    long long seen = g_peak.load(std::memory_order_relaxed);
    while (now > seen && !g_peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
}

void* counted_alloc(std::size_t size) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (g_armed.load(std::memory_order_relaxed) && p != nullptr) {
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
    if (g_armed.load(std::memory_order_relaxed)) {
        g_frees.fetch_add(1, std::memory_order_relaxed);
#ifdef BENCH_HAS_USABLE_SIZE
        g_live.fetch_sub(static_cast<long long>(malloc_usable_size(p)), std::memory_order_relaxed);
#endif
    }
    std::free(p);
}

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
};

long long live() { return g_live.load(std::memory_order_relaxed); }
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

/** @brief Send one u32-LE length-prefixed record of @p len zero bytes. */
bool send_tcp_frame(int fd, std::size_t len) {
    std::array<std::uint8_t, 4> pfx{};
    pfx[0] = static_cast<std::uint8_t>(len & 0xFFu);
    pfx[1] = static_cast<std::uint8_t>((len >> 8) & 0xFFu);
    pfx[2] = static_cast<std::uint8_t>((len >> 16) & 0xFFu);
    pfx[3] = static_cast<std::uint8_t>((len >> 24) & 0xFFu);
    if (!write_all_fd(fd, pfx.data(), pfx.size())) return false;
    static std::array<std::uint8_t, 4096> zero{};  // static: no allocation inside a window
    std::size_t left = len;
    while (left != 0) {
        const std::size_t chunk = std::min(left, zero.size());
        if (!write_all_fd(fd, zero.data(), chunk)) return false;
        left -= chunk;
    }
    return true;
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

/** @brief Send one MASKED client→server BINARY WebSocket frame of @p len zero bytes. */
bool send_ws_frame(int fd, std::size_t len) {
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
    if (!write_all_fd(fd, hdr.data(), h)) return false;
    static std::array<std::uint8_t, 4096> zero{};
    std::size_t left = len;
    while (left != 0) {
        const std::size_t chunk = std::min(left, zero.size());
        if (!write_all_fd(fd, zero.data(), chunk)) return false;
        left -= chunk;
    }
    return true;
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

// --- the arms ----------------------------------------------------------------

/** @brief Largest peer count an arm may request (the fd table is a fixed stack array). */
constexpr std::size_t kMaxPeers = 64;

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
    quiesce();
    const long long t0 = live();
    out.link_base = t0 - pre;

    // --- TK: k peers established -------------------------------------------
    for (std::size_t i = 0; i < k; ++i) fds[i] = dial(srv->local_port());
    if (bus != nullptr && !wait_peers(*bus, k)) std::fprintf(stderr, "warn: peers != %zu\n", k);
    quiesce();
    const long long tk = live();
    out.per_conn = (tk - t0) / static_cast<long long>(k);

    // --- THW: one big frame per peer ---------------------------------------
    reset_peak();
    for (std::size_t i = 0; i < k; ++i) push(fds[i], big);
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
    return run_stream_arm<tr::net::transport_tcp_server>(
        k, big,
        [] {
            return new tr::net::transport_tcp_server(0, &tr::mem::heap_backend(), 0, 0, true, 0);
        },
        [](std::uint16_t p) { return dial_tcp(p); },
        [](int fd, std::size_t n) { (void)send_tcp_frame(fd, n); });
}

sample_t arm_ws(std::size_t k, std::size_t big) {
    return run_stream_arm<tr::net::transport_ws_server>(
        k, big, [] { return new tr::net::transport_ws_server(0, 0, true, 0); },
        [](std::uint16_t p) {
            const int fd = dial_tcp(p);
            if (fd < 0) return -1;
            if (!ws_upgrade(fd)) {
                ::close(fd);
                return -1;
            }
            return fd;
        },
        [](int fd, std::size_t n) { (void)send_ws_frame(fd, n); });
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
    tr::mem::mem_backend_t* backend = mode == udp_mode_t::ROPE_POOL
                                          ? static_cast<tr::mem::mem_backend_t*>(&pool)
                                          : &tr::mem::heap_backend();
    const long long pre = live();
    auto srv = std::make_unique<tr::net::udp_transport_t>(0, std::string{}, 0, backend, 0);
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
        hellos.push_back(tr::net::can::encode_advertise(a));
        ctrl_ids.push_back(tr::net::can::encode_can_id({0, node, tr::net::kCanControlEndpoint}));
    }

    const long long pre = live();
    tr::net::transport_can_config_t cfg{};
    cfg.node = 1;
    auto link = std::make_unique<tr::net::socketcan_link_t>(ifname, 0);
    if (!link->ok()) {
        ::close(raw);
        return out;
    }
    auto srv = std::make_unique<tr::net::transport_can>(std::move(link), cfg);
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
    std::vector<long long> per_conn, high_water, retained, after_td, recycle, link_base;
    void reserve(std::size_t n) {
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
    }
    void emit(const char* name) {
        report(name, "link_base", link_base);
        report(name, "per_conn", per_conn);
        report(name, "hw_peak", high_water);
        report(name, "hw_retained", retained);
        report(name, "after_td", after_td);
        report(name, "recycle", recycle);
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
        sizeof(tr::net::tcp_transport_t), sizeof(tr::net::transport_tcp_server),
        sizeof(tr::net::udp_transport_t), sizeof(tr::net::transport_ws_server),
        sizeof(tr::net::transport_ws_client), sizeof(tr::net::transport_can));
    std::printf("# sizeof: framer=%zu socketcan_link=%zu can_cfg=%zu\n",
                sizeof(tr::net::length_prefix_framer), sizeof(tr::net::socketcan_link_t),
                sizeof(tr::net::transport_can_config_t));
    std::fflush(stdout);

    series_t tcp, ws, udp_span, udp_rope, udp_pool, can, can_grp;
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
    std::printf("# allocs=%lld frees=%lld residual_live=%lld\n",
                g_allocs.load(std::memory_order_relaxed), g_frees.load(std::memory_order_relaxed),
                g_live.load(std::memory_order_relaxed));
    return 0;
}
