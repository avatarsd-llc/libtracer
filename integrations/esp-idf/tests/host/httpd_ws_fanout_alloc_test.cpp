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
 * which is exactly what once crashed this link on a reply-sized copy (`tx_frame_t`'s doc
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
 *   3. a fan-out WIDER than one chunk still allocates nothing but the one flush item the
 *      fake queues (#949, #2095). The frame is gathered once into one shared block that
 *      every peer's slot references, and the whole fan-out posts ONE flush to the control
 *      queue, so the cost of a broadcast does not grow with the peer count.
 *
 * Keeping the fake's own heap out of the measured window is what makes property 1 a clean
 * zero rather than a fragile delta: the control socket is set REFUSING, so every enqueue
 * inside the window fails at `enqueue_locked` before the fake's `std::deque` is ever
 * pushed. Every allocation the counter sees is therefore the link's, and the refusal hands
 * each peer's frame straight back.
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
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
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
tr::mem::poly_ptr_t<httpd_ws_link_t> make_link() {
    return tr::mem::make_poly<httpd_ws_link_t>(
        tr::mem::net_source(), handle(), "/ws",
        tr::net::httpd_ws_config_t{.peer_named = true, .send_timeout_ms = kSendBoundMs});
}

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief One broadcast — the entry point every subscription push takes. */
void broadcast(httpd_ws_link_t& link) { link.send(std::span<const std::byte>(kBody)); }

/** @brief Retire the link, the fake's sessions and its queue settings between cases. */
void reset(tr::mem::poly_ptr_t<httpd_ws_link_t>& link) {
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
 *        — not once for the destination snapshot, not once per frame — beyond the one
 *        shared block, and only where memory.io is the heap.
 *
 * The frame is gathered once into one block drawn from the link's memory source, and every
 * destination peer's TX slot takes a reference to it (#2095), so nothing in the window has
 * a reason to touch the global heap. A frame past the inline size would take the large class or its
 * documented nothrow fallback; the body here is far below it, so the count is the fan-out's own.
 *
 * The companion assertion is what stops the zero from being vacuous: the control socket
 * was offered exactly one frame per peer, so the broadcast measured here really did
 * enumerate and gather for all four.
 */
void test_a_broadcast_allocates_nothing() {
    std::printf("a broadcast to a full chunk of peers:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");

    const std::size_t peers = 4;
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

    // Refuse every enqueue from here on, so the fake's own std::deque is never pushed
    // inside the window — every allocation counted is the link's.
    fake_httpd::instance().set_queue_refusing(true);
    const std::uint32_t drops_before = link->enqueue_drops();

    const std::size_t allocs_before = g_allocs.load(std::memory_order_relaxed);
    broadcast(*link);
    const std::size_t allocs_after = g_allocs.load(std::memory_order_relaxed);

    // The broadcast draws its one shared block from memory.io. On a slab-pooled build that
    // is the pool, so the global heap sees nothing; on a heap-rooted build memory.io IS the
    // heap, and the count is that one block, whatever the peer count (#2095).
    const std::size_t want = tr::graph::config_t::kSlabPool ? 0 : 1;
    check_eq(allocs_after - allocs_before, want,
             "the broadcast touched the global heap only for its one shared block, if at all");
    check_eq(link->enqueue_drops() - drops_before, peers,
             "and it really fanned out: one frame offered to the control socket per peer");

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
 * chunk holds today (`kFanoutChunk` is a link-internal constant in httpd_ws_link.cpp —
 * this suite cannot read it). If that constant ever grows past six this case stops
 * straddling a boundary and becomes an ordinary fan-out check; it does not become wrong.
 *
 * It is measured in two stages. Stage A offers the fan-out to a REFUSING control socket,
 * so every destination the scan produces is a counted refusal: exactly six, which a
 * skipped peer would make five and a re-visited one seven. Stage B lets the frames through
 * and looks at each peer's socket. With a TX slot per peer (#2095) there is no link-wide
 * depth left to drop past, so EVERY open peer is written exactly one frame — the flush
 * walks the same chunked snapshot, and a skip or repeat at its seam shows up here too.
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

    // Stage A — enumeration.
    const std::uint32_t offers_before = link->enqueue_drops();
    fake_httpd::instance().set_queue_refusing(true);
    broadcast(*link);
    fake_httpd::instance().set_queue_refusing(false);
    check_eq(link->enqueue_drops() - offers_before, open_fds.size(),
             "the scan offered a frame to every OPEN peer exactly once across the chunk seam");

    // Stage B — delivery.
    std::vector<std::size_t> writes_before;
    for (const int fd : open_fds) writes_before.push_back(fake_httpd::instance().writes(fd));
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    const std::uint32_t drops_before = link->enqueue_drops();

    broadcast(*link);
    drain();

    check_eq(fake_httpd::instance().frames_sent() - sent_before, open_fds.size(),
             "the whole fan-out reached the wire");
    check_eq(link->enqueue_drops() - drops_before, 0,
             "and nothing was dropped: no link-wide depth to fall past (#2095)");
    // Two writes per frame, not one: `httpd_ws_send_frame_async` puts a frame on the socket
    // as a header write and a payload write (see fake_httpd's transcription of it).
    for (std::size_t i = 0; i < open_fds.size(); ++i) {
        const std::size_t w = fake_httpd::instance().writes(open_fds[i]) - writes_before[i];
        char what[112];
        std::snprintf(what, sizeof(what), "peer fd=%d was written exactly one frame (%zu writes)",
                      open_fds[i], w);
        check(w == 2, what);
    }

    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — a WIDE fan-out allocates nothing per peer (#949, #2095).
// ---------------------------------------------------------------------------
/**
 * @brief The cost of a broadcast does not grow with the peer count.
 *
 * Until #949 a send that found no free TX slot allocated a work item AND a payload buffer
 * on the global heap; after it, a fan-out past the link-wide pool dropped its tail. With a
 * slot per peer (#2095) neither happens: the frame is gathered once into one shared block,
 * every peer's slot references it, and the whole fan-out posts ONE flush to the control
 * queue.
 *
 * The control socket is left ACCEPTING, so the one flush is really queued and the fake's
 * own `std::deque` push is inside the window — at most one block. Anything per peer would
 * show as ten.
 */
void test_a_wide_fanout_allocates_nothing_per_peer() {
    std::printf("a fan-out to ten peers:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");

    const std::size_t wide = 10;
    std::vector<int> fds;
    for (std::size_t i = 0; i < wide; ++i) {
        fds.push_back(900 + static_cast<int>(i));
        claim(fds.back());
    }
    drain();

    // Warm-up: one full broadcast on the ordinary path, drained, so nothing first-use is
    // counted below.
    broadcast(*link);
    drain();

    const std::size_t allocs_before = g_allocs.load(std::memory_order_relaxed);
    const std::uint32_t drops_before = link->enqueue_drops();
    const std::size_t queued_before = fake_httpd::instance().queue_depth();
    broadcast(*link);
    const std::size_t allocs_after = g_allocs.load(std::memory_order_relaxed);

    check(allocs_after - allocs_before <= 1,
          "the wide fan-out allocated at most the fake's one queue block — nothing per peer");
    check_eq(fake_httpd::instance().queue_depth() - queued_before, 1,
             "and posted ONE flush for all ten peers");
    check_eq(link->enqueue_drops() - drops_before, 0, "nothing was dropped");

    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, wide,
             "the one flush put a frame on every peer's socket");
    reset(link);
}

// ---------------------------------------------------------------------------
// 4 — a broadcast rings ONE doorbell, however many peers (#2146).
// ---------------------------------------------------------------------------
/**
 * @brief A burst rings the httpd task once, not once per peer.
 *
 * The fake's control queue is set PREEMPTING: each post runs at once, as a higher-priority
 * httpd task does on silicon, where every post is a hand-off to that task and back. Rung
 * per frame, the flush each post ran cleared the link's "flush posted" flag before the next
 * peer's frame was stored, so a broadcast to 8 peers posted 8 flushes (about 24 task
 * switches on silicon). Rung once after every frame is stored, it posts one, and that one
 * flush writes all 8.
 */
void test_a_broadcast_rings_one_doorbell() {
    std::printf("a broadcast to eight peers, against a preempting httpd task:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");
    const std::size_t peers = 8;
    for (std::size_t i = 0; i < peers; ++i) claim(1000 + static_cast<int>(i));
    drain();

    fake_httpd::instance().set_preempting(true);
    const std::size_t posted_before = fake_httpd::instance().work_posted();
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    broadcast(*link);
    fake_httpd::instance().set_preempting(false);
    check_eq(fake_httpd::instance().work_posted() - posted_before, 1,
             "the broadcast posted ONE flush to the httpd task");
    check_eq(fake_httpd::instance().frames_sent() - sent_before, peers,
             "and that one flush wrote every peer's frame");
    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link fan-out allocation (#961)\n");
    test_a_broadcast_allocates_nothing();
    test_the_fanout_reaches_every_open_peer_exactly_once();
    test_a_wide_fanout_allocates_nothing_per_peer();
    test_a_broadcast_rings_one_doorbell();
    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
