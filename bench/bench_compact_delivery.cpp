/**
 * @file
 * @brief The STEADY-STATE compacted delivery — what an established flow costs per frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Delivery compaction (RFC-0004 §E.1) exists to make an established flow cheap: the route is
 * advertised once, and every later sample rides as `COMPACT{label, payload}`. Nothing measured
 * what that steady state actually costs, so
 * [ADR-0062](../docs/adr/0062-resolve-once-label-bindings-hold-resolutions-not-names.md)'s premise
 * — that the label removed the wire cost but not the RESOLUTION cost — was argued from code reading
 * alone.
 *
 * `bench_forward_demux` times a FORWARD hop, which never resolves. `bench_terminus_tier` times a
 * COLD resolve, which an established flow pays once. Neither times the frame that dominates a
 * running system: the Nth COMPACT on a warm binding.
 *
 * Two modes, because a label means one of two things:
 *   - `compact-terminus` — the label resolves locally; the hop expands it and writes.
 *   - `compact-forward`  — the label swaps and re-emits downstream.
 *
 * Both are measured on a WARM binding (the advertise and first frames run before the window
 * opens), and allocations are counted around one frame — so the RAM axis is exact rather than
 * inferred. Batch-amortized and self-calibrating for the same reason the other benches are: one
 * delivery is close enough to `clock_gettime` that per-op timing measures the clock.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Payload sizes swept — the value each compacted sample carries. */
constexpr std::size_t kPayloadSizes[] = {4, 64, 512};

constexpr double kDefaultBudgetSeconds = 1.0;

[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

using bench::calibrate_batch;  // hoisted to bench_common.hpp (#553) — one definition

std::size_t g_allocs = 0;
std::size_t g_bytes = 0;
bool g_arm = false;

}  // namespace

// Every allocating form, not just the throwing one. libtracer's own heap backend allocates
// through `::operator new(bytes, std::nothrow)` (mem_heap.hpp), so overriding only
// `operator new(size)` made the payload-sized store copy INVISIBLE to this counter — the
// published figures undercounted. bench_forward_heap has always overridden the full set;
// this now matches it, so the two benches' allocation columns are comparable.
//
// And every DEALLOCATING form (#793), for a reason ASan reports as `alloc-dealloc-mismatch`:
// a replacement set with a hole leaves that one form to the sanitizer's own operator, which
// is then handed a pointer this file `malloc`ed. The sized+aligned `operator delete` is the
// hole libstdc++'s `std::pmr::memory_resource::deallocate` walks into on every pmr control
// block — inert today only because no sanitizer job runs this bench. Set completed from
// `core/tests/terminus_flatten_backend_test.cpp`, including — since #801 — its over-aligned
// `operator new`, so the two benches and the test now share ONE shape with no exception.
namespace {
void* counted(std::size_t n) {
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    return std::malloc(n == 0 ? 1 : n);
}

/**
 * @brief The aligned counted allocation (#801) — `aligned_alloc` for a genuinely OVER-aligned
 *        request, whose result `free` accepts; `malloc` for a fundamental one.
 *
 * Forwarding an over-aligned request to plain `malloc` (what this file did until #801) hands
 * back memory aligned only to `max_align_t`, so a `std::pmr` control block asking for 32-byte
 * alignment is under-aligned — real UB, latent only because nothing on today's measured path
 * asks for more. The `>` test is the same one `bench_forward_heap` documents at length: a
 * fundamental-alignment request is already correct on `malloc`, and keeping it there is what
 * leaves the published live-bytes figures where they are.
 */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);  // malloc already suits it
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    // aligned_alloc requires a size that is a multiple of the alignment.
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}
}  // namespace

void* operator new(std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    void* const p = counted_aligned(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

/** @brief A link that swallows what it is handed — no I/O, no allocation in the window. */
struct sink_link_t : tr::net::transport_t {
    std::size_t sends = 0;
    void send(std::span<const std::byte>) override { ++sends; }
    void send(std::span<const std::span<const std::byte>>) override { ++sends; }
};

std::vector<std::byte> path_tlv(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

std::vector<std::byte> value_tlv(std::size_t n) {
    const std::vector<std::byte> payload(n, std::byte{0xAB});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload));
    return out;
}

/**
 * @brief One measured point: N compacted deliveries on a WARM binding.
 * @param terminus true ⇒ the label resolves locally; false ⇒ it swaps and forwards.
 */
void run_point(std::size_t payload, bool terminus) {
    graph_t g;
    if (terminus) (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router(g);
    sink_link_t up;
    sink_link_t down;
    router.add_child("net/ws-client/up", up);
    router.add_child("net/ws-server/down", down);

    // A terminus route resolves locally; a forwarding route sits BELOW the down mount.
    const std::vector<std::byte> route =
        terminus ? path_tlv({"sink"}) : path_tlv({"net", "ws-server", "down", "far"});
    router.on_frame("net/ws-client/up", tr::net::encode_advertise(9, route));

    const std::vector<std::byte> frame = tr::net::encode_compact(9, value_tlv(payload));
    const auto deliver = [&] { router.on_frame("net/ws-client/up", frame); };

    // WARM the binding before anything is measured — the first frame resolves cold and
    // memoizes, and it is precisely the frame this bench is NOT about.
    for (int i = 0; i < 64; ++i) deliver();

    // RAM axis: allocations around exactly one warm delivery, counted outside the timed
    // window so the counter never distorts the latency number.
    g_allocs = 0;
    g_bytes = 0;
    g_arm = true;
    deliver();
    g_arm = false;
    const std::size_t allocs = g_allocs;
    const std::size_t bytes = g_bytes;

    const std::size_t batch = calibrate_batch(deliver);
    bench::Latency lat;
    const std::uint64_t t0 = bench::now_ns();
    const auto deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) deliver();
        lat.add((bench::now_ns() - a) / batch);
        ++batches;
        total = bench::now_ns() - t0;
    }

    const double ops = static_cast<double>(batches) * static_cast<double>(batch);
    const double ops_per_s = total == 0 ? 0.0 : ops * 1e9 / static_cast<double>(total);
    const bench::Latency::Summary s = lat.summarize();
    const char* const mode = terminus ? "compact-terminus" : "compact-forward";
    bench::emit("libtracer", mode, payload, 1, 1, ops_per_s, ops_per_s, 0.0, s);
    std::printf("NOTE mode=%s payload=%zu batch=%zu samples=%zu allocs=%zu bytes=%zu\n", mode,
                payload, batch, batches, allocs, bytes);
    if (terminus ? (down.sends != 0) : (down.sends == 0))
        std::printf("WARN mode=%s delivered to the WRONG leg\n", mode);
}

}  // namespace

int main() {
    std::printf("# Steady-state compacted delivery on a WARM binding (RFC-0004 §E.1 / ADR-0062)\n");
    for (const std::size_t p : kPayloadSizes) {
        run_point(p, /*terminus=*/true);
        run_point(p, /*terminus=*/false);
    }
    return 0;
}
