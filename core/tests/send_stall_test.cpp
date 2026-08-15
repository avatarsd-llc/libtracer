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
 *     policy strikes the stalling peer and closes it at the third strike (arm 4) or at once
 *     when the record half-reached the wire and desynced the stream (arm 5), and a
 *     completed record clears the strikes (arm 6). Deterministic — a socketpair with no
 *     reader stays full forever.
 *  2. **A real `tcp_transport_t` against a peer that never reads** (arm 7). The property
 *     that actually matters end to end, and the one a helper test cannot show: `send()`
 *     RETURNS, the frame is counted rather than silently lost, and the link is torn down.
 *     Before the fix this arm hangs the test process; that is the regression it exists to
 *     redden.
 *
 * A third level was added by **#1295**, which found the bound was a per-CALL property
 * rather than a node one: a directed send is a round of ONE, so it took the whole window
 * for itself, and N of them to N stalled peers serialized N windows behind the same write
 * mutex. The residual arms are the cap derivation that makes the denominator real
 * (`derive_max_peers` — `max_peers = 0` is no longer "uncapped") and the concurrent
 * directed batch that has to fit inside one window on a real peer-named server.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
    using tr::net::stream_endpoint_t::note_write_result;
    using tr::net::stream_endpoint_t::set_snd_timeout;
    using tr::net::stream_endpoint_t::stalled_tx_;
    using tr::net::stream_endpoint_t::write_all;
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
    check(!r.partial, "nothing of it reached the wire, so the stream is still in framing sync");
    check(took >= static_cast<std::int64_t>(bound),
          "it did not give up early: the peer got its whole window");
    check(took < static_cast<std::int64_t>(bound) * 20,
          "and it RETURNED — the pre-#838 path blocks here forever");
    std::printf("  bound=%u ms, returned after %lld ms\n", bound, static_cast<long long>(took));
}

/**
 * @brief Arm 4: three stalls in a row close the peer; one or two do not.
 */
void test_streak_closes_the_peer() {
    std::printf("the brokenness detector strikes the stalling peer, three and out:\n");
    const stalled_pair_t p;
    stall_probe_t probe;
    std::uint8_t streak = 0;
    const tr::net::write_result_t stall{tr::net::write_outcome_t::STALLED, false};

    check(probe.note_write_result(stall, p.a, streak), "a stalled record is a SHED frame");
    check(streak == 1 && !is_shut_down(p.a), "one stall is transient backpressure, not a verdict");
    (void)probe.note_write_result(stall, p.a, streak);
    check(streak == 2 && !is_shut_down(p.a), "two can straddle a burst — still open");
    (void)probe.note_write_result(stall, p.a, streak);
    check(is_shut_down(p.a), "the third consecutive stall closes the peer");
    check(streak == 0, "and the verdict is spent (the session is on its way out)");
    check(probe.stalled_tx_.load(std::memory_order_relaxed) == 3,
          "every stalled record is COUNTED — a timed-out peer is never a silent drop");
}

/**
 * @brief Arm 5: a half-written record condemns the session at once, bypassing the streak.
 */
void test_partial_record_condemns_at_once() {
    std::printf("a record that half-reached the wire condemns the session immediately:\n");
    const stalled_pair_t p;
    stall_probe_t probe;
    std::uint8_t streak = 0;
    const tr::net::write_result_t partial{tr::net::write_outcome_t::STALLED, true};

    check(probe.note_write_result(partial, p.a, streak), "still a shed frame");
    check(is_shut_down(p.a),
          "one is enough: the peer's framing is desynced permanently, a different fault class");
}

/**
 * @brief Arm 6: a completed record clears the strikes — the trichotomy's middle arm.
 */
void test_completion_resets_the_streak() {
    std::printf("a peer that takes the next frame is riding out a burst, not broken:\n");
    const stalled_pair_t p;
    stall_probe_t probe;
    std::uint8_t streak = 0;
    const tr::net::write_result_t stall{tr::net::write_outcome_t::STALLED, false};
    const tr::net::write_result_t done{tr::net::write_outcome_t::COMPLETE, false};

    (void)probe.note_write_result(stall, p.a, streak);
    (void)probe.note_write_result(stall, p.a, streak);
    check(streak == 2, "two strikes accrued");
    check(!probe.note_write_result(done, p.a, streak), "a completed record sheds nothing");
    check(streak == 0, "and clears them");
    (void)probe.note_write_result(stall, p.a, streak);
    check(!is_shut_down(p.a), "so the next stall starts a fresh streak instead of closing");
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
 * @brief #1295 — the admission cap is what makes the denominator real, so it is derived
 *        from the window rather than taken on trust.
 */
void test_max_peers_derivation() {
    std::printf("the concurrent-peer cap is resolved against the window, never 'unbounded':\n");
    using tr::net::derive_max_peers;
    using tr::net::derive_send_bound_ms;
    using tr::net::max_admissible_peers;

    check(max_admissible_peers(10000) == 100,
          "the ceiling is window / kBoundedWaitMs — past it every peer draws the FLOOR and "
          "the aggregate stops fitting the window");
    check(max_admissible_peers(0) == tr::net::kDefaultLivenessWindowMs / tr::net::kBoundedWaitMs,
          "an unconfigured window resolves its ceiling from the default clamp");
    check(max_admissible_peers(50) == 1, "a window under one floor still admits ONE peer");

    check(derive_max_peers(10000, 0) == 100,
          "0 is no longer UNBOUNDED (#1295): it takes the ceiling, which is a number");
    check(derive_max_peers(10000, 8) == 8, "a cap inside the ceiling is honoured verbatim");
    check(derive_max_peers(10000, 4096) == 100,
          "and one above it is CLAMPED — the extra slots could only be admitted by making "
          "the node's aggregate blocking claim untrue");

    // The property the whole ruling rests on, stated as arithmetic: whatever window and
    // requested cap a deployment injects, ONE per-record bound times the peers that can
    // hold one fits inside one window — so the peers a hostile party can open no longer
    // multiply the node's blocking. (A window narrower than kBoundedWaitMs is the one
    // irreducible case: the floor is a quantum, not a divisible budget, and the cap then
    // collapses to a single peer.)
    bool aggregate_fits = true;
    for (const std::uint32_t w : {0U, 50U, 100U, 400U, 1200U, 10000U, 600000U})
        for (const std::size_t req : {std::size_t{0}, std::size_t{1}, std::size_t{7},
                                      std::size_t{64}, std::size_t{100000}}) {
            const std::size_t cap = derive_max_peers(w, req);
            const std::uint32_t window = w != 0 ? w : tr::net::kDefaultLivenessWindowMs;
            const std::size_t budget =
                window > tr::net::kBoundedWaitMs ? window : tr::net::kBoundedWaitMs;
            if (cap < 1 || static_cast<std::size_t>(derive_send_bound_ms(w, cap)) * cap > budget)
                aggregate_fits = false;
        }
    check(aggregate_fits,
          "bound x cap <= one window, for every window and every requested cap — the node "
          "aggregate, not a per-call property a peer multiplies by reconnecting");
}

/**
 * @brief Arm 7: the end-to-end property, over a real `tcp_transport_t`.
 *
 * A peer that accepts the connection and then never reads a byte. Pre-#838 the first send
 * that fills the window never returns and this function hangs; the fix makes it return,
 * count the shed frame, and take the dead peer down.
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
    tr::net::tcp_transport_t link("127.0.0.1", ntohs(local.sin_port), &tr::mem::heap_backend(),
                                  /*max_frame=*/0, /*recv_stack=*/0, /*defer_recv=*/false, window);
    check(link.ok(), "the dial succeeded");
    check(link.liveness_window_ms() == window, "and the injected window is the one in force");

    const int peer = ::accept(listener, nullptr, nullptr);
    check(peer >= 0, "the peer accepted the connection — and now never reads a byte");

    // Push frames until the window jams. Each send is bounded, so this loop is bounded too:
    // worst case a handful of windows, not a hang.
    const std::vector<std::byte> frame(256 * 1024);
    const auto t0 = clock_t_::now();
    for (int i = 0; i < 64 && link.stalled_tx() == 0; ++i) link.send(std::span(frame));
    const std::int64_t took = ms_since(t0);

    check(link.stalled_tx() > 0, "the stalled peer cost the link frames, and they are COUNTED");
    check(link.dropped_tx() >= link.stalled_tx(),
          "each one also lands in the link's shed-frame counter — never a silent drop");
    check(took < 64 * static_cast<std::int64_t>(window),
          "and no single send blocked past its bound");

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
 * @brief #1295, the arm that matters: N concurrent DIRECTED sends to N stalled peers cost
 *        the node ONE window, not N of them.
 *
 * The defect this reddens on: a directed send is a round of one, so it used to pass `1` as
 * the divisor and take the WHOLE liveness window for itself. Directed sends on one server
 * serialize behind `write_m_`, so N callers targeting N stalled peers queued up N full
 * windows — bounded per call, unbounded per node, which a peer multiplies simply by
 * opening more connections. With the fix the divisor is the admission cap, so the batch
 * fits one window however many peers are stalled.
 *
 * Restore the `1` at `transport_tcp_server::peer_endpoint_t::send` and the elapsed time
 * below goes from about one window to about `peers` windows — past the assertion.
 */
void test_concurrent_directed_sends_share_one_window() {
    std::printf("N concurrent directed sends to N stalled peers cost ONE window, not N:\n");
    // Sized so the division is exact: window/peers is well above kBoundedWaitMs, and so the
    // per-record bound is a real quotient rather than the floor.
    const std::uint32_t window = 1600;
    const std::size_t peers = 4;
    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), /*max_frame=*/0, peers,
                                         /*peer_named=*/true, /*recv_stack=*/0, window);
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
    // ride the stall out. The jam is INSIDE the timed region on purpose — the record that
    // finally stalls half-reached the wire, and a partial record condemns the session at
    // once (#838's short-write rule), so a peer cannot be pre-jammed and still be there to
    // block on afterwards. Buffer fills are memcpy-speed; the only thing worth milliseconds
    // in here is the blocking, which is exactly what is being measured. Sends past the
    // condemnation fail immediately and cost nothing.
    const std::vector<std::byte> frame(512 * 1024);
    const int frames_per_peer = 24;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> senders;
    for (std::size_t i = 0; i < peers; ++i)
        senders.emplace_back([&server, &frame, &ready, &go, frames_per_peer, i] {
            const std::string name = "p" + std::to_string(i);
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

    check(server.stalled_tx() >= peers,
          "every peer did stall — the measurement below is of real blocking, not of a "
          "batch that happened to fit in the socket buffers");
    check(took < 2 * static_cast<std::int64_t>(window),
          "and the whole batch fits about ONE window — before #1295 each directed send took "
          "a full window of its own, so N of them took N windows, which is what this fails on");
    check(took >= static_cast<std::int64_t>(window / peers),
          "not passing by giving up early either: a stalled peer still got its share");
    std::printf(
        "  peers=%zu window=%u ms directed bound=%u ms — the concurrent batch took "
        "%lld ms, stalled=%llu\n",
        peers, window, server.directed_send_bound_ms(), static_cast<long long>(took),
        static_cast<unsigned long long>(server.stalled_tx()));

    for (const int c : clients) ::close(c);
}

}  // namespace

int main() {
    std::printf("== #838: host stream sends are bounded, counted and attributed ==\n");
    test_bounded_write_returns();
    test_streak_closes_the_peer();
    test_partial_record_condemns_at_once();
    test_completion_resets_the_streak();
    test_bound_derivation();
    test_max_peers_derivation();
    test_tcp_link_sheds_and_drops_a_stalled_peer();
    test_concurrent_directed_sends_share_one_window();
    // The shared runner's verdict, not a hard-coded "all checks passed": this suite used to
    // print that line and `return 0` whatever the counter said, so a red check here could
    // never redden ctest — and #1295's guard is only a guard if it can (#874's summary).
    return tr::testing::summary("send_stall_test");
}
