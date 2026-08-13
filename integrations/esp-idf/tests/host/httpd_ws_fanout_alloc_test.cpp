/**
 * @file
 * @brief #961 — a BROADCAST must not allocate. The fan-out snapshot is an on-stack chunk,
 *        so a subscription push landing in a heap trough drops a frame rather than
 *        aborting the node inside libstdc++'s bad_alloc stub.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 teardown, #835 send-stall, #954 session-identity and #949
 * tx-pool suites: the REAL chip translation unit
 * (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against the host fake of
 * `esp_http_server` (fake_httpd.hpp). What is new here is the INSTRUMENT — this file
 * replaces the global `operator new`/`delete` family, so an allocation anywhere under
 * `send()` is a number the test can read rather than a code reading. The family is
 * replaced WHOLE, and under a sanitizer that is not a nicety; see the block above
 * `operator new` below for the abort a subset replacement produces.
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
 * Three properties, and they are not the same property:
 *   1. a broadcast to a full chunk of peers allocates NOTHING — the headline, and the
 *      assertion that reddens against the pre-fix code (one vector allocation);
 *   2. the fan-out still reaches every open peer exactly once when the peer set spans
 *      MORE than one chunk and departed slots sit inside the scanned range. Without this,
 *      property 1 is satisfied by a link that fans out to nobody, or by a resumable scan
 *      that skips or repeats a peer at the chunk boundary;
 *   3. a fan-out to MORE peers than the TX pool has slots still allocates nothing (#949).
 *      Until that issue the over-offer took a heap work item plus a heap payload copy per
 *      peer, so the one path most likely to meet a heap trough was the one that answered a
 *      full pool by allocating; the answer is now a counted drop, and this is the
 *      instrument that can tell the two apart.
 *
 * Keeping the fake's own heap out of the measured window is what makes property 1 a clean
 * zero rather than a fragile delta: the control socket is set REFUSING, so every enqueue
 * inside the window fails at `enqueue_locked` before the fake's `std::deque` is ever
 * pushed. Every allocation the counter sees is therefore the link's, and the refusal hands
 * each slot straight back, so the pool is idle again the moment the window closes.
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

namespace {

/** @brief `malloc`, but counted — the one routine every replaced `new` form funnels into. */
void* counted_alloc(std::size_t size, std::size_t align) {
    const std::size_t n = size != 0 ? size : 1;
    // aligned_alloc requires a size that is a whole multiple of the alignment.
    void* const p = align <= alignof(std::max_align_t)
                        ? std::malloc(n)
                        : std::aligned_alloc(align, ((n + align - 1) / align) * align);
    if (p != nullptr) g_allocs.fetch_add(1, std::memory_order_relaxed);
    return p;
}

}  // namespace

/**
 * @brief The counting allocator, replaced as the WHOLE family — every `new` and `delete`
 *        form the standard defines, never a subset.
 *
 * A partial replacement is not merely incomplete here, it is *wrong under a sanitizer*.
 * ASan serves every form a TU leaves undefined from its own runtime and tags the block
 * `operator new`; a `new (std::nothrow) T` allocated there and released through a replaced
 * `delete` (which calls `free`) aborts the process with
 * `alloc-dealloc-mismatch (operator new vs free)`. That is not hypothetical for this
 * suite: the link's `detach_sessions()` takes exactly that pair on the teardown of case 1
 * (`httpd_ws_link.cpp` — `new (std::nothrow) detach_req_t`, then `delete raw`), so a
 * subset replacement kills the run on the sanitizer legs before case 2 is reached.
 * Defining all of them keeps every pair `malloc`/`free`, which is consistent under ASan
 * and counted here. Same family, for the same reason, as
 * `core/tests/transport_alloc_softfail_test.cpp`.
 */
void* operator new(std::size_t size) {
    void* const p = counted_alloc(size, alignof(std::max_align_t));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    void* const p = counted_alloc(size, alignof(std::max_align_t));
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
    void* const p = counted_alloc(size, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size, std::align_val_t a) {
    void* const p = counted_alloc(size, static_cast<std::size_t>(a));
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
 * Nothing here stalls a socket, so the bound is never spent; it is passed only so the
 * links do not derive one, which would make the suite's timing depend on the watchdog
 * period.
 */
constexpr std::uint32_t kSendBoundMs = 20;

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
    fake_httpd::instance().set_queue_refusing(false);
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

    // Refuse every enqueue from here on, so the fake's own std::deque is never pushed
    // inside the window — every allocation counted is the link's.
    fake_httpd::instance().set_queue_refusing(true);
    const std::uint32_t drops_before = link->enqueue_drops();

    const std::size_t allocs_before = g_allocs.load(std::memory_order_relaxed);
    broadcast(*link);
    const std::size_t allocs_after = g_allocs.load(std::memory_order_relaxed);

    check_eq(allocs_after - allocs_before, 0, "the broadcast touched the global heap ZERO times");
    check_eq(link->enqueue_drops() - drops_before, peers,
             "and it really fanned out: one frame offered to the control socket per peer");
    check_eq(link->tx_slots_busy(), 0,
             "each of those frames was gathered into a pool slot, and the refusal gave it back");

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
 *
 * It is measured in TWO stages, and #949 is why. With the heap work-item fallback deleted,
 * the TX pool is the link's outstanding-send bound, and a fan-out wider than the pool
 * cannot put every frame on a socket in one pass: the peers past the claimable depth are
 * dropped and counted. Delivery alone can therefore no longer witness the whole scan.
 *
 *   - Stage A offers the fan-out to a REFUSING control socket. No slot is held past the
 *     refused enqueue, so the pool never exhausts and every destination the scan produces
 *     is counted: exactly six, which a skipped peer would make five and a re-visited one
 *     seven. That is the chunk-boundary guard.
 *   - Stage B lets the frames through and looks at each peer's socket: no peer may be
 *     written twice, and exactly the claimable depth must be written once. A repeat at the
 *     seam shows up here as a peer with two frames even when the totals still add up. That
 *     depth, for the in-call sender this suite is, is `tx_slot_capacity()` plus the in-call
 *     reserve (#1218 — the reserve is a slot past the pool, and an in-call send may take
 *     it).
 *
 * What stage B alone cannot separate is a scan that skips the last peer from the pool
 * bound dropping it, which is precisely why stage A — where the bound is not in play —
 * owns the enumeration.
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

    // Stage A — enumeration, with the pool bound taken out of play.
    const std::uint32_t offers_before = link->enqueue_drops();
    fake_httpd::instance().set_queue_refusing(true);
    broadcast(*link);
    fake_httpd::instance().set_queue_refusing(false);
    check_eq(link->enqueue_drops() - offers_before, open_fds.size(),
             "the scan offered a frame to every OPEN peer exactly once across the chunk seam");
    check_eq(link->tx_slots_busy(), 0, "and held no slot afterwards");

    // Stage B — delivery, where the pool bound applies.
    std::vector<std::size_t> writes_before;
    for (const int fd : open_fds) writes_before.push_back(fake_httpd::instance().writes(fd));
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    const std::uint32_t drops_before = link->enqueue_drops();

    broadcast(*link);
    drain();

    const std::size_t depth =
        httpd_ws_link_t::tx_slot_capacity() + httpd_ws_link_t::tx_reply_reserve();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, depth,
             "a pool's worth of the fan-out reached the wire");
    check_eq(link->enqueue_drops() - drops_before, open_fds.size() - depth,
             "and the peers past the pool were DROPPED and counted, not heap-queued (#949)");
    // Two writes per frame, not one: `httpd_ws_send_frame_async` puts a frame on the socket
    // as a header write and a payload write (see fake_httpd's transcription of it).
    std::size_t served = 0;
    for (std::size_t i = 0; i < open_fds.size(); ++i) {
        const std::size_t w = fake_httpd::instance().writes(open_fds[i]) - writes_before[i];
        char what[112];
        std::snprintf(what, sizeof(what), "peer fd=%d was written at most one frame (%zu writes)",
                      open_fds[i], w);
        check(w == 0 || w == 2, what);
        if (w == 2) ++served;
    }
    check_eq(served, depth, "exactly a pool's worth of DISTINCT peers were served");

    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — a fan-out WIDER than the TX pool still allocates nothing (#949).
// ---------------------------------------------------------------------------
/**
 * @brief The deleted heap fallback, pinned by its absence, on the path most likely to meet
 *        a heap trough.
 *
 * Until #949 a send that found no free TX slot allocated a work item AND a payload buffer
 * on the global heap and posted them anyway. A broadcast is where that bit hardest: the
 * over-offer is one heap pair per peer past the claimable depth, arriving in a burst, with
 * the outstanding-send count bounded by the heap rather than by the control queue behind
 * it. The answer is now a counted drop, and the two numbers below are what separate them —
 * a link that still fell back would allocate here and count nothing.
 *
 * The control socket is left ACCEPTING for this case, unlike case 1: the pooled part of the
 * fan-out must really be queued, so the over-offer is a pool miss and not a refusal. The
 * fake's own `std::deque` push is therefore inside the window and is subtracted by
 * measuring a first, pool-sized broadcast and requiring the WIDE one to add nothing beyond
 * what that one cost.
 */
void test_a_fanout_past_the_pool_allocates_nothing() {
    std::printf("a fan-out to more peers than the TX pool has slots:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");

    // The claimable depth for a send on the httpd task — the pool plus the in-call reserve
    // (#1218). Everything past it is the over-offer this case is about.
    const std::size_t depth =
        httpd_ws_link_t::tx_slot_capacity() + httpd_ws_link_t::tx_reply_reserve();
    const std::size_t wide = 2 * depth;
    std::vector<int> fds;
    for (std::size_t i = 0; i < wide; ++i) {
        fds.push_back(900 + static_cast<int>(i));
        claim(fds.back());
    }
    drain();

    // Warm-up: one full broadcast on the ordinary path, drained, so nothing first-use is
    // counted below. It also measures what ONE queued frame costs the fake's deque.
    broadcast(*link);
    drain();
    check_eq(link->tx_slots_busy(), 0, "the warm-up drained: the pool is idle again");

    const std::size_t allocs_before = g_allocs.load(std::memory_order_relaxed);
    const std::uint32_t drops_before = link->enqueue_drops();
    broadcast(*link);
    const std::size_t allocs_after = g_allocs.load(std::memory_order_relaxed);

    // `depth` frames were really queued, and the fake pushes each onto a std::deque, so
    // the window is not expected to be zero — it is expected to hold NO allocation for the
    // peers past the pool. The deque grows in blocks, so the bound is the pooled frames'
    // own cost: one allocation each, at most.
    check(allocs_after - allocs_before <= depth,
          "the wide fan-out allocated at most one block per QUEUED frame — nothing for the "
          "peers past the pool");
    check_eq(link->enqueue_drops() - drops_before, wide - depth,
             "and those peers were counted as drops (so the zero above is not vacuous)");
    check_eq(link->tx_slots_busy(), depth, "the pool is exactly full, not overdrawn");

    drain();
    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link fan-out allocation (#961)\n");
    test_a_broadcast_allocates_nothing();
    test_the_fanout_reaches_every_open_peer_exactly_once();
    test_a_fanout_past_the_pool_allocates_nothing();
    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
