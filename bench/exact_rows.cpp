/**
 * @file
 * @brief The exact-count rows of #1808 (see exact_rows.hpp): RAM probes, blocks per write and
 *        the STREAM write's stripe-lock sections.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Linked into `bench_forward_heap`, whose TU owns the counting `operator new` this file reads
 * through heap_probe.hpp. The lock counter is `-Wl,--wrap=pthread_mutex_lock`, the instrument
 * `core/tests/stream_admit_one_lock_test.cpp` asserts #1713 with: only the STREAM vertex's
 * stripe mutex is counted, and that mutex is LEARNED from a one-section control
 * (`graph_t::ring_reserved_bytes`), so the counter is shown live before it is read.
 */
#include "exact_rows.hpp"

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "heap_probe.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_slab_pool.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/mem_source_backend.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

namespace {

/** @brief The one mutex whose acquisitions are counted (null: count nothing). */
std::atomic<pthread_mutex_t*> g_watch{nullptr};
/** @brief Acquisitions of @ref g_watch on this thread. */
thread_local std::size_t g_locks = 0;
/** @brief Acquisitions of ANY mutex on this thread (the control's learning count). */
thread_local std::size_t g_any = 0;
/** @brief The mutex this thread acquired last (the control learns the stripe from it). */
thread_local pthread_mutex_t* g_last = nullptr;

}  // namespace

extern "C" {
/** @brief The real `pthread_mutex_lock` (`-Wl,--wrap`). */
int __real_pthread_mutex_lock(pthread_mutex_t* m);  // NOLINT(bugprone-reserved-identifier)

/** @brief Count an acquisition (and the watched stripe's), then take the mutex for real. */
int __wrap_pthread_mutex_lock(pthread_mutex_t* m) {  // NOLINT(bugprone-reserved-identifier)
    if (m == g_watch.load(std::memory_order_relaxed)) ++g_locks;
    ++g_any;
    g_last = m;
    return __real_pthread_mutex_lock(m);
}
}

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;

/** @brief A per-unit count kept to a thousandth: `total * 1000 / units`. */
std::size_t x1000(long long total, std::size_t units) {
    return total > 0 && units > 0 ? static_cast<std::size_t>(total) * 1000 / units : 0;
}

/** @brief A view over a fresh heap segment of @p n bytes (minted outside every window). */
tr::view::view_t heap_view(std::size_t n, std::uint8_t fill) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
    if (n != 0) std::memset(seg->bytes.data(), fill, n);
    return tr::view::view_t::over(std::move(seg));
}

/** @brief A view over a fresh heap segment holding a copy of @p bytes. */
tr::view::view_t bytes_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief A callback edge that does nothing: the probe prices the edge, not the delivery. */
void noop_cb(void*, const tr::graph::value_t&) {}

/**
 * @brief The block @p n bytes at @p align take from the host slab pool's size-class table
 *        (`config_t::kSizeClasses`, #1777): that class's block size, or @p n itself when no
 *        class serves it and the request falls back to the root as a block of its own.
 * @param[out] oversize Set when no class serves the request.
 */
std::size_t host_class_bytes(std::size_t n, std::size_t align, bool& oversize) {
    const tr::mem::host_pool_t& table = tr::mem::host_root().tables();
    const std::size_t i = table.class_of(n, align);
    oversize = i == tr::mem::host_pool_t::kNoClass;
    return oversize ? n : table.class_bytes(i);
}

/**
 * @brief A malloc-backed source that counts what it serves, so a seam-served block never
 *        reaches the counted global `operator new` and the two columns stay disjoint.
 *
 * Each block is also classified against the host slab pool's table (@ref host_class_bytes):
 * the bytes it would occupy there and whether it would fall past the last class.
 */
class counting_seam_t final : public tr::mem::block_source_t {
   public:
    counting_seam_t() noexcept : block_source_t("exact-rows") {}
    std::atomic<std::size_t> blocks{0}; /**< @brief Blocks served. */
    std::atomic<std::size_t> bytes{0};  /**< @brief Bytes requested by those blocks. */
    /** @brief The host class block bytes those requests select (@ref host_class_bytes). */
    std::atomic<std::size_t> class_bytes{0};
    /** @brief Of those blocks, the ones no host class serves (oversize, from the root). */
    std::atomic<std::size_t> oversize{0};
    /** @brief The one request size refused while nonzero (the `defer` fixture's spill). */
    std::atomic<std::size_t> refuse_bytes{0};
    /** @brief Serve from `aligned_alloc`, counting. */
    [[nodiscard]] void* try_alloc(std::size_t n, std::size_t align) noexcept override {
        const std::size_t refused = refuse_bytes.load(std::memory_order_relaxed);
        if (refused != 0 && n == refused) return nullptr;
        blocks.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(n, std::memory_order_relaxed);
        bool over = false;
        class_bytes.fetch_add(host_class_bytes(n, align, over), std::memory_order_relaxed);
        if (over) oversize.fetch_add(1, std::memory_order_relaxed);
        const std::size_t a = align < alignof(std::max_align_t) ? alignof(std::max_align_t) : align;
        const std::size_t rounded = ((n == 0 ? 1 : n) + a - 1) / a * a;
        return std::aligned_alloc(a, rounded);
    }
    /** @brief Release to the C allocator. */
    void release(void* p, std::size_t, std::size_t) noexcept override { std::free(p); }
};

/** @brief A transport that sends nowhere: what a link costs the router, not its socket. */
struct null_link_t : tr::net::transport_t {
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
};

/**
 * @brief Print one row from a window's counts over @p n units: the gated `ramprobe` row when
 *        the graph drew its tables per object (@p per_object), else the ungated `slabfoot`
 *        row of a default graph, whose window counts the host slab pool's whole slabs (#1778).
 */
void print_ram(const char* what, const probe::counts_t& c, std::size_t n, bool per_object) {
    if (per_object)
        std::printf("RESULT ramprobe %s blocks_x1000=%zu bytes_x1000=%zu n=%zu\n", what,
                    x1000(static_cast<long long>(c.allocs), n), x1000(c.live_bytes, n), n);
    else
        std::printf("RESULT slabfoot %s live_x1000=%zu blocks_x1000=%zu n=%zu (ungated)\n", what,
                    x1000(c.live_bytes, n), x1000(static_cast<long long>(c.allocs), n), n);
}

/** @brief Units each RAM probe spreads its window over. */
constexpr std::size_t kRamN = 256;

/**
 * @brief RAM per callback edge (`edge_callback`) and per WIRE subscriber edge (`edge_wire`).
 *
 * One vertex, a warm first edge outside the window, then @ref kRamN more inside it. Live
 * usable-size bytes and heap blocks per edge. The wire edge names its link
 * `192.168.x.y:9000`, past the small-string buffer like a real `host:port` link, and that
 * name is built inside the window because the edge keeps it.
 *
 * Each probe runs twice (#1778). The gated `ramprobe` row is taken on a graph injected with
 * `tr::mem::heap_source()`, so every table block is its own counted `operator new`: the
 * per-object cost, what a build without the slab pool pays and what main measured before
 * the graph's tables moved onto the seam. The ungated `slabfoot` row is the same window on a
 * default graph, whose tables come from the host slab pool and are counted a slab at a time.
 */
bool ram_edges_on(bool per_object) {
    tr::mem::block_source_t& src = per_object ? tr::mem::heap_source() : tr::mem::default_root();
    {
        graph_t g(src);
        const path_t p = *path_t::parse("/edge/cb");
        (void)g.register_vertex(p, role_t::STORED_VALUE);
        if (!g.subscribe(p, noop_cb, nullptr).has_value()) return false;
        bool ok = true;
        probe::window_t win;
        for (std::size_t i = 0; i < kRamN; ++i)
            ok = g.subscribe(p, noop_cb, nullptr).has_value() && ok;
        const probe::counts_t c = win.result();
        if (!ok) return false;
        print_ram("edge_callback", c, kRamN, per_object);
    }
    {
        graph_t g(src);
        const vertex_handle_t v =
            g.register_vertex(*path_t::parse("/edge/wire"), role_t::STORED_VALUE);
        // The remote SUBSCRIBER form (no PATH child) and the smallest well-formed route.
        const std::vector<std::byte> sub{std::byte{0x04}, std::byte{0x40}, std::byte{0x00},
                                         std::byte{0x00}};
        const std::vector<std::byte> route{std::byte{0x06}, std::byte{0x00}, std::byte{0x00},
                                           std::byte{0x00}};
        const tr::view::view_t sub_v = bytes_view(sub);
        const tr::view::view_t route_v = bytes_view(route);
        const auto link = [](std::size_t i) {
            return "192.168." + std::to_string(i / 250) + "." + std::to_string(i % 250) + ":9000";
        };
        // Each edge's link pairs, one distinct connection per link, as the router mints them.
        const auto pair = [](std::size_t i) {
            return tr::graph::link_pair_t{.conn = {static_cast<std::uint32_t>(i), 1}};
        };
        if (!g.subscribe_wire(v, sub_v, route_v, link(kRamN), {}, {}, {}, pair(kRamN)).has_value())
            return false;
        // The link names are built OUTSIDE the window (#1781): `subscribe_wire` takes the link
        // as a view, so a name the caller builds per call is the caller's block, not the edge's.
        std::vector<std::string> links;
        links.reserve(kRamN);
        for (std::size_t i = 0; i < kRamN; ++i) links.push_back(link(i));
        bool ok = true;
        probe::window_t win;
        for (std::size_t i = 0; i < kRamN; ++i)
            ok = g.subscribe_wire(v, sub_v, route_v, links[i], {}, {}, {}, pair(i)).has_value() &&
                 ok;
        const probe::counts_t c = win.result();
        if (!ok) return false;
        print_ram("edge_wire", c, kRamN, per_object);
    }
    return true;
}

/** @brief Both arms of @ref ram_edges_on: the gated per-object rows, then the slab rows. */
bool ram_edges() { return ram_edges_on(true) && ram_edges_on(false); }

/**
 * @brief RAM per LINK (`link`): what one `fwd_router_t::add_child` costs the router.
 *
 * The transports are the caller's and are built outside the window; the mount names are
 * RFC-0014 shaped (`net/tcp/conn-NNNNN`) and built inside it, because the router keeps them.
 */
bool ram_links() {
    graph_t g;
    tr::net::fwd_router_t router(g);
    std::deque<null_link_t> links(kRamN + 1);
    if (!router.attach_link("net/tcp/warm", links.back())) return false;
    bool ok = true;
    probe::window_t win;
    for (std::size_t i = 0; i < kRamN; ++i) {
        char name[40];
        std::snprintf(name, sizeof name, "net/tcp/conn-%05zu", i);
        ok = router.attach_link(name, links[i]) && ok;
    }
    const probe::counts_t c = win.result();
    if (!ok) return false;
    print_ram("link", c, kRamN, true);
    return true;
}

/**
 * @brief RAM a vertex holding a 1 KiB last-known value occupies beyond an empty one
 *        (`vertex_value_1k`), the 1 KiB twin of the gated `vertex_value` probe (4 B).
 *
 * The vertices are registered outside the window. Inside it, each value is minted (one owned
 * 1 KiB segment, as a producer hands one over) and written, so the live balance holds the
 * segment the vertex keeps and the record around it: the RAM a 1 KiB value really costs.
 *
 * Two arms, as for the edges (#1778). The gated `ramprobe` row injects
 * `tr::mem::heap_source()` into the graph and mints each segment through a backend over that
 * same source, so the record and the segment are each one counted `operator new`. The
 * ungated `slabfoot` row is the old window: a default graph, segments from the host slab
 * pool's value sub-pool, counted a slab at a time (one kept free slab over 256 units moved
 * the old row by 384 B, #1879).
 */
bool ram_value_1k_on(bool per_object) {
    static tr::mem::source_backend_t heap_segments{tr::mem::heap_source()};
    graph_t g(per_object ? tr::mem::heap_source() : tr::mem::default_root());
    std::vector<vertex_handle_t> vs;
    vs.reserve(kRamN);
    for (std::size_t i = 0; i < kRamN; ++i) {
        char pb[24];
        std::snprintf(pb, sizeof pb, "/kv/v%04zu", i);
        vs.push_back(g.register_vertex(*path_t::parse(pb), role_t::STORED_VALUE));
    }
    const auto mint = [per_object](std::uint8_t fill) {
        tr::view::segment_ptr_t seg =
            per_object ? tr::view::segment_alloc(heap_segments, 1024) : tr::view::heap_alloc(1024);
        if (seg) std::memset(seg->bytes.data(), fill, 1024);
        return tr::view::view_t::over(std::move(seg));
    };
    bool ok = true;
    probe::window_t win;
    for (std::size_t i = 0; i < kRamN; ++i)
        ok = g.write(vs[i], mint(static_cast<std::uint8_t>(i))).has_value() && ok;
    const probe::counts_t c = win.result();
    if (!ok) return false;
    print_ram("vertex_value_1k", c, kRamN, per_object);
    return true;
}

/** @brief Both arms of @ref ram_value_1k_on: the gated per-object row, then the slab row. */
bool ram_value_1k() { return ram_value_1k_on(true) && ram_value_1k_on(false); }

/**
 * @brief Blocks per write at each payload-ladder size, for both ways a value arrives: from
 *        the graph's injected source (`seam`, with the bytes it asked for) and from the global
 *        heap (`heap`, the escape).
 *
 *  - `owned`: the producer hands over a sole-owner segment, minted outside the window; the
 *    write adopts it and draws only its record.
 *  - `rope2`: the value arrives as a two-link rope over buffers the producer keeps (a header
 *    and a body, say). The write keeps the links and draws one record sized for two of them.
 *
 * Measured at this commit: one seam block per write in both shapes and at every size (40 B for
 * one link, 64 B for two), and nothing from the global heap. The payload is never copied on a
 * leaf write, whatever its size; a change that starts copying it, or draws a second block,
 * fails the ratchet at the size where it does.
 *
 * The allocation seam's per-write accounting (#1773): a size-classed source (#1777) is judged
 * on exactly these counts, and today's default source is what they ratchet.
 */
bool write_blocks() {
    constexpr std::size_t kWrites = 64;
    for (const bool rope2 : {false, true}) {
        for (const std::size_t S : bench::kPayloadLadder) {
            counting_seam_t seam;  // outlives the graph: its blocks are released into it
            graph_t g(seam);
            const vertex_handle_t v = g.register_vertex(*path_t::parse("/w"), role_t::STORED_VALUE);
            const std::vector<std::byte> head(S - S / 2, std::byte{0x5A});
            const std::vector<std::byte> body(S / 2, std::byte{0xA5});
            const auto value = [&] {
                if (!rope2) return tr::view::rope_t{heap_view(S, 2)};
                tr::view::rope_t r{tr::view::view_t::over(tr::view::borrow_const(head))};
                r.append(tr::view::view_t::over(tr::view::borrow_const(body)));
                return r;
            };
            for (std::size_t i = 0; i < 8; ++i)
                if (!g.write(v, value()).has_value()) return false;  // warm
            std::vector<tr::view::rope_t> vals;
            vals.reserve(kWrites);
            for (std::size_t i = 0; i < kWrites; ++i) vals.push_back(value());
            const std::size_t b0 = seam.blocks.load();
            const std::size_t y0 = seam.bytes.load();
            bool ok = true;
            probe::window_t win;
            for (tr::view::rope_t& val : vals) ok = g.write(v, std::move(val)).has_value() && ok;
            const probe::counts_t c = win.result();
            if (!ok) return false;
            std::printf(
                "RESULT writeblocks %s S=%zu seam_x1000=%zu seam_bytes_x1000=%zu heap_x1000=%zu "
                "n=%zu\n",
                rope2 ? "rope2" : "owned", S,
                x1000(static_cast<long long>(seam.blocks.load() - b0), kWrites),
                x1000(static_cast<long long>(seam.bytes.load() - y0), kWrites),
                x1000(static_cast<long long>(c.allocs), kWrites), kWrites);
        }
    }
    return true;
}

/**
 * @brief Size-class selection per write on the host slab pool's table, at every payload-ladder
 *        size (#1908): the acceptance evidence of #1777's size-classed pool, exact.
 *
 * The producer mints each value through a backend over the graph's own counting source, as
 * a default graph's producer mints from the value sub-pool, so the window holds BOTH halves of
 * a write: the segment (header and payload in one block) and the record the vertex keeps.
 * Each block is classified against `config_t::kSizeClasses` (@ref host_class_bytes):
 *
 *     RESULT seamclass S=<size> blocks_x1000= req_bytes_x1000= class_bytes_x1000=
 *            oversize_x1000= n=<writes>
 *
 * `req_bytes` is what the write asked for, `class_bytes` what the classes it selects hold
 * (the rounding slack is their difference), and `oversize` the blocks no class serves, which
 * fall back to the root as blocks of their own. 984 and 985 B straddle glibc's one-block
 * boundary and must land in one class; a placement change that moves a ladder size into a
 * bigger class, or past the last one, grows a column and fails the exact ratchet.
 */
bool seam_classes() {
    constexpr std::size_t kWrites = 64;
    for (const std::size_t S : bench::kPayloadLadder) {
        counting_seam_t seam;                      // outlives the backend and the graph
        tr::mem::source_backend_t segments{seam};  // the producer's mint, on the same seam
        graph_t g(seam);
        const vertex_handle_t v = g.register_vertex(*path_t::parse("/c"), role_t::STORED_VALUE);
        const auto mint = [&] {
            tr::view::segment_ptr_t seg = tr::view::segment_alloc(segments, S);
            if (seg) std::memset(seg->bytes.data(), 3, S);
            return tr::view::view_t::over(std::move(seg));
        };
        for (std::size_t i = 0; i < 8; ++i)
            if (!g.write(v, mint()).has_value()) return false;  // warm
        const std::size_t b0 = seam.blocks.load(), y0 = seam.bytes.load();
        const std::size_t c0 = seam.class_bytes.load(), o0 = seam.oversize.load();
        bool ok = true;
        for (std::size_t i = 0; i < kWrites; ++i) ok = g.write(v, mint()).has_value() && ok;
        if (!ok) return false;
        const auto per = [&](std::size_t now, std::size_t then) {
            return x1000(static_cast<long long>(now - then), kWrites);
        };
        std::printf(
            "RESULT seamclass S=%zu blocks_x1000=%zu req_bytes_x1000=%zu class_bytes_x1000=%zu "
            "oversize_x1000=%zu n=%zu\n",
            S, per(seam.blocks.load(), b0), per(seam.bytes.load(), y0),
            per(seam.class_bytes.load(), c0), per(seam.oversize.load(), o0), kWrites);
    }
    return true;
}

/** @brief The delivery count a STREAM case's subscriber keeps. */
void count_cb(void* ctx, const tr::graph::value_t&) {
    static_cast<std::atomic<std::size_t>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief LEARN the stripe mutex @p v rides from a one-section verb, and arm the counter on it.
 * @return false when the wrap is never reached (a standard library whose mutex is out of line).
 */
bool watch_stripe_of(graph_t& g, vertex_handle_t v) {
    g_watch.store(nullptr, std::memory_order_relaxed);
    g_any = 0;
    g_last = nullptr;
    (void)g.ring_reserved_bytes(v);
    if (g_any != 1) return false;
    g_watch.store(g_last, std::memory_order_relaxed);
    return true;
}

/** @brief Print one `streamlock` row. */
void print_lock(const char* what, std::size_t sections, std::size_t heap, std::size_t delivered,
                std::size_t n) {
    std::printf(
        "RESULT streamlock %s sections_x1000=%zu heap_x1000=%zu delivered_x1000=%zu n=%zu\n", what,
        x1000(static_cast<long long>(sections), n), x1000(static_cast<long long>(heap), n),
        x1000(static_cast<long long>(delivered), n), n);
}

/**
 * @brief The STREAM write's stripe-lock sections and heap blocks, per write (#1713, #1808).
 *
 *  - `w1`: the steady-state write, past two hazard retire batches of warm-up (the #873
 *    carve-out). #1713's claim is ONE section and ZERO heap, and the gate holds it to that.
 *  - `spill`: a write over `ring_take_t::kInline + 2` queued entries; still one section, plus
 *    the one spill, drawn from the values source since #1778 (so its heap column is 0).
 *  - `defer`: the same write with the spill refused — every global allocation, and the
 *    values source's spill-sized request, since the spill draws from the graph's values source
 *    (#1778) — so the window is deferred: delivered is 0, and the next write delivers it all.
 *  - `w4` and `w2`: four, then two, writers on one vertex at once; sections per write (heap
 *    is not printed: each new thread stocks its own reclamation list once, which is not the
 *    write's cost). `w2` completes the 1 / 2 / 4 writer set of the timed `stream-w<T>` rows.
 *
 * Values and ring reservations come from injected counting sources, so the heap column is only
 * what the write path takes from the global heap.
 */
bool stream_locks() {
    constexpr std::size_t kN = 64;
    constexpr std::size_t kBacklog = tr::graph::vertex_t::ring_take_t::kInline + 2;
    counting_seam_t values;
    counting_seam_t ring;
    graph_t g(values);
    const vertex_handle_t v = g.register_vertex(*path_t::parse("/s"), role_t::STREAM);
    if (!g.set_policy(v,
                      {.retention = tr::graph::retention_t::N, .depth = 16, .ring_source = &ring})
             .has_value())
        return false;
    std::atomic<std::size_t> seen{0};
    if (!g.subscribe(*path_t::parse("/s"), count_cb, &seen).has_value()) return false;
    for (std::size_t i = 0; i < 2 * tr::graph::kHazardReaderSlots + 4; ++i)
        if (!g.write(v, heap_view(4, 1)).has_value()) return false;
    if (!watch_stripe_of(g, v)) {
        std::printf("NOTE streamlock: the lock wrap is not reached here; no rows\n");
        return true;
    }

    std::vector<tr::view::view_t> vals;
    const auto mint = [&](std::size_t n) {
        vals.clear();
        for (std::size_t i = 0; i < n; ++i) vals.push_back(heap_view(4, 2));
    };
    mint(kN);
    seen.store(0);
    g_locks = 0;
    probe::reset();
    probe::arm();
    for (tr::view::view_t& val : vals) (void)g.write(v, std::move(val));
    probe::disarm();
    print_lock("w1", g_locks, probe::snapshot().allocs, seen.load(), kN);

    std::size_t sections = 0, heap = 0;
    seen.store(0);
    for (std::size_t c = 0; c < kN; ++c) {
        mint(kBacklog + 1);
        for (std::size_t i = 0; i < kBacklog; ++i) (void)g.assign(v, std::move(vals[i]));
        g_locks = 0;
        probe::reset();
        probe::arm();
        (void)g.write(v, std::move(vals[kBacklog]));
        probe::disarm();
        sections += g_locks;
        heap += probe::snapshot().allocs;
    }
    print_lock("spill", sections, heap, seen.load(), kN);

    sections = 0;
    std::size_t deferred_delivered = 0;
    seen.store(0);
    for (std::size_t c = 0; c < kN; ++c) {
        mint(kBacklog + 2);
        for (std::size_t i = 0; i < kBacklog; ++i) (void)g.assign(v, std::move(vals[i]));
        const std::size_t before = seen.load();
        g_locks = 0;
        probe::g_refuse.store(true);
        values.refuse_bytes.store((kBacklog + 1) * sizeof(tr::graph::value_ref_t));
        (void)g.write(v, std::move(vals[kBacklog]));
        values.refuse_bytes.store(0);
        probe::g_refuse.store(false);
        sections += g_locks;
        deferred_delivered += seen.load() - before;
        (void)g.write(v, std::move(vals[kBacklog + 1]));  // catches the deferred window up
    }
    if (seen.load() != kN * (kBacklog + 2)) {
        std::printf("FAIL streamlock defer: %zu of %zu entries delivered after recovery\n",
                    seen.load(), kN * (kBacklog + 2));
        return false;
    }
    print_lock("defer", sections, 0, deferred_delivered, kN);

    // `w4`, then `w2` (#1908): the four-writer case runs first, as it did before `w2` existed,
    // so its threads meet the same heap.
    for (const std::size_t threads : {std::size_t{4}, std::size_t{2}}) {
        constexpr std::size_t kPer = 2000;
        std::atomic<std::size_t> total{0};
        seen.store(0);
        std::vector<std::thread> ts;
        for (std::size_t t = 0; t < threads; ++t) {
            ts.emplace_back([&] {
                std::vector<tr::view::view_t> mine;
                mine.reserve(kPer);
                for (std::size_t i = 0; i < kPer; ++i) mine.push_back(heap_view(4, 3));
                g_locks = 0;
                for (tr::view::view_t& val : mine) (void)g.write(v, std::move(val));
                total.fetch_add(g_locks);
            });
        }
        for (std::thread& th : ts) th.join();
        (void)g.propagate(v);
        std::printf("RESULT streamlock w%zu sections_x1000=%zu delivered_x1000=%zu n=%zu\n",
                    threads, x1000(static_cast<long long>(total.load()), threads * kPer),
                    x1000(static_cast<long long>(seen.load()), threads * kPer), threads * kPer);
    }
    g_watch.store(nullptr, std::memory_order_relaxed);
    return true;
}

}  // namespace

namespace exact_rows {

int print_all() {
    if (!ram_edges() || !ram_links() || !ram_value_1k() || !write_blocks() || !stream_locks() ||
        !seam_classes()) {
        std::printf("FAIL: an exact-count fixture (#1808) did not do what its row claims\n");
        return 2;
    }
    return 0;
}

}  // namespace exact_rows
