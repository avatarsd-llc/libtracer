/**
 * @file
 * @brief Stream delivery over the RFC-0029 PAIR chain against RFC-0004 §E.1 COMPACT, at 1 and
 *        3 hops — measured BEFORE COMPACT is deleted (stage 6, #1949).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A REPORT, not a veto (maintainer ruling): it feeds the stage-6 RFC and never gates a build.
 *
 * Two arms carry the SAME operation to the SAME `STREAM` vertex over the SAME production-wired
 * chain (every link a connection vertex made by `transport_vertex_t`, which is what a PAIR hop
 * dereferences):
 *
 *  - `pair`    — `FWD{op=WRITE, dst = PATH{PAIR x (H+1)}, src = empty}`. Element k is read by node
 *                k and consumed; the frame SHRINKS by 11 B per hop. No state is held at any hop.
 *  - `compact` — one `ADVERTISE` binds a label at every hop (untimed warm-up, reported as the
 *                cold cost), then every sample rides as `COMPACT{label, payload}`.
 *
 * The arms are NOT like-for-like on the reply leg. A forwarded PAIR write grows `src` at every
 * forwarder, so the terminus builds a RESULT and it is relayed back down to node 0 — where it is
 * dropped silently (the origin receives 0 frames; no router counter moves). So `pair` pays for an
 * ack that is built, carried H hops and discarded; COMPACT emits none. The shape is asserted
 * exactly (H reply frames, 0 at the origin) so any change is loud. To price it, `pair-norelay`
 * repeats the PAIR arm with the reply's relay cut (the terminus still builds and sends it):
 * `pair` minus `pair-norelay` is the relay cost, and `pair-norelay` vs `compact` is the nearest
 * like-for-like the library allows without a core change.
 *
 * Cells: hops {1,3} x payload {64, 1024, 16384} B x batch N {1, 8, 32}. N > 1 is the optional
 * BATCH arm: N sample frames composed into ONE `BATCH` value (`compose_batch`, composed
 * UNTIMED — the app's cost, not the library's forward path) and written once; per-sample figures
 * divide by N. The pair cost grows with N (the terminus walks the BATCH children), COMPACT's does
 * not. `tlv8`/`tlv64` cut the TLV bytes of each hop's frame into 8/64 B fields: arithmetic on the
 * ws-style frame, NOT the CAN carriage (which is header-elided and carries a FWD as a directed
 * group); it supports no CAN conclusion. Rows with `warm=NO-alloc-per-frame` allocate per frame
 * (16 KiB batches cross the pinned mmap threshold) and measure the allocator, not the protocol.
 *
 * Columns of the report (one `CELL` line each, plus the standard `RESULT` row):
 *   wire_B_per_hop  bytes the frame carries on each hop's link, origin to sink
 *   ns_op           p50 per delivered frame, whole chain, synchronous, no I/O
 *   MBps            payload throughput (samples x payload / second)
 *   allocs/bytes    heap allocations around one warm delivery (RAM axis; the process peak RSS
 *                   is the one `RSS family=` line at the end)
 *   tlv8/tlv64      TLV bytes per hop cut into 8/64 B fields (NOT CAN carriage)
 *
 * NOT a wall-clock network number: the chain runs synchronously on this thread.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../core/tests/fwd_frame_builder.hpp"    // host-only frame builders
#include "../core/tests/pair_body.hpp"            // host-only PAIR element encoder
#include "../core/tests/route_frame_builder.hpp"  // host-only frame builders (#1779)
#include "bench_common.hpp"
#include "bench_process.hpp"
#include "libtracer/batch.hpp"
#include "libtracer/can_framing.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_vertex.hpp"

namespace {
std::size_t g_allocs = 0;
std::size_t g_bytes = 0;
bool g_arm = false;

void* counted(std::size_t n) {
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    return std::malloc(n == 0 ? 1 : n);
}
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    return std::aligned_alloc(align, ((n == 0 ? 1 : n) + align - 1) / align * align);
}
}  // namespace

// The full allocating and deallocating set, as bench_compact_delivery does (#793, #801).
void* operator new(std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
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

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::net::transport_vertex_t;
using tr::wire::opt_t;
using tr::wire::type_t;

constexpr std::size_t kMaxNodes = 4;  // 3 hops
constexpr std::size_t kPayloads[] = {64, 1024, 16384};
constexpr std::size_t kBatches[] = {1, 8, 32};
constexpr std::size_t kHops[] = {1, 3};
constexpr double kDefaultBudgetSeconds = 0.5;

[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

[[noreturn]] void fail(const char* cell, const char* why) {
    std::fprintf(stderr, "FAIL %s: %s\n", cell, why);
    std::fflush(stdout);
    std::exit(1);
}

/** @brief In-process link end: `send` hands the bytes to the peer end's receiver. */
struct wire_link_t : transport_t {
    wire_link_t* peer = nullptr;
    std::size_t bytes = 0;
    std::size_t frames = 0;
    void inject(std::span<const std::byte> f) { rx_.deliver_borrowed(f); }
    void send(std::span<const std::byte> f) override {
        bytes += f.size();
        ++frames;
        if (peer != nullptr) peer->inject(f);
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        scratch_.clear();
        for (const auto& s : iov) scratch_.insert(scratch_.end(), s.begin(), s.end());
        bytes += scratch_.size();
        ++frames;
        if (peer != nullptr) peer->inject(scratch_);
    }
    void clear() {
        bytes = 0;
        frames = 0;
    }

   private:
    std::vector<std::byte> scratch_;
};

struct node_t {
    graph_t g;
    fwd_router_t r{g};
    transport_vertex_t net{g, r};
};

[[nodiscard]] std::vector<std::byte> path_tlv(const std::vector<std::string>& segs) {
    std::vector<std::byte> body;
    for (const std::string& s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

[[nodiscard]] std::vector<std::byte> value_tlv(std::size_t n) {
    const std::vector<std::byte> payload(n, std::byte{0xAB});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
    return out;
}

/** @brief The delivered value: one VALUE, or ONE `BATCH` record of @p n of them. */
[[nodiscard]] std::vector<std::byte> payload_for(std::size_t payload, std::size_t n) {
    if (n == 1) return value_tlv(payload);
    const std::vector<std::byte> one = value_tlv(payload);
    std::vector<tr::view::view_t> samples;
    for (std::size_t i = 0; i < n; ++i) {
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(one.size());
        std::memcpy(seg->bytes.data(), one.data(), one.size());
        samples.push_back(tr::view::view_t::over(std::move(seg)));
    }
    const tr::view::rope_t rope = tr::wire::compose_batch(
        tr::mem::heap_backend(), tr::wire::batch_carriage_t::STANDALONE, 0, samples);
    const tr::view::view_t mat = rope.materialize();
    const auto b = mat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

/** @brief Nodes 0..hops wired in a line through `transport_vertex_t`, sink at the far end. */
struct chain_t {
    std::size_t hops;
    std::vector<std::unique_ptr<node_t>> nodes;
    std::array<wire_link_t, kMaxNodes> down{};
    std::array<wire_link_t, kMaxNodes> up{};
    wire_link_t origin;
    std::size_t delivered = 0;

    static void count(void* ctx, const tr::graph::value_t&) { ++*static_cast<std::size_t*>(ctx); }

    explicit chain_t(std::size_t h) : hops(h) {
        for (std::size_t i = 0; i <= hops; ++i) nodes.push_back(std::make_unique<node_t>());
        const auto bind = [&](std::size_t i, const char* mod, const std::string& name,
                              transport_t& link) {
            node_t& n = *nodes[i];
            n.net.provide_link(mod, name, link);
            (void)n.net.register_module(mod, mod, tr::net::conn_role_t::DIAL);
            const auto st = n.g.write(path_t(std::string("/net/") + mod + "/conn"),
                                      tr::net::conn_spec_t(name).view());
            if (!st.has_value()) fail("wire", "connection creation refused");
        };
        for (std::size_t i = 0; i < hops; ++i) {
            down[i].peer = &up[i + 1];
            up[i + 1].peer = &down[i];
            bind(i, "dn", "n" + std::to_string(i + 1), down[i]);
            bind(i + 1, "up", "n" + std::to_string(i), up[i + 1]);
        }
        bind(0, "app", "o", origin);
        (void)nodes[hops]->g.register_vertex(*path_t::parse("/sink"), role_t::STREAM);
        (void)nodes[hops]->g.subscribe(*path_t::parse("/sink"), &chain_t::count, &delivered);
    }

    [[nodiscard]] std::vector<std::byte> pair_dst() {
        std::vector<std::byte> body;
        for (std::size_t i = 0; i < hops; ++i) {
            graph_t& g = nodes[i]->g;
            const auto v = g.find(path_t("/net/dn/n" + std::to_string(i + 1)).key());
            const auto slot = v ? g.vertex_slot(*v) : std::nullopt;
            if (!slot) fail("pair", "no connection-vertex slot");
            tr::testing::emit_path_pair(body, *slot);
        }
        graph_t& g = nodes[hops]->g;
        const auto v = g.find(path_t("/sink").key());
        const auto slot = v ? g.vertex_slot(*v) : std::nullopt;
        if (!slot) fail("pair", "no sink slot");
        tr::testing::emit_path_pair(body, *slot);
        std::vector<std::byte> out;
        tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
        return out;
    }

    [[nodiscard]] std::vector<std::byte> compact_route() const {
        std::vector<std::string> segs;
        for (std::size_t i = 0; i < hops; ++i) {
            segs.push_back("net");
            segs.push_back("dn");
            segs.push_back("n" + std::to_string(i + 1));
        }
        segs.push_back("sink");
        return path_tlv(segs);
    }

    void clear() {
        for (auto& l : down) l.clear();
        for (auto& l : up) l.clear();
        origin.clear();
    }
    [[nodiscard]] std::size_t rev_frames() const {
        std::size_t n = origin.frames;
        for (const auto& l : up) n += l.frames;
        return n;
    }
};

struct cell_t {
    double p50_ns = 0, ops_s = 0;
    std::size_t allocs = 0, abytes = 0, origin_B = 0, rss_kb = 0;
    std::uint64_t cold_ns = 0;
    std::vector<std::size_t> hop_B;  // bytes on each hop's link, per frame
    std::size_t fwd = 0, rev = 0, rev_B = 0;
};

[[nodiscard]] cell_t run_cell(const char* arm, std::size_t hops, std::size_t payload,
                              std::size_t n) {
    char name[96];
    std::snprintf(name, sizeof name, "%s/h%zu/p%zu/n%zu", arm, hops, payload, n);
    const bool pair = std::strncmp(arm, "pair", 4) == 0;
    const bool norelay = std::strcmp(arm, "pair-norelay") == 0;
    const std::size_t rss0 = bench::peak_rss_kb();
    cell_t c;
    {
        chain_t ch(hops);
        const std::vector<std::byte> body = payload_for(payload, n);
        std::vector<std::byte> frame;
        std::uint16_t label = 0x0042;
        const std::uint64_t t0 = bench::now_ns();
        if (pair) {
            frame = tr::testing::b_fwd_raw_op(static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE),
                                              ch.pair_dst(), path_tlv({}), {}, body);
        } else {
            // The cold cost: ONE advertise walks the chain and leaves a binding at every hop.
            ch.origin.inject(tr::net::encode_advertise(label, ch.compact_route()));
            frame = tr::net::encode_compact(label, body);
        }
        ch.origin.inject(frame);
        c.cold_ns = bench::now_ns() - t0;
        if (ch.delivered != 1) fail(name, "the first frame did not reach the sink");
        c.origin_B = frame.size();

        const auto op = [&] { ch.origin.inject(frame); };
        for (int i = 0; i < 64; ++i) op();
        ch.clear();
        const std::size_t before = ch.delivered;
        op();
        if (ch.delivered != before + 1) fail(name, "a warm frame was not delivered exactly once");
        for (std::size_t i = 0; i < hops; ++i) c.hop_B.push_back(ch.down[i].bytes);
        for (std::size_t i = 0; i < hops; ++i) c.fwd += ch.down[i].frames;
        c.rev = ch.rev_frames();
        if (c.fwd != hops) fail(name, "not exactly one frame per hop");
        c.rev_B = ch.origin.bytes;
        for (const auto& l : ch.up) c.rev_B += l.bytes;
        // The two spellings do NOT emit the same traffic: a PAIR write is relayed with `src`
        // GROWN at every forwarder (RFC-0029 §6.1; the empty-`src` marker only survives on a
        // directly attached origin), so the terminus builds a RESULT and it is relayed down to
        // node 0, which drops it silently (#2042): the origin receives 0 frames and no router
        // counter moves. A COMPACT emits no reply. The shape is asserted exactly so a change in
        // either direction is loud, and the reply leg has its own columns.
        if (pair ? (c.rev != hops || ch.origin.frames != 0) : c.rev != 0)
            fail(name, "the reply-leg census changed: pair must ack H frames, compact none");

        // The relay ablation: the terminus still builds and sends its RESULT, no hop relays it.
        if (norelay)
            for (auto& l : ch.up) l.peer = nullptr;
        g_allocs = g_bytes = 0;
        g_arm = true;
        op();
        g_arm = false;
        c.allocs = g_allocs;
        c.abytes = g_bytes;

        const bench::batch_timing_t t =
            bench::time_batches(op, static_cast<std::uint64_t>(budget_seconds() * 1e9));
        c.p50_ns = t.p50_ps / 1e3;
        c.ops_s = t.ops_per_s;
        std::string mode =
            std::string("chain-") + arm + "-h" + std::to_string(hops) + "-n" + std::to_string(n);
        const double mbps = c.ops_s * static_cast<double>(n * payload) / 1e6;
        bench::emit_batch("libtracer", mode.c_str(), payload, hops, n,
                          c.ops_s * static_cast<double>(n), c.ops_s * static_cast<double>(n), mbps,
                          t);
    }
    c.rss_kb = bench::peak_rss_kb() - std::min(bench::peak_rss_kb(), rss0);
    return c;
}

void report(const char* arm, std::size_t hops, std::size_t payload, std::size_t n,
            const cell_t& c) {
    std::string hb, can8, can64;
    for (const std::size_t b : c.hop_B) {
        hb += (hb.empty() ? "" : "/") + std::to_string(b);
        can8 +=
            (can8.empty() ? "" : "/") + std::to_string((b + tr::net::can::kCanClassicMaxData - 1) /
                                                       tr::net::can::kCanClassicMaxData);
        can64 += (can64.empty() ? "" : "/") + std::to_string((b + tr::net::can::kCanFdMaxData - 1) /
                                                             tr::net::can::kCanFdMaxData);
    }
    const double ns_sample = c.p50_ns / static_cast<double>(n);
    const double mbps = c.ops_s * static_cast<double>(n * payload) / 1e6;
    std::printf(
        "CELL warm=%s arm=%s hops=%zu payload=%zu N=%zu origin_B=%zu wire_B_per_hop=%s ns_op=%.1f "
        "ns_sample=%.1f samples_s=%.0f MBps=%.1f allocs=%zu alloc_B=%zu cold_ns=%llu "
        "tlv8=%s tlv64=%s rev_frames=%zu rev_B=%zu\n",
        c.allocs == 0 ? "yes" : "NO-alloc-per-frame", arm, hops, payload, n, c.origin_B, hb.c_str(),
        c.p50_ns, ns_sample, c.ops_s * static_cast<double>(n), mbps, c.allocs, c.abytes,
        static_cast<unsigned long long>(c.cold_ns), can8.c_str(), can64.c_str(), c.rev, c.rev_B);
}

}  // namespace

int main(int /*argc*/, char** argv) {
    bench::pin_allocator_state(argv);
    bench::emit_clock_floor();
    bench::emit_alloc_state();
    const std::size_t start_kb = bench::peak_rss_kb();
    std::printf("# PAIR chain vs COMPACT, stream delivery, report only (#1949)\n");
    for (const std::size_t h : kHops)
        for (const std::size_t p : kPayloads)
            for (const std::size_t n : kBatches) {
                // Alternate the arms inside one process and one cell, so drift is common-mode.
                const cell_t a = run_cell("pair", h, p, n);
                const cell_t b = run_cell("compact", h, p, n);
                const cell_t a2 = run_cell("pair-norelay", h, p, n);
                report("pair", h, p, n, a);
                report("pair-norelay", h, p, n, a2);
                report("compact", h, p, n, b);
            }
    bench::emit_family_rss("chain-vs-compact", start_kb);
    return 0;
}
