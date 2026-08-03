/**
 * @file
 * @brief #831 — the composed-root folded READ's per-node POINT headers draw from `graph_t`'s
 *        injected `value_backend` (ADR-0060), not the global heap — and exhaustion degrades by
 *        value (`BACKPRESSURE`), never a throw.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `read_subtree_folded`'s pass-3 emit frames one exactly-sized OWNED POINT header per included
 * subtree node. It was built by `view::heap_alloc`, hard-wired to `mem::heap_backend()` — so an
 * ADR-0067-class node with `mr`, `ctl`, `value_backend`, `flat`, `egress` and its transport
 * backend all pointed at one slab still leaked this framing to `malloc`, at a count a PEER
 * chooses (it picks which composed root to READ, and thus how many nodes fold).
 *
 * These are payload framing bytes — each header's length field wraps that node's stored TLV and
 * the name record below it — so they belong on the ADR-0060 value seam, not the route-byte-sized
 * ADR-0074 egress seam. This test is the ablation that proves the redirect landed.
 *
 * @section instrument The instrument is checked before the guard is
 *
 * "Draws from the seam" is non-vacuous only against the old code: with the headers back on
 * `view::heap_alloc`, the injected backend is asked for NOTHING on this path — no value written
 * here is multi-link, so the seam's other two (write-path flatten) sites never fire. Every armed
 * case therefore asserts a POSITIVE instrument first:
 *
 *   - (a) `served() == node_count` and every served size is exactly the 4-byte header — revert
 *     the redirect and `served()` is 0, reddening both;
 *   - (a2) a strict global-`new` differential: a POOL-backed graph makes exactly `2 * node_count`
 *     FEWER global allocations across the identical folded read than a heap-backed one (the heap
 *     backend spends one global `new` on the bytes and one on the `segment_t` per header, where
 *     the pool carves both from its slab). On the old code the two arms are equal and this
 *     reddens;
 *   - (b) exhaustion answered BY VALUE — a fully refusing backend yields `BACKPRESSURE` (not an
 *     abort, not a partial rope), and refusing only the LAST header still rejects the whole
 *     read. On the old code the refusing backend is never consulted and the read SUCCEEDS,
 *     reddening both;
 *   - (c) the default/un-injected path is byte-identical to the pool-backed one (the ADR-0053
 *     differential oracle) — this one is the *invariant*, and stays green either way by design.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/mem_pool.hpp"
#include "libtracer/tracer.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. */
std::size_t g_allocs = 0;
bool g_arm = false;

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) ++g_allocs;
    return std::malloc(n == 0 ? 1 : n);
}

/**
 * @brief The aligned counted allocation — `aligned_alloc` for a genuinely OVER-aligned request
 *        (which `free` accepts), `malloc` for a fundamental one.
 */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) ++g_allocs;
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

}  // namespace

// Every allocating and deallocating form is replaced (the `terminus_egress_backend_test`
// precedent): the heap backend allocates through `::operator new(bytes, std::nothrow)`, so a hole
// in the set would make a header INVISIBLE to the counter, and a missing delete form is an
// `alloc-dealloc-mismatch` under ASan.
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
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::view::rope_t;
using tr::view::view_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/**
 * @brief The short-form POINT header width `read_subtree_folded` emits for a body under 64 KiB —
 *        TYPE + OPT + the u16 length. Every node in this fixture is far below the widen point.
 */
constexpr std::size_t kShortHeaderBytes = 4;

/** @brief The subtree the fixture builds: the root plus four registered descendants. */
constexpr int kNodeCount = 5;

/**
 * @brief A `mem_backend_t` that serves from an upstream backend until its budget runs out, then
 *        refuses — the exhaustion stand-in, with the refusal point movable.
 *
 * Delegation, not a private allocator (the `terminus_egress_backend_test` precedent): a served
 * segment is the upstream's own, so it reclaims exactly as an un-injected graph's would. The ONLY
 * difference between serving and refusing is the `nullptr`.
 */
class arming_backend_t final : public tr::mem::mem_backend_t {
   public:
    explicit arming_backend_t(tr::mem::mem_backend_t& upstream = tr::mem::heap_backend()) noexcept
        : mem_backend_t("test_folded_value"), up_(upstream) {}

    [[nodiscard]] tr::view::segment_t* alloc(
        std::size_t size, tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        if (serve_first_ >= 0) {  // serve the leading N draws, then refuse
            if (serve_first_ == 0) {
                ++refusals_;
                return nullptr;
            }
            --serve_first_;
        } else if (budget_ == 0) {
            ++refusals_;
            return nullptr;
        } else if (budget_ > 0) {
            --budget_;
        }
        tr::view::segment_t* const seg = up_.alloc(size, hint);
        if (seg != nullptr) {
            ++served_;
            sizes_.push_back(size);
        }
        return seg;
    }
    void destroy(tr::view::segment_t* seg) noexcept override { up_.destroy(seg); }
    [[nodiscard]] std::size_t alignment() const noexcept override { return up_.alignment(); }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override {
        return up_.max_segment_size();
    }

    /** @brief Refuse every allocation from now on. */
    void arm() noexcept { budget_ = 0; }
    /** @brief Serve the first @p k draws, then refuse — isolates a LATE header refusal. */
    void serve_first(int k) noexcept { serve_first_ = k; }
    /** @brief How many allocations were REFUSED — the refusal instrument. */
    [[nodiscard]] int refusals() const noexcept { return refusals_; }
    /** @brief How many were SERVED — the "the seam is consulted at all" instrument. */
    [[nodiscard]] int served() const noexcept { return served_; }
    /** @brief Were ALL served draws exactly @p n bytes (the site-specific instrument)? */
    [[nodiscard]] bool all_served_size(std::size_t n) const noexcept {
        return !sizes_.empty() && std::all_of(sizes_.begin(), sizes_.end(),
                                              [n](std::size_t s) noexcept { return s == n; });
    }

   private:
    tr::mem::mem_backend_t& up_;
    int budget_ = -1;       // <0 ⇒ unlimited
    int serve_first_ = -1;  // <0 ⇒ inactive; else draws to serve before refusing forever
    int refusals_ = 0;
    int served_ = 0;
    std::vector<std::size_t> sizes_{};
};

/** @brief Carve a caller-owned slab into a pool of @p slot-byte slots. */
struct scratch_pool_t {
    std::vector<std::byte> slab;
    tr::mem::pool_t pool;
    explicit scratch_pool_t(std::size_t slot, std::size_t slots = 64)
        : slab(slots * (sizeof(tr::view::segment_t) + slot + 64)), pool(slab, slot) {}
};

/**
 * @brief Build the fixture subtree under @p g and return the composed ROOT's handle.
 *
 * `/s` (root) with `/s/a`, `/s/b`, `/s/a/x`, `/s/a/y` — @ref kNodeCount registered vertices, each
 * carrying a SINGLE-link stored value so no write ever touches the value seam's flatten sites.
 * That is what makes the header count the ONLY thing the seam sees on a folded read.
 */
vertex_handle_t build_subtree(graph_t& g, std::span<const std::byte> payload) {
    const vertex_handle_t root = g.register_vertex(path_t("/s"), role_t::STORED_VALUE);
    for (const char* p : {"/s/a", "/s/b", "/s/a/x", "/s/a/y"}) {
        const vertex_handle_t v = g.register_vertex(path_t(p), role_t::STORED_VALUE);
        (void)g.write(v, rope_t{view_t::over(tr::view::borrow_const(payload))});
    }
    (void)g.write(root, rope_t{view_t::over(tr::view::borrow_const(payload))});
    return root;
}

/** @brief The folded read's bytes, flattened — the differential oracle's comparand. */
std::vector<std::byte> folded_bytes(graph_t& g, vertex_handle_t root) {
    const auto r = g.read_subtree_folded(root, "peer");
    if (!r) return {};
    const view_t flat = r->flatten();
    const std::span<const std::byte> b = flat.bytes();
    return std::vector<std::byte>(b.begin(), b.end());
}

}  // namespace

/** @brief Run the #831 folded-READ value-seam ablation. */
int main() {
    std::printf("folded READ POINT headers draw from value_backend (#831, ADR-0060):\n");

    const std::array<std::byte, 3> payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};

    // (a) ROUTING — the injected backend is asked for exactly one header per node, each of
    // exactly the short-form header width. Checked BEFORE anything else: on the old
    // `view::heap_alloc` code the backend is asked for nothing and served() is 0.
    {
        arming_backend_t be;
        graph_t g(std::pmr::get_default_resource(), &be);
        const vertex_handle_t root = build_subtree(g, payload);
        const int before = be.served();
        check(before == 0, "no write in the fixture touches the value seam (single-link stores)");
        const auto r = g.read_subtree_folded(root, "peer");
        check(r.has_value(), "the folded read succeeds through the injected backend");
        check(be.served() == kNodeCount, "one seam draw per folded node (the header count)");
        check(be.all_served_size(kShortHeaderBytes),
              "every seam draw is exactly the POINT header width");
        check(be.refusals() == 0, "an unarmed backend refuses nothing");
    }

    // (a2) ...and NOT the global heap: the identical folded read over a POOL-backed graph makes
    // exactly kNodeCount FEWER global allocations than over a heap-backed one. On the old code
    // the two arms are equal.
    {
        graph_t heap_g;
        const vertex_handle_t heap_root = build_subtree(heap_g, payload);
        scratch_pool_t sp(/*slot=*/64);
        graph_t pool_g(std::pmr::get_default_resource(), &sp.pool);
        const vertex_handle_t pool_root = build_subtree(pool_g, payload);
        // Warm both paths once outside the count so no first-touch lazily-built state is
        // attributed to either arm.
        (void)folded_bytes(heap_g, heap_root);
        (void)folded_bytes(pool_g, pool_root);

        g_allocs = 0;
        g_arm = true;
        (void)folded_bytes(heap_g, heap_root);
        const std::size_t heap_arm = g_allocs;
        g_allocs = 0;
        (void)folded_bytes(pool_g, pool_root);
        const std::size_t pool_arm = g_allocs;
        g_arm = false;

        std::printf("    (global new: heap arm %zu, pool arm %zu)\n", heap_arm, pool_arm);
        check(pool_arm < heap_arm, "a pool-backed folded read hits the global heap LESS");
        // `heap_backend_t::alloc` makes TWO global-new calls per segment — the bytes and the
        // `segment_t` control object — where the pool carves both from its slab, so the exact
        // expected delta is 2 per node.
        check(heap_arm - pool_arm == 2u * static_cast<std::size_t>(kNodeCount),
              "the difference is exactly the per-node header count (2 global news each)");
    }

    // (b) EXHAUSTION answered BY VALUE. A fully refusing backend makes the FIRST header
    // allocation fail: BACKPRESSURE, by value, never an abort and never a partial rope.
    {
        arming_backend_t be;
        graph_t g(std::pmr::get_default_resource(), &be);
        const vertex_handle_t root = build_subtree(g, payload);
        be.arm();
        const auto r = g.read_subtree_folded(root, "peer");
        check(be.refusals() > 0, "the armed backend WAS consulted (the refusal instrument)");
        check(!r.has_value() && r.error() == status_t::BACKPRESSURE,
              "a refused header degrades to BACKPRESSURE by value");
    }

    // ...and a LATE refusal is not silently truncated: serve every header but the last, and the
    // whole read still rejects rather than returning a short subtree.
    {
        arming_backend_t be;
        graph_t g(std::pmr::get_default_resource(), &be);
        const vertex_handle_t root = build_subtree(g, payload);
        be.serve_first(kNodeCount - 1);
        const auto r = g.read_subtree_folded(root, "peer");
        check(be.served() == kNodeCount - 1, "the seam served every header but the last");
        check(!r.has_value() && r.error() == status_t::BACKPRESSURE,
              "a LATE refusal rejects the whole read, never a truncated subtree");
    }

    // (c) The ADR-0053 differential oracle: the reply bytes are identical whether the headers
    // come from the default heap or an injected pool. This is a source-of-bytes fix, not a wire
    // change.
    {
        graph_t heap_g;
        const vertex_handle_t heap_root = build_subtree(heap_g, payload);
        scratch_pool_t sp(/*slot=*/64);
        graph_t pool_g(std::pmr::get_default_resource(), &sp.pool);
        const vertex_handle_t pool_root = build_subtree(pool_g, payload);
        const std::vector<std::byte> a = folded_bytes(heap_g, heap_root);
        const std::vector<std::byte> b = folded_bytes(pool_g, pool_root);
        check(!a.empty(), "the default-path folded read produces bytes");
        check(a == b, "pool-backed and default folded replies are byte-identical");
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
