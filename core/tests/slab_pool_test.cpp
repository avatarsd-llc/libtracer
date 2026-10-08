/**
 * @file
 * @brief The size-classed slab pool and the host default root (#1777, ADR-0083 Decision 6).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What the issue's acceptance criteria name, one case each:
 *   - a pool over a counting root asks it for whole slabs only, at the slab's own alignment,
 *     carves blocks of the class a request falls in, and serves an oversize request its own
 *     block at its own size;
 *   - a fully free slab above the high-water cap goes back to the root at once, one within
 *     the cap stays until @ref tr::mem::slab_pool_t::trim;
 *   - a root that refuses a slab is a counted refusal (`refused`, `largest_refused`), never
 *     an abort, and the per-class detail sees blocks, slabs and releases;
 *   - a default host graph's values come from the value sub-pool, and while it writes the
 *     platform allocator sees whole slabs and nothing else;
 *   - concurrent writers and cross-thread frees through the value sub-pool's per-thread
 *     caches: the tsan lane runs this suite, so a plain-memory race on a class or a cache is
 *     a report;
 *   - #1646: over a bounded root (a `pool_source_t` on a static slab) a power-of-two ladder
 *     serves a peer-random size distribution with a bounded miss rate and no refusal, where
 *     the exact-size pool alone on the same slab refuses; the rounding waste is in
 *     `class_stats`.
 *
 * The instrument for the platform allocator is a counting replacement of the global
 * `operator new` family; it records only while armed, and only sizes.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_slab_pool.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

/** @brief Requests the armed window saw. */
std::atomic<std::size_t> g_news{0};
/** @brief Of those, requests that were NOT a whole number of base slabs. */
std::atomic<std::size_t> g_odd{0};
/** @brief The first odd request's size, for the log. */
std::atomic<std::size_t> g_odd_bytes{0};
/** @brief Arms the counters. */
std::atomic<bool> g_arm{false};

/** @brief Record one request of @p n bytes while armed. */
void note(std::size_t n) noexcept {
    if (!g_arm.load(std::memory_order_relaxed)) return;
    g_news.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || n % tr::mem::kSlabBytes != 0) {
        if (g_odd.fetch_add(1, std::memory_order_relaxed) == 0)
            g_odd_bytes.store(n, std::memory_order_relaxed);
    }
}

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    note(n);
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation — `aligned_alloc` only for a genuinely OVER-aligned
 *         request (which `free` accepts), `malloc` for a fundamental one. */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    note(n);
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

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::testing::check;

/** @brief A root that records every draw and release, and refuses on demand. */
class counting_root_t final : public tr::mem::block_source_t {
   public:
    counting_root_t() noexcept : block_source_t("test_root") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse) return nullptr;
        void* p = nullptr;
        if (doubled_aligned && align == kAlign) {
            // The doubled fallback draw, handed out aligned to the slab after all.
            p = heap().try_alloc(bytes, bytes / 2);
            dbl = p;
            dbl_bytes = bytes;
        } else {
            p = misalign || doubled_aligned ? misaligned(bytes, align)
                                            : heap().try_alloc(bytes, align);
        }
        if (p != nullptr) {
            ++draws;
            last_bytes = bytes;
            last_align = align;
            live += bytes;
        }
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        ++releases;
        live -= bytes;
        if (doubled_aligned && p == dbl && p != nullptr) {
            dbl_released_as_drawn = bytes == dbl_bytes && align == kAlign;
            heap().release(p, dbl_bytes, dbl_bytes / 2);
            dbl = nullptr;
            return;
        }
        if (misalign || doubled_aligned) {
            auto* const b = static_cast<std::byte*>(p);
            std::size_t off;
            std::memcpy(&off, b - sizeof off, sizeof off);
            heap().release(b - off, bytes + 2 * kMisalignPad, kAlign);
            return;
        }
        heap().release(p, bytes, align);
    }

    bool refuse = false;   /**< @brief Refuse every draw while set. */
    bool misalign = false; /**< @brief Ignore the alignment asked, and never meet it. */
    /** @brief Misalign every slab draw, but hand the doubled fallback out slab-ALIGNED. */
    bool doubled_aligned = false;
    void* dbl = nullptr;                /**< @brief The live doubled block, in that mode. */
    std::size_t dbl_bytes = 0;          /**< @brief Its size as drawn. */
    bool dbl_released_as_drawn = false; /**< @brief It came back at the size and alignment drawn. */
    std::size_t draws = 0;              /**< @brief Draws served. */
    std::size_t releases = 0;           /**< @brief Releases seen. */
    std::size_t last_bytes = 0;         /**< @brief The last draw's size. */
    std::size_t last_align = 0;         /**< @brief The last draw's alignment. */
    std::size_t live = 0;               /**< @brief Bytes drawn and not yet released. */

   private:
    static constexpr std::size_t kAlign = alignof(std::max_align_t);
    static constexpr std::size_t kMisalignPad = 2 * kAlign;
    static tr::mem::block_source_t& heap() noexcept { return tr::mem::heap_source(); }

    /** @brief A block of @p bytes that is NOT @p align-aligned, its offset stored before it. */
    static void* misaligned(std::size_t bytes, std::size_t align) noexcept {
        auto* const b = static_cast<std::byte*>(heap().try_alloc(bytes + 2 * kMisalignPad, kAlign));
        if (b == nullptr) return nullptr;
        std::size_t off = kMisalignPad;
        if (reinterpret_cast<std::uintptr_t>(b + off) % align == 0) off += kAlign;
        std::memcpy(b + off - sizeof off, &off, sizeof off);
        return b + off;
    }
};

/** @brief A small table, so the cases can fill slabs in a few hundred requests. */
constexpr std::array<std::size_t, 3> kRows{64, 256, 1024};
/** @brief The pool under test: per-class detail on, whatever the build's default. */
using pool_t = tr::mem::slab_pool_t<tr::graph::guard_t, kRows.size(), /*kCounters=*/true>;
/** @brief The base slab of the cases: the smallest the pool takes. */
constexpr std::size_t kBase = 4096;
constexpr std::size_t kAlign = alignof(std::max_align_t);

/** @brief Classes, slab sizes, alignment and the oversize path. */
void test_classes_and_slabs() {
    std::printf("slab pool: classes, slabs and oversize blocks:\n");
    counting_root_t root;
    {
        pool_t pool("t", std::span<const std::size_t, 3>{kRows}, root, kBase, 1);
        check(pool.class_of(1, kAlign) == 0 && pool.class_of(64, kAlign) == 0 &&
                  pool.class_of(65, kAlign) == 1 && pool.class_of(1024, kAlign) == 2 &&
                  pool.class_of(1025, kAlign) == pool_t::kNoClass,
              "a request falls in the smallest row that holds it; past the last, no class");
        check(pool.class_of(16, 128) == pool_t::kNoClass,
              "an alignment past the slab header's is an oversize block");
        check(pool.slab_bytes(0) == kBase && pool.slab_bytes(2) == 16384,
              "a class's slab is the base doubled until it holds 8 blocks after its header");

        void* const a = pool.try_alloc(40, kAlign);
        check(
            a != nullptr && root.draws == 1 && root.last_bytes == kBase && root.last_align == kBase,
            "the first block draws ONE whole slab, aligned to its own size");
        check(a != nullptr && reinterpret_cast<std::uintptr_t>(a) % kAlign == 0,
              "and the block keeps max_align_t's alignment");
        if (a != nullptr) std::memset(a, 0x11, 64);  // the whole class block is ours
        void* const b = pool.try_alloc(64, kAlign);
        check(b != nullptr && b != a && root.draws == 1, "the second comes from the same slab");
        check(pool.stats().in_use == kBase && pool.stats().peak == kBase,
              "the census counts slab bytes: what the pool costs the node");

        void* const big = pool.try_alloc(5000, kAlign);
        check(big != nullptr && root.draws == 2 && root.last_bytes == 5008,
              "an oversize request is its own block, at its own size (16 B granules)");
        check(pool.stats().in_use == kBase, "outside the census, which counts slab bytes");
        if (big != nullptr) std::memset(big, 0x22, 5000);
        pool.release(big, 5000, kAlign);
        check(root.releases == 1 && pool.stats().in_use == kBase,
              "it goes straight back to the root when released, passing the census by");

        pool.release(a, 40, kAlign);
        pool.release(b, 64, kAlign);
        check(root.releases == 1, "the emptied slab is within the cap of 1: it stays");
        check(pool.class_stats(0).slabs == 1 && pool.class_stats(0).live == 0,
              "the class holds one slab and no live block");
        pool.trim();
        check(root.releases == 2 && root.live == 0 && pool.stats().in_use == 0,
              "trim() releases the fully free slab whatever the cap");
        check(pool.class_stats(0).released == 1, "and the class counts the release");
        check(pool.stats().peak == kBase, "the high-water mark keeps the peak");
    }
    check(root.live == 0, "the pool left nothing behind at the root");
}

/** @brief A fully free slab above the cap is released at once. */
void test_release_above_the_cap() {
    std::printf("slab pool: release above the high-water cap:\n");
    counting_root_t root;
    pool_t pool("t", std::span<const std::size_t, 3>{kRows}, root, kBase, 2);
    const std::size_t per_slab = (kBase - pool_t::kHeaderBytes) / 64;
    std::vector<void*> blocks;
    for (std::size_t i = 0; i < 4 * per_slab; ++i) blocks.push_back(pool.try_alloc(64, kAlign));
    check(std::none_of(blocks.begin(), blocks.end(), [](void* p) { return p == nullptr; }),
          "four slabs' worth of blocks are served");
    check(root.draws == 4 && pool.class_stats(0).slabs == 4 &&
              pool.class_stats(0).live == 4 * per_slab,
          "from exactly four slabs, carved full");
    for (void* p : blocks) pool.release(p, 64, kAlign);
    check(root.releases == 2 && pool.class_stats(0).slabs == 2,
          "freeing all of them releases the two slabs above the cap of 2, at once");
    check(pool.stats().in_use == 2 * kBase, "the census drops by those two slabs");
    blocks.clear();
    for (std::size_t i = 0; i < 2 * per_slab; ++i) blocks.push_back(pool.try_alloc(64, kAlign));
    check(root.draws == 4, "the retained slabs serve the next burst without a draw");
    for (void* p : blocks) pool.release(p, 64, kAlign);
    pool.trim();
    check(root.live == 0, "trim() returns the rest");
}

/** @brief A root that ignores the slab's alignment still yields a working pool. */
void test_misaligning_root() {
    std::printf("slab pool: a root that ignores the alignment it is asked for:\n");
    counting_root_t root;
    root.misalign = true;
    {
        pool_t pool("t", std::span<const std::size_t, 3>{kRows}, root, kBase, 1);
        std::vector<void*> blocks;
        for (int i = 0; i < 200; ++i) blocks.push_back(pool.try_alloc(64, kAlign));
        check(std::none_of(blocks.begin(), blocks.end(), [](void* p) { return p == nullptr; }),
              "every block is served");
        for (void* b : blocks) std::memset(b, 0x33, 64);
        check(root.last_bytes == 2 * kBase && root.last_align == kAlign,
              "a misaligned slab goes back, and is cut from a block twice its size");
        for (void* b : blocks) pool.release(b, 64, kAlign);
        check(pool.class_stats(0).live == 0, "every block finds its slab again on release");
        pool.trim();
    }
    check(root.live == 0, "and every block the root handed out went back to it");
}

/** @brief A doubled fallback block that happens to be slab-aligned goes back as drawn. */
void test_doubled_block_aligned_by_chance() {
    std::printf("slab pool: a doubled block the root aligned to the slab anyway:\n");
    counting_root_t root;
    root.doubled_aligned = true;
    {
        pool_t pool("t", std::span<const std::size_t, 3>{kRows}, root, kBase, 1);
        void* const a = pool.try_alloc(64, kAlign);
        check(a != nullptr && root.last_bytes == 2 * kBase && root.last_align == kAlign,
              "the misaligned slab draw goes back and a doubled block is drawn");
        check(root.dbl != nullptr && reinterpret_cast<std::uintptr_t>(root.dbl) % kBase == 0,
              "the root handed the doubled block out slab-aligned");
        pool.release(a, 64, kAlign);
        pool.trim();
        check(root.dbl == nullptr && root.dbl_released_as_drawn,
              "the slab goes back at twice its size and max_align_t's alignment, as drawn");
    }
    check(root.live == 0, "and the root is square");
}

/** @brief An alloc/free pair at a slab boundary keeps its slab: the cap counts FREE slabs. */
void test_no_thrash_at_a_slab_boundary() {
    std::printf("slab pool: an alloc/free pair at a slab boundary does not thrash:\n");
    counting_root_t root;
    pool_t pool("t", std::span<const std::size_t, 3>{kRows}, root, kBase, 2);
    const std::size_t per_slab = (kBase - pool_t::kHeaderBytes) / 64;
    std::vector<void*> blocks;
    for (std::size_t i = 0; i < 3 * per_slab; ++i) blocks.push_back(pool.try_alloc(64, kAlign));
    check(root.draws == 3 && pool.class_stats(0).slabs == 3,
          "three slabs carved full: more live slabs than the cap of 2");
    for (int k = 0; k < 100; ++k) {
        void* const p = pool.try_alloc(64, kAlign);
        pool.release(p, 64, kAlign);
    }
    check(root.draws == 4 && root.releases == 0,
          "100 turns past the boundary draw ONE slab and release none");
    check(pool.class_stats(0).slabs == 4,
          "the emptied fourth slab is kept, being the only free one");
    for (void* p : blocks) pool.release(p, 64, kAlign);
    check(pool.class_stats(0).slabs == 2 && root.releases == 2,
          "freeing the rest keeps two free slabs, the cap, and releases the others");
    pool.trim();
    check(root.live == 0, "trim() returns the rest");
}

/** @brief A refusing root is a counted refusal, by value. */
void test_refusal_census() {
    std::printf("slab pool: a refusing root is a counted refusal:\n");
    counting_root_t root;
    pool_t pool("t", std::span<const std::size_t, 3>{kRows}, root, kBase, 1);
    root.refuse = true;
    check(pool.try_alloc(200, kAlign) == nullptr, "no slab, no block: nullptr, by value");
    check(pool.try_alloc(100000, kAlign) == nullptr, "an oversize request is refused the same");
    check(pool.stats().refused == 2 && pool.stats().largest_refused == 100000,
          "both are counted, and the largest is kept");
    check(pool.stats().in_use == 0, "and nothing is held");
    root.refuse = false;
    void* const p = pool.try_alloc(200, kAlign);
    check(p != nullptr && pool.stats().refused == 2, "the pool serves again once the root does");
    pool.release(p, 200, kAlign);
}

/** @brief A default graph's values come from the value sub-pool, in whole slabs only. */
void test_default_graph_sees_only_slabs() {
    std::printf("host default root: a graph's values come from whole slabs:\n");
    if constexpr (!tr::mem::kSlabPool) {
        std::printf("  (the build has no host slab pool: nothing to check)\n");
        return;
    }
    graph_t g;
    check(g.derives_sub_pools(), "a default host graph derives its sub-pools");
    // Enough vertices holding big enough values that the pool must draw fresh slabs inside
    // the window, so "only whole slabs" is not satisfied by "nothing at all".
    constexpr std::size_t kVerts = 64;
    constexpr std::array<std::size_t, 5> kSizes{16, 64, 1024, 8192, 16384};
    std::vector<tr::graph::vertex_handle_t> verts;
    for (std::size_t i = 0; i < kVerts; ++i)
        verts.push_back(
            g.register_vertex(path_t(("/v/" + std::to_string(i)).c_str()), role_t::STORED_VALUE));
    std::vector<std::vector<std::byte>> bytes;
    for (const std::size_t n : kSizes) bytes.emplace_back(n, std::byte{0x5a});
    // Warm up: twice per vertex, so a deferred reclamation slot's per-vertex bookkeeping (a
    // hazard slot allocates its retire node on the first REPLACED value) is outside the window.
    for (int pass = 0; pass < 2; ++pass)
        for (const auto v : verts) (void)g.write(v, tr::testing::make_value(bytes[0]));

    const std::size_t in_use_before = tr::mem::value_source().stats().in_use;
    g_news = 0;
    g_odd = 0;
    g_arm = true;
    for (const auto& b : bytes)
        for (const auto v : verts) (void)g.write(v, tr::testing::make_value(b));
    g_arm = false;
    const std::size_t writes = kVerts * kSizes.size();
    std::printf("    %zu platform requests over %zu writes, %zu not whole slabs (first %zu B)\n",
                g_news.load(), writes, g_odd.load(), g_odd_bytes.load());
    check(g_news.load() > 0, "the writes grew the pool: the platform allocator WAS asked");
    check(g_odd.load() == 0,
          "and every request it saw was a whole number of slabs, never a value's own size");
    check(g_news.load() < writes / 4, "far fewer requests than writes");
    check(tr::mem::value_source().stats().in_use > in_use_before,
          "the value sub-pool holds the graph's values");
    const auto read = g.read(verts.back());
    check(read.has_value() && (*read)->flatten().bytes().size() == kSizes.back(),
          "and the last value reads back whole");
}

/**
 * @brief #1990: a 64 KiB payload is a classed block on the host default table, header and all.
 *
 * The ladder's last doubling ends at 64 KiB of PAYLOAD, and the one-block segment that carries
 * such a payload is a header past it, so it fell to the root as an oversize block. The table
 * has a class for it. An oversize block passes the census uncounted, so a run of live 64 KiB
 * values moves `in_use` by their slabs only when a class serves them.
 */
void test_64k_payload_is_classed() {
    std::printf("host default root: a 64 KiB payload is a classed block (#1990):\n");
    if constexpr (!tr::mem::kSlabPool) {
        std::printf("  (the build has no host slab pool: nothing to check)\n");
        return;
    }
    constexpr std::size_t kPayload = 65536;
    constexpr std::size_t kSegment =
        tr::mem::segment_block_bytes(kPayload, tr::mem::heap_backend_t::kBlockAlign);
    tr::mem::host_root_t& root = tr::mem::host_root();
    const tr::mem::host_pool_t& pool = root.values().shared();
    check(pool.class_of(kSegment, tr::mem::heap_backend_t::kBlockAlign) !=
              tr::mem::host_pool_t::kNoClass,
          "the 64 KiB payload's one-block segment has a class on the default table");

    // More live values than the class's cap of free slabs could hold, so the slabs they take
    // are drawn inside the window whatever earlier cases left behind.
    constexpr std::size_t kVerts = 64;
    graph_t g;
    std::vector<tr::graph::vertex_handle_t> verts;
    for (std::size_t i = 0; i < kVerts; ++i)
        verts.push_back(
            g.register_vertex(path_t(("/big/" + std::to_string(i)).c_str()), role_t::STORED_VALUE));
    const std::vector<std::byte> bytes(kPayload, std::byte{0x6b});
    const std::size_t before = root.values().stats().in_use;
    bool wrote = true;
    for (const auto v : verts)
        wrote = g.write(v, tr::testing::make_value(bytes)).has_value() && wrote;
    const std::size_t grew = root.values().stats().in_use - before;
    std::printf("    %zu live 64 KiB values: the value sub-pool's slabs grew by %zu B\n", kVerts,
                grew);
    check(wrote, "every 64 KiB write is accepted");
    check(grew >= kVerts / 2 * kSegment,
          "and the value sub-pool holds them in its slabs, not as oversize blocks of the root");
    const auto read = g.read(verts.back());
    check(read.has_value() && (*read)->flatten().bytes().size() == kPayload,
          "and the last value reads back whole");
}

/** @brief Concurrent writers and cross-thread frees through the thread caches. */
void test_concurrent_writers() {
    std::printf("host default root: concurrent writers and cross-thread frees:\n");
    constexpr int kThreads = 8;
    constexpr int kRounds = 2000;
    graph_t g;
    std::vector<tr::graph::vertex_handle_t> own;
    for (int t = 0; t < kThreads; ++t)
        own.push_back(
            g.register_vertex(path_t(("/w/" + std::to_string(t)).c_str()), role_t::STORED_VALUE));
    const auto shared = g.register_vertex(path_t("/w/shared"), role_t::STORED_VALUE);

    std::mutex mu;
    std::vector<void*> handoff;  // blocks one thread draws and another frees
    std::atomic<int> torn{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            tr::mem::block_source_t& values = tr::mem::value_source();
            std::vector<std::byte> b(16 + static_cast<std::size_t>(t) * 300,
                                     std::byte(static_cast<unsigned>(t)));
            for (int i = 0; i < kRounds; ++i) {
                (void)g.write(own[static_cast<std::size_t>(t)], tr::testing::make_value(b));
                if (i % 4 == 0) (void)g.write(shared, tr::testing::make_value(b));
                void* const p = values.try_alloc(b.size(), alignof(std::max_align_t));
                if (p != nullptr) std::memset(p, t, b.size());
                std::vector<void*> mine;
                {
                    const std::lock_guard lock(mu);
                    if (p != nullptr) handoff.push_back(p);
                    if (handoff.size() > 16) mine.swap(handoff);
                }
                // Freed here, whoever drew them: a block of another thread's size goes back
                // through THIS thread's cache, of ITS class.
                for (void* q : mine) {
                    const auto first = static_cast<unsigned char*>(q)[0];
                    const std::size_t n = 16 + static_cast<std::size_t>(first) * 300;
                    if (first >= kThreads) torn.fetch_add(1, std::memory_order_relaxed);
                    values.release(q, n, alignof(std::max_align_t));
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    for (void* q : handoff) {
        const auto first = static_cast<unsigned char*>(q)[0];
        tr::mem::value_source().release(q, 16 + static_cast<std::size_t>(first) * 300,
                                        alignof(std::max_align_t));
    }
    check(torn.load() == 0, "no handed-off block was overwritten while another thread held it");
    bool intact = true;
    for (int t = 0; t < kThreads; ++t) {
        const auto r = g.read(own[static_cast<std::size_t>(t)]);
        const auto flat = r ? (*r)->flatten() : tr::view::view_t{};
        const auto s = flat.bytes();
        intact = intact && s.size() == 16 + static_cast<std::size_t>(t) * 300 &&
                 std::all_of(s.begin(), s.end(),
                             [t](std::byte x) { return x == std::byte(static_cast<unsigned>(t)); });
    }
    check(intact, "every writer's last value reads back whole and unmixed");
    check(tr::mem::value_source().stats().refused == 0, "and the pool refused nothing");
}

/** @brief A power-of-two ladder, 16 B to 1 KiB: the table a peer-sized seam rounds into. */
constexpr auto kPow2 = tr::graph::size_class_ladder_t<16, 1, 1024>::kTable;
/** @brief The bounded slab both arms of the peer-size case draw from. */
constexpr std::size_t kPeerSlab = 256 * 1024;
/** @brief Requests in the peer-size case. */
constexpr std::size_t kPeerOps = 200000;
/** @brief The most blocks the peer-size case keeps live at once. */
constexpr std::size_t kPeerLive = 64;

/** @brief One live block of the peer-size case. */
struct peer_block_t {
    void* p;           /**< @brief The block. */
    std::size_t bytes; /**< @brief What the peer asked for. */
    std::byte tag;     /**< @brief The byte it was filled with. */
};

/** @brief What the peer-size case saw. */
struct peer_run_t {
    std::size_t allocs = 0;  /**< @brief Requests made. */
    std::size_t refused = 0; /**< @brief Of those, answered `nullptr`. */
};

/**
 * @brief Drive @p src with a peer-random size distribution (1 B to 1 KiB, uniform, up to
 *        @ref kPeerLive live), checking every block is still whole when freed.
 *
 * @param rounding Called after each step with the bytes the live blocks lose to rounding,
 *                 as the test computes them from @p class_bytes; may be empty.
 * @return The requests made and refused.
 */
template <typename Src, typename ClassBytes, typename OnStep>
peer_run_t drive_peer_sizes(Src& src, ClassBytes class_bytes, OnStep on_step) {
    std::uint64_t x = 0x9E3779B97F4A7C15ULL;
    const auto next = [&x] {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    std::array<peer_block_t, kPeerLive> live{};
    std::size_t n = 0;
    peer_run_t run;
    std::size_t rounding = 0;
    bool whole = true;
    for (std::size_t op = 0; op < kPeerOps; ++op) {
        const std::uint64_t r = next();
        if (n < kPeerLive && (n == 0 || (r & 1U) != 0)) {
            const std::size_t bytes = 1 + static_cast<std::size_t>((r >> 8) % 1024);
            void* const p = src.try_alloc(bytes, kAlign);
            ++run.allocs;
            if (p == nullptr) {
                ++run.refused;
            } else {
                const auto tag = static_cast<std::byte>(op);
                std::memset(p, static_cast<int>(tag), bytes);
                live[n++] = {p, bytes, tag};
                rounding += class_bytes(bytes) - bytes;
            }
        } else {
            const std::size_t k = static_cast<std::size_t>(r >> 8) % n;
            const peer_block_t b = live[k];
            const auto* const q = static_cast<const std::byte*>(b.p);
            whole = whole && q[0] == b.tag && q[b.bytes - 1] == b.tag;
            src.release(b.p, b.bytes, kAlign);
            rounding -= class_bytes(b.bytes) - b.bytes;
            live[k] = live[--n];
        }
        on_step(rounding);
    }
    for (std::size_t k = 0; k < n; ++k) src.release(live[k].p, live[k].bytes, kAlign);
    check(whole, "every block reads back whole when freed: no two blocks overlap");
    return run;
}

/** @brief #1646: peer-chosen sizes over a bounded root, laddered versus exact-size. */
void test_ladder_over_a_bounded_root() {
    std::printf("slab pool: a ladder over a bounded root, peer-chosen sizes (#1646):\n");
    using ladder_t = tr::mem::slab_pool_t<tr::no_guard_t, kPow2.size(), /*kCounters=*/true>;
    alignas(16384) static std::byte slab[kPeerSlab];

    std::size_t draws = 0;
    {
        std::array<tr::mem::size_class_t, 8> roots{};
        tr::mem::pool_source_t<> root{std::span<std::byte>(slab), roots};
        ladder_t pool("peer", std::span<const std::size_t, kPow2.size()>{kPow2}, root, kBase, 1);
        const auto class_bytes = [&pool](std::size_t n) {
            return pool.class_bytes(pool.class_of(n, kAlign));
        };
        bool rounding_matches = true;
        const auto census = [&](std::size_t expect) {
            std::size_t sum = 0;
            for (std::size_t i = 0; i < kPow2.size(); ++i) sum += pool.class_stats(i).rounding;
            rounding_matches = rounding_matches && sum == expect;
        };
        const peer_run_t run = drive_peer_sizes(pool, class_bytes, census);
        for (std::size_t i = 0; i < kPow2.size(); ++i) {
            const tr::mem::slab_class_stats_t c = pool.class_stats(i);
            draws += c.slabs + c.released;
            std::printf("    class %5zu B: %zu slab(s) held, %zu released\n", c.bytes, c.slabs,
                        c.released);
        }
        std::printf(
            "    ladder: %zu of %zu requests refused, %zu missed to the root (a slab "
            "draw); root carved %zu of %zu B in %zu exact classes, %zu overflowed\n",
            run.refused, run.allocs, draws, root.used(), kPeerSlab, root.classes_used(),
            root.overflowed());
        check(run.refused == 0 && pool.stats().refused == 0,
              "the ladder serves every peer-sized request from the bounded slab");
        check(draws * 100 < run.allocs, "and misses to the root are bounded: under 1 %");
        check(root.used() <= kPeerSlab / 2, "the root carves under half its slab, then recycles");
        check(root.classes_used() <= 3 && root.overflowed() == 0,
              "the root sees only slab sizes, so its exact classes stay degenerate");
        check(rounding_matches, "class_stats().rounding is the bytes lost to rounding, live");
        for (std::size_t i = 0; i < kPow2.size(); ++i) {
            if (pool.class_stats(i).live != 0 || pool.class_stats(i).rounding != 0)
                rounding_matches = false;
        }
        check(rounding_matches, "and every class reads zero once its blocks are back");
    }
    {
        std::array<tr::mem::size_class_t, 64> classes{};
        tr::mem::pool_source_t<> exact{std::span<std::byte>(slab), classes};
        const peer_run_t run =
            drive_peer_sizes(exact, [](std::size_t n) { return n; }, [](std::size_t) {});
        std::printf(
            "    exact-size alone: %zu of %zu requests refused, %zu overflowed, %zu "
            "classes\n",
            run.refused, run.allocs, exact.overflowed(), exact.classes_used());
        check(run.refused * 2 > run.allocs,
              "the exact-size pool alone on the same slab refuses most peer sizes");
    }
}

}  // namespace

int main() {
    std::printf("#1777 — the size-classed slab pool and the host default root\n\n");
    test_classes_and_slabs();
    test_release_above_the_cap();
    test_misaligning_root();
    test_doubled_block_aligned_by_chance();
    test_no_thrash_at_a_slab_boundary();
    test_refusal_census();
    test_default_graph_sees_only_slabs();
    test_64k_payload_is_classed();
    test_concurrent_writers();
    test_ladder_over_a_bounded_root();
    return tr::testing::summary("slab_pool");
}
