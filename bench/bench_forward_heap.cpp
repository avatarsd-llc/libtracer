/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The 16KB-RAM zero-heap forward-path bench (ADR-0038 §16KB-RAM feasibility gate).
 *
 * Measures — not asserts — how many heap allocations one FWD *forward hop* costs on
 * the current reference path, so the Stage-2 flip (heap-free forward: offset-dispatch
 * + pooled segment heads + stack iov, ADR-0038 invariants #1/#2/#5) is driven to zero
 * against a real baseline. Today's `fwd_router_t` forward branch full-decodes every
 * frame (`wire::decode` -> `vector<tlv_t>`) and rebuilds the shrunk/grown headers with
 * `std::vector`, so this run is EXPECTED to report a non-zero count now; the gate is
 * that count reaching 0 after Stage-2. A per-run threshold (env `ZEROHEAP_MAX`, default
 * off = report-only) lets CI flip it to a hard gate when Stage-2 lands.
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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "heap_probe.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

// --- the counting allocator override (all variants) --------------------------

namespace {
void* counted_alloc(std::size_t size) {
    if (probe::g_armed.load(std::memory_order_relaxed)) {
        probe::g_allocs.fetch_add(1, std::memory_order_relaxed);
        probe::g_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size ? size : 1);
    return p;
}
void counted_free(void* p) {
    if (p == nullptr) return;
    if (probe::g_armed.load(std::memory_order_relaxed))
        probe::g_frees.fetch_add(1, std::memory_order_relaxed);
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
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    counted_free(p);
}

// --- the forward-hop fixture -------------------------------------------------

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

// A transport that only records the bytes it is asked to send (no I/O, no alloc while
// armed beyond what the router hands it — the send itself is the measured egress).
struct capture_transport_t : transport_t {
    std::size_t sends = 0;
    std::size_t last_len = 0;
    using transport_t::send;
    void send(std::span<const std::byte> f) override {
        ++sends;
        last_len = f.size();
    }
    void set_receiver(receiver_t) override {}
};

// Append a NAME-only PATH TLV over `segs`.
void emit_path(std::vector<std::byte>& out, std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::detail::emit_name(body, s);
    tr::detail::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
}

// Build FWD{ op=WRITE, dst=<dst...>, src=<src...>, VALUE payload } — the frame a
// forward hop shrinks (dst) and grows (src).
std::vector<std::byte> make_fwd(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src,
                                std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::detail::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    emit_path(body, src);
    tr::detail::emit_tlv(body, type_t::VALUE, opt_t{}, payload);
    std::vector<std::byte> frame;
    tr::detail::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
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

    const std::byte payload[4] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
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
    std::printf("RESULT zeroheap forward allocs=%zu frees=%zu bytes=%zu egress_len=%zu forwarded=%d\n",
                c.allocs, c.frees, c.bytes, out_link.last_len, forwarded ? 1 : 0);
    std::printf(
        "  (baseline: today's fwd_router full-decodes + rebuilds with std::vector, so allocs>0 is\n"
        "   EXPECTED pre-Stage-2; ADR-0038 gate = allocs==0 on the forward path after the flip.)\n");

    if (!forwarded) {
        std::printf("FAIL: the frame did not forward — fixture broken, not a heap result\n");
        return 2;
    }

    // Optional hard gate: `ZEROHEAP_MAX=N` fails the run if allocs>N. Default: report-only
    // (Stage-1 baseline). CI flips this to `ZEROHEAP_MAX=0` when Stage-2 lands.
    if (const char* cap = std::getenv("ZEROHEAP_MAX")) {
        const auto max_allocs = static_cast<std::size_t>(std::strtoul(cap, nullptr, 10));
        if (c.allocs > max_allocs) {
            std::printf("ZEROHEAP: FAIL (allocs=%zu > max=%zu)\n", c.allocs, max_allocs);
            return 1;
        }
        std::printf("ZEROHEAP: PASS (allocs=%zu <= max=%zu)\n", c.allocs, max_allocs);
    }
    return 0;
}
