/**
 * @file
 * @brief #838, #2252 — a host stream send into a stalled peer never waits, is counted, and
 *        the peer that keeps stalling is closed.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The defect (#838): `tcp_transport_t::send` held `write_m_` across a fully blocking
 * `write_all_iov`. A peer that stopped reading — alive, TCP window full, socket open — parked
 * the SENDING APPLICATION THREAD inside that syscall indefinitely. #838 bounded the block by a
 * liveness window; #2252 removes it: every write is nonblocking, what the socket did not take
 * stays held in the link's egress, and the link's driving thread resumes it on writability.
 *
 * Two levels, because the two halves fail differently:
 *
 *  1. **The helper, over a socketpair whose buffers are FULL.** A write returns at once as
 *     STALLED (arm 1), the egress strikes the stalling peer once its lane is full and closes
 *     it after `kEgressStallStrikes` whole rings of offers past that depth that moved nothing
 *     (arm 2), a record the socket took only part of keeps its place and finishes, so the
 *     framing never desyncs (arm 3, #2144), and a turn that moves bytes clears the strikes
 *     (arm 4). Deterministic — a socketpair with no reader stays full forever.
 *  2. **A real `tcp_transport_t` against a peer that never reads** (arms 5–7): `send()`
 *     returns at once, every record behind a held one reaches the peer in order once it reads,
 *     a full ring takes its vertex's arm, counted, and a peer that never reads is torn down.
 *
 * #1295's residual arm is the concurrent directed batch on a real peer-named server, which
 * since #2144 costs the senders no wait at all, and the admission arm (#2247) admits 4,096
 * real peers, the injected cap alone.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/posix_endpoint.hpp"
#include "libtracer/transport_tcp.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;
using clock_t_ = std::chrono::steady_clock;

/**
 * @brief Test shim republishing the protected write path and its stall policy.
 *
 * The same idiom `write_fault_class_test` uses: a derived class may re-export a protected
 * member, which lets the policy be driven over a raw socketpair with no transport, framing
 * or recv thread in the way.
 */
struct stall_probe_t : tr::net::stream_endpoint_t {
    using tr::net::stream_endpoint_t::conn_fd_;
    using tr::net::stream_endpoint_t::drops_;
    using tr::net::stream_endpoint_t::stalled_tx_;
    using tr::net::stream_endpoint_t::write_all;

    /** @brief Drive the egress on @p fd. */
    explicit stall_probe_t(int fd = -1) { conn_fd_.store(fd, std::memory_order_relaxed); }
    /** @brief The fd belongs to the test's pair, not to this probe. */
    ~stall_probe_t() { conn_fd_.store(-1, std::memory_order_relaxed); }
    stall_probe_t(const stall_probe_t&) = delete;
    stall_probe_t& operator=(const stall_probe_t&) = delete;

    /** @brief One record through the link's egress, as `tcp_transport_t::send` puts it. */
    void send(std::span<const std::byte> rec) {
        const ::iovec v{const_cast<std::byte*>(rec.data()), rec.size()};
        const std::span<const ::iovec> vec(&v, 1);
        tr::net::egress_once_t once([&] { return frame_of(vec); });
        egress_send(once, [&](int fd) { return write_gather(fd, vec); });
    }
};

/** @brief The longest one send may take into a full socket: a frame copy and one refused
 *         syscall, with slack for a loaded sanitizer runner. A blocking write slept a whole
 *         liveness window here. */
constexpr std::int64_t kNoWaitMs = 250;

/** @brief Milliseconds elapsed since @p from. */
[[nodiscard]] std::int64_t ms_since(clock_t_::time_point from) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now() - from).count();
}

/**
 * @brief A connected socketpair whose send direction is FULL.
 *
 * `a` carries a small send buffer, `b` is never read, and everything that fits has already
 * been pushed in — so every further write on `a` is refused.
 */
struct stalled_pair_t {
    int a = -1; /**< @brief The sending end (full). */
    int b = -1; /**< @brief The peer that never reads. */

    stalled_pair_t() {
        int sv[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
        a = sv[0];
        b = sv[1];
        // Small buffers so the fill below is quick; the kernel may round these up, which
        // only costs the loop a few more passes.
        const int small = 4096;
        ::setsockopt(a, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
        ::setsockopt(b, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        jam(a);
    }

    /** @brief Fill @p fd 's send direction, nonblocking, so the fill itself cannot hang. */
    static void jam(int fd) {
        std::vector<std::byte> junk(4096);
        while (::send(fd, junk.data(), junk.size(), MSG_NOSIGNAL | MSG_DONTWAIT) > 0) {
        }
    }

    ~stalled_pair_t() {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }

    stalled_pair_t(const stalled_pair_t&) = delete;
    stalled_pair_t& operator=(const stalled_pair_t&) = delete;
};

/** @brief True once @p fd has been shut down: a write on it fails immediately. */
[[nodiscard]] bool is_shut_down(int fd) {
    const std::byte probe{0x01};
    return ::send(fd, &probe, 1, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 && errno == EPIPE;
}

/**
 * @brief Arm 1: a write into a full socket returns at once, and names the right fault.
 */
void test_full_socket_write_returns() {
    std::printf("a write into a peer that stopped reading returns at once:\n");
    const stalled_pair_t p;
    check(p.a >= 0, "socketpair up, send buffer full");

    std::array<std::byte, 64> payload{};
    const auto t0 = clock_t_::now();
    const tr::net::write_result_t r =
        stall_probe_t::write_all(p.a, std::span<const std::byte>(payload));
    const std::int64_t took = ms_since(t0);

    check(r.outcome == tr::net::write_outcome_t::STALLED,
          "the record is classified STALLED — not a disconnect, and not a malformed call");
    check(r.moved == 0, "nothing of it reached the wire, so the stream is still in framing sync");
    check(took < kNoWaitMs, "and it RETURNED at once — the pre-#838 path blocks here forever");
    std::printf("  returned after %lld ms\n", static_cast<long long>(took));
}

/**
 * @brief Jam @p probe's lane: one record held on the wire and the control lane's whole depth
 *        waiting behind it. Every turn moves nothing, and none is a strike: the lane had room.
 */
void fill_lane(stall_probe_t& probe, std::span<const std::byte> rec) {
    for (std::size_t i = 0; i <= tr::net::kEgressControlDepth; ++i) probe.send(rec);
}

/** @brief Strikes in one whole ring of the control lane: the unit the verdict counts in. */
constexpr std::uint64_t kRing = tr::net::kEgressControlDepth;

/** @brief Offer @p rings whole rings of records past @p probe's full lane. */
void strike_rings(stall_probe_t& probe, std::span<const std::byte> rec, std::uint64_t rings) {
    for (std::uint64_t i = 0; i < rings * kRing; ++i) probe.send(rec);
}

/** @brief Whole rings of strikes that condemn a peer (`kEgressStallStrikes`). */
constexpr std::uint64_t kCap = tr::net::kEgressStallStrikes;

/**
 * @brief Arm 2: past a full lane, `kEgressStallStrikes` whole rings of offers in a row that
 *        move nothing close the peer; one fewer does not — and the frames inside the lane's
 *        depth wait instead of being shed.
 */
void test_streak_closes_the_peer() {
    std::printf("the brokenness detector strikes the stalling peer, then closes it:\n");
    const stalled_pair_t p;
    stall_probe_t probe(p.a);
    const std::array<std::byte, 64> rec{};

    fill_lane(probe, rec);
    check(probe.stalled_tx_.load() == 0 && probe.drops_.snapshot().dropped_tx == 0,
          "inside the lane's depth a turn that moves nothing waits: no strike, nothing shed");
    strike_rings(probe, rec, kCap - 1);
    check(probe.stalled_tx_.load() == (kCap - 1) * kRing && !is_shut_down(p.a),
          "one ring short of the cap is backpressure, not a verdict");
    strike_rings(probe, rec, 1);
    check(is_shut_down(p.a), "the ring that reaches the cap closes the peer");
    check(probe.stalled_tx_.load(std::memory_order_relaxed) == kCap * kRing,
          "every stalled turn is COUNTED — a stalled peer is never a silent drop");
    check(probe.drops_.snapshot().dropped_tx == kCap * kRing,
          "each strike cost the lane its oldest frame, counted (#2144)");
}

/**
 * @brief Arm 3 (#2144): a record the socket took only part of keeps its place and finishes on
 *        the next turn, so the stream never desyncs and the peer is not condemned for it.
 *
 * Before #2144 a part-written record condemned the session at once: the rest of it was lost,
 * so the peer's framing could never recover. The egress keeps the frame and its offset.
 */
void test_partial_record_finishes() {
    std::printf("a record that half-reached the wire is finished, not abandoned:\n");
    int sv[2] = {-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair up");
    const int small = 4096;
    ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    stall_probe_t probe(sv[0]);

    std::vector<std::byte> big(1024 * 1024);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = std::byte(i % 251);
    probe.send(big);  // takes what fits and returns, the rest held
    check(!is_shut_down(sv[0]), "the part-written record does not condemn the peer");
    check(probe.drops_.snapshot().dropped_tx == 0, "and is not shed");

    // The peer reads again; each later send resumes the held record (this probe has no
    // driving thread to do it on writability), then its own record follows it.
    std::vector<std::byte> got;
    std::atomic<std::size_t> got_n{0};
    std::thread reader([&] {
        const timeval tv{.tv_sec = 5, .tv_usec = 0};
        ::setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::array<std::byte, 65536> chunk;
        while (got.size() < big.size() + 64) {
            const ssize_t n = ::recv(sv[1], chunk.data(), chunk.size(), 0);
            if (n <= 0) break;
            got.insert(got.end(), chunk.begin(), chunk.begin() + n);
            got_n.store(got.size(), std::memory_order_release);
        }
    });
    const std::vector<std::byte> marker(64, std::byte{0xCC});
    for (int i = 0; i < 5000 && got_n.load(std::memory_order_acquire) < big.size() + 64; ++i) {
        probe.send(marker);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    reader.join();
    bool whole = got.size() >= big.size() + 64;
    for (std::size_t i = 0; whole && i < big.size(); ++i) whole = got[i] == big[i];
    for (std::size_t i = 0; whole && i < 64; ++i) whole = got[big.size() + i] == std::byte{0xCC};
    check(whole, "the peer read the whole record, then the next one: framing intact");
    ::close(sv[0]);
    ::close(sv[1]);
}

/**
 * @brief Arm 4: a turn that moves bytes clears the strikes — the trichotomy's middle arm.
 */
void test_completion_resets_the_streak() {
    std::printf("a peer that takes the next frame is riding out a burst, not broken:\n");
    const stalled_pair_t p;
    stall_probe_t probe(p.a);
    const std::array<std::byte, 64> rec{};

    fill_lane(probe, rec);
    strike_rings(probe, rec, kCap - 1);
    check(probe.stalled_tx_.load() == (kCap - 1) * kRing, "one ring short of the cap accrued");
    // The peer reads everything it was sent: the socket has room again.
    const int flags = ::fcntl(p.b, F_GETFL, 0);
    ::fcntl(p.b, F_SETFL, flags | O_NONBLOCK);
    std::array<std::byte, 65536> sink;
    for (int i = 0; i < 20; ++i) {
        while (::recv(p.b, sink.data(), sink.size(), 0) > 0) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    probe.send(rec);
    check(probe.stalled_tx_.load() == (kCap - 1) * kRing && !is_shut_down(p.a),
          "the turn that moved the waiting frames clears the strikes");
    // Jam it again: as many rings again would pass the cap, had the streak not restarted.
    stalled_pair_t::jam(p.a);
    fill_lane(probe, rec);
    strike_rings(probe, rec, kCap - 1);
    check(probe.stalled_tx_.load() == 2 * (kCap - 1) * kRing && !is_shut_down(p.a),
          "so the next stall starts a fresh streak instead of closing");
}

/**
 * @brief #2247 — the admission cap is the injected `max_peers` alone: 4,096 real peers are
 *        admitted, the next is refused, and none of the admitted is evicted for it.
 *
 * #1295 clamped every server to the window over its 100 ms floor (100 peers), because
 * each admitted peer could cost the node one blocking write. #2144 made those writes
 * nonblocking, so the ceiling had no reason left. Each peer is a real loopback connection: the
 * clients and the server's sessions together need about 8,200 descriptors, so the soft limit is
 * raised to that first. The server accepts in arrival order, so once the 4,097th client reads
 * its refusal every earlier one has been admitted.
 */
void test_admits_4096_peers() {
    std::printf("the admission cap is the injected max_peers, 4,096 real peers:\n");
    constexpr std::size_t kPeers = 4096;
    constexpr rlim_t kFds = 2 * kPeers + 256;
    ::rlimit fds{};
    ::getrlimit(RLIMIT_NOFILE, &fds);
    fds.rlim_cur = std::min(kFds, fds.rlim_max);
    ::setrlimit(RLIMIT_NOFILE, &fds);
    check(fds.rlim_cur == kFds, "the descriptor limit holds 4,096 peers and their sessions");

    tr::net::tcp_server_transport_t server(0, {.max_peers = kPeers, .peer_named = true});
    check(server.ok(), "the peer-named listener is up");
    check(server.max_peers() == kPeers,
          "a cap above the old window ceiling is enforced as given, not clamped to 100");

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(server.local_port());
    const auto dial = [&to] {
        const int c = ::socket(AF_INET, SOCK_STREAM, 0);
        return ::connect(c, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 ? c : -1;
    };
    std::vector<int> clients(kPeers);
    std::size_t connected = 0;
    for (int& c : clients) connected += (c = dial()) >= 0 ? 1U : 0U;
    check(connected == kPeers, "every client connected");

    // The newcomer past the cap is accepted and closed at once: it reads end-of-stream.
    const int extra = dial();
    ::pollfd p{extra, POLLIN, 0};
    char byte = 0;
    check(::poll(&p, 1, 20000) == 1, "the 4,097th client hears from the server");
    check(::recv(extra, &byte, 1, 0) == 0, "the 4,097th peer is refused with a clean close");
    std::size_t admitted = 0;
    server.bus()->enumerate_peers([&admitted](std::string_view) { ++admitted; });
    check(admitted == kPeers, "every one of the 4,096 holds a slot; none was evicted");
    ::close(extra);
    for (const int c : clients) ::close(c);
}

/**
 * @brief Arm 7: the end-to-end property, over a real `tcp_transport_t`.
 *
 * A peer that accepts the connection and then never reads a byte. Pre-#838 the first send
 * that fills the window never returns and this function hangs. Now every send returns at once
 * (#2252), the frames it could not write wait in the link's egress, only those its ring had
 * no room for are shed, and the strikes take the dead peer down.
 */
void test_tcp_link_sheds_and_drops_a_stalled_peer() {
    std::printf("a real tcp link sheds frames for a stalled peer and then drops it:\n");
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    check(listener >= 0, "listener socket");
    const int one = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // A small receive buffer on the accepting side, so the peer's window fills after a few
    // frames instead of megabytes of them.
    const int small = 4096;
    ::setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    check(::bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "bound");
    check(::listen(listener, 1) == 0, "listening");
    socklen_t llen = sizeof(local);
    check(::getsockname(listener, reinterpret_cast<sockaddr*>(&local), &llen) == 0,
          "port resolved");

    tr::net::tcp_transport_t link("127.0.0.1", ntohs(local.sin_port), {});
    check(link.ok(), "the dial succeeded");
    link.start_receiving();

    const int peer = ::accept(listener, nullptr, nullptr);
    check(peer >= 0, "the peer accepted the connection — and now never reads a byte");

    // Push frames until the strikes reach the cap. No send waits, so the loop is bounded by
    // its count alone.
    const std::vector<std::byte> frame(64 * 1024);
    std::int64_t slowest = 0;
    for (int sent = 0; sent < 4096 && link.stalled_tx() < kCap * kRing; ++sent) {
        const auto t0 = clock_t_::now();
        link.send(std::span(frame));
        slowest = std::max(slowest, ms_since(t0));
    }

    check(link.stalled_tx() >= kCap * kRing,
          "the stalled peer cost the link write turns, and they are COUNTED");
    check(slowest < kNoWaitMs, "and no send waited on the stalled peer");
    for (int i = 0; i < 4; ++i) link.send(std::span(frame));
    check(link.drop_stats().dropped_tx > 0,
          "a send to the condemned peer is shed and counted — never a silent drop");

    // The verdict reaches the connection: shutdown makes the recv loop see EOF, which runs
    // the ordinary remote-departure teardown.
    for (int i = 0; i < 400 && link.link_up(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!link.link_up(), "the peer that would not take bytes is dropped, not blocked on");
    std::printf("  stalled=%llu dropped=%llu, slowest send %lld ms\n",
                static_cast<unsigned long long>(link.stalled_tx()),
                static_cast<unsigned long long>(link.drop_stats().dropped_tx),
                static_cast<long long>(slowest));

    ::close(peer);
    ::close(listener);
}

/**
 * @brief #1295 and #2144: N concurrent DIRECTED sends to N stalled peers cost the senders no
 *        wait at all, and no stalled peer delays another.
 *
 * #1295's defect: a directed send took the whole liveness window for itself, and directed
 * sends on one server serialized behind one write mutex, so N callers targeting N stalled
 * peers queued up N full windows. #2144 removes the mutex and makes every server write
 * nonblocking: a full socket refuses the syscall at once, the frame waits in that peer's own
 * ring, and the batch costs syscalls, not windows.
 */
void test_concurrent_directed_sends_cost_no_wait() {
    std::printf("N concurrent directed sends to N stalled peers cost the senders no wait:\n");
    const std::size_t peers = 4;
    tr::net::tcp_server_transport_t server(0, {.max_peers = peers, .peer_named = true});
    check(server.ok(), "the peer-named listener is up");
    check(server.max_peers() == peers, "the injected cap is the one enforced");

    // N clients that connect, then never read a byte. Small receive buffers so a couple of
    // frames fill each window instead of megabytes of them.
    std::vector<int> clients;
    for (std::size_t i = 0; i < peers; ++i) {
        const int c = ::socket(AF_INET, SOCK_STREAM, 0);
        check(c >= 0, "client socket");
        const int small = 4096;
        ::setsockopt(c, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(server.local_port());
        check(::connect(c, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0, "client connected");
        clients.push_back(c);
    }

    // Wait for every slot to be accepted and named, so the directed lookups all resolve.
    std::size_t seen = 0;
    for (int i = 0; i < 400 && seen < peers; ++i) {
        seen = 0;
        server.bus()->enumerate_peers([&seen](std::string_view) { ++seen; });
        if (seen < peers) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(seen == peers, "every peer occupies a slot and is directed-addressable");

    // Each thread drives ONE peer: push frames until its socket stops taking bytes, then
    // keep pushing into the stall. Buffer fills are memcpy-speed; anything worth
    // milliseconds in here would be blocking, which is exactly what must not happen.
    const std::vector<std::byte> frame(512 * 1024);
    const int frames_per_peer = 24;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> senders;
    for (std::size_t i = 0; i < peers; ++i)
        senders.emplace_back([&server, &frame, &ready, &go, frames_per_peer, i] {
            const std::string name = "p" + std::to_string(i) + "~1";  // first tenancy
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int n = 0; n < frames_per_peer; ++n) {
                // Resolve per use, never cached across a departure (#1153).
                tr::net::transport_t* to_peer = server.bus()->peer_link(name);
                if (to_peer == nullptr) return;
                to_peer->send(std::span<const std::byte>(frame));
            }
        });
    while (ready.load(std::memory_order_relaxed) < static_cast<int>(peers))
        std::this_thread::yield();
    const auto t0 = clock_t_::now();
    go.store(true, std::memory_order_release);
    for (std::thread& t : senders) t.join();
    const std::int64_t took = ms_since(t0);

    check(server.drop_stats().dropped_tx >= peers,
          "every peer did stall — the batch ran into full sockets and overflowed the rings, "
          "it did not just fit in the socket buffers");
    // Copying the frames costs milliseconds (most of a second under TSan); a sender that
    // waited even one 100 ms quantum per send would pay 2.4 s for its 24.
    check(took < kNoWaitMs * frames_per_peer / 3,
          "and the whole batch took copies, not waits — no sender waited on a stalled peer's "
          "socket, its own or another's (#2144)");
    std::printf("  peers=%zu — the concurrent batch took %lld ms, dropped=%llu\n", peers,
                static_cast<long long>(took),
                static_cast<unsigned long long>(server.drop_stats().dropped_tx));

    for (const int c : clients) ::close(c);
}

/**
 * @brief A dialed `tcp_transport_t` whose accepted peer does not read until told to — the
 *        fixture for the two queue-delivery cases below.
 */
struct parked_peer_t {
    int listener = -1; /**< @brief The listening socket. */
    int peer = -1;     /**< @brief The accepted end, which reads only in @ref read_records. */
    std::unique_ptr<tr::net::tcp_transport_t> link; /**< @brief The link under test. */

    /** @brief Dial, with a small peer receive buffer. */
    parked_peer_t() {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        const int small = 4096;
        ::setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        (void)::bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local));
        (void)::listen(listener, 1);
        socklen_t llen = sizeof(local);
        (void)::getsockname(listener, reinterpret_cast<sockaddr*>(&local), &llen);
        link = std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", ntohs(local.sin_port),
                                                          tr::net::tcp_config_t{});
        link->start_receiving();
        peer = ::accept(listener, nullptr, nullptr);
        const timeval tv{.tv_sec = 5, .tv_usec = 0};
        ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~parked_peer_t() {
        link.reset();
        if (peer >= 0) ::close(peer);
        if (listener >= 0) ::close(listener);
    }
    parked_peer_t(const parked_peer_t&) = delete;
    parked_peer_t& operator=(const parked_peer_t&) = delete;

    /** @brief One record as it arrived: its length and its first byte (the test's marker). */
    struct record_t {
        std::size_t len = 0;   /**< @brief Body bytes after the 4-byte length prefix. */
        std::uint8_t mark = 0; /**< @brief The body's first byte. */
    };

    /** @brief Read exactly @p n bytes, or fail on EOF / the 5 s receive timeout. */
    bool read_n(std::byte* dst, std::size_t n) const {
        std::size_t off = 0;
        while (off < n) {
            const ssize_t r = ::recv(peer, dst + off, n - off, 0);
            if (r <= 0) return false;
            off += static_cast<std::size_t>(r);
        }
        return true;
    }

    /** @brief Start reading now: collect up to @p want records, in arrival order. */
    std::vector<record_t> read_records(std::size_t want) const {
        std::vector<record_t> out;
        std::vector<std::byte> body;
        while (out.size() < want) {
            std::array<std::byte, 4> prefix{};
            if (!read_n(prefix.data(), prefix.size())) break;
            const std::size_t len = std::to_integer<std::size_t>(prefix[0]) |
                                    (std::to_integer<std::size_t>(prefix[1]) << 8) |
                                    (std::to_integer<std::size_t>(prefix[2]) << 16) |
                                    (std::to_integer<std::size_t>(prefix[3]) << 24);
            body.resize(len);
            if (!read_n(body.data(), len)) break;
            out.push_back(
                {len, len == 0 ? std::uint8_t{0} : std::to_integer<std::uint8_t>(body[0])});
        }
        return out;
    }
};

/** @brief Send @p rec on @p p 's link and answer whether it returned at once. */
bool returns_at_once(parked_peer_t& p, std::span<const std::byte> rec) {
    const auto t0 = clock_t_::now();
    p.link->send(rec);
    return ms_since(t0) < kNoWaitMs;
}

/**
 * @brief #1619, #2144, #2252: a send into a peer that stopped reading returns at once with its
 *        record held, a second publisher returns at once behind it, and once the peer reads
 *        every record arrives, in admission order, with no further send.
 *
 * Nothing is sent after the peer starts reading, so the held record and the two queued
 * behind it can only be finished by the link's own wait on writability.
 */
void test_held_and_queued_records_are_all_delivered_in_order() {
    std::printf("a send into a stalled peer returns at once; every record is delivered:\n");
    parked_peer_t p;
    check(p.link->ok() && p.peer >= 0, "the link is dialed and the peer accepted");
    const std::vector<std::byte> big(8 * 1024 * 1024, std::byte{0x11});
    check(returns_at_once(p, big), "an 8 MiB send returned at once with its record cut short");
    check(p.link->drop_stats().dropped_tx == 0, "the rest is held, not shed");
    const std::vector<std::byte> a(64, std::byte{0xA1});
    const std::vector<std::byte> b(64, std::byte{0xA2});
    check(returns_at_once(p, a) && returns_at_once(p, b),
          "two more publishers returned at once, queued behind it");

    const std::vector<parked_peer_t::record_t> got = p.read_records(3);
    check(got.size() == 3, "all three records reached the peer");
    if (got.size() == 3) {
        check(got[0].len == big.size() && got[0].mark == 0x11, "the cut record first, whole");
        check(got[1].len == 64 && got[1].mark == 0xA1, "then the first queued record");
        check(got[2].len == 64 && got[2].mark == 0xA2, "then the second, in admission order");
    }
    check(p.link->drop_stats().dropped_tx == 0, "and nothing was dropped");
}

/**
 * @brief #1619 / #2144: past the ring's depth a record is never waited on. A delivery waits in
 *        its vertex's own ring, at the depth the vertex declared, and a full one takes the
 *        vertex's arm — best-effort sheds its OLDEST waiting record, reliable refuses the
 *        NEWEST — and the loss is counted; everything else still arrives, in order.
 */
void full_ring_case(bool reliable) {
    constexpr std::size_t depth = 8;
    parked_peer_t p;
    static int vertex = 0;  // the lane's identity: compared, never dereferenced
    const tr::graph::delivery_lane_t lane{
        .key = &vertex, .depth = static_cast<std::uint16_t>(depth), .reliable = reliable};
    const auto deliver = [&](std::uint8_t mark) {
        const std::vector<std::byte> body(64, std::byte{mark});
        const tr::graph::value_ref_t v = tr::graph::value_ref_t::adopt(
            tr::graph::value_t::make_copy(body, tr::mem::heap_source()));
        p.link->send(std::span<const std::span<const std::byte>>(), *v, lane);
    };
    check(p.link->ok() && p.peer >= 0, "the link is dialed and the peer accepted");
    const std::vector<std::byte> big(8 * 1024 * 1024, std::byte{0x11});
    check(returns_at_once(p, big), "the first record is held on the peer");
    const std::uint64_t before = p.link->drop_stats().dropped_tx;
    for (std::size_t i = 0; i < depth; ++i) deliver(static_cast<std::uint8_t>(0xB0 + i));
    check(p.link->drop_stats().dropped_tx == before, "the vertex's ring took all of its depth");
    const auto t0 = clock_t_::now();
    deliver(0xEE);
    check(ms_since(t0) < 100, "the record past the depth returned at once");
    check(p.link->drop_stats().dropped_tx == before + 1, "and one record was lost and counted");

    const std::vector<parked_peer_t::record_t> got = p.read_records(depth + 2);
    check(got.size() == depth + 1, "the writer's record and a full ring arrived, no more");
    std::vector<std::uint8_t> want{0x11};
    for (std::size_t i = reliable ? 0 : 1; i < depth; ++i)
        want.push_back(static_cast<std::uint8_t>(0xB0 + i));
    if (!reliable) want.push_back(0xEE);
    bool order = got.size() == want.size();
    for (std::size_t i = 0; order && i < got.size(); ++i) order = got[i].mark == want[i];
    check(order, reliable ? "reliable: in order, the refused newest record not among them"
                          : "best-effort: in order, the shed oldest record not among them");
}

void test_full_ring_takes_its_arm() {
    std::printf("a full best-effort ring sheds its oldest record and counts it:\n");
    full_ring_case(false);
    std::printf("a full reliable ring refuses the newest record and counts it:\n");
    full_ring_case(true);
}
}  // namespace

int main() {
    // #1438 — see tcp_test's twin of this guard: the directed-send cases resolve their
    // targets through `bus()->peer_link(...)`, which is null on a build with no bus module,
    // so this suite crashed there instead of reporting. Skip, do not crash.
    if constexpr (!tr::net::kBusLinks)
        return tr::testing::skipped("send_stall",
                                    "this build closed the ADR-0044 bus module out "
                                    "(kBusLinks = false); the directed per-peer sends under "
                                    "test have no peer tier to resolve through");
    std::printf("== #838, #2252: host stream sends never wait, are counted and attributed ==\n");
    test_full_socket_write_returns();
    test_streak_closes_the_peer();
    test_partial_record_finishes();
    test_completion_resets_the_streak();
    test_admits_4096_peers();
    test_tcp_link_sheds_and_drops_a_stalled_peer();
    test_held_and_queued_records_are_all_delivered_in_order();
    test_full_ring_takes_its_arm();
    test_concurrent_directed_sends_cost_no_wait();
    // The shared runner's verdict, not a hard-coded "all checks passed": this suite used to
    // print that line and `return 0` whatever the counter said, so a red check here could
    // never redden ctest — and #1295's guard is only a guard if it can (#874's summary).
    return tr::testing::summary("send_stall_test");
}
