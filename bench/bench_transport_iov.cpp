/**
 * @file
 * @brief At what scatter-gather width does a REAL transport start allocating per frame?
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_forward_heap` hard-gates the forward hop at **zero** allocations, and that gate is
 * load-bearing (ADR-0038 inv. #2). But it drives a **stub** link: `capture_transport_t` only
 * sums the span sizes it is handed. The real transports assemble an `::iovec` table first, and
 * **both spill to the heap above a fixed inline width** —
 * `transport_udp.cpp` (`kMaxInlineIov = 16`) and `transport_tcp.cpp`
 * (`prefixed_iov_t::kMaxInlineIov = 16`, `+1` for the length prefix). Those allocations are
 * invisible to the `allocs=0` gate, because the stub never runs that code.
 *
 * So there is a per-frame heap allocation on the shipping forward path that no instrument can
 * see, waiting behind a span count nothing measures. This bench measures it.
 *
 * @section why Why it matters now
 *
 * Today the hop stays well under the ceiling: `kFwdMaxIov` is a structural **9** (counted from
 * `gather`'s emit sequence), and
 * `fwd_rebuild_t::gather` emits at most **9** regions for a contiguous source (since #508 the
 * whole mount run is ONE precomputed span, not one per segment). 9 < 17, so nothing allocates.
 *
 * Two live changes push on that headroom, which is why the ceiling wants an instrument rather
 * than a comment:
 *
 *  - The maintainer's 2026-07-30 ruling that the mount descent be "limited by MRU or not limited
 *    at all" removes `kMountPeekMax`. Any design that makes the region count grow with mount
 *    width — rather than keeping the mount one span — walks straight into this.
 *  - A **rope** source may split any region further, so a rope with many links can exceed the
 *    inline width on a frame whose contiguous twin would not.
 *
 * @section what What this reports
 *
 * A width sweep through the REAL transports over loopback, counting allocations on the SENDING
 * thread only (the counter is `thread_local`, so the receive thread's own buffers cannot be
 * mistaken for send-path allocations). It prints the exact width at which the heap appears and
 * checks the forward hop's own worst case against it.
 *
 * It is a **measurement**, not a pass/fail gate: spilling above 16 spans is the documented,
 * deliberate design ("the iovec count is small and bounded … only an unusually large gather
 * falls back to the heap vector"). What was missing is any way to know where the boundary sits
 * and how much headroom the forward hop actually has.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <thread>
#include <vector>

#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"

namespace {

/**
 * @brief Allocation counters for the CALLING thread only.
 *
 * `thread_local` rather than atomic-global on purpose: both transports run a receive thread
 * that blocks in `recvfrom`/`recv` with a timeout, and a global counter would attribute that
 * thread's buffers to the send under test. Isolating by thread is what makes a per-send count
 * meaningful at all.
 */
thread_local std::size_t g_allocs = 0;
thread_local std::size_t g_bytes = 0;
thread_local bool g_counting = false;

void* counted_alloc(std::size_t size) {
    if (g_counting) {
        ++g_allocs;
        g_bytes += size;
    }
    return std::malloc(size == 0 ? 1 : size);
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
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

/** @brief Widest gather this sweep drives — comfortably past both transports' inline arrays. */
constexpr std::size_t kMaxWidth = 24;

/** @brief The forward hop's own compile-time region ceiling, for the headroom check. */
constexpr std::size_t kFwdCeiling = tr::net::kFwdMaxIov;

/** @brief One measured width. */
struct row_t {
    std::size_t width = 0;
    std::size_t allocs = 0;
    std::size_t bytes = 0;
};

/**
 * @brief Send @p width spans through @p link and return what the send allocated.
 *
 * The payload storage is built ONCE by the caller and reused, so nothing here attributes the
 * fixture's own vector growth to the transport.
 */
template <class Link>
[[nodiscard]] row_t measure(Link& link, const std::vector<std::vector<std::byte>>& store,
                            std::size_t width) {
    std::vector<std::span<const std::byte>> iov;
    iov.reserve(width);
    for (std::size_t i = 0; i < width; ++i) iov.emplace_back(store[i]);

    // Warm once OUTSIDE the counted window: the first send through a fresh link may touch
    // lazily-built state, and that is a one-off, not a per-frame cost.
    link.send(std::span<const std::span<const std::byte>>(iov));

    g_allocs = 0;
    g_bytes = 0;
    g_counting = true;
    link.send(std::span<const std::span<const std::byte>>(iov));
    g_counting = false;

    return row_t{width, g_allocs, g_bytes};
}

/** @brief Print a sweep and return the first width that allocated (0 ⇒ none did). */
[[nodiscard]] std::size_t report(const char* what, const std::vector<row_t>& rows) {
    std::printf("\n%s\n", what);
    std::printf("  %-8s %-8s %s\n", "spans", "allocs", "bytes");
    std::size_t first_spill = 0;
    for (const row_t& r : rows) {
        std::printf("  %-8zu %-8zu %zu%s\n", r.width, r.allocs, r.bytes,
                    r.allocs > 0 && first_spill == 0 ? "   <- first heap allocation" : "");
        if (r.allocs > 0 && first_spill == 0) first_spill = r.width;
    }
    if (first_spill == 0) {
        std::printf("  no allocation at any width up to %zu\n", kMaxWidth);
    }
    return first_spill;
}

}  // namespace

int main() {
    // Payload spans: tiny and fixed, so the only thing that varies across the sweep is the
    // NUMBER of regions — which is the variable under test. A datagram of 24 x 8 B stays far
    // inside every MTU, so nothing is rejected for size and no arm measures a drop.
    std::vector<std::vector<std::byte>> store(kMaxWidth,
                                              std::vector<std::byte>(8, std::byte{0xA5}));

    std::vector<row_t> udp_rows;
    std::vector<row_t> tcp_rows;

    {
        // Peer points at a bound sibling so the datagram is deliverable and `send` takes its
        // real path — a udp_transport_t with no peer returns early and would measure nothing.
        // Both bind EPHEMERALLY (#1362): the 47xxx literals these used to name are inside the
        // kernel's own ip_local_port_range, so a fixed number is not a reservation — another
        // socket can already own it and the bind fails, which on a bench reads as a mystery
        // zero. `b` is constructed first so `a` can name the port the OS actually granted.
        tr::net::udp_transport_t b(0, "", 0);
        tr::net::udp_transport_t a(0, "127.0.0.1", b.local_port());
        (void)b;
        for (std::size_t w = 1; w <= kMaxWidth; ++w) udp_rows.push_back(measure(a, store, w));
    }

    {
        // The listen-role ctor takes a bind port; the dial-role one takes host+port. The accept
        // runs on the server's own thread, so the client's first write can race it — a short
        // settle keeps this measuring the gather rather than a connect retry.
        tr::net::tcp_transport_t server(std::uint16_t{0});  // ephemeral bind (#1362)
        tr::net::tcp_transport_t client("127.0.0.1", server.local_port());
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        for (std::size_t w = 1; w <= kMaxWidth; ++w) tcp_rows.push_back(measure(client, store, w));
    }

    const std::size_t udp_spill =
        report("udp_transport_t::send(iov) — real sendmsg gather", udp_rows);
    const std::size_t tcp_spill =
        report("tcp_transport_t::send(iov) — real prefixed_iov_t gather", tcp_rows);

    std::printf(
        "\nSUMMARY the shipping transports assemble an iovec table before the syscall and fall\n"
        "        back to the heap above a fixed inline width. bench_forward_heap cannot see\n"
        "        this: its capture_transport_t stub never runs the assembly.\n");
    std::printf("        udp first heap allocation at %zu spans\n", udp_spill);
    std::printf("        tcp first heap allocation at %zu spans\n", tcp_spill);

    const std::size_t spill = std::min(udp_spill == 0 ? kMaxWidth + 1 : udp_spill,
                                       tcp_spill == 0 ? kMaxWidth + 1 : tcp_spill);
    std::printf(
        "\nHEADROOM forward hop ceiling kFwdMaxIov = %zu regions; the narrower transport spills\n"
        "         at %zu. Headroom = %zu regions.\n",
        kFwdCeiling, spill, spill > kFwdCeiling ? spill - kFwdCeiling : 0);
    if (spill <= kFwdCeiling) {
        std::printf(
            "         WARNING the forward hop's own ceiling REACHES the spill width, so a\n"
            "         worst-case frame allocates on the real transport while the zero-heap\n"
            "         gate still reports allocs=0.\n");
        return 1;
    }
    std::printf(
        "         A rope source may split any region further, so this headroom is the budget a\n"
        "         rope-heavy frame spends -- it is NOT a guarantee.\n");
    return 0;
}
