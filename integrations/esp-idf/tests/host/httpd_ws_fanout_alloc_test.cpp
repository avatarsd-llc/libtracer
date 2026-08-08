/**
 * @file
 * @brief #961 — a BROADCAST must not allocate. The fan-out snapshot is an on-stack chunk,
 *        so a subscription push landing in a heap trough drops a frame rather than
 *        aborting the node inside libstdc++'s bad_alloc stub.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 teardown, #835 send-stall, #954 session-identity and #944
 * tx-strand suites: the REAL chip translation unit
 * (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against the host fake of
 * `esp_http_server` (fake_httpd.hpp). What is new here is the INSTRUMENT — this file
 * replaces the global `operator new`, so an allocation anywhere under `send()` is a
 * number the test can read rather than a code reading.
 *
 * The defect it pins: `httpd_ws_link_t::send` opened by building a `std::vector` of
 * destinations. That is the one container shape the rest of the TU is written to avoid —
 * its THROWING allocator turns a failed growth into `abort()` under `-fno-exceptions`,
 * which is exactly what once crashed this link on a reply-sized copy (`tx_work_t`'s doc
 * records it). On the fan-out path it was worse than a repeat: it sat AHEAD of every
 * `new (std::nothrow)` fallback the TX path has, so the drop-on-OOM contract the header
 * advertises was void before the first fallback could apply — and it was silent, because
 * no counter moves and no log is written when a bad_alloc stub aborts.
 *
 * Two properties, and they are not the same property:
 *   1. a broadcast to a full chunk of peers allocates NOTHING — the headline, and the
 *      assertion that reddens against the pre-fix code (one vector allocation);
 *   2. the fan-out still reaches every open peer exactly once when the peer set spans
 *      MORE than one chunk and departed slots sit inside the scanned range. Without this,
 *      property 1 is satisfied by a link that fans out to nobody, or by a resumable scan
 *      that skips or repeats a peer at the chunk boundary.
 *
 * Keeping the fake's own heap out of the measured window is what makes property 1 a clean
 * zero rather than a fragile delta: the control queue is CAPPED and its one entry spent,
 * so every enqueue inside the window is binned at the cap check (`enqueue_locked`) before
 * the fake's `std::deque` is ever pushed. Every allocation the counter sees is therefore
 * the link's. That is the same lever the #944 suite uses, for a different reason.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_httpd.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

/**
 * @brief Global allocations counted since process start — the instrument this suite reads.
 *
 * Atomic because the link's TX path is callable from any task; every case here is
 * single-threaded, so the count is exact rather than merely race-free.
 */
std::atomic<std::size_t> g_allocs{0};

/** @brief Replacement global allocator: counts, then defers to `malloc`. Replacing the
 *         scalar pair is enough — libstdc++'s array and nothrow forms both route through
 *         it, so `new (std::nothrow) T[n]` is counted too. */
void* operator new(std::size_t n) {
    void* const p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return p;
}

/** @brief The matching deallocation. */
void operator delete(void* p) noexcept { std::free(p); }

/** @brief The sized form, so a sized delete does not fall through to a different free. */
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

using tr::net::httpd_ws_link_t;

int g_failures = 0;

/** @brief Record one assertion's verdict. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Record a verdict that also reports the number that decided it. */
void check_eq(std::size_t got, std::size_t want, std::string_view what) {
    const bool ok = got == want;
    std::printf("  [%s] %.*s (got %zu, want %zu)\n", ok ? "PASS" : "FAIL",
                static_cast<int>(what.size()), what.data(), got, want);
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief A minimal frame body — the link only has to accept, gather and route it. */
const std::byte kBody[] = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};

/**
 * @brief The per-socket send bound these links are built with, milliseconds.
 *
 * Chosen only so case 1's cleanup runs in milliseconds: the strand window is
 * `tx_slot_capacity() * send_timeout_ms_`, and case 1 deliberately leaves the pool
 * stranded (its work items were binned), so the suite has to wait that window out before
 * the sweep will give the slots back. Nothing here stalls a socket, so the bound itself is
 * never spent.
 */
constexpr std::uint32_t kSendBoundMs = 20;

/** @brief Sleep comfortably past the strand window. */
void wait_out_the_strand_window() {
    const auto window = std::chrono::milliseconds(
        static_cast<long long>(httpd_ws_link_t::tx_slot_capacity()) * kSendBoundMs);
    std::this_thread::sleep_for(window + window / 2);
}

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief A link that adopts the fake server with the suite's tight send bound. */
std::unique_ptr<httpd_ws_link_t> make_link() {
    return std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true, kSendBoundMs);
}

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief One broadcast — the entry point every subscription push takes. */
void broadcast(httpd_ws_link_t& link) { link.send(std::span<const std::byte>(kBody)); }

/** @brief Retire the link, the fake's sessions and its queue settings between cases. */
void reset(std::unique_ptr<httpd_ws_link_t>& link) {
    link.reset();
    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().close_all();
    drain();
}

// ---------------------------------------------------------------------------
// 1 — a broadcast allocates nothing.
// ---------------------------------------------------------------------------
/**
 * @brief The headline. A fan-out to a full chunk of peers must not touch the global heap
 *        at all — not once for the destination snapshot, not once per frame.
 *
 * The peer count is `tx_slot_capacity()` so that every gathered frame lands in a POOL
 * slot's inline buffer: a frame past the pool would take its documented nothrow heap
 * fallback and confound the count with an allocation that is not the defect. That is the
 * point of the measurement — the fallbacks are allowed to allocate, the fan-out is not.
 *
 * The two companion assertions are what stop the zero from being vacuous: the pool is
 * fully claimed and the control socket was offered exactly one frame per peer, so the
 * broadcast measured here really did enumerate and gather for all four.
 */
void test_a_broadcast_allocates_nothing() {
    std::printf("a broadcast to a full chunk of peers:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");

    const std::size_t peers = httpd_ws_link_t::tx_slot_capacity();
    std::vector<int> fds;
    for (std::size_t i = 0; i < peers; ++i) {
        fds.push_back(600 + static_cast<int>(i));
        claim(fds.back());
    }
    drain();

    // Warm up on the ordinary accepting path, then drain: whatever is first-use on this
    // path (the fake's deque node, any lazy init in the log or the socket bound) is spent
    // OUTSIDE the window, so the window measures the steady state and not a cold start.
    broadcast(*link);
    drain();
    check_eq(link->tx_slots_busy(), 0, "the warm-up drained: the pool is idle again");

    // Cap the control queue at one entry and spend that entry on a filler the drain never
    // sees. From here every enqueue is BINNED at the cap check, so the fake's own
    // std::deque is never pushed inside the window — every allocation counted is the
    // link's. (The same lever the #944 suite uses, for a different purpose.)
    fake_httpd::instance().set_queue_capacity(1);
    fake_httpd::instance().post([] {});
    const std::size_t drops_before = fake_httpd::instance().queue_drops();

    const std::size_t allocs_before = g_allocs.load(std::memory_order_relaxed);
    broadcast(*link);
    const std::size_t allocs_after = g_allocs.load(std::memory_order_relaxed);

    check_eq(allocs_after - allocs_before, 0, "the broadcast touched the global heap ZERO times");
    check_eq(fake_httpd::instance().queue_drops() - drops_before, peers,
             "and it really fanned out: one frame offered to the control socket per peer");
    check_eq(link->tx_slots_busy(), peers, "each of those frames was gathered into a pool slot");

    // Cleanup: those work items were binned, so the slots are stranded until the sweep
    // reclaims them. Wait the window out and let an ordinary broadcast trigger it, exactly
    // as the #944 suite does — otherwise the destructor would sit on a pool that cannot
    // drain.
    wait_out_the_strand_window();
    fake_httpd::instance().set_queue_capacity(0);
    broadcast(*link);
    drain();
    check_eq(link->tx_slots_busy(), 0, "the stranded pool was reclaimed and drained");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — the fan-out still reaches every open peer, exactly once, across chunks.
// ---------------------------------------------------------------------------
/**
 * @brief The guard without which case 1 is satisfied by a link that fans out to nobody.
 *
 * The snapshot is a fixed chunk and the scan RESUMES after releasing `peers_m_`, so the
 * chunk boundary is a new seam: a peer could be skipped, or visited twice, exactly where
 * one chunk ends and the next begins. This stages the shape that would expose it — more
 * open peers than one chunk holds, with DEPARTED slots sitting inside the scanned range
 * (a slot is recycled in place, never removed, so a departure leaves a hole the scan must
 * step over without losing its place).
 *
 * Eight peers are admitted and two of them leave, which is more open peers than the
 * chunk holds today (`kFanoutChunk` is a link-internal constant, `kDefaultPeerCap` = 4 in
 * httpd_ws_link.cpp — this suite cannot read it). If that constant ever grows past six
 * this case stops straddling a boundary and becomes an ordinary fan-out check; it does
 * not become wrong.
 */
void test_the_fanout_reaches_every_open_peer_exactly_once() {
    std::printf("a fan-out whose open peers span more than one chunk:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");

    std::vector<int> fds;
    for (int i = 0; i < 8; ++i) {
        fds.push_back(700 + i);
        claim(fds.back());
    }
    drain();

    // Two peers depart from INSIDE the range — one in the first chunk, one at the seam.
    const int departed[] = {fds[1], fds[4]};
    for (const int fd : departed) fake_httpd::instance().close_session(fd);
    drain();

    std::vector<int> open_fds;
    for (const int fd : fds)
        if (fd != departed[0] && fd != departed[1]) open_fds.push_back(fd);
    check_eq(open_fds.size(), 6, "six peers remain open across the chunk boundary");

    std::vector<std::size_t> writes_before;
    for (const int fd : open_fds) writes_before.push_back(fake_httpd::instance().writes(fd));
    const std::size_t sent_before = fake_httpd::instance().frames_sent();

    broadcast(*link);
    drain();

    check_eq(fake_httpd::instance().frames_sent() - sent_before, open_fds.size(),
             "the fan-out delivered exactly one frame per OPEN peer, and none besides");
    for (std::size_t i = 0; i < open_fds.size(); ++i) {
        char what[96];
        std::snprintf(what, sizeof(what), "peer fd=%d was written exactly once", open_fds[i]);
        check_eq(fake_httpd::instance().writes(open_fds[i]) - writes_before[i], 1, what);
    }

    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link fan-out allocation (#961)\n");
    test_a_broadcast_allocates_nothing();
    test_the_fanout_reaches_every_open_peer_exactly_once();
    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
