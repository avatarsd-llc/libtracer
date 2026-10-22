/**
 * @file
 * @brief #838 — a host stream send into a stalled peer is BOUNDED, counted, and the peer
 *        that keeps stalling is closed rather than blocked on forever.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The defect: `tcp_transport_t::send` held `write_m_` across a fully blocking
 * `write_all_iov` with no `SO_SNDTIMEO` anywhere in the file. A peer that stopped reading —
 * alive, TCP window full, socket open — parked the SENDING APPLICATION THREAD inside that
 * syscall indefinitely, and every other sender on the link behind the mutex with it. The
 * host twin of #835 / PR #837, whose ruling this follows: bound the write, never silently
 * drop, attribute the stall to the peer that earned it.
 *
 * Two levels, because the two halves of the fix fail differently:
 *
 *  1. **The helper, over a socketpair whose buffers are FULL.** The unit that has to be
 *     right: a bounded write returns inside its bound instead of never (arms 1–2), the
 *     egress strikes the stalling peer once its lane is full and closes it after three whole
 *     rings of offers past that depth that moved nothing (arm 4), a record the socket took only
 * part of keeps its place and finishes, so the framing never desyncs (arm 5, #2144), and a turn
 * that moves bytes clears the strikes (arm 6). Deterministic — a socketpair with no reader stays
 * full forever.
 *  2. **A real `tcp_transport_t` against a peer that never reads** (arm 7). The property
 *     that actually matters end to end, and the one a helper test cannot show: `send()`
 *     RETURNS, the frame is counted rather than silently lost, and the link is torn down.
 *     Before the fix this arm hangs the test process; that is the regression it exists to
 *     redden.
 *
 * A third level was added by **#1295**, which found the bound was a per-CALL property
 * rather than a node one: a directed send is a round of ONE, so it took the whole window
 * for itself, and N of them to N stalled peers serialized N windows behind the same write
 * mutex. The residual arm is the concurrent directed batch on a real peer-named server,
 * which since #2144, with the shared write mutex gone and every server write nonblocking,
 * costs the senders no wait at all. That also retired #1295's window-derived peer ceiling,
 * so the admission arm (#2247) admits 4,096 real peers, the injected cap alone.
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
    using tr::net::stream_endpoint_t::dropped_tx_;
    using tr::net::stream_endpoint_t::liveness_window_ms_;
    using tr::net::stream_endpoint_t::set_snd_timeout;
    using tr::net::stream_endpoint_t::stalled_tx_;
    using tr::net::stream_endpoint_t::write_all;

    /** @brief Drive the egress on @p fd with a one-quantum bound (the window's floor). */
    explicit stall_probe_t(int fd = -1) {
        liveness_window_ms_ = tr::net::kBoundedWaitMs;
        conn_fd_.store(fd, std::memory_order_relaxed);
    }
    /** @brief The fd belongs to the test's pair, not to this probe. */
    ~stall_probe_t() { conn_fd_.store(-1, std::memory_order_relaxed); }
    stall_probe_t(const stall_probe_t&) = delete;
    stall_probe_t& operator=(const stall_probe_t&) = delete;

    /** @brief One record through the link's egress, as `tcp_transport_t::send` puts it. */
    void send(std::span<const std::byte> rec) {
        const ::iovec v{const_cast<std::byte*>(rec.data()), rec.size()};
        const std::span<const ::iovec> vec(&v, 1);
        tr::net::egress_once_t once([&] { return frame_of(vec); });
        egress_send(
            once, [&](int fd, std::uint32_t bound) { return write_gather(fd, vec, bound, false); });
    }
};

/** @brief Milliseconds elapsed since @p from. */
[[nodiscard]] std::int64_t ms_since(clock_t_::time_point from) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now() - from).count();
}

/**
 * @brief A connected socketpair whose send direction is FULL and bounded.
 *
 * `a` carries a small send buffer, `b` is never read, and everything that fits has already
 * been pushed in — so every further write on `a` can only expire. `SO_SNDTIMEO` is armed by
 * the code under test's own helper, which is also what makes the fill loop terminate.
 */
struct stalled_pair_t {
    int a = -1; /**< @brief The sending end (full, bounded). */
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
        // Fill non-blocking, so the fill itself cannot be the thing that hangs.
        const int flags = ::fcntl(a, F_GETFL, 0);
        ::fcntl(a, F_SETFL, flags | O_NONBLOCK);
        std::vector<std::byte> junk(4096);
        while (::send(a, junk.data(), junk.size(), MSG_NOSIGNAL) > 0) {
        }
        ::fcntl(a, F_SETFL, flags);
        stall_probe_t::set_snd_timeout(a);
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
    return ::send(fd, &probe, 1, MSG_NOSIGNAL) < 0 && errno == EPIPE;
}

/**
 * @brief Arms 1–3: the bound exists, it is honoured, and it names the right fault.
 */
void test_bounded_write_returns() {
    std::printf("a write into a peer that stopped reading returns at its bound:\n");
    const stalled_pair_t p;
    check(p.a >= 0, "socketpair up, send buffer full, SO_SNDTIMEO armed");

    const std::uint32_t bound = 300;
    std::array<std::byte, 64> payload{};
    const auto t0 = clock_t_::now();
    const tr::net::write_result_t r =
        stall_probe_t::write_all(p.a, std::span<const std::byte>(payload), bound);
    const std::int64_t took = ms_since(t0);

    check(r.outcome == tr::net::write_outcome_t::STALLED,
          "the record is classified STALLED — not a disconnect, and not a malformed call");
    check(r.moved == 0, "nothing of it reached the wire, so the stream is still in framing sync");
    check(took >= static_cast<std::int64_t>(bound),
          "it did not give up early: the peer got its whole window");
    check(took < static_cast<std::int64_t>(bound) * 20,
          "and it RETURNED — the pre-#838 path blocks here forever");
    std::printf("  bound=%u ms, returned after %lld ms\n", bound, static_cast<long long>(took));
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

/**
 * @brief Arm 4: past a full lane, three whole rings of offers in a row that move nothing
 *        close the peer; one or two do not — and the frames inside the lane's depth wait
 *        instead of being shed.
 */
void test_streak_closes_the_peer() {
    std::printf("the brokenness detector strikes the stalling peer, three and out:\n");
    const stalled_pair_t p;
    stall_probe_t probe(p.a);
    const std::array<std::byte, 64> rec{};

    fill_lane(probe, rec);
    check(probe.stalled_tx_.load() == 0 && probe.dropped_tx_.load() == 0,
          "inside the lane's depth a turn that moves nothing waits: no strike, nothing shed");
    strike_rings(probe, rec, 1);
    check(probe.stalled_tx_.load() == kRing && !is_shut_down(p.a),
          "one ring of stalls is transient backpressure, not a verdict");
    strike_rings(probe, rec, 1);
    check(probe.stalled_tx_.load() == 2 * kRing && !is_shut_down(p.a),
          "two can straddle a burst — still open");
    strike_rings(probe, rec, 1);
    check(is_shut_down(p.a), "the third consecutive ring closes the peer");
    check(probe.stalled_tx_.load(std::memory_order_relaxed) == 3 * kRing,
          "every stalled turn is COUNTED — a timed-out peer is never a silent drop");
    check(probe.dropped_tx_.load() == 3 * kRing,
          "each strike cost the lane its oldest frame, counted (#2144)");
}

/**
 * @brief Arm 5 (#2144): a record the socket took only part of keeps its place and finishes on
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
    stall_probe_t::set_snd_timeout(sv[0]);
    stall_probe_t probe(sv[0]);

    std::vector<std::byte> big(1024 * 1024);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = std::byte(i % 251);
    probe.send(big);  // takes what fits, then runs its bound out with the rest unwritten
    check(!is_shut_down(sv[0]), "the part-written record does not condemn the peer");
    check(probe.dropped_tx_.load() == 0, "and is not shed");

    // The peer reads again; the next send resumes the held record, then writes its own.
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
    for (int i = 0; i < 50 && probe.stalled_tx_.load() < tr::net::kMaxConsecutiveStalls; ++i) {
        probe.send(marker);
        if (got_n.load(std::memory_order_acquire) >= big.size() + 64) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
 * @brief Arm 6: a turn that moves bytes clears the strikes — the trichotomy's middle arm.
 */
void test_completion_resets_the_streak() {
    std::printf("a peer that takes the next frame is riding out a burst, not broken:\n");
    const stalled_pair_t p;
    stall_probe_t probe(p.a);
    const std::array<std::byte, 64> rec{};

    fill_lane(probe, rec);
    strike_rings(probe, rec, 2);
    check(probe.stalled_tx_.load() == 2 * kRing, "two rings of strikes accrued");
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
    check(probe.stalled_tx_.load() == 2 * kRing && !is_shut_down(p.a),
          "the turn that moved the waiting frames clears the strikes");
    // Jam it again: one fresh ring is a fresh streak of one, not the third.
    ::fcntl(p.a, F_SETFL, ::fcntl(p.a, F_GETFL, 0) | O_NONBLOCK);
    std::vector<std::byte> junk(4096);
    while (::send(p.a, junk.data(), junk.size(), MSG_NOSIGNAL) > 0) {
    }
    ::fcntl(p.a, F_SETFL, ::fcntl(p.a, F_GETFL, 0) & ~O_NONBLOCK);
    fill_lane(probe, rec);
    strike_rings(probe, rec, 1);
    check(probe.stalled_tx_.load() == 3 * kRing && !is_shut_down(p.a),
          "so the next stall starts a fresh streak instead of closing");
}

/**
 * @brief The derivation itself: a round fits in one window, and never rounds to "forever".
 */
void test_bound_derivation() {
    std::printf("the per-record bound is derived, floored, and never zero:\n");
    using tr::net::derive_send_bound_ms;
    check(derive_send_bound_ms(10000, 1) == 10000, "one peer in the round gets the whole window");
    check(derive_send_bound_ms(10000, 4) == 2500,
          "four peers split it — the whole fan-out still fits inside one window");
    check(derive_send_bound_ms(0, 1) == tr::net::kDefaultLivenessWindowMs,
          "an unconfigured deployment gets the conservative default clamp, never 'unbounded'");
    check(derive_send_bound_ms(100, 1000) == tr::net::kBoundedWaitMs,
          "the division is floored: SO_SNDTIMEO 0 means BLOCK FOREVER, which is the defect");
    check(derive_send_bound_ms(10000, 0) == 10000,
          "an empty round is one bound, not a divide-by-0");
}

/**
 * @brief #2247 — the admission cap is the injected `max_peers` alone: 4,096 real peers are
 *        admitted, the next is refused, and none of the admitted is evicted for it.
 *
 * #1295 clamped every server to `window / kBoundedWaitMs` (100 on the default window), because
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
 * that fills the window never returns and this function hangs; the fix makes it return,
 * count the stalled turns, and take the dead peer down. Since #2144 the frames it could not
 * write wait in the link's egress, and only those its ring had no room for are shed.
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

    // A SHORT liveness window, injected exactly as a deployment would: the number is the
    // app's, not the library's, which is the whole point of the provenance ruling.
    const std::uint32_t window = 400;
    tr::net::tcp_transport_t link("127.0.0.1", ntohs(local.sin_port),
                                  {.liveness_window_ms = window});
    check(link.ok(), "the dial succeeded");
    check(link.liveness_window_ms() == window, "and the injected window is the one in force");

    const int peer = ::accept(listener, nullptr, nullptr);
    check(peer >= 0, "the peer accepted the connection — and now never reads a byte");

    // Push frames until the window jams. Each send is bounded, so this loop is bounded too:
    // worst case a handful of windows, not a hang.
    const std::vector<std::byte> frame(256 * 1024);
    const auto t0 = clock_t_::now();
    int sent = 0;
    for (; sent < 64 && link.stalled_tx() < tr::net::kMaxConsecutiveStalls * kRing; ++sent)
        link.send(std::span(frame));
    const std::int64_t took = ms_since(t0);

    check(link.stalled_tx() >= tr::net::kMaxConsecutiveStalls * kRing,
          "the stalled peer cost the link write turns, and they are COUNTED");
    check(took < (sent + 1) * static_cast<std::int64_t>(window),
          "and no single send blocked past its bound");
    for (int i = 0; i < 4; ++i) link.send(std::span(frame));
    check(link.dropped_tx() > 0,
          "a send to the condemned peer is shed and counted — never a silent drop");

    // The verdict reaches the connection: shutdown makes the recv loop see EOF, which runs
    // the ordinary remote-departure teardown.
    for (int i = 0; i < 400 && link.link_up(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!link.link_up(), "the peer that would not take bytes is dropped, not blocked on");
    std::printf("  stalled=%llu dropped=%llu after %lld ms\n",
                static_cast<unsigned long long>(link.stalled_tx()),
                static_cast<unsigned long long>(link.dropped_tx()), static_cast<long long>(took));

    ::close(peer);
    ::close(listener);
}

/**
 * @brief #1295 and #2144: N concurrent DIRECTED sends to N stalled peers cost the senders no
 *        wait at all, and no stalled peer delays another.
 *
 * #1295's defect: a directed send took the whole liveness window for itself, and directed
 * sends on one server serialized behind one write mutex, so N callers targeting N stalled
 * peers queued up N full windows. #1295 divided the bound by the admission cap so the batch
 * fit one window. #2144 removes the mutex and makes every server write nonblocking: a full
 * socket refuses the syscall at once, the frame waits in that peer's own ring, and the batch
 * costs syscalls, not windows.
 */
void test_concurrent_directed_sends_share_one_window() {
    std::printf("N concurrent directed sends to N stalled peers cost the senders no wait:\n");
    // Sized so the division is exact: window/peers is well above kBoundedWaitMs, and so the
    // per-record bound is a real quotient rather than the floor.
    const std::uint32_t window = 1600;
    const std::size_t peers = 4;
    tr::net::tcp_server_transport_t server(
        0, {.max_peers = peers, .peer_named = true, .liveness_window_ms = window});
    check(server.ok(), "the peer-named listener is up");
    check(server.max_peers() == peers, "the injected cap is the one enforced");
    check(server.directed_send_bound_ms() == window / peers,
          "and a DIRECTED send divides by that cap — not by the round's size of one");

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

    check(server.dropped_tx() >= peers,
          "every peer did stall — the batch ran into full sockets and overflowed the rings, "
          "it did not just fit in the socket buffers");
    // A sender that waited on a stalled socket would pay a directed bound per send:
    // frames_per_peer of them, about six windows. Copying the frames costs milliseconds
    // (most of a second under TSan), so two windows separates the two cleanly.
    check(took < 2 * static_cast<std::int64_t>(window),
          "and the whole batch took a fraction of one bound per send — no sender waited on "
          "a stalled peer's socket, its own or another's (#2144)");
    std::printf(
        "  peers=%zu window=%u ms directed bound=%u ms — the concurrent batch took "
        "%lld ms, dropped=%llu\n",
        peers, window, server.directed_send_bound_ms(), static_cast<long long>(took),
        static_cast<unsigned long long>(server.dropped_tx()));

    for (const int c : clients) ::close(c);
}

/**
 * @brief RFC 0028 slice 2 (#1619): a second publisher never waits on the first one's write.
 *
 * The defect this reddens on: `tcp_transport_t::send` held `write_m_` across the bounded
 * write, so while one publisher sat in a write to a peer that had stopped reading, a second
 * publisher to the same link queued on the mutex for the rest of that write — up to the whole
 * liveness window. Delivery is in-call, so that second publisher is whatever unrelated task
 * wrote a vertex. With enqueue-then-write the second publisher copies its record into the
 * link's queue and returns at once; the writer already on the wire drains it.
 *
 * The bound asserted is a small fraction of the window, so it is independent of the write
 * budget: before the fix the second send took about the rest of the window.
 */
void test_second_publisher_never_waits_on_a_stalled_write() {
    std::printf("a second publisher returns at once while the first is stuck on a stalled peer:\n");
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    check(listener >= 0, "listener socket");
    const int one = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
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

    // A LONG window, so "the rest of the first write" is unmistakable against the bound below.
    const std::uint32_t window = 3000;
    tr::net::tcp_transport_t link("127.0.0.1", ntohs(local.sin_port),
                                  {.liveness_window_ms = window});
    check(link.ok(), "the dial succeeded");
    const int peer = ::accept(listener, nullptr, nullptr);
    check(peer >= 0, "the peer accepted the connection — and now never reads a byte");

    // The first publisher: one record far larger than both socket buffers, so its write is
    // still in flight when the second publisher arrives.
    const std::vector<std::byte> big(8 * 1024 * 1024);
    std::atomic<bool> first_done{false};
    std::thread first([&] {
        link.send(std::span(big));
        first_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(!first_done.load(std::memory_order_acquire),
          "the first publisher is stuck in its write to the stalled peer");

    const std::vector<std::byte> small_frame(64);
    const auto t0 = clock_t_::now();
    link.send(std::span(small_frame));
    const std::int64_t took = ms_since(t0);
    const bool first_still_writing = !first_done.load(std::memory_order_acquire);
    check(took < 100,
          "the second publisher returned at once — it did not wait out the first one's write");
    check(first_still_writing, "and it returned while the first write was still in flight");
    std::printf("  second send took %lld ms (window %u ms)\n", static_cast<long long>(took),
                window);

    first.join();
    ::close(peer);
    ::close(listener);
}

/**
 * @brief A dialed `tcp_transport_t` whose accepted peer does not read until told to — the
 *        fixture for the two queue-delivery cases below.
 */
struct parked_peer_t {
    int listener = -1; /**< @brief The listening socket. */
    int peer = -1;     /**< @brief The accepted end, which reads only in @ref read_records. */
    std::unique_ptr<tr::net::tcp_transport_t> link; /**< @brief The link under test. */

    /** @brief Dial with a window of @p window ms and a small peer receive buffer. */
    explicit parked_peer_t(std::uint32_t window) {
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
        link = std::make_unique<tr::net::tcp_transport_t>(
            "127.0.0.1", ntohs(local.sin_port),
            tr::net::tcp_config_t{.liveness_window_ms = window});
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

/**
 * @brief #1619: every record a publisher queued behind a stalled write reaches the peer, in
 *        admission order, once the peer reads again.
 *
 * The case above proves a queued publisher RETURNS at once; this one proves its record is
 * not lost. A queue that discarded what it queued would pass the case above and fail here.
 */
void test_queued_records_are_all_delivered_in_order() {
    std::printf("every record queued behind a stalled write is delivered, in order:\n");
    parked_peer_t p(/*window=*/3000);
    check(p.link->ok() && p.peer >= 0, "the link is dialed and the peer accepted");
    const std::vector<std::byte> big(8 * 1024 * 1024, std::byte{0x11});
    std::atomic<bool> first_done{false};
    std::thread first([&] {
        p.link->send(std::span(big));
        first_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(!first_done.load(std::memory_order_acquire), "the first write is parked on the peer");
    const std::vector<std::byte> a(64, std::byte{0xA1});
    const std::vector<std::byte> b(64, std::byte{0xA2});
    p.link->send(std::span(a));
    p.link->send(std::span(b));
    check(!first_done.load(std::memory_order_acquire), "two records queued behind it");

    // The peer reads again, well inside the window: the writer finishes its own record and
    // then drains the two it queued.
    const std::vector<parked_peer_t::record_t> got = p.read_records(3);
    first.join();
    check(got.size() == 3, "all three records reached the peer");
    if (got.size() == 3) {
        check(got[0].len == big.size() && got[0].mark == 0x11, "the writer's own record first");
        check(got[1].len == 64 && got[1].mark == 0xA1, "then the first queued record");
        check(got[2].len == 64 && got[2].mark == 0xA2, "then the second, in admission order");
    }
    check(p.link->dropped_tx() == 0, "and nothing was dropped");
}

/**
 * @brief #1619 / #2144: past the ring's depth a record is never waited on. A delivery waits in
 *        its vertex's own ring, at the depth the vertex declared, and a full one takes the
 *        vertex's arm — best-effort sheds its OLDEST waiting record, reliable refuses the
 *        NEWEST — and the loss is counted; everything else still arrives, in order.
 */
void full_ring_case(bool reliable) {
    constexpr std::size_t depth = 8;
    parked_peer_t p(/*window=*/3000);
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
    std::atomic<bool> first_done{false};
    std::thread first([&] {
        p.link->send(std::span(big));
        first_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(!first_done.load(std::memory_order_acquire), "the first write is parked on the peer");
    const std::uint64_t before = p.link->dropped_tx();
    for (std::size_t i = 0; i < depth; ++i) deliver(static_cast<std::uint8_t>(0xB0 + i));
    check(p.link->dropped_tx() == before, "the vertex's ring took all of its depth");
    const auto t0 = clock_t_::now();
    deliver(0xEE);
    check(ms_since(t0) < 100, "the record past the depth returned at once");
    check(p.link->dropped_tx() == before + 1, "and one record was lost and counted");
    check(!first_done.load(std::memory_order_acquire), "while the first write was still parked");

    const std::vector<parked_peer_t::record_t> got = p.read_records(depth + 2);
    first.join();
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

/**
 * @brief #2144 (review): a dialer record the socket cut short finishes on writability alone.
 *
 * The send's turn runs its one-quantum bound out with part of an 8 MiB record on the wire and
 * returns. Nothing else is sent: the link's own receive thread waits for writability while the
 * frame is held and resumes it there, so the peer reads the whole record.
 */
void test_dialer_resumes_a_cut_record() {
    std::printf("a dialer record cut short finishes on writability, with no second send:\n");
    parked_peer_t p(/*window=*/tr::net::kBoundedWaitMs);
    check(p.link->ok() && p.peer >= 0, "the link is dialed and the peer accepted");
    const std::vector<std::byte> big(8 * 1024 * 1024, std::byte{0x33});
    const auto t0 = clock_t_::now();
    p.link->send(std::span(big));
    check(ms_since(t0) < 2000, "the send returned at its bound with the record cut short");
    check(p.link->dropped_tx() == 0, "the rest is held, not shed");
    const std::vector<parked_peer_t::record_t> got = p.read_records(1);
    check(got.size() == 1 && got[0].len == big.size() && got[0].mark == 0x33,
          "the peer read the whole record: the link's own wait resumed it");
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
    std::printf("== #838: host stream sends are bounded, counted and attributed ==\n");
    test_bounded_write_returns();
    test_streak_closes_the_peer();
    test_partial_record_finishes();
    test_completion_resets_the_streak();
    test_bound_derivation();
    test_admits_4096_peers();
    test_tcp_link_sheds_and_drops_a_stalled_peer();
    test_second_publisher_never_waits_on_a_stalled_write();
    test_queued_records_are_all_delivered_in_order();
    test_full_ring_takes_its_arm();
    test_dialer_resumes_a_cut_record();
    test_concurrent_directed_sends_share_one_window();
    // The shared runner's verdict, not a hard-coded "all checks passed": this suite used to
    // print that line and `return 0` whatever the counter said, so a red check here could
    // never redden ctest — and #1295's guard is only a guard if it can (#874's summary).
    return tr::testing::summary("send_stall_test");
}
