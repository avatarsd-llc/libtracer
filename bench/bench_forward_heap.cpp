/**
 * @file
 * @brief The 16KB-RAM zero-heap forward-path bench (ADR-0038 §16KB-RAM feasibility gate).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Three armed windows: (1) one FWD *forward hop* — offset-dispatch + stack heads +
 * stack iov (ADR-0038 invariants #1/#2), hard-gated at ZERO allocations by CI
 * (`ZEROHEAP_MAX=0`); (2) one *terminus* resolve (ADR-0041) — REPORT-ONLY, since a
 * terminus may allocate (ADR-0039): the arena draws from the router's injected
 * memory_resource (the default heap here, so every draw is counted and visible);
 * (3) the *per-vertex steady-heap* probe (#361 §8) — REPORT-ONLY, LIVE usable-size
 * bytes a default STORED_VALUE leaf holds at steady state, and the increment one
 * small LKV write adds — the diet trend the gh-pages history tracks.
 *
 * This TU owns the global operator-new/delete override (probe/heap_probe.hpp): all
 * allocation variants — plain, sized, aligned (what `heap_alloc`'s `operator new(size,
 * align_val_t, nothrow)` uses), and nothrow — funnel through a counting malloc wrapper,
 * so nothing on the path is missed. Single-threaded by construction: a synchronous
 * substrate (CAN/UART) forwards inline on its receive with no async handoff (ADR-0038),
 * so the forward hop is exercised by feeding a frame straight into on_frame() and
 * capturing the bytes the wired transport is handed — no threads, no sockets, no lwIP.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { counted_free(p); }

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
    }

    // Optional hard gate: `ZEROHEAP_MAX=N` fails the run if FORWARD allocs>N (the
    // terminus window above stays report-only). CI runs `ZEROHEAP_MAX=0`.
    if (const char* cap = std::getenv("ZEROHEAP_MAX")) {
        const auto max_allocs = static_cast<std::size_t>(std::strtoul(cap, nullptr, 10));
        if (c.allocs > max_allocs) {
            std::printf("ZEROHEAP: FAIL (allocs=%zu > max=%zu)\n", c.allocs, max_allocs);
            return 1;
        }
        std::printf("ZEROHEAP: PASS (allocs=%zu <= max=%zu)\n", c.allocs, max_allocs);
    }
    (void)term_allocs;
    return 0;
}
