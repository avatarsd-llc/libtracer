/**
 * @file
 * @brief The value-path census behind RFC-0028: how many heap blocks, how many bytes of
 *        heap, and how many payload copies ONE publish costs at each stage of the path —
 *        local publish, LKV store, fan-out to K callback / K target subscribers, egress
 *        gather, and wire ingress — measured, now, on the POSIX build, against a minimal
 *        prototype of the RFC's core slice (one intrusive refcounted block per publish).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every row is a per-publish AVERAGE over a warm window (the first 200 publishes settle the
 * one-offs: the wide-fan-out thread-local buffer, the edge array, the vertex ext), so any
 * amortised one-off reads as 0.00x. The counter is the `bench_source_role_alloc` override
 * (every `operator new` variant, or half the blocks are invisible) plus a byte tally, and it
 * is a SEPARATE binary from every timing bench for the reason stated there: the override
 * costs a relaxed load per allocation, so a timing TU carrying it would bias the number
 * against the arm that allocates more. The `ns` column here is indicative only.
 *
 * "Copies" are counted where the library performs them BY CONSTRUCTION — a `memcpy` of the
 * payload into a block the library allocated — and are reported as bytes: the bytes-per-
 * publish column already contains them (a copy needs a block to land in), and the `copies`
 * column names how many whole-payload copies that is.
 *
 * Output, one tag per stage:
 *
 *     LEAN_PATH stage size K allocs_per_op bytes_per_op payload_copies ns_per_op
 *
 * Stages:
 *   local-cb       graph_t::write to a STORED_VALUE with K callback subscribers, the value
 *                  built OUTSIDE the window (a rope over a persistent segment) — isolates the
 *                  library's own per-publish cost from the producer's.
 *   local-target   the same write with K TARGET subscribers (subscribe(src, target)), each
 *                  target a distinct STORED_VALUE vertex.
 *   local-handler  the same write with K HANDLER targets (subscribe(src, target), each target a
 *                  HANDLER vertex with an `on_write`): since RFC-0028 slice 7 the handler reads
 *                  the published block by `const value_t&`, so the claim is the local-target
 *                  one — the source's one block, and no per-handler clone of its links.
 *   local-none     the local-cb write to a vertex declared `retention_t::NONE` (RFC-0028
 *                  §5.4, slice 6): the pure relay, whose callback subscribers are delivered a
 *                  `value_storage_t` on the writer's stack — the §6.6 claim is 0 blocks.
 *   producer-own   what the producer pays to hand the library `size` bytes it does not own:
 *                  `view::over_bytes` (one segment, one memcpy) — the local-publish
 *                  ownership copy of design doc zero-copy-and-flatten ①.
 *   egress-gather  `transport_t::send(iov)` through the BASE default a link inherits when it
 *                  does not override the scatter-gather entry: one block, one whole-frame
 *                  copy per frame (copy ⑨ of the same doc, host form).
 *   egress-queued  the tx_handoff_t queue a link drains from (RFC-0028 §4.7), as it runs
 *                  with a writer in flight: admit a RETAINED record (the fill copies a short
 *                  head, the slot keeps one reference to the value) and hand it to the writer.
 *                  The payload is not copied (RFC-0028 §6.9, slice 9).
 *   egress-queued-copy  the same queue with the pre-slice-9 fill: the whole frame copied into
 *                  the slot. The row slice 9 retires, kept for scale.
 *   ingress-copy   a FWD{WRITE} frame resolved through the terminus (view tier) whose value
 *                  TLV is BELOW the vertex's copy-or-share threshold (the build default,
 *                  `config_t::kShareThresholdBytes`): the ownership copy, landing inline in
 *                  the value's own block (RFC-0028 §5.1 / §5.3). The frame asks for a reply
 *                  (non-empty src), so the window also holds the reply's assembly.
 *   ingress-pin    the SAME arm — same vertex, same default threshold — at a size AT OR ABOVE
 *                  it: the value links the receive segment (ADR-0042 §3), a refcount share.
 *                  One arm, two rows: the size alone picks the row (RFC-0028 §6.5's gate).
 *                  Each frame arrives in its own receive block, minted outside the window the
 *                  way the transport mints it (`view::alloc_rx`, with the ingress-loan reserve
 *                  since slice 9), so the value header is placed IN the block.
 *   ingress-*-noack  the same two rows for a DELIVERY-shaped frame: empty src, no reply
 *                  (RFC-0028 §6.9's claim — the stored value's only record is the loan).
 *   proto-fanout   the RFC-0028 prototype: ONE block holding {refcount, length, bytes},
 *                  published to a slot and shared to K target slots by refcount, egress as
 *                  a one-entry iov. Allocations per publish are the claim; the ns are the
 *                  same host, same size, same K as `local-target`, for scale.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// --- the counting allocator override (all variants) ------------------------------------------

namespace {

std::atomic<long long> g_allocs{0};
std::atomic<long long> g_bytes{0};
std::atomic<bool> g_armed{false};

void* counted_alloc(std::size_t size) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (g_armed.load(std::memory_order_relaxed) && p != nullptr) {
        g_allocs.fetch_add(1, std::memory_order_relaxed);
        g_bytes.fetch_add(static_cast<long long>(size), std::memory_order_relaxed);
    }
    return p;
}

void counted_free(void* p) {
    if (p != nullptr) std::free(p);
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
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }

// --- harness ---------------------------------------------------------------------------------

#include <array>
#include <span>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tlv_view.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/tx_handoff.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::view::segment_ptr_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

constexpr std::size_t kWarm = 200;
constexpr std::size_t kOps = 1000;
constexpr std::size_t kSizes[] = {16, 4096, 65536};
constexpr std::size_t kFans[] = {1, 8, 32};

/** @brief One measured window: warm, arm, run, report. @p op is the publish under test. */
template <class Op>
void measure(const char* stage, std::size_t size, std::size_t k, std::size_t copies, Op&& op) {
    for (std::size_t i = 0; i < kWarm; ++i) op();
    g_allocs.store(0, std::memory_order_relaxed);
    g_bytes.store(0, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_seq_cst);
    const std::uint64_t t0 = bench::now_ns();
    for (std::size_t i = 0; i < kOps; ++i) op();
    const std::uint64_t t1 = bench::now_ns();
    g_armed.store(false, std::memory_order_seq_cst);
    std::printf("LEAN_PATH\t%s\t%zu\t%zu\t%.3f\t%.1f\t%zu\t%.0f\n", stage, size, k,
                static_cast<double>(g_allocs.load()) / kOps,
                static_cast<double>(g_bytes.load()) / kOps, copies,
                static_cast<double>(t1 - t0) / kOps);
    std::fflush(stdout);
}

/** @brief A rope over ONE persistent heap segment of @p size bytes: copying it is a refcount. */
struct value_t {
    segment_ptr_t seg;
    explicit value_t(std::size_t size) : seg(tr::view::heap_alloc(size)) {
        std::memset(seg->bytes.data(), 0xAB, size);
    }
    [[nodiscard]] rope_t make() const {
        rope_t r;
        r.append(view_t::over(seg));
        return r;
    }
};

void callback_sink(void* ctx, const tr::graph::value_t&) {
    static_cast<std::atomic<std::uint64_t>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

void run_local_cb(std::size_t size, std::size_t k) {
    graph_t g;
    const path_t src = *path_t::parse("/bench/src");
    const vertex_handle_t v = g.register_vertex(src, role_t::STORED_VALUE);
    std::atomic<std::uint64_t> recv{0};
    for (std::size_t i = 0; i < k; ++i) (void)g.subscribe(src, &callback_sink, &recv);
    const value_t fx{size};
    measure("local-cb", size, k, 0, [&] { (void)g.write(v, fx.make()); });
    if (recv.load() != (kWarm + kOps) * k) std::fprintf(stderr, "local-cb: fan-out not wired\n");
}

void run_local_none(std::size_t size, std::size_t k) {
    graph_t g;
    const path_t src = *path_t::parse("/bench/src");
    const vertex_handle_t v = g.register_vertex(src, role_t::STORED_VALUE);
    if (!g.set_policy(v, {.retention = tr::graph::retention_t::NONE}))
        std::fprintf(stderr, "local-none: NONE refused\n");
    std::atomic<std::uint64_t> recv{0};
    for (std::size_t i = 0; i < k; ++i) (void)g.subscribe(src, &callback_sink, &recv);
    const value_t fx{size};
    measure("local-none", size, k, 0, [&] { (void)g.write(v, fx.make()); });
    if (recv.load() != (kWarm + kOps) * k) std::fprintf(stderr, "local-none: fan-out not wired\n");
}

void run_local_target(std::size_t size, std::size_t k) {
    graph_t g;
    const path_t src = *path_t::parse("/bench/src");
    const vertex_handle_t v = g.register_vertex(src, role_t::STORED_VALUE);
    for (std::size_t i = 0; i < k; ++i) {
        const path_t t = *path_t::parse("/bench/t" + std::to_string(i));
        (void)g.register_vertex(t, role_t::STORED_VALUE);
        if (!g.subscribe(src, t)) std::fprintf(stderr, "local-target: subscribe failed\n");
    }
    const value_t fx{size};
    measure("local-target", size, k, 0, [&] { (void)g.write(v, fx.make()); });
    const auto d = g.delivery_drops();
    if (d.no_target + d.denied + d.out_of_memory + d.fan_out_truncated != 0)
        std::fprintf(stderr, "local-target: deliveries dropped\n");
}

/** @brief The HANDLER targets' `on_write`: counts deliveries into the `ctx` it was given. */
tr::graph::result_t<void> handler_sink(void* ctx, const tr::graph::value_t&,
                                       const tr::graph::write_ctx_t&) {
    static_cast<std::atomic<std::uint64_t>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    return {};
}

void run_local_handler(std::size_t size, std::size_t k) {
    graph_t g;
    const path_t src = *path_t::parse("/bench/src");
    const vertex_handle_t v = g.register_vertex(src, role_t::STORED_VALUE);
    std::atomic<std::uint64_t> recv{0};
    for (std::size_t i = 0; i < k; ++i) {
        const path_t t = *path_t::parse("/bench/h" + std::to_string(i));
        tr::graph::handlers_t h;
        h.on_write = {&handler_sink, &recv};
        (void)g.register_vertex(t, role_t::HANDLER, h);
        if (!g.subscribe(src, t)) std::fprintf(stderr, "local-handler: subscribe failed\n");
    }
    const value_t fx{size};
    measure("local-handler", size, k, 0, [&] { (void)g.write(v, fx.make()); });
    if (recv.load() != (kWarm + kOps) * k)
        std::fprintf(stderr, "local-handler: fan-out not wired\n");
}

void run_producer_own(std::size_t size) {
    const std::vector<std::byte> app(size, std::byte{0xCD});
    measure("producer-own", size, 0, 1, [&] {
        const auto v = tr::view::over_bytes(std::span<const std::byte>(app));
        if (!v) std::abort();
    });
}

/** @brief A link that overrides ONLY the contiguous entry, so `send(iov)` takes the base
 *         gather — the shape every transport without native scatter-gather has. */
struct gather_link_t final : tr::net::transport_t {
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    void send(std::span<const std::byte> frame) override {
        ++frames;
        bytes += frame.size();
    }
    using tr::net::transport_t::send;
};

void run_egress_gather(std::size_t size) {
    gather_link_t link;
    const value_t fx{size};
    const rope_t r = fx.make();
    std::array<std::span<const std::byte>, 2> iov{r.links()[0].bytes(),
                                                  std::span<const std::byte>{}};
    measure("egress-gather", size, 1, 1,
            [&] { link.send(std::span<const std::span<const std::byte>>(iov.data(), 1)); });
    if (link.bytes != (kWarm + kOps) * size) std::fprintf(stderr, "egress-gather: short send\n");
}

/** @brief The retained record the queue rows admit: the short head a delivery carries. */
constexpr std::size_t kQueuedHead = 24;

void run_egress_queued(std::size_t size, bool retained) {
    tr::net::tx_handoff_t q(4, tr::mem::heap_source());
    const tr::graph::value_ref_t v = tr::graph::value_ref_t::adopt(tr::graph::value_t::make_copy(
        std::vector<std::byte>(size, std::byte{0xAB}), tr::mem::heap_source()));
    const std::array<std::byte, kQueuedHead> head{};
    std::uint64_t bytes = 0;
    // A writer in flight: every admit below queues, and each op hands the writer one record.
    (void)q.admit([](tr::mem::block_array_t<std::byte>&) -> std::size_t { return 1; });
    const auto fill_head = [&](tr::mem::block_array_t<std::byte>& slot) -> std::size_t {
        if (!slot.reserve(head.size())) return 0;
        std::memcpy(slot.data(), head.data(), head.size());
        return head.size();
    };
    const auto fill_whole = [&](tr::mem::block_array_t<std::byte>& slot) -> std::size_t {
        const std::size_t n = head.size() + size;
        if (!slot.reserve(n)) return 0;
        std::memcpy(slot.data(), head.data(), head.size());
        std::memcpy(slot.data() + head.size(), v->links()[0].bytes().data(), size);
        return n;
    };
    measure(retained ? "egress-queued" : "egress-queued-copy", size, 1, retained ? 0 : 1, [&] {
        const auto a = retained ? q.admit(fill_head, v.get()) : q.admit(fill_whole);
        if (a != tr::net::tx_handoff_t::admit_t::QUEUED) std::fprintf(stderr, "queue refused\n");
        bytes += q.next().size();
    });
    while (q.next()) {
    }
    if (bytes != (kWarm + kOps) * (kQueuedHead + size))
        std::fprintf(stderr, "egress-queued: short record\n");
}

/** @brief One receive block holding @p n frame bytes, minted as the transport mints it. */
view_t mint_rx(std::span<const std::byte> frame) {
    tr::view::rx_block_t blk =
        tr::view::alloc_rx(tr::mem::heap_backend(), frame.size(), tr::graph::kShareThresholdBytes);
    std::memcpy(blk.frame(frame.size()).data(), frame.data(), frame.size());
    return blk.take(frame.size());
}

/** @brief A FWD{WRITE} addressed at a local vertex, carrying @p payload_bytes of value. With
 *         @p acked false the src is EMPTY (the delivery shape): no reply is assembled. */
std::vector<std::byte> make_write_frame(std::size_t payload_bytes, bool acked = true) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst;
    for (std::string_view s : {"sensor", "temp"}) (void)tr::wire::emit_path_segment(dst, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{}, dst);
    std::vector<std::byte> src;
    if (acked) (void)tr::wire::emit_path_segment(src, "origin");
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{}, src);
    std::vector<std::byte> payload(payload_bytes, std::byte{0xAB});
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload));
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

void run_ingress(std::size_t size, bool acked) {
    graph_t g;
    const vertex_handle_t v =
        g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    tr::graph::op_resolver_t r(g);
    const std::vector<std::byte> bytes = make_write_frame(size, acked);
    // ONE arm (RFC-0028 §6.5): the vertex keeps the build's default threshold, and the value
    // TLV's size alone decides whether this row is the copy or the share. The TLV is the
    // payload plus its 4-byte header, so 4096 B of payload is already at the host default.
    const std::size_t tlv = size + 4;
    const bool share = tlv >= g.share_threshold_bytes(v);
    // Every frame gets its OWN receive block, minted OUTSIDE the window (a transport mints it
    // on receive, and a loaned reserve is claimed once per block); the window holds the
    // terminus decode, the write, and — when the frame asks for one — the reply assembly.
    std::vector<view_t> frames;
    frames.reserve(kWarm + kOps);
    for (std::size_t i = 0; i < kWarm + kOps; ++i) frames.push_back(mint_rx(bytes));
    std::size_t next = 0;
    const char* const stage = acked ? (share ? "ingress-pin" : "ingress-copy")
                                    : (share ? "ingress-pin-noack" : "ingress-copy-noack");
    measure(stage, size, 1, share ? 0 : 1, [&] {
        rope_t one;
        one.append(std::move(frames[next++]));
        const auto fv = tr::wire::tlv_view_t::over(std::move(one));
        if (fv) (void)r.resolve(*fv, "cli");
    });
}

// --- the RFC-0028 prototype ----------------------------------------------------------------

/** @brief ONE block: the refcount, the length, the bytes. No wrapper, no control block. */
struct lean_value_t {
    std::atomic<std::uint32_t> refs;
    std::uint32_t length;
    [[nodiscard]] std::byte* bytes() noexcept { return reinterpret_cast<std::byte*>(this + 1); }
    [[nodiscard]] static lean_value_t* loan(std::size_t n) noexcept {
        void* p = ::operator new(sizeof(lean_value_t) + n, std::nothrow);
        if (p == nullptr) return nullptr;
        auto* v = new (p) lean_value_t{};
        v->refs.store(1, std::memory_order_relaxed);
        v->length = static_cast<std::uint32_t>(n);
        return v;
    }
    static void retain(lean_value_t* v) noexcept {
        v->refs.fetch_add(1, std::memory_order_relaxed);
    }
    static void release(lean_value_t* v) noexcept {
        if (v != nullptr && v->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            ::operator delete(v);
    }
};

/** @brief A single-writer LKV slot: publish is an exchange, the old value is released. */
struct lean_slot_t {
    std::atomic<lean_value_t*> p{nullptr};
    void publish(lean_value_t* v) noexcept {
        lean_value_t::release(p.exchange(v, std::memory_order_acq_rel));
    }
    ~lean_slot_t() { lean_value_t::release(p.load()); }
};

void run_proto(std::size_t size, std::size_t k) {
    lean_slot_t src;
    std::vector<lean_slot_t> targets(k);
    std::uint64_t egress_bytes = 0;
    measure("proto-fanout", size, k, 0, [&] {
        // Producer: loan, fill in place (the producer's own write, not a library copy), publish.
        lean_value_t* v = lean_value_t::loan(size);
        std::memset(v->bytes(), 0xAB, size);
        // Fan-out: K shares, K target stores — each an exchange + a release of the old value.
        for (lean_slot_t& t : targets) {
            lean_value_t::retain(v);
            t.publish(v);
        }
        // Egress: the block IS the iov entry.
        const std::span<const std::byte> iov[1] = {{v->bytes(), v->length}};
        egress_bytes += iov[0].size();
        src.publish(v);
    });
    if (egress_bytes != (kWarm + kOps) * size) std::fprintf(stderr, "proto: short egress\n");
}

}  // namespace

int main() {
    for (const std::size_t size : kSizes) {
        for (const std::size_t k : kFans) run_local_cb(size, k);
        for (const std::size_t k : kFans) run_local_none(size, k);
        for (const std::size_t k : kFans) run_local_target(size, k);
        for (const std::size_t k : kFans) run_local_handler(size, k);
        run_producer_own(size);
        run_egress_gather(size);
        run_egress_queued(size, true);
        run_egress_queued(size, false);
        run_ingress(size, true);
        run_ingress(size, false);
        for (const std::size_t k : kFans) run_proto(size, k);
    }
    return 0;
}
