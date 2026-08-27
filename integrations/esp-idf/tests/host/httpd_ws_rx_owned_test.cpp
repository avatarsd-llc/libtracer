/**
 * @file
 * @brief #1565 — OPT-IN OWNING RX delivery from a bounded, injected pool: the frame is
 *        recv'd once into the integrator's memory and travels from there by refcount, and
 *        an exhausted pool is a named drop rather than a heap fallback.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #949 tx-pool, #961 fan-out and #1566 large-class suites: the REAL
 * chip translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against
 * the host fake of `esp_http_server` (fake_httpd.hpp), with the fan-out suite's instrument —
 * the WHOLE global `operator new`/`delete` family, replaced, so "this path allocated" is a
 * number rather than a code reading. WHOLE, because a subset makes ASan abort on the
 * `new (std::nothrow)`/`free` pair `detach_sessions()` takes on every teardown here.
 *
 * The condition the mode exists for: an unfragmented frame is delivered BORROWED — scratch
 * backed, zero per-frame allocation — which is exactly right for the decode-and-forget
 * consumer the router is, and exactly wrong for one that must OWN the payload past the
 * callback (publish it as a stored value, queue it to another task). That consumer copies it
 * out of the borrowed scratch itself, so its inbound path is the ingress recv PLUS a second
 * full copy, on every frame, at tens of frames a second on an ESP32-class part.
 *
 * Five properties, and they are five different properties:
 *   1. the DEFAULT is untouched — no backend named means borrowed delivery, `delivers_ropes()`
 *      false, and the same zero allocations the path has always had. This is the property the
 *      dispatch-inline cliff (#1223/#1250) makes non-negotiable, and it is measured, not
 *      asserted;
 *   2. with a backend named, the frame arrives OWNING and the sink can KEEP it past the
 *      callback — the whole point. Its bytes must still be right after the link has moved on,
 *      which is what makes "owning" mean something rather than "borrowed with extra steps";
 *   3. the owning delivery costs ZERO global allocations: the segment comes from the injected
 *      pool, not from the heap;
 *   4. an exhausted pool is a NAMED DROP — `rx_dropped_pool`, and `dropped_rx` through the
 *      generic `transport_drop_stats_t` view — with no heap fallback, and the SESSION SURVIVES
 *      it: the stream stays framed and the next frame is served;
 *   5. a FRAGMENTED message reaches the same owning sink. Without this, turning the mode on
 *      would silently starve a rope-only sink of every message a browser split in two.
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fake_httpd.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/mem_source_backend.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

/**
 * @brief Global allocations counted since process start — the instrument this suite reads.
 *
 * Atomic because the link's paths are callable from any task; every case here is
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
 *        form the standard defines, never a subset (see the file header).
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

/** @brief The one socket every case here serves. */
constexpr int kFd = 800;

/** @brief The per-socket send bound these links are built with — nothing here stalls a
 *         socket, so it is passed only to keep the suite off the derivation. */
constexpr std::uint32_t kSendBoundMs = 20;

/**
 * @brief The integrator's bounded RX pool, as a node would inject it: a static slab plus a
 *        handful of size-class slots, wrapped in the `mem_backend_t` every transport takes.
 *
 * ADR-0067's recycling-source shape, and the point of the whole mode: the bytes belong to
 * the deployment, exhaustion is a `nullptr` that never reaches the platform heap, and the
 * pool's own `capacity`/`in_use`/`peak` are ITS census to report, not the link's.
 */
struct rx_pool_t {
    /** @brief Build a pool whose slab is @p slab_bytes wide. */
    explicit rx_pool_t(std::size_t slab_bytes)
        : slab(slab_bytes), source({slab.data(), slab.size()}, {classes.data(), classes.size()}) {}

    std::vector<std::byte> slab;                    /**< @brief Caller-owned storage. */
    std::array<tr::mem::size_class_t, 4> classes{}; /**< @brief Caller-owned free-list slots. */
    tr::mem::pool_source_t<> source;                /**< @brief The bounded recycling source. */
    tr::mem::source_backend_t backend{source};      /**< @brief What the link is handed. */
};

/** @brief The frame body every case sends — big enough that a stray heap copy of it would
 *         be unmistakable, small enough to fit the RX scratch. */
std::vector<std::byte> body_of(std::size_t n, std::byte fill) {
    return std::vector<std::byte>(n, fill);
}

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief A link that adopts the fake server; @p rx is the injected owning-RX source (null =
 *         the default, borrowed delivery). */
std::unique_ptr<httpd_ws_link_t> make_link(tr::mem::mem_backend_t* rx) {
    return std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true, kSendBoundMs, 0, 0, 0, 0, 0,
                                             0, rx);
}

/** @brief Retire the link, the fake's sessions and its queue settings between cases. */
void reset(std::unique_ptr<httpd_ws_link_t>& link) {
    link.reset();
    fake_httpd::instance().set_queue_refusing(false);
    fake_httpd::instance().close_all();
    drain();
}

/** @brief What a BORROWED sink saw: the bytes, copied out, because they are gone after. */
struct span_sink_t {
    std::size_t calls = 0;       /**< @brief Deliveries observed. */
    std::vector<std::byte> last; /**< @brief A copy of the last frame's bytes. */
    void operator()(tr::net::peer_handle_t, std::span<const std::byte> f) {
        ++calls;
        last.assign(f.begin(), f.end());
    }
};

/**
 * @brief What an OWNING sink saw: the ropes themselves, KEPT past the callback.
 *
 * Keeping them is the assertion, not a convenience — a mode that hands up a rope the sink
 * cannot outlive has bought the consumer nothing over the borrowed span it already had.
 */
struct rope_sink_t {
    std::size_t calls = 0;              /**< @brief Deliveries observed. */
    std::vector<tr::view::rope_t> kept; /**< @brief Every rope, still held. */
    void operator()(tr::net::peer_handle_t, tr::view::rope_t r) {
        ++calls;
        kept.push_back(std::move(r));
    }
};

/** @brief The bytes of a single-link rope the sink kept, read back through its view. */
std::vector<std::byte> bytes_of(const tr::view::rope_t& r) {
    std::vector<std::byte> out;
    for (const tr::view::view_t& v : r.links()) {
        const std::span<const std::byte> b = v.bytes();
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

// ---------------------------------------------------------------------------
// 1 — the default did not move.
// ---------------------------------------------------------------------------
/**
 * @brief The property the dispatch-inline cliff makes non-negotiable, measured.
 *
 * A link constructed the way every link in the tree is constructed today names no backend.
 * It must answer `delivers_ropes()` false — so `fwd_router_t::add_child` installs the
 * borrowed receiver it always has — and an inbound frame that fits the RX scratch must cost
 * the global heap NOTHING. A regression that put an allocation on this path would show here
 * as a non-zero, whatever it did to the counters.
 */
void test_the_borrowed_default_is_untouched() {
    std::printf("an inbound frame on a link that named no RX backend:\n");
    auto link = make_link(nullptr);
    check(link->ok(), "the adopting link registered its URI");
    check(!link->delivers_ropes(), "it delivers BORROWED — the default");
    check(link->rx_backend() == nullptr, "and reports no injected source");
    span_sink_t sink;
    link->set_peer_receiver(sink);

    const std::vector<std::byte> body = body_of(512, std::byte{0x41});
    // Claim the peer and spend every first-use cost (the slot, its name, the resolution
    // pool, any lazy init in the log) OUTSIDE the measured window.
    fake_httpd::instance().open_session(kFd);
    (void)fake_httpd::instance().deliver_frame(kFd, body);
    drain();
    check_eq(sink.calls, 1, "the warm-up frame was delivered");

    const std::size_t before = g_allocs.load(std::memory_order_relaxed);
    (void)fake_httpd::instance().deliver_frame(kFd, body);
    const std::size_t after = g_allocs.load(std::memory_order_relaxed);

    check_eq(after - before, 0, "the borrowed RX path touched the global heap ZERO times");
    check_eq(sink.calls, 2, "and the frame really was delivered (the zero is not vacuous)");
    check_eq(sink.last.size(), body.size(), "with its whole payload");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 + 3 — a named backend delivers OWNING, keepably, and off the heap.
// ---------------------------------------------------------------------------
/**
 * @brief The headline: the sink is handed a rope it may KEEP, drawn from the injected pool.
 *
 * Three measurements. `delivers_ropes()` must flip, or the router installs the wrong sink
 * and the mode is unreachable in production. The delivery must cost the global heap nothing
 * — the bytes come from the integrator's slab. And the ropes must still read correctly after
 * the link has moved on to later frames, which is the difference between owning the payload
 * and borrowing it with extra steps.
 */
void test_a_named_backend_delivers_owning_from_the_pool() {
    std::printf("an inbound frame on a link that named a bounded RX pool:\n");
    rx_pool_t pool{16384};
    auto link = make_link(&pool.backend);
    check(link->delivers_ropes(), "it delivers OWNING ropes");
    check(link->rx_backend() == &pool.backend, "and reports the source it was given");
    rope_sink_t sink;
    link->set_peer_rope_receiver(sink);

    const std::vector<std::byte> first = body_of(600, std::byte{0x11});
    const std::vector<std::byte> second = body_of(600, std::byte{0x22});
    fake_httpd::instance().open_session(kFd);
    (void)fake_httpd::instance().deliver_frame(kFd, first);
    drain();
    check_eq(sink.calls, 1, "the warm-up frame was delivered to the ROPE sink");

    const std::size_t before = g_allocs.load(std::memory_order_relaxed);
    (void)fake_httpd::instance().deliver_frame(kFd, second);
    const std::size_t after = g_allocs.load(std::memory_order_relaxed);

    // The sink's own `push_back` may grow its vector, which is the TEST's allocation and not
    // the link's; the bound is therefore "at most one", and the property that matters is
    // that no per-frame payload block was taken.
    check(after - before <= 1, "the owning RX path took no payload block from the global heap");
    check_eq(sink.calls, 2, "and both frames arrived owning");
    check_eq(link->stats().rx_dropped_pool, 0, "with nothing refused by the pool");

    // The point of the mode: the ropes are still ours, and still right, after the link has
    // gone on to serve later frames — and after it has been destroyed entirely.
    reset(link);
    check_eq(sink.kept.size(), 2, "both ropes are still held after the link is gone");
    if (sink.kept.size() == 2) {
        check(bytes_of(sink.kept[0]) == first, "the first rope still reads its own payload");
        check(bytes_of(sink.kept[1]) == second, "and the second still reads its own");
    }
    // Released HERE, while the pool is still alive — rule 2 on rx_backend().
    sink.kept.clear();
}

// ---------------------------------------------------------------------------
// 4 — an exhausted pool is a named drop, and the session survives it.
// ---------------------------------------------------------------------------
/**
 * @brief Fail-closed, and bounded: the refusal is counted, nothing is served from the heap,
 *        and the peer keeps its socket.
 *
 * The slab is sized to hold ONE frame of the size sent, and the sink KEEPS every rope it is
 * given, so the second frame finds the pool genuinely empty rather than merely busy. What
 * must then happen is a counted drop — visible both as the link's own `rx_dropped_pool` and
 * through the generic `transport_drop_stats_t::dropped_rx` a `transport_t*` holder reads —
 * with no allocation, and with the session still able to receive once a rope is released.
 * A fallback onto the heap would pass the first two and hand back the unbounded footprint
 * the injection exists to bound.
 */
void test_an_exhausted_pool_drops_and_counts() {
    std::printf("two frames against a pool with room for one:\n");
    rx_pool_t pool{1024};
    auto link = make_link(&pool.backend);
    rope_sink_t sink;
    link->set_peer_rope_receiver(sink);

    const std::vector<std::byte> body = body_of(512, std::byte{0x33});
    fake_httpd::instance().open_session(kFd);
    (void)fake_httpd::instance().deliver_frame(kFd, body);
    drain();
    check_eq(sink.calls, 1, "the first frame was served from the pool");
    check_eq(link->stats().rx_dropped_pool, 0, "and refused nothing");

    const std::size_t before = g_allocs.load(std::memory_order_relaxed);
    const esp_err_t verdict = fake_httpd::instance().deliver_frame(kFd, body);
    const std::size_t after = g_allocs.load(std::memory_order_relaxed);

    check_eq(link->stats().rx_dropped_pool, 1, "the second was refused by the pool and COUNTED");
    check_eq(sink.calls, 1, "and never reached the sink");
    check_eq(after - before, 0, "the refusal did NOT fall back to the global heap");
    check(verdict == ESP_OK, "the handler still succeeded — the session is not torn down");
    check_eq(link->drop_stats().dropped_rx, 1,
             "and a generic transport_t* holder sees it as dropped_rx");

    // Give the pool its block back: the same peer must be served again, which is what makes
    // the drop backpressure rather than a wedge.
    sink.kept.clear();
    (void)fake_httpd::instance().deliver_frame(kFd, body);
    check_eq(sink.calls, 2, "with a rope released, the pool serves the peer again");
    check_eq(link->stats().rx_dropped_pool, 1, "and nothing further was refused");

    sink.kept.clear();
    reset(link);
}

// ---------------------------------------------------------------------------
// 5 — a fragmented message reaches the owning sink too.
// ---------------------------------------------------------------------------
/**
 * @brief The starvation this mode would otherwise cause, pinned by its absence.
 *
 * With `delivers_ropes()` true the router installs a ROPE sink and no span sink, and the
 * borrowed delivery path reaches neither — so a reassembled message handed up borrowed would
 * vanish without a trace. The fragmented arm therefore pays one copy into a pool segment and
 * delivers owning, which this case measures by its OUTCOME: the whole message, in one rope,
 * at the same sink the unfragmented frames arrive at.
 */
void test_a_fragmented_message_arrives_owning() {
    std::printf("a two-fragment message on an owning link:\n");
    rx_pool_t pool{16384};
    auto link = make_link(&pool.backend);
    rope_sink_t sink;
    link->set_peer_rope_receiver(sink);

    const std::vector<std::byte> head = body_of(300, std::byte{0xA1});
    const std::vector<std::byte> tail = body_of(200, std::byte{0xB2});
    fake_httpd::instance().open_session(kFd);
    (void)fake_httpd::instance().deliver_frame(kFd, head, /*final=*/false);
    check_eq(sink.calls, 0, "the opening fragment delivers nothing on its own");
    (void)fake_httpd::instance().deliver_frame(kFd, tail, /*final=*/true, HTTPD_WS_TYPE_CONTINUE);

    check_eq(sink.calls, 1, "the completed message reached the OWNING sink");
    if (sink.calls == 1) {
        std::vector<std::byte> want = head;
        want.insert(want.end(), tail.begin(), tail.end());
        check(bytes_of(sink.kept.front()) == want, "whole, in order, and keepable");
    }
    check_eq(link->stats().rx_dropped_pool, 0, "and nothing was refused");

    sink.kept.clear();
    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link owning RX delivery from a bounded pool (#1565)\n");
    test_the_borrowed_default_is_untouched();
    test_a_named_backend_delivers_owning_from_the_pool();
    test_an_exhausted_pool_drops_and_counts();
    test_a_fragmented_message_arrives_owning();
    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
