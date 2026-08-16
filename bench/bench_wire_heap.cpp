/**
 * @file
 * @brief How many heap allocations does ONE frame cost on the REAL wire — egress AND ingress?
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_forward_heap` hard-gates the forward hop at `allocs=0`, and its own file header says
 * that number "says nothing about the real wire": it drives `capture_transport_t`, a STUB link
 * that only sums the span sizes it is handed. `bench_transport_iov` closed half the gap — it
 * found the `::iovec` spill width on the EGRESS side — but it counts only the sending thread
 * and only the gather, never the receive loop. Nothing in the tree has ever counted what one
 * frame costs coming OFF the wire.
 *
 * This bench drives REAL sockets (loopback TCP/UDP/WS), with a counting global `operator new`
 * whose counters are **`thread_local`**, so the send side and the transport's own receive
 * thread are attributed separately. Egress is bracketed around N `send()` calls on the calling
 * thread; ingress is read INSIDE the delivery sink, which runs on the receive thread — the
 * delta across N deliveries is what the receive loop actually spent per frame.
 *
 * @section axes What is swept
 *
 *  - payload size (64 / 512 / 4096 / 65536 B) at one span — does the count track size?
 *  - span count (1..24) at 8 B/span — where does the `::iovec` table first hit the heap?
 *  - sink tier (owning rope sink vs borrowed span sink) — the receive-strategy branch.
 *  - RX backend (default `heap_backend` vs an injected `pool_t`) — the ADR-0042 §2 seam.
 *
 * @section discipline Measurement discipline
 *
 * Allocation counts are integers, not timings, but they are still reported as MEDIANS over
 * n >= 9 repetitions with min/max, and every arm within a repetition runs interleaved
 * (round-robin per rep, never all-of-one-then-all-of-the-other). A cell whose min == max is
 * deterministic; a cell whose range straddles another arm's range gets NO CLAIM.
 *
 * Control arms that must NOT move: an idle window on the measuring thread (0), and a
 * `stub_link_t` send that reproduces `bench_forward_heap`'s stub (0 at every width).
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/loopback.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"

namespace {

/**
 * @brief Per-thread allocation counters.
 *
 * `thread_local`, not a global atomic: every transport here runs its own receive thread
 * concurrently with the send under test, and a shared counter would blend the two. Arming is
 * global (one atomic flag) but accumulation is per-thread, so each side reads only its own.
 */
thread_local std::size_t t_allocs = 0;
thread_local std::size_t t_bytes = 0;
std::atomic<bool> g_armed{false};

void* counted_alloc(std::size_t size) noexcept {
    if (g_armed.load(std::memory_order_relaxed)) {
        ++t_allocs;
        t_bytes += size;
    }
    return std::malloc(size == 0 ? 1 : size);
}

}  // namespace

void* operator new(std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    void* p = counted_alloc(size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new(std::size_t size, std::align_val_t) { return operator new(size); }
void* operator new[](std::size_t size, std::align_val_t) { return operator new(size); }
void* operator new(std::size_t size, std::align_val_t, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new[](std::size_t size, std::align_val_t, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
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

namespace {

/** @brief How many frames each measured window drives. */
constexpr std::size_t kFrames = 200;
/** @brief Widest gather swept — past both transports' 16-entry inline arrays. */
constexpr std::size_t kMaxWidth = 24;

/** @brief One arm's per-frame result for one repetition. */
struct cell_t {
    double allocs = 0.0; /**< @brief Allocations per frame. */
    double bytes = 0.0;  /**< @brief Bytes requested per frame. */
};

/** @brief Accumulates every repetition's cells, keyed by arm label. */
std::map<std::string, std::vector<cell_t>> g_results;

void record(const std::string& key, double allocs, double bytes) {
    g_results[key].push_back(cell_t{allocs, bytes});
}

/** @brief The stub link `bench_forward_heap` drives — a control that must stay at 0. */
class stub_link_t final : public tr::net::transport_t {
   public:
    void send(std::span<const std::byte> frame) override { total_ += frame.size(); }
    void send(std::span<const std::span<const std::byte>> iov) override {
        for (const std::span<const std::byte>& s : iov) total_ += s.size();
    }

   private:
    std::size_t total_ = 0;
};

/**
 * @brief The receive-side probe: latches the RECEIVE thread's own counters at two exact frames.
 *
 * Both sinks are plain functions taking this as `ctx`, so the sink itself allocates nothing.
 * A "latest delivery" publish would race the reader (the window can overshoot by a frame or
 * two, which shows up as a 1.99-instead-of-2.00 artifact), so the window's two endpoints are
 * named by FRAME INDEX up front and the sink latches exactly those two — the divisor is then
 * exactly `mark_b - mark_a`, not a sampled frame count.
 */
struct rx_probe_t {
    std::atomic<std::size_t> frames{0};
    std::atomic<std::size_t> mark_a{~std::size_t{0}};
    std::atomic<std::size_t> mark_b{~std::size_t{0}};
    std::atomic<std::size_t> a_allocs{0};
    std::atomic<std::size_t> a_bytes{0};
    std::atomic<std::size_t> b_allocs{0};
    std::atomic<std::size_t> b_bytes{0};

    void latch(std::size_t idx) {
        if (idx == mark_a.load(std::memory_order_relaxed)) {
            a_allocs.store(t_allocs, std::memory_order_relaxed);
            a_bytes.store(t_bytes, std::memory_order_relaxed);
        } else if (idx == mark_b.load(std::memory_order_relaxed)) {
            b_allocs.store(t_allocs, std::memory_order_relaxed);
            b_bytes.store(t_bytes, std::memory_order_relaxed);
        }
    }

    static void on_rope(void* ctx, tr::view::rope_t frame) {
        auto* self = static_cast<rx_probe_t*>(ctx);
        (void)frame.total_length();
        const std::size_t idx = self->frames.fetch_add(1, std::memory_order_relaxed);
        self->latch(idx);
    }
    static void on_span(void* ctx, std::span<const std::byte> frame) {
        auto* self = static_cast<rx_probe_t*>(ctx);
        (void)frame.size();
        const std::size_t idx = self->frames.fetch_add(1, std::memory_order_relaxed);
        self->latch(idx);
    }
};

/** @brief Block until @p probe has delivered at least @p want frames (or the deadline passes). */
[[nodiscard]] bool wait_frames(const rx_probe_t& probe, std::size_t want, int ms = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (probe.frames.load(std::memory_order_acquire) < want) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

/** @brief Byte storage for the gather spans; built once and reused so nothing here is counted. */
struct payload_t {
    std::vector<std::vector<std::byte>> store;
    std::vector<std::span<const std::byte>> spans;

    payload_t(std::size_t width, std::size_t per_span) {
        store.assign(width, std::vector<std::byte>(per_span, std::byte{0xA5}));
        spans.reserve(width);
        for (std::size_t i = 0; i < width; ++i) spans.emplace_back(store[i]);
    }
    [[nodiscard]] std::span<const std::span<const std::byte>> iov() const { return spans; }
    [[nodiscard]] std::size_t total() const { return store.size() * store[0].size(); }
};

/**
 * @brief Drive @p link with @p kFrames gathered sends, counting BOTH sides of the wire.
 *
 * The egress count is the sending thread's own delta; the ingress count is the receive
 * thread's delta between the first and the last delivery of the window, so it covers exactly
 * `frames` frames' worth of receive-loop work.
 */
void run_pair(const char* arm, tr::net::transport_t& link, rx_probe_t& probe, const payload_t& pay,
              std::size_t frames, bool measure_ingress) {
    // Warm OUTSIDE the window: a fresh link's first send may touch lazily-built state, and
    // the receive loop's scratch buffer is a one-off, not a per-frame cost.
    for (std::size_t i = 0; i < 8; ++i) link.send(pay.iov());
    if (measure_ingress && !wait_frames(probe, 8)) {
        std::printf("  WARN %s: warm-up frames did not arrive\n", arm);
        return;
    }

    // Name the window's two endpoints by FRAME INDEX before arming, so the divisor is exact.
    const std::size_t seen = probe.frames.load(std::memory_order_relaxed);
    const std::size_t idx_a = seen + 4;  // a few frames of slack past whatever is in flight
    const std::size_t idx_b = idx_a + frames;
    probe.mark_a.store(idx_a, std::memory_order_relaxed);
    probe.mark_b.store(idx_b, std::memory_order_relaxed);

    t_allocs = 0;
    t_bytes = 0;
    g_armed.store(true, std::memory_order_seq_cst);
    for (std::size_t i = 0; i < frames + 8; ++i) link.send(pay.iov());
    const std::size_t tx_a = t_allocs;
    const std::size_t tx_b = t_bytes;

    bool got = true;
    if (measure_ingress) got = wait_frames(probe, idx_b + 1);
    g_armed.store(false, std::memory_order_seq_cst);

    const double tx_n = static_cast<double>(frames + 8);
    record(std::string(arm) + "|egress", static_cast<double>(tx_a) / tx_n,
           static_cast<double>(tx_b) / tx_n);

    if (!measure_ingress) return;
    if (!got) {
        std::printf("  WARN %s: window did not complete (%zu of %zu)\n", arm,
                    probe.frames.load(std::memory_order_relaxed), idx_b + 1);
        return;
    }
    const double n = static_cast<double>(frames);
    const std::size_t da = probe.b_allocs.load(std::memory_order_relaxed) -
                           probe.a_allocs.load(std::memory_order_relaxed);
    const std::size_t db = probe.b_bytes.load(std::memory_order_relaxed) -
                           probe.a_bytes.load(std::memory_order_relaxed);
    record(std::string(arm) + "|ingress", static_cast<double>(da) / n, static_cast<double>(db) / n);
}

/** @brief Egress-only window (no receiver installed / no ingress of interest). */
void run_egress(const char* arm, tr::net::transport_t& link, const payload_t& pay,
                std::size_t frames) {
    for (std::size_t i = 0; i < 8; ++i) link.send(pay.iov());
    t_allocs = 0;
    t_bytes = 0;
    g_armed.store(true, std::memory_order_seq_cst);
    for (std::size_t i = 0; i < frames; ++i) link.send(pay.iov());
    g_armed.store(false, std::memory_order_seq_cst);
    record(std::string(arm) + "|egress",
           static_cast<double>(t_allocs) / static_cast<double>(frames),
           static_cast<double>(t_bytes) / static_cast<double>(frames));
}

/** @brief The single-span `send(span)` overload (the non-gathered API). */
void run_egress_flat(const char* arm, tr::net::transport_t& link, const payload_t& pay,
                     std::size_t frames) {
    const std::span<const std::byte> one = pay.spans[0];
    for (std::size_t i = 0; i < 8; ++i) link.send(one);
    t_allocs = 0;
    t_bytes = 0;
    g_armed.store(true, std::memory_order_seq_cst);
    for (std::size_t i = 0; i < frames; ++i) link.send(one);
    g_armed.store(false, std::memory_order_seq_cst);
    record(std::string(arm) + "|egress",
           static_cast<double>(t_allocs) / static_cast<double>(frames),
           static_cast<double>(t_bytes) / static_cast<double>(frames));
}

// ---------------------------------------------------------------------------
// The arms
// ---------------------------------------------------------------------------

/** @brief The four payload sizes the maintainer's question names. */
constexpr std::size_t kSizes[] = {64, 512, 4096, 65536};

void arm_control() {
    // An idle window on the measuring thread: must be exactly zero.
    t_allocs = 0;
    t_bytes = 0;
    g_armed.store(true, std::memory_order_seq_cst);
    volatile int sink = 0;
    for (int i = 0; i < 1000; ++i) sink = sink + 1;
    (void)sink;
    g_armed.store(false, std::memory_order_seq_cst);
    record("CONTROL idle-window", static_cast<double>(t_allocs), static_cast<double>(t_bytes));

    // The stub link bench_forward_heap drives: zero at every width, by construction.
    stub_link_t stub;
    const payload_t wide(kMaxWidth, 8);
    run_egress("CONTROL stub-link w24", stub, wide, kFrames);
    const payload_t big(1, 65536);
    run_egress("CONTROL stub-link 64KiB", stub, big, kFrames);
}

void arm_tcp_sizes() {
    tr::net::tcp_transport_t server(0);
    if (!server.ok()) return;
    rx_probe_t probe;
    server.set_rope_receiver(&rx_probe_t::on_rope, &probe);
    tr::net::tcp_transport_t client("127.0.0.1", server.local_port());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    for (std::size_t sz : kSizes) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "tcp rope-sink %6zuB x1span", sz);
        run_pair(arm, client, probe, pay, kFrames, true);
    }
}

void arm_tcp_span_sink() {
    tr::net::tcp_transport_t server(0);
    if (!server.ok()) return;
    rx_probe_t probe;
    server.set_receiver(&rx_probe_t::on_span, &probe);
    tr::net::tcp_transport_t client("127.0.0.1", server.local_port());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    for (std::size_t sz : kSizes) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "tcp span-sink %6zuB x1span", sz);
        run_pair(arm, client, probe, pay, kFrames, true);
    }
}

void arm_tcp_pool_backend() {
    // ADR-0042 §2's injected RX seam: a bounded pool instead of the default heap. The slab is
    // caller-owned and outlives the transport, so nothing here is a per-frame allocation.
    static std::vector<std::byte> slab(64u * 1024u * 128u);
    tr::mem::pool_t pool(slab, 8192);
    tr::net::tcp_transport_t server(0, &pool);
    if (!server.ok()) return;
    rx_probe_t probe;
    server.set_rope_receiver(&rx_probe_t::on_rope, &probe);
    tr::net::tcp_transport_t client("127.0.0.1", server.local_port());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    for (std::size_t sz : {std::size_t{64}, std::size_t{512}, std::size_t{4096}}) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "tcp POOL-rx  %6zuB x1span", sz);
        run_pair(arm, client, probe, pay, kFrames, true);
    }
}

void arm_tcp_widths() {
    tr::net::tcp_transport_t server(0);
    if (!server.ok()) return;
    rx_probe_t probe;
    server.set_rope_receiver(&rx_probe_t::on_rope, &probe);
    tr::net::tcp_transport_t client("127.0.0.1", server.local_port());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    for (std::size_t w = 1; w <= kMaxWidth; ++w) {
        const payload_t pay(w, 8);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "tcp width %02zu spans x8B", w);
        run_pair(arm, client, probe, pay, kFrames, true);
    }
}

void arm_udp_sizes() {
    tr::net::udp_transport_t server(0, "127.0.0.1", 1);
    if (!server.ok()) return;
    rx_probe_t probe;
    server.set_rope_receiver(&rx_probe_t::on_rope, &probe);
    tr::net::udp_transport_t client(0, "127.0.0.1", server.local_port());
    if (!client.ok()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // 65536 exceeds the IPv4 datagram limit, so the top size here is a deliverable 60000.
    for (std::size_t sz :
         {std::size_t{64}, std::size_t{512}, std::size_t{4096}, std::size_t{60000}}) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "udp rope-sink %6zuB x1span", sz);
        run_pair(arm, client, probe, pay, kFrames, true);
    }
}

void arm_udp_span_sink() {
    tr::net::udp_transport_t server(0, "127.0.0.1", 1);
    if (!server.ok()) return;
    rx_probe_t probe;
    server.set_receiver(&rx_probe_t::on_span, &probe);
    tr::net::udp_transport_t client(0, "127.0.0.1", server.local_port());
    if (!client.ok()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (std::size_t sz : {std::size_t{64}, std::size_t{4096}}) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "udp span-sink %6zuB x1span", sz);
        run_pair(arm, client, probe, pay, kFrames, true);
    }
}

void arm_udp_widths() {
    tr::net::udp_transport_t server(0, "127.0.0.1", 1);
    if (!server.ok()) return;
    rx_probe_t probe;
    server.set_rope_receiver(&rx_probe_t::on_rope, &probe);
    tr::net::udp_transport_t client(0, "127.0.0.1", server.local_port());
    if (!client.ok()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (std::size_t w = 1; w <= kMaxWidth; ++w) {
        const payload_t pay(w, 8);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "udp width %02zu spans x8B", w);
        run_pair(arm, client, probe, pay, kFrames, true);
    }
}

void arm_tcp_server_widths() {
    // The MULTI-peer server: its broadcast builds a pristine record AND a per-write scratch
    // copy, so it has two independent spill points on one send.
    tr::net::transport_tcp_server server(0);
    if (!server.ok()) return;
    rx_probe_t probe;
    server.set_rope_receiver(&rx_probe_t::on_rope, &probe);
    tr::net::tcp_transport_t client("127.0.0.1", server.local_port());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    for (std::size_t w : {std::size_t{1}, std::size_t{8}, std::size_t{16}, std::size_t{17},
                          std::size_t{18}, std::size_t{24}}) {
        const payload_t pay(w, 8);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "tcp-server bcast w%02zu", w);
        run_egress(arm, server, pay, kFrames);
    }
    for (std::size_t sz : kSizes) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "tcp-server bcast %6zuB flat", sz);
        run_egress_flat(arm, server, pay, kFrames);
    }
    // The DIRECTED facade `peer_link` hands out — what a FWD forward to a named bus peer
    // actually sends through (child_registry_t's fallback), and the path with no pristine
    // copy. A different spill cost from the broadcast on the SAME transport.
    std::string peer;
    server.enumerate_peers([&peer](std::string_view p) { peer = std::string(p); });
    tr::net::transport_t* directed = peer.empty() ? nullptr : server.peer_link(peer);
    if (directed == nullptr) return;
    for (std::size_t w : {std::size_t{1}, std::size_t{16}, std::size_t{17}, std::size_t{24}}) {
        const payload_t pay(w, 8);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "tcp-server peer_link w%02zu", w);
        run_egress(arm, *directed, pay, kFrames);
    }
    for (std::size_t sz : kSizes) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "tcp-server peer_link %6zuB", sz);
        run_egress(arm, *directed, pay, kFrames);
    }
}

void arm_ws() {
    tr::net::transport_ws_server server(0);
    if (!server.ok()) return;
    rx_probe_t srv_probe;
    server.set_rope_receiver(&rx_probe_t::on_rope, &srv_probe);
    tr::net::transport_ws_client client("127.0.0.1", server.local_port());
    if (!client.ok()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // Client -> server: the MASKED encode (a whole-payload copy) plus, for the gathered
    // overload, transport_t's default flatten (the client overrides only send(span)).
    for (std::size_t sz : kSizes) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "ws client %6zuB flat", sz);
        run_egress_flat(arm, client, pay, kFrames);
        std::snprintf(arm, sizeof(arm), "ws client %6zuB x1span-iov", sz);
        run_pair(arm, client, srv_probe, pay, kFrames, true);
    }

    // Server -> client: the gathered (unmasked) path vs the flat one.
    rx_probe_t cli_probe;
    client.set_rope_receiver(&rx_probe_t::on_rope, &cli_probe);
    for (std::size_t sz : kSizes) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "ws server %6zuB flat", sz);
        run_egress_flat(arm, server, pay, kFrames);
        std::snprintf(arm, sizeof(arm), "ws server %6zuB x1span-iov", sz);
        run_pair(arm, server, cli_probe, pay, kFrames, true);
    }
    for (std::size_t w : {std::size_t{1}, std::size_t{16}, std::size_t{17}, std::size_t{24}}) {
        const payload_t pay(w, 8);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "ws server width %02zu", w);
        run_egress(arm, server, pay, kFrames);
    }
}

void arm_loopback() {
    tr::net::loopback_channel_t ch;
    rx_probe_t probe;
    ch.b().set_receiver(&rx_probe_t::on_span, &probe);
    for (std::size_t sz : {std::size_t{64}, std::size_t{4096}}) {
        const payload_t pay(1, sz);
        char arm[96];
        std::snprintf(arm, sizeof(arm), "loopback %6zuB flat", sz);
        run_egress_flat(arm, ch.a(), pay, kFrames);
    }
}

// ---------------------------------------------------------------------------
// The end-to-end arm: a REAL socket feeding a REAL fwd_router_t
// ---------------------------------------------------------------------------

/** @brief Append a NAME-only PATH TLV over @p segs. */
void emit_path(std::vector<std::byte>& out, std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{}, body);
}

/** @brief `FWD{ op=WRITE, dst, src, VALUE }` — the frame a forward hop shrinks and grows. */
[[nodiscard]] std::vector<std::byte> make_fwd(std::initializer_list<std::string_view> dst,
                                              std::initializer_list<std::string_view> src,
                                              std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    emit_path(body, src);
    tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{}, payload);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, tr::wire::type_t::FWD, tr::wire::opt_t{.pl = true}, body);
    return frame;
}

/**
 * @brief The router's EGRESS link, tapped: latches the forwarding thread's counters either
 *        side of the real send, so one window separates three terms on ONE thread.
 *
 * The forward hop runs on the inbound transport's receive thread, so the receive loop's
 * segment allocation, the router's own work, and the egress transport's gather all land in
 * the same `thread_local` counter. Latching before and after the inner send splits them:
 * `pre[k+1] - pre[k]` is the whole per-frame cost, `post[k] - pre[k]` the egress term alone.
 */
class tap_link_t final : public tr::net::transport_t {
   public:
    explicit tap_link_t(tr::net::transport_t* inner) : inner_(inner) {}

    void send(std::span<const std::byte> frame) override {
        const std::size_t before = t_allocs;
        const std::size_t before_b = t_bytes;
        if (inner_ != nullptr) inner_->send(frame);
        account(before, before_b);
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        const std::size_t before = t_allocs;
        const std::size_t before_b = t_bytes;
        spans_.store(iov.size(), std::memory_order_relaxed);
        if (inner_ != nullptr) inner_->send(iov);
        account(before, before_b);
    }

    std::atomic<std::size_t> frames{0};
    std::atomic<std::size_t> mark_a{~std::size_t{0}};
    std::atomic<std::size_t> mark_b{~std::size_t{0}};
    std::atomic<std::size_t> a_allocs{0};
    std::atomic<std::size_t> a_bytes{0};
    std::atomic<std::size_t> b_allocs{0};
    std::atomic<std::size_t> b_bytes{0};
    std::atomic<std::size_t> egress_allocs{0};
    std::atomic<std::size_t> egress_bytes{0};
    std::atomic<std::size_t> spans_{0};

   private:
    void account(std::size_t before, std::size_t before_b) {
        const std::size_t idx = frames.fetch_add(1, std::memory_order_relaxed);
        const std::size_t ma = mark_a.load(std::memory_order_relaxed);
        const std::size_t mb = mark_b.load(std::memory_order_relaxed);
        if (idx > ma && idx <= mb) {
            egress_allocs.fetch_add(t_allocs - before, std::memory_order_relaxed);
            egress_bytes.fetch_add(t_bytes - before_b, std::memory_order_relaxed);
        }
        if (idx == ma) {
            a_allocs.store(before, std::memory_order_relaxed);
            a_bytes.store(before_b, std::memory_order_relaxed);
        } else if (idx == mb) {
            b_allocs.store(before, std::memory_order_relaxed);
            b_bytes.store(before_b, std::memory_order_relaxed);
        }
    }

    tr::net::transport_t* inner_;
};

/**
 * @brief One FWD forward hop, end to end on a real wire: TCP in, @p out_kind out.
 *
 * This is the composition the maintainer's latency argument rests on and that no existing
 * instrument covers: `bench_forward_heap` gates the router alone against a stub link, and
 * `bench_transport_iov` counts the gather alone on the sender's thread. Here the receive
 * loop, the router and the egress transport all run on ONE thread, and one counter sees all
 * three.
 */
void arm_router_over_wire(const char* label, bool real_egress) {
    tr::graph::graph_t graph;
    tr::net::fwd_router_t router(graph);

    tr::net::tcp_transport_t in_link(0);
    if (!in_link.ok()) return;

    // The egress leg: either a real second TCP socket (full wire-to-wire) or the stub
    // `bench_forward_heap` drives, so the difference between the two arms IS the transport's
    // egress term, measured rather than added.
    tr::net::tcp_transport_t egress_peer(0);
    stub_link_t stub;
    tr::net::tcp_transport_t egress_dial("127.0.0.1", egress_peer.local_port());
    tr::net::transport_t* inner =
        real_egress ? static_cast<tr::net::transport_t*>(&egress_dial) : &stub;
    tap_link_t tap(inner);

    router.add_child("net/ws-client/out", tap);
    router.add_child("net/ws-server/in", in_link);

    tr::net::tcp_transport_t client("127.0.0.1", in_link.local_port());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const std::byte payload[4] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                  std::byte{0xEF}};
    const std::vector<std::byte> frame =
        make_fwd({"net", "ws-client", "out", "sensor", "temp"}, {"reply"},
                 std::span<const std::byte>(payload, 4));

    for (std::size_t i = 0; i < 16; ++i) client.send(std::span<const std::byte>(frame));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (tap.frames.load(std::memory_order_relaxed) < 16) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::printf("  WARN %s: no frames forwarded — wiring failed\n", label);
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    const std::size_t seen = tap.frames.load(std::memory_order_relaxed);
    tap.egress_allocs.store(0, std::memory_order_relaxed);
    tap.egress_bytes.store(0, std::memory_order_relaxed);
    tap.mark_a.store(seen + 4, std::memory_order_relaxed);
    tap.mark_b.store(seen + 4 + kFrames, std::memory_order_relaxed);

    g_armed.store(true, std::memory_order_seq_cst);
    for (std::size_t i = 0; i < kFrames + 16; ++i) client.send(std::span<const std::byte>(frame));
    const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    bool got = true;
    while (tap.frames.load(std::memory_order_relaxed) <= tap.mark_b.load()) {
        if (std::chrono::steady_clock::now() > d2) {
            got = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    g_armed.store(false, std::memory_order_seq_cst);
    if (!got) {
        std::printf("  WARN %s: window did not complete\n", label);
        return;
    }

    const double n = static_cast<double>(kFrames);
    const std::size_t total = tap.b_allocs.load() - tap.a_allocs.load();
    const std::size_t total_b = tap.b_bytes.load() - tap.a_bytes.load();
    const std::size_t eg = tap.egress_allocs.load();
    const std::size_t eg_b = tap.egress_bytes.load();
    record(std::string(label) + " TOTAL/frame", static_cast<double>(total) / n,
           static_cast<double>(total_b) / n);
    record(std::string(label) + " egress-leg", static_cast<double>(eg) / n,
           static_cast<double>(eg_b) / n);
    record(std::string(label) + " rx+router", static_cast<double>(total - eg) / n,
           static_cast<double>(total_b - eg_b) / n);
    record(std::string(label) + " fwd spans", static_cast<double>(tap.spans_.load()), 0.0);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

[[nodiscard]] double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void report() {
    std::printf("\n%-34s %6s %9s %9s %9s %12s\n", "arm", "n", "med", "min", "max", "med bytes");
    std::printf("%s\n", std::string(88, '-').c_str());
    for (const auto& [key, cells] : g_results) {
        std::vector<double> a;
        std::vector<double> b;
        a.reserve(cells.size());
        b.reserve(cells.size());
        for (const cell_t& c : cells) {
            a.push_back(c.allocs);
            b.push_back(c.bytes);
        }
        const auto [lo, hi] = std::minmax_element(a.begin(), a.end());
        std::printf("%-34s %6zu %9.3f %9.3f %9.3f %12.1f\n", key.c_str(), cells.size(), median(a),
                    *lo, *hi, median(b));
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int reps = argc > 1 ? std::atoi(argv[1]) : 9;
    std::printf("bench_wire_heap: per-frame heap allocations on the REAL wire\n");
    std::printf("  frames/window=%zu  reps=%d  (arms interleaved round-robin per rep)\n", kFrames,
                reps);
    for (int r = 0; r < reps; ++r) {
        arm_control();
        arm_tcp_sizes();
        arm_udp_sizes();
        arm_ws();
        arm_tcp_span_sink();
        arm_udp_span_sink();
        arm_tcp_widths();
        arm_udp_widths();
        arm_tcp_server_widths();
        arm_tcp_pool_backend();
        arm_loopback();
        arm_router_over_wire("ROUTER wire->stub ", false);
        arm_router_over_wire("ROUTER wire->wire ", true);
        std::printf("  rep %d done\n", r + 1);
        std::fflush(stdout);
    }
    report();
    return 0;
}
