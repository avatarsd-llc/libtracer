/**
 * @file
 * @brief #1566 — the BOUNDED LARGE TX size class: a declared class takes the routine
 *        multi-KB frame off the per-frame heap arm, and exhausting it is a counted drop
 *        rather than an unbounded fallback.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #949 tx-pool and #961 fan-out suites: the REAL chip translation
 * unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against the host fake
 * of `esp_http_server` (fake_httpd.hpp). The INSTRUMENT is the fan-out suite's — this file
 * replaces the whole global `operator new`/`delete` family — because the claim under test
 * is an allocation count and not a code reading, and because the family must be replaced
 * WHOLE (a subset makes ASan abort on a mismatched `new (std::nothrow)`/`free` pair; the
 * link's `detach_sessions()` takes exactly that pair on every teardown here).
 *
 * The condition the class exists for: a steady stream of moderately large frames — a
 * periodic state snapshot fanned to several subscribers at tens of Hz — ran entirely on the
 * `new (std::nothrow) std::byte[total]` arm, so N subscribers × rate multi-KB allocations
 * per second hit an embedded heap for a working set that was small and perfectly
 * predictable. What that costs is fragmentation and an unbounded transient footprint, not
 * correctness, which is why it is measured here as ALLOCATIONS rather than as failures.
 *
 * Six properties, and they are six different properties:
 *   1. with NO class declared nothing moved — a frame past the inline capacity still takes
 *      the heap arm. This is the baseline that keeps every zero below from being vacuous,
 *      and the pin on "no behaviour change for a link that did not ask";
 *   2. with a class declared, a frame in its band allocates NOTHING;
 *   3. a frame that FITS `tx_inline_bytes` allocates nothing either way — the band the
 *      issue explicitly took off the table is byte-identical to what it was;
 *   4. a frame past the class's own slot size still takes the heap arm, which is the
 *      demotion the ruling asked for rather than a removal: one composed-root snapshot is
 *      a heap block worth taking, a stream of them is not;
 *   5. an exhausted class DROPS and counts — `tx_large_dropped` plus the link-level
 *      `enqueue_drops` — and never falls back to the heap, because a fallback would hand
 *      back exactly the footprint the declaration was made to bound;
 *   6. a declaration that cannot be honoured is INERT and says so: `tx_large_bytes()`
 *      reports the class the link is really running, so an operator sizing against it is
 *      never quoted a number that was silently rejected (#1160).
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
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
 *        form the standard defines, never a subset (see the file header for the ASan
 *        `alloc-dealloc-mismatch` a subset produces).
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

/** @brief The per-socket send bound these links are built with, milliseconds — nothing here
 *         stalls a socket, so it is passed only to keep the suite off the derivation. */
constexpr std::uint32_t kSendBoundMs = 20;

/** @brief The declared class in this suite: two buffers of this many bytes each. */
constexpr std::size_t kLargeBytes = 8192;
constexpr std::size_t kLargeSlots = 2; /**< @brief …and this many of them. */

/** @brief The one socket every case here serves. */
constexpr int kFd = 700;

/** @brief A payload INSIDE the class's band: past the inline capacity, under the class. */
std::vector<std::byte> band_frame() {
    return std::vector<std::byte>(httpd_ws_link_t::kDefaultTxInlineBytes + 512, std::byte{0x5A});
}

/** @brief A payload that fits a work slot's inline buffer — the untouched band. */
std::vector<std::byte> inline_frame() {
    return std::vector<std::byte>(httpd_ws_link_t::kDefaultTxInlineBytes - 8, std::byte{0x3C});
}

/** @brief A payload past the declared class — the exceptional tail the heap arm keeps. */
std::vector<std::byte> tail_frame() {
    return std::vector<std::byte>(kLargeBytes + 512, std::byte{0x77});
}

/** @brief A minimal frame body — what claiming a peer costs. */
const std::byte kBody[] = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief A link that adopts the fake server, declaring the given large size class (0/0 =
 *         no class at all, which is every link that shipped before #1566). */
std::unique_ptr<httpd_ws_link_t> make_link(std::size_t large_bytes, std::size_t large_slots) {
    return std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true, kSendBoundMs, 0, 0, 0, 0,
                                             large_bytes, large_slots);
}

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief The directed endpoint of the single peer currently open. */
tr::net::transport_t* only_peer(httpd_ws_link_t& link) {
    std::string name;
    link.enumerate_peers([&name](std::string_view p) { name = std::string(p); });
    return name.empty() ? nullptr : link.peer_link(name);
}

/** @brief Retire the link, the fake's sessions and its queue settings between cases. */
void reset(std::unique_ptr<httpd_ws_link_t>& link) {
    link.reset();
    fake_httpd::instance().set_queue_refusing(false);
    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().close_all();
    drain();
}

/**
 * @brief Send @p frame once with the control queue REFUSING, and report the allocations it
 *        cost.
 *
 * Refusing is what keeps the fake's own `std::deque` out of the measured window: the
 * enqueue fails before anything is pushed, so every allocation counted is the link's. The
 * refusal also hands the work slot — and any large buffer bound to it — straight back, so
 * the window can be repeated without draining.
 */
std::size_t allocs_for_one_send(tr::net::transport_t* peer, std::span<const std::byte> frame) {
    fake_httpd::instance().set_queue_refusing(true);
    // Warm up ONCE on this exact size, drained of nothing (the enqueue is refused anyway),
    // so anything first-use — a lazy init in the log path — is spent outside the window.
    peer->send(frame);
    const std::size_t before = g_allocs.load(std::memory_order_relaxed);
    peer->send(frame);
    const std::size_t after = g_allocs.load(std::memory_order_relaxed);
    fake_httpd::instance().set_queue_refusing(false);
    return after - before;
}

// ---------------------------------------------------------------------------
// 1 — with no class declared, the heap arm is exactly where it was.
// ---------------------------------------------------------------------------
/**
 * @brief The baseline, and the "nothing moved for a link that did not ask" pin.
 *
 * A link constructed the way every link in the tree is constructed today declares no large
 * class, so a frame past `tx_inline_bytes` must still take `new (std::nothrow)` — one
 * allocation, per frame, per subscriber. Without this case the zeros in case 2 could be
 * produced by a link that had stopped sending.
 */
void test_no_class_declared_keeps_the_heap_arm() {
    std::printf("a multi-KB frame on a link that declared no large class:\n");
    auto link = make_link(0, 0);
    check(link->ok(), "the adopting link registered its URI");
    check_eq(link->tx_large_bytes(), 0, "no class: the effective slot size is zero");
    check_eq(link->tx_large_slot_capacity(), 0, "no class: the effective slot count is zero");
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();

    const std::vector<std::byte> frame = band_frame();
    check_eq(allocs_for_one_send(peer, frame), 1,
             "the frame took the per-frame nothrow heap payload, exactly as it always has");
    check_eq(link->stats().tx_large_dropped, 0,
             "and nothing was charged to a class that does not exist");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — a declared class takes the band off the heap entirely.
// ---------------------------------------------------------------------------
/**
 * @brief The headline. The same frame, on a link whose integrator declared a class that
 *        covers it, must not touch the global heap at all.
 *
 * The companion assertions are what stop the zero from being vacuous: the class really was
 * occupied while the frame was in flight, the frame really did reach the socket once
 * drained, and the buffer really did come back — a class that leaked a buffer per send
 * would pass the allocation count and fail here on the second pass.
 */
void test_a_declared_class_costs_no_allocation() {
    std::printf("the same frame on a link that declared an 8 KiB x 2 class:\n");
    auto link = make_link(kLargeBytes, kLargeSlots);
    check_eq(link->tx_large_bytes(), kLargeBytes, "the declared slot size is the effective one");
    check_eq(link->tx_large_slot_capacity(), kLargeSlots, "and so is the declared slot count");
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();

    const std::vector<std::byte> frame = band_frame();
    check_eq(allocs_for_one_send(peer, frame), 0,
             "the frame in the class's band touched the global heap ZERO times");
    check_eq(link->tx_large_in_use(), 0, "and the refusal handed its buffer straight back");

    // Now let one through for real: the bytes must reach the socket, and the buffer must be
    // held for exactly as long as the send is outstanding.
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    peer->send(std::span<const std::byte>(frame));
    check_eq(link->tx_large_in_use(), 1, "a queued large frame holds one buffer of the class");
    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, 1, "and it reached the socket");
    check_eq(link->tx_large_in_use(), 0, "the buffer came back when the send drained");
    check_eq(link->stats().tx_large_peak, 1, "the class's high-water mark is one");
    check_eq(link->stats().tx_large_dropped, 0, "and nothing was dropped");

    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — the inline band is byte-identical.
// ---------------------------------------------------------------------------
/**
 * @brief The band the ruling explicitly took off the table: a frame that FITS
 *        `tx_inline_bytes` gathers into its work slot's inline buffer, class or no class.
 *
 * Measured on both link shapes with the same frame, so a regression that routed a fitting
 * frame at the new class (or at anything else) shows up as a non-zero on the right-hand
 * side of a comparison that is otherwise trivially true.
 */
void test_the_inline_band_did_not_move() {
    std::printf("a frame that fits the inline capacity, with and without a class:\n");
    const std::vector<std::byte> frame = inline_frame();

    auto plain = make_link(0, 0);
    claim(kFd);
    tr::net::transport_t* peer = only_peer(*plain);
    check(peer != nullptr, "the peer resolved on the classless link");
    if (peer == nullptr) return;
    drain();
    check_eq(allocs_for_one_send(peer, frame), 0, "no class: the fitting frame allocates nothing");
    reset(plain);

    auto classed = make_link(kLargeBytes, kLargeSlots);
    claim(kFd);
    peer = only_peer(*classed);
    check(peer != nullptr, "the peer resolved on the classed link");
    if (peer == nullptr) return;
    drain();
    check_eq(allocs_for_one_send(peer, frame), 0,
             "with a class: the fitting frame still allocates nothing");
    check_eq(classed->tx_large_in_use(), 0, "and it never reached for the class at all");
    check_eq(classed->stats().tx_large_peak, 0, "which the class's untouched peak confirms");
    reset(classed);
}

// ---------------------------------------------------------------------------
// 4 — past the class, the heap arm survives as the exceptional tail.
// ---------------------------------------------------------------------------
/**
 * @brief The demotion, pinned as a demotion and not a removal.
 *
 * A frame larger than the declared slot size cannot be served from the class without
 * making the class as large as the largest frame the node will ever send, which is the
 * bound nobody can declare. It keeps the nothrow heap payload — one allocation, and the
 * frame is still delivered rather than dropped.
 */
void test_past_the_class_the_heap_tail_survives() {
    std::printf("a frame larger than the declared class:\n");
    auto link = make_link(kLargeBytes, kLargeSlots);
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();

    const std::vector<std::byte> frame = tail_frame();
    check_eq(allocs_for_one_send(peer, frame), 1, "the tail frame took the nothrow heap payload");
    check_eq(link->tx_large_in_use(), 0, "and did not consume a buffer of the class");
    check_eq(link->stats().tx_large_peak, 0, "which the class's untouched peak confirms");

    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    peer->send(std::span<const std::byte>(frame));
    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, 1,
             "and it was DELIVERED — the tail is demoted, not removed");

    reset(link);
}

// ---------------------------------------------------------------------------
// 5 — an exhausted class drops and counts, and never falls back to the heap.
// ---------------------------------------------------------------------------
/**
 * @brief The fail-closed arm (#949's named drop, applied to the new class).
 *
 * The class is declared with ONE buffer and two large frames are offered without a drain
 * between them, so the second finds it empty. Three measurements, and all three are needed:
 * the class's own counter says which resource refused, the link-level `enqueue_drops` says
 * the frame was lost (so the class does not hide a loss from the generic
 * `transport_drop_stats_t` view), and the allocation count says the drop was not quietly
 * served from the heap — which would give back the exact footprint the declaration bounds.
 */
void test_an_exhausted_class_drops_and_counts() {
    std::printf("two large frames offered to a one-buffer class:\n");
    auto link = make_link(kLargeBytes, 1);
    check_eq(link->tx_large_slot_capacity(), 1, "the class holds exactly one buffer");
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();

    const std::vector<std::byte> frame = band_frame();
    // Warm up and drain, so the fake's first deque node is not counted in the window.
    peer->send(std::span<const std::byte>(frame));
    drain();

    const std::uint32_t drops_before = link->enqueue_drops();
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    peer->send(std::span<const std::byte>(frame));  // takes the only buffer, stays queued
    check_eq(link->tx_large_in_use(), 1, "the first frame holds the class's only buffer");

    const std::size_t allocs_before = g_allocs.load(std::memory_order_relaxed);
    peer->send(std::span<const std::byte>(frame));  // finds it empty
    const std::size_t allocs_after = g_allocs.load(std::memory_order_relaxed);

    check_eq(link->stats().tx_large_dropped, 1, "the exhausted class counted the loss as its own");
    check_eq(link->enqueue_drops() - drops_before, 1, "and the link counted it as a lost frame");
    check_eq(allocs_after - allocs_before, 0, "the drop did NOT fall back to the heap");
    check_eq(link->tx_slots_busy(), 1, "the dropped frame's work slot came straight back");

    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, 1,
             "only the frame that got a buffer reached the socket — the other was never offered");
    check_eq(link->tx_large_in_use(), 0, "the drain returned the buffer");
    check_eq(allocs_for_one_send(peer, frame), 0, "and the class serves again, still for free");

    reset(link);
}

// ---------------------------------------------------------------------------
// 6 — a declaration that cannot be honoured is inert, and says so.
// ---------------------------------------------------------------------------
/**
 * @brief #1160's rule applied to the new numbers: report the class the link is RUNNING.
 *
 * Two ways to declare a class that cannot exist — a slot size no bigger than the inline
 * capacity (an empty band: nothing can ever route to it) and a count of zero (a bound of
 * zero: every large frame would be dropped). Both are refused rather than clamped into
 * something the integrator did not ask for, both leave the link fully functional on the
 * heap arm, and both must READ as refused, or an operator sizing against `tx_large_bytes()`
 * would be quoted a number this link never allocated.
 */
void test_a_rejected_declaration_is_inert_and_reported() {
    std::printf("declarations that cannot be honoured:\n");
    auto narrow = make_link(httpd_ws_link_t::kDefaultTxInlineBytes, kLargeSlots);
    check_eq(narrow->tx_large_bytes(), 0, "a slot no wider than the inline capacity is refused");
    check_eq(narrow->tx_large_slot_capacity(), 0, "and its count is refused with it");
    claim(kFd);
    tr::net::transport_t* peer = only_peer(*narrow);
    check(peer != nullptr, "the peer resolved on the narrow-declaration link");
    if (peer == nullptr) return;
    drain();
    const std::vector<std::byte> frame = band_frame();
    check_eq(allocs_for_one_send(peer, frame), 1, "so the frame takes the heap arm, as before");
    const std::size_t narrow_bytes = narrow->buffer_bytes();
    reset(narrow);

    auto countless = make_link(kLargeBytes, 0);
    check_eq(countless->tx_large_bytes(), 0, "a class of zero buffers is refused too");
    check_eq(countless->tx_large_slot_capacity(), 0, "count and size are zeroed together");
    check_eq(countless->buffer_bytes(), narrow_bytes,
             "and neither refused declaration is charged to the link's RAM census");
    reset(countless);

    auto real = make_link(kLargeBytes, kLargeSlots);
    check_eq(real->buffer_bytes(), narrow_bytes + kLargeBytes * kLargeSlots,
             "while an honoured one IS charged, exactly its own size x count");
    reset(real);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link bounded large TX size class (#1566)\n");
    test_no_class_declared_keeps_the_heap_arm();
    test_a_declared_class_costs_no_allocation();
    test_the_inline_band_did_not_move();
    test_past_the_class_the_heap_tail_survives();
    test_an_exhausted_class_drops_and_counts();
    test_a_rejected_declaration_is_inert_and_reported();
    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
