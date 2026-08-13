/**
 * @file
 * @brief The 16KB-RAM zero-heap forward-path bench (ADR-0038 §16KB-RAM feasibility gate).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @warning **What this gate does NOT cover.** It drives `capture_transport_t`, a stub link that
 * only sums the span sizes it is handed. The shipping transports assemble an `::iovec` table
 * before the syscall and **both fall back to the heap above 16 spans** (`transport_udp.cpp`,
 * `transport_tcp.cpp`). `allocs=0` here therefore says nothing about the real wire: measured at
 * 17 spans / ~288 B by **`bench_transport_iov`**, which exists because that term was invisible.
 * Headroom from `kFwdMaxIov` (9) to the spill (17) is **8 regions**, and a rope source may split
 * any region further. Read the two benches together; neither is sufficient alone.
 *
 * Four armed windows: (1) one FWD *forward hop* — offset-dispatch + stack heads +
 * stack iov (ADR-0038 invariants #1/#2), hard-gated at ZERO allocations by CI
 * (`ZEROHEAP_MAX=0`); (2) one *terminus* resolve (ADR-0041) — REPORT-ONLY, since a
 * terminus may allocate (ADR-0039): the arena draws from the router's injected
 * memory seams (the default heap here, so every draw is counted and visible; since #588
 * the terminus ARENA draws from the router's nothrow `rx` block source, not from `mr_`);
 * (3) the *per-vertex steady-heap* probe (#361 §8) — REPORT-ONLY, LIVE usable-size
 * bytes a default STORED_VALUE leaf holds at steady state, and the increment one
 * small LKV write adds — the diet trend the gh-pages history tracks; (4) the
 * *registration escape* probe (#551) — REPORT-ONLY, how much of a RUNTIME vertex
 * registration bypasses the graph's own injected seams. Window (4) is the
 * only one that counts what did NOT happen through a seam rather than what an
 * operation cost, so it reads both counters at once: global blocks that escaped, and
 * blocks the resource served.
 *
 * This TU owns the global operator-new/delete override (probe/heap_probe.hpp): all
 * allocation variants — plain, sized, aligned (what `heap_alloc`'s `operator new(size,
 * align_val_t, nothrow)` uses), and nothrow — funnel through a counting malloc wrapper,
 * so nothing on the path is missed. Single-threaded by construction: a synchronous
 * substrate (CAN/UART) forwards inline on its receive with no async handoff (ADR-0038),
 * so the forward hop is exercised by feeding a frame straight into on_frame() and
 * capturing the bytes the wired transport is handed — no threads, no sockets, no lwIP.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if __has_include(<malloc.h>)
#include <malloc.h>  // malloc_usable_size: feeds the live-bytes (steady-heap) balance
#define BENCH_HAS_USABLE_SIZE 1
#endif

#include "heap_probe.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

// --- the counting allocator override (all variants) --------------------------

namespace {
void* counted_alloc(std::size_t size) {
    const bool armed = probe::g_armed.load(std::memory_order_relaxed);
    if (armed) {
        probe::g_allocs.fetch_add(1, std::memory_order_relaxed);
        probe::g_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size ? size : 1);
#ifdef BENCH_HAS_USABLE_SIZE
    if (armed && p != nullptr)
        probe::g_live_bytes.fetch_add(static_cast<long long>(malloc_usable_size(p)),
                                      std::memory_order_relaxed);
#endif
    return p;
}
/**
 * @brief The aligned counted allocation (#801) — `aligned_alloc` for a genuinely OVER-aligned
 *        request, whose result `free` accepts; `malloc` for a fundamental one.
 *
 * Forwarding an over-aligned request to plain `malloc` (what this file did until #801) hands
 * back memory aligned only to `max_align_t`, so a `std::pmr` control block asking for 32-byte
 * alignment is under-aligned — real UB, latent only because nothing on today's measured path
 * asks for more.
 *
 * The alignment TEST is `> alignof(std::max_align_t)`, not `>=`, and that is the whole
 * correctness question. `malloc` is specified to return storage suitably aligned for any
 * object with FUNDAMENTAL alignment, so forwarding a fundamental-alignment request to it is
 * correct C — the bug was forwarding an OVER-aligned one, which is what this arm now catches.
 * Keeping the fundamental case on `malloc` also keeps every published figure where it was:
 * libtracer's heap backend asks for exactly `alignof(std::max_align_t)` (mem_heap.hpp), and
 * `malloc_usable_size` of an `aligned_alloc` block rounds up to the alignment, so routing it
 * would move the gh-pages-tracked `vertex_value` live-bytes trend 104 -> 120 B — measured, and
 * caught by the perf gate's RAM ratchet as a 15.4 % pullback. That number is a property of the
 * measuring instrument, not of the library, so moving it would report a regression that did not
 * happen.
 */
void* counted_aligned_alloc(std::size_t size, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted_alloc(size);  // malloc already suits
    const bool armed = probe::g_armed.load(std::memory_order_relaxed);
    if (armed) {
        probe::g_allocs.fetch_add(1, std::memory_order_relaxed);
        probe::g_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    // aligned_alloc requires a size that is a multiple of the alignment.
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1) / align * align;
    void* p = std::aligned_alloc(align, rounded);
#ifdef BENCH_HAS_USABLE_SIZE
    if (armed && p != nullptr)
        probe::g_live_bytes.fetch_add(static_cast<long long>(malloc_usable_size(p)),
                                      std::memory_order_relaxed);
#endif
    return p;
}
void counted_free(void* p) {
    if (p == nullptr) return;
    if (probe::g_armed.load(std::memory_order_relaxed)) {
        probe::g_frees.fetch_add(1, std::memory_order_relaxed);
#ifdef BENCH_HAS_USABLE_SIZE
        probe::g_live_bytes.fetch_sub(static_cast<long long>(malloc_usable_size(p)),
                                      std::memory_order_relaxed);
#endif
    }
    std::free(p);
}
}  // namespace

void* operator new(std::size_t size) {
    void* p = counted_alloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    void* p = counted_alloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new(std::size_t size, std::align_val_t align) {
    void* p = counted_aligned_alloc(size, static_cast<std::size_t>(align));
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size, std::align_val_t align) { return operator new(size, align); }
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return counted_aligned_alloc(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return counted_aligned_alloc(size, static_cast<std::size_t>(align));
}
// Every DEALLOCATING form (#793) — the array, nothrow and sized+aligned twins were the hole.
// A replacement set with a hole leaves that one form to the sanitizer's own operator, which
// is then handed a pointer this file allocated: ASan reports `alloc-dealloc-mismatch` the
// first time such a job runs this bench. The sized+aligned `operator delete` is the one
// libstdc++'s `std::pmr::memory_resource::deallocate` walks into on every pmr control block.
// Set completed from `core/tests/terminus_flatten_backend_test.cpp`, whose over-aligned
// `operator new` arm this file now shares as well (#801) — see counted_aligned_alloc above.
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    counted_free(p);
}

// --- the forward-hop fixture -------------------------------------------------

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/**
 * @brief A transport that only records the bytes it is asked to send (no I/O, no alloc while armed
 *        beyond what the router hands it — the send itself is the measured egress).
 */
struct capture_transport_t : transport_t {
    std::size_t sends = 0;
    std::size_t last_len = 0;
    void send(std::span<const std::byte> f) override {
        ++sends;
        last_len = f.size();
    }
    /**
     * @brief Override the scatter-gather send so the measured window sees the ROUTER's cost, not
     *        the base class's flatten-into-a-temp-vector (which would add a measurement artifact
     *        alloc).
     *
     * A real zero-copy transport (sendmsg/writev/RDMA) overrides this
     * exactly this way — sum the spans, no copy, no heap.
     */
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::size_t total = 0;
        for (const auto& s : iov) total += s.size();
        ++sends;
        last_len = total;
    }
};

/**
 * @brief A pass-through `std::pmr::memory_resource` that counts what it is asked to serve.
 *
 * The escape probe needs BOTH sides of the ADR-0039 seam in one window: the global
 * operator-new counter says how many blocks bypassed the resource, and this says how many
 * the resource actually served. Reporting only the first would leave "0 escapes" and "the
 * graph allocated nothing at all" indistinguishable.
 *
 * Serves from the default resource rather than a slab, deliberately: a slab would also
 * change WHERE the bytes come from, and this probe is measuring routing, not locality.
 */
class counting_resource_t final : public std::pmr::memory_resource {
   public:
    std::size_t allocs = 0; /**< @brief Total allocations served. */
    std::size_t live = 0;   /**< @brief Allocations not yet released. */

   private:
    /** @brief Serve from the default resource, counting. */
    void* do_allocate(std::size_t n, std::size_t align) override {
        ++allocs;
        ++live;
        return std::pmr::get_default_resource()->allocate(n, align);
    }
    /** @brief Release to the default resource, counting. */
    void do_deallocate(void* p, std::size_t n, std::size_t align) override {
        --live;
        std::pmr::get_default_resource()->deallocate(p, n, align);
    }
    /** @brief Identity equality — a stateful counter is only equal to itself. */
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        return this == &o;
    }
};

/** @brief Append a NAME-only PATH TLV over `segs`. */
void emit_path(std::vector<std::byte>& out, std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
}

/**
 * @brief Build FWD{ op=WRITE, dst=<dst...>, src=<src...>, VALUE payload } — the frame a forward hop
 *        shrinks (dst) and grows (src).
 */
std::vector<std::byte> make_fwd(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src,
                                std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    emit_path(body, src);
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, payload);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

}  // namespace

int main() {
    // One node with two transport children: a frame arriving on "in" whose dst names
    // "out" is a pure FORWARD hop (strip "out" from dst, prepend "in" to src, send on
    // "out") — the exact hot path the 16KB node runs, no terminus, no local vertex.
    graph_t graph;
    fwd_router_t router(graph);
    capture_transport_t in_link;
    capture_transport_t out_link;
    router.add_child("in", in_link);
    router.add_child("out", out_link);

    const std::byte payload[4] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                  std::byte{0xEF}};
    const std::vector<std::byte> frame =
        make_fwd({"out", "sensor", "temp"}, {"reply"}, std::span<const std::byte>(payload, 4));

    // Warm once (prime any lazy statics) OUTSIDE the measured window.
    router.on_frame("in", frame);
    const std::size_t warm_sends = out_link.sends;

    // Measure a single forward hop.
    probe::window_t win;
    router.on_frame("in", frame);
    const probe::counts_t c = win.result();

    const bool forwarded = out_link.sends == warm_sends + 1;
    std::printf(
        "RESULT zeroheap forward allocs=%zu frees=%zu bytes=%zu egress_len=%zu forwarded=%d\n",
        c.allocs, c.frees, c.bytes, out_link.last_len, forwarded ? 1 : 0);

    if (!forwarded) {
        std::printf("FAIL: the frame did not forward — fixture broken, not a heap result\n");
        return 2;
    }

    // --- terminus mode (ADR-0041, REPORT-ONLY) --------------------------------
    // A terminus is ALLOWED to allocate (ADR-0039 §context-1); this window makes
    // the cost visible and bounded, not zero-gated: arena decode draws from the
    // router's injected memory_resource (default = heap here, so every draw is
    // counted), the reply head is ONE exactly-sized segment, the ownership
    // copies are one each. A host that injects a pool resource over its slab
    // moves the arena draws off the global heap entirely.
    std::optional<tr::graph::vertex_handle_t> v;
    if (const auto path = tr::graph::path_t::parse("/sensor/temp")) {
        v = graph.register_vertex(*path, tr::graph::role_t::STORED_VALUE);  // infallible (ADR-0056)
    }
    std::size_t term_allocs = 0;
    bool replied = false;
    if (v) {
        std::vector<std::byte> stored;
        tr::wire::emit_tlv(stored, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload, 4));
        tr::view::view_t sv = tr::view::over_bytes(stored).value_or(tr::view::view_t{});
        (void)graph.write(*v, sv);

        std::vector<std::byte> read_body;
        const std::byte rop{static_cast<std::uint8_t>(tr::graph::fwd_op_t::READ)};
        tr::wire::emit_tlv(read_body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&rop, 1));
        emit_path(read_body, {"sensor", "temp"});
        emit_path(read_body, {"reply"});
        std::vector<std::byte> read_frame;
        tr::wire::emit_tlv(read_frame, type_t::FWD, opt_t{.pl = true}, read_body);

        router.on_frame("in", read_frame);  // warm outside the window
        const std::size_t warm_in = in_link.sends;
        probe::window_t twin;
        router.on_frame("in", read_frame);
        const probe::counts_t tc = twin.result();
        term_allocs = tc.allocs;
        replied = in_link.sends == warm_in + 1;
        std::printf(
            "RESULT terminus allocs=%zu frees=%zu bytes=%zu reply_len=%zu replied=%d "
            "(report-only — a terminus may allocate, ADR-0039)\n",
            tc.allocs, tc.frees, tc.bytes, in_link.last_len, replied ? 1 : 0);
        if (!replied) {
            std::printf("FAIL: the READ terminus did not reply — fixture broken\n");
            return 2;
        }
    }

    // --- wide fan-out zero-alloc window (the >kInlineFanout snapshot cliff) ----
    // A vertex with > kInlineFanout (8) in-process subscribers USED to malloc a fresh
    // overflow vector on EVERY publish (graph_t::fan_out). With the thread-local reusable
    // overflow lease, a WARM wide fan-out is zero-alloc. Measured via `propagate` (deliver
    // only — a `write` would store a fresh LKV `make_shared` and mask the fan-out alloc).
    // Gated at 0 (ZEROHEAP_MAX) like the forward hop: the snapshot buffer's capacity persists.
    std::size_t fanout_allocs = 0;
    std::size_t fanout_bytes = 0;
    bool wide_delivered = false;
    {
        graph_t fg;
        std::atomic<std::uint64_t> got{0};
        auto cb = [&](const tr::view::rope_t&) { got.fetch_add(1, std::memory_order_relaxed); };
        std::optional<tr::graph::vertex_handle_t> fv;
        if (const auto p = tr::graph::path_t::parse("/wide/v")) {
            fv = fg.register_vertex(*p, tr::graph::role_t::STORED_VALUE);
            for (int i = 0; i < 16; ++i) (void)fg.subscribe(*p, cb);  // 16 > kInlineFanout (8)
        }
        if (fv) {
            std::vector<std::byte> wstored;
            tr::wire::emit_tlv(wstored, type_t::VALUE, opt_t{},
                               std::span<const std::byte>(payload, 4));
            const tr::view::view_t wsv = tr::view::over_bytes(wstored).value_or(tr::view::view_t{});
            (void)fg.write(*fv, wsv);  // store the LKV once (outside the window)
            fg.propagate(*fv);         // WARM propagate: prime the free-list + buffer capacity
            const std::uint64_t warm_got = got.load();
            probe::window_t fwin;
            fg.propagate(*fv);  // MEASURED: warm wide fan-out -> zero-alloc with the lease
            const probe::counts_t fc = fwin.result();
            fanout_allocs = fc.allocs;
            fanout_bytes = fc.bytes;
            wide_delivered = got.load() == warm_got + 16;
            // The residual 2 allocs / ~26 B are propagate_impl's subtree-sweep `build_key`,
            // NOT the fan-out: the >kInlineFanout overflow-vector cliff (a ~2 KB reserve
            // every publish) is what the lease eliminated. `bytes` reports the balance —
            // the byte-gate below fails if the KB-scale overflow ever returns. (This said
            // "1 alloc" for a while after the count moved to 2; the GATE is on bytes, so the
            // drift was invisible — worth stating, since a stale figure in a comment beside a
            // live number is how someone concludes the gate caught something it did not.)
            std::printf(
                "RESULT zeroheap fanout_wide allocs=%zu frees=%zu bytes=%zu subs=16 delivered=%d\n",
                fc.allocs, fc.frees, fc.bytes, wide_delivered ? 1 : 0);
            if (!wide_delivered) {
                std::printf("FAIL: wide fan-out did not deliver to all 16 subscribers\n");
                return 2;
            }
        }
    }

    // --- per-vertex steady-heap probe (#361 §8, REPORT-ONLY) -------------------
    // The vertex-diet trend: LIVE usable-size bytes a default STORED_VALUE leaf
    // holds at steady state (struct + name key + child-link slot), and the
    // increment one small LKV write adds (shared_ptr control block + rope). The
    // live balance nets out transient churn (path-parse temporaries, container
    // regrowth), so it IS the number an MCU's heap watermark moves by per
    // endpoint. `bytes=` in these two lines therefore reports the live balance
    // per vertex, not gross alloc bytes; the gross figure rides in the tail.
    {
        graph_t diet_graph;
        if (const auto warm = tr::graph::path_t::parse("/ep/warm"))
            (void)diet_graph.register_vertex(*warm, tr::graph::role_t::STORED_VALUE);

        // The write payload + view are built OUTSIDE the armed windows: only the
        // graph's own cost is measured, not the fixture's frame construction.
        std::vector<std::byte> diet_stored;
        tr::wire::emit_tlv(diet_stored, type_t::VALUE, opt_t{},
                           std::span<const std::byte>(payload, 4));
        const tr::view::view_t diet_sv =
            tr::view::over_bytes(diet_stored).value_or(tr::view::view_t{});

        constexpr std::size_t kDietN = 512;
        bool diet_ok = true;
        probe::reset();
        probe::arm();
        for (std::size_t i = 0; i < kDietN; ++i) {
            char pb[24];
            std::snprintf(pb, sizeof pb, "/ep/v%04zu", i);
            const auto p = tr::graph::path_t::parse(pb);
            diet_ok = diet_ok && p.has_value();
            if (p) (void)diet_graph.register_vertex(*p, tr::graph::role_t::STORED_VALUE);
        }
        const probe::counts_t reg = probe::snapshot();
        for (std::size_t i = 0; i < kDietN; ++i) {
            char pb[24];
            std::snprintf(pb, sizeof pb, "/ep/v%04zu", i);
            if (const auto p = tr::graph::path_t::parse(pb))
                diet_ok = diet_ok && diet_graph.write(*p, diet_sv).has_value();
        }
        const probe::counts_t wrt = probe::snapshot();
        probe::disarm();

        const auto per = [](long long total) {
            return total > 0 ? static_cast<std::size_t>(total) / kDietN : std::size_t{0};
        };
        std::printf(
            "RESULT zeroheap vertex allocs=%zu frees=%zu bytes=%zu n=%zu gross_bytes=%zu "
            "ok=%d (report-only — live usable-size bytes per default leaf, #361 §8)\n",
            reg.allocs / kDietN, reg.frees / kDietN, per(reg.live_bytes), kDietN,
            reg.bytes / kDietN, diet_ok ? 1 : 0);
        std::printf(
            "RESULT zeroheap vertex_value allocs=%zu frees=%zu bytes=%zu n=%zu gross_bytes=%zu "
            "ok=%d (report-only — live bytes one 4B LKV write adds per vertex)\n",
            (wrt.allocs - reg.allocs) / kDietN, (wrt.frees - reg.frees) / kDietN,
            per(wrt.live_bytes - reg.live_bytes), kDietN, (wrt.bytes - reg.bytes) / kDietN,
            diet_ok ? 1 : 0);
        if (!diet_ok) {
            std::printf("FAIL: vertex-diet fixture did not register/write — not a heap result\n");
            return 2;
        }

        // --- per-SESSION-ANCHOR steady-heap probe (#1223, REPORT-ONLY) ------------
        // ADR-0044's 2026-08-13 amendment rests on a number — ~0.5 KB at 4 peers — and that
        // number is only honest if a session identity anchor costs what a bare vertex costs.
        // It is the SAME allocation shape (one vertex_t + its NAME record + one slot-deque
        // pointer, no handlers, no ext block, no value), so the line printed here should read
        // like the `vertex` line above; a materially larger one would undercut the amendment
        // and belongs in the PR that caused it.
        //
        // CHURN is measured alongside, because the whole claim is that reconnects are free:
        // the second pass retires and revives every anchor in place and must allocate NOTHING.
        tr::graph::graph_t anchor_graph;
        bool anchor_ok = true;
        probe::reset();
        probe::arm();
        for (std::size_t i = 0; i < kDietN; ++i) {
            char ab[32];
            std::snprintf(ab, sizeof ab, ":net/ws/srv/p%04zu", i);
            anchor_ok = anchor_ok && anchor_graph.register_session_anchor(ab).has_value();
        }
        const probe::counts_t anc = probe::snapshot();
        for (std::size_t i = 0; i < kDietN; ++i) {
            char ab[32];
            std::snprintf(ab, sizeof ab, ":net/ws/srv/p%04zu", i);
            if (const auto h = anchor_graph.find_session_anchor(ab)) {
                anchor_ok = anchor_ok && anchor_graph.retire(*h).has_value();
                anchor_ok = anchor_ok && anchor_graph.register_session_anchor(ab).has_value();
            } else {
                anchor_ok = false;
            }
        }
        const probe::counts_t chn = probe::snapshot();
        probe::disarm();
        std::printf(
            "RESULT zeroheap session_anchor allocs=%zu frees=%zu bytes=%zu n=%zu gross_bytes=%zu "
            "ok=%d (report-only — live usable-size bytes per accepted-session anchor, #1223)\n",
            anc.allocs / kDietN, anc.frees / kDietN, per(anc.live_bytes), kDietN,
            anc.bytes / kDietN, anchor_ok ? 1 : 0);
        std::printf(
            "RESULT zeroheap session_anchor_churn allocs=%zu frees=%zu bytes=%zu n=%zu "
            "gross_bytes=%zu ok=%d (report-only — a retire+revive of the SAME slot, #1223)\n",
            (chn.allocs - anc.allocs) / kDietN, (chn.frees - anc.frees) / kDietN,
            per(chn.live_bytes - anc.live_bytes), kDietN, (chn.bytes - anc.bytes) / kDietN,
            anchor_ok ? 1 : 0);
        if (!anchor_ok) {
            std::printf("FAIL: session-anchor fixture did not register/revive — not a result\n");
            return 2;
        }
    }

    // --- per-vertex APP-FIELD-TABLE steady-heap probe (#388, REPORT-ONLY) ------
    // The RFC-0010 economy trend: LIVE bytes a representative 5-field owner
    // descriptor table adds per vertex (the extension block + the table's own
    // storage) — the number that decides whether per-endpoint app-field schemas
    // beat the /meta child-vertex workaround on the MCU (#388's ask for a gate
    // row alongside the per-leaf number).
    {
        graph_t app_graph;
        const auto mk_table = [] {
            std::vector<tr::graph::app_field_t> table;
            table.reserve(5);
            static constexpr const char* kNames[5] = {"kp", "ki", "kd", "mode", "label"};
            for (const char* name : kNames) {
                tr::graph::app_field_t f;
                f.name = name;
                f.access = tr::graph::app_access_t::RW;
                f.descriptor.assign(16, std::byte{0x11});  // a §B.1-sized record stand-in
                table.push_back(std::move(f));
            }
            return table;
        };
        if (const auto warm = tr::graph::path_t::parse("/app/warm")) {
            const auto h = app_graph.register_vertex(*warm, tr::graph::role_t::STORED_VALUE);
            app_graph.set_app_fields(h, mk_table());
        }
        constexpr std::size_t kAppN = 256;
        bool app_ok = true;
        probe::reset();
        probe::arm();
        for (std::size_t i = 0; i < kAppN; ++i) {
            char pb[24];
            std::snprintf(pb, sizeof pb, "/app/v%04zu", i);
            const auto p = tr::graph::path_t::parse(pb);
            app_ok = app_ok && p.has_value();
            if (p) {
                const auto h = app_graph.register_vertex(*p, tr::graph::role_t::STORED_VALUE);
                app_graph.set_app_fields(h, mk_table());
            }
        }
        const probe::counts_t app = probe::snapshot();
        probe::disarm();
        const std::size_t leaf_and_table =
            app.live_bytes > 0 ? static_cast<std::size_t>(app.live_bytes) / kAppN : 0;
        std::printf(
            "RESULT zeroheap vertex_app5 allocs=%zu frees=%zu bytes=%zu n=%zu gross_bytes=%zu "
            "ok=%d (report-only — live bytes per leaf WITH a 5-field app table, #388)\n",
            app.allocs / kAppN, app.frees / kAppN, leaf_and_table, kAppN, app.bytes / kAppN,
            app_ok ? 1 : 0);
        if (!app_ok) {
            std::printf("FAIL: app-field fixture did not register — not a heap result\n");
            return 2;
        }
    }

    // --- the BORROWED install of the same table (ADR-0058, REPORT-ONLY) --------
    // The row above measures `set_app_fields`, which COPIES the declaration into the
    // table's `backing`. ADR-0058 also gives owners `set_app_fields_static`, whose slots
    // VIEW caller storage — built precisely for the MCU case #388 argues from, where the
    // descriptor tables are compile-time constants in flash. That path decides whether
    // per-endpoint schemas beat the `/meta` workaround on the target, and until now
    // nothing measured it: the economics were claimed, never gated. This row is the
    // borrowed twin of `vertex_app5`, same five fields and same descriptor width, so the
    // pair reads as the copy-vs-view delta rather than two unrelated numbers.
    {
        graph_t app_graph;
        static constexpr std::array<std::byte, 16> kDescriptor{};
        static constexpr std::string_view kNames[5] = {"kp", "ki", "kd", "mode", "label"};
        // Static storage, as the borrowed contract requires: these must outlive every
        // vertex that views them, which is what makes the declaration cost zero RAM.
        static std::array<tr::graph::app_field_static_t, 5> kTable = [] {
            std::array<tr::graph::app_field_static_t, 5> t{};
            for (std::size_t i = 0; i < 5; ++i) {
                t[i].name = kNames[i];
                t[i].access = tr::graph::app_access_t::RW;
                t[i].descriptor = std::span<const std::byte>(kDescriptor);
            }
            return t;
        }();
        if (const auto warm = tr::graph::path_t::parse("/sapp/warm")) {
            const auto h = app_graph.register_vertex(*warm, tr::graph::role_t::STORED_VALUE);
            app_graph.set_app_fields_static(h, kTable);
        }
        constexpr std::size_t kAppN = 256;
        bool sapp_ok = true;
        probe::reset();
        probe::arm();
        for (std::size_t i = 0; i < kAppN; ++i) {
            char pb[24];
            std::snprintf(pb, sizeof pb, "/sapp/v%04zu", i);
            const auto p = tr::graph::path_t::parse(pb);
            sapp_ok = sapp_ok && p.has_value();
            if (p) {
                const auto h = app_graph.register_vertex(*p, tr::graph::role_t::STORED_VALUE);
                app_graph.set_app_fields_static(h, kTable);
            }
        }
        const probe::counts_t sapp = probe::snapshot();
        probe::disarm();
        const std::size_t leaf_and_view =
            sapp.live_bytes > 0 ? static_cast<std::size_t>(sapp.live_bytes) / kAppN : 0;
        std::printf(
            "RESULT zeroheap vertex_app5_static allocs=%zu frees=%zu bytes=%zu n=%zu "
            "gross_bytes=%zu ok=%d (report-only — live bytes per leaf with a BORROWED "
            "5-field app table, ADR-0058 / #388)\n",
            sapp.allocs / kAppN, sapp.frees / kAppN, leaf_and_view, kAppN, sapp.bytes / kAppN,
            sapp_ok ? 1 : 0);
        if (!sapp_ok) {
            std::printf("FAIL: borrowed app-field fixture did not register — not a heap result\n");
            return 2;
        }
    }

    // --- ADR-0039 injection-seam escape probe (#551, REPORT-ONLY) --------------
    // How many heap blocks a RUNTIME vertex registration takes that the graph's own
    // injected `std::pmr::memory_resource` never sees.
    //
    // Registration used to be a setup-time operation, which ADR-0039 explicitly carves
    // out of the seam. RFC-0014 ended that: `transport_vertex_t::make_connection` calls
    // `register_vertex_key` when a CREATE op resolves, on whichever transport thread
    // received the frame — so a peer now drives vertex allocation. The carve-out no
    // longer covers it, and nothing measured whether the seam was honored.
    //
    // The fixture is the shape RFC-0014 actually produces, not a bare leaf: a
    // `/net/<module>/<name>` path whose NAME record EXCEEDS `path_key_t::kInlineBytes`
    // (a connection name is peer-chosen, and `conn-192-168-1-50-47301` is 23 characters
    // against a 12-character inline budget), registered with a handler so the extension
    // block and the value seam are both installed. Every one of the six allocation sites
    // fires.
    //
    // `allocs=` is the number that matters and it is exact, host-independent and
    // ratcheted (perf_gate.py): today it counts the blocks that ESCAPE to the global
    // heap. Each #551 slice drives it down; the ratchet is what stops it climbing back.
    //
    // It reads FOUR, while #551's ledger names SIX allocation SITES, and both are right.
    // A site is not a per-vertex cost: `children_t` is allocated once for a parent's
    // FIRST child, and the child vector's growth is geometric, so across kRegN siblings
    // the two together contribute well under one block per vertex and the per-vertex
    // integer division drops them. What remains, once per registration, is the
    // `path_key_t` spill, the `vertex_t` block, the extension block and the value seam.
    // Do not read a drop from 4 to 2 as "two sites fixed" — read the sites off the ledger
    // and this row off the tree shape it was measured on.
    {
        counting_resource_t reg_mr;
        graph_t reg_graph{&reg_mr};
        constexpr std::size_t kRegN = 256;
        bool reg_ok = true;

        // The parent chain is built OUTSIDE the armed window: what is measured is the
        // marginal cost of ONE fresh connection vertex, not the spine it hangs from.
        if (const auto mount = tr::graph::path_t::parse("/net/tcp"))
            (void)reg_graph.register_vertex(*mount, tr::graph::role_t::STORED_VALUE);

        std::vector<std::vector<std::byte>> reg_keys;
        reg_keys.reserve(kRegN);
        for (std::size_t i = 0; i < kRegN; ++i) {
            char pb[64];
            std::snprintf(pb, sizeof pb, "/net/tcp/conn-192-168-1-50-%05zu", i);
            const auto p = tr::graph::path_t::parse(pb);
            reg_ok = reg_ok && p.has_value();
            if (p) reg_keys.emplace_back(p->key().begin(), p->key().end());
        }

        const std::size_t mr_before = reg_mr.allocs;
        probe::reset();
        probe::arm();
        for (std::vector<std::byte>& key : reg_keys) {
            tr::graph::handlers_t h;
            h.on_write = [](const tr::view::rope_t&) -> tr::graph::result_t<void> { return {}; };
            reg_ok =
                reg_ok && reg_graph
                              .register_vertex_key(std::move(key), tr::graph::role_t::STORED_VALUE,
                                                   std::move(h))
                              .has_value();
        }
        const probe::counts_t reg = probe::snapshot();
        probe::disarm();
        const std::size_t mr_served = reg_mr.allocs - mr_before;

        const auto per_reg = [](long long total) {
            return total > 0 ? static_cast<std::size_t>(total) / kRegN : std::size_t{0};
        };
        std::printf(
            "RESULT zeroheap reg_escape allocs=%zu frees=%zu bytes=%zu n=%zu mr_served=%zu "
            "ok=%d (report-only — GLOBAL-heap blocks per RUNTIME registration that bypass "
            "the injected memory_resource; ADR-0039 / RFC-0014, #551)\n",
            reg.allocs / kRegN, reg.frees / kRegN, per_reg(reg.live_bytes), kRegN,
            mr_served / kRegN, reg_ok ? 1 : 0);
        if (!reg_ok) {
            std::printf("FAIL: registration-escape fixture did not register — not a heap result\n");
            return 2;
        }
    }

    // Hard gate (always on): the warm WIDE FAN-OUT must not re-open the >kInlineFanout
    // overflow-vector cliff — a ~2 KB per-publish `reserve` that the thread-local reusable
    // overflow lease eliminated. Gated on BYTES so the orthogonal ~13 B subtree-sweep
    // `build_key` residual passes while a returned KB-scale overflow fails loudly.
    (void)fanout_allocs;
    if (fanout_bytes > 256) {
        std::printf(
            "FAN-OUT: FAIL (warm wide fan-out allocated %zu B — the >kInlineFanout "
            "overflow cliff returned; expected only the ~13 B build_key)\n",
            fanout_bytes);
        return 1;
    }

    // Optional hard gate: `ZEROHEAP_MAX=N` fails the run if the FORWARD hop allocs>N (the
    // terminus + fanout_wide windows above are byte-gated / report-only). CI runs
    // `ZEROHEAP_MAX=0` — the forward splice is zero-alloc.
    if (const char* cap = std::getenv("ZEROHEAP_MAX")) {
        const auto max_allocs = static_cast<std::size_t>(std::strtoul(cap, nullptr, 10));
        if (c.allocs > max_allocs) {
            std::printf("ZEROHEAP: FAIL (forward allocs=%zu > max=%zu)\n", c.allocs, max_allocs);
            return 1;
        }
        std::printf("ZEROHEAP: PASS (forward allocs=%zu <= max=%zu; fanout_wide bytes=%zu)\n",
                    c.allocs, max_allocs, fanout_bytes);
    }
    (void)term_allocs;
    return 0;
}
