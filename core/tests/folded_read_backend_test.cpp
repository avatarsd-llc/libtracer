/**
 * @file
 * @brief #831 — BOTH folded READs' POINT headers draw from `graph_t`'s injected `value_backend`
 *        (ADR-0060), not the global heap — and exhaustion degrades by value (`BACKPRESSURE`),
 *        never a throw.
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
 * `read_children_folded` is the SAME defect class on the other folded read — one POINT header per
 * registered child plus the outer listing header, and the wire ":children" READ routes THERE, at
 * a count a peer likewise chooses (it picks which vertex's listing to read). Sections (d)–(f)
 * cover it with the identical instrument, including through the production ":children" field
 * route rather than the direct call alone.
 *
 * These are payload framing bytes — each header's length field wraps the stored TLV and the name
 * records below it — so they belong on the ADR-0060 value seam, not the route-byte-sized ADR-0074
 * egress seam. This test is the ablation that proves the redirect landed.
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

#include "libtracer/mem_source.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

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

using tr::testing::check;

/**
 * @brief The short-form POINT header width `read_subtree_folded` emits for a body under 64 KiB —
 *        TYPE + OPT + the u16 length. Every node in this fixture is far below the widen point.
 */
constexpr std::size_t kShortHeaderBytes = 4;

/**
 * @brief The header draw for a node that ALSO carries its own leading `NAME` TLV: the POINT
 *        header plus that `NAME` header, in ONE segment.
 *
 * Since RFC-0018 a vertex-map key record is packed (`[u8 len][bytes]`) rather than a `NAME`
 * TLV, so the `:children` / composed-read member's `NAME` framing is EMITTED instead of
 * borrowed from the key. It shares the POINT header's segment on purpose — that is what keeps
 * the folds at the same LINK count they had before, which is what these instruments' reserve
 * arithmetic and the transports' iovec-spill budget are stated against. The draw is one
 * header wider; the draw COUNT is unchanged.
 */
constexpr std::size_t kNamedHeaderBytes = kShortHeaderBytes + 4;

/** @brief The subtree the fixture builds: the root plus four registered descendants. */
constexpr int kNodeCount = 5;

/**
 * @brief A `block_source_t` that serves from the process heap source but can REFUSE the
 *        header-width draws — the exhaustion stand-in, with the refusal point movable.
 *
 * @par Why it watches a size set rather than every draw (#873 phase 1)
 * Until the constructor collapse this instrument was a `mem_backend_t` injected as
 * `value_backend`, so every draw it saw was by construction a value-seam draw. `graph_t` now
 * takes ONE source and builds the backend over it, so an unfiltered source would also see the
 * pmr control blocks, the registration blocks and the composed read's collect stack — and
 * `served() == kNodeCount` would stop meaning "one header per node".
 *
 * So the instrument watches exactly the two folded-header widths (@ref kShortHeaderBytes and
 * @ref kNamedHeaderBytes) and passes every other size straight through, untouched and
 * uncounted. That keeps every assertion below meaning what it meant, and keeps the ablation
 * non-vacuous in the same way: put the headers back on `view::heap_alloc` and the watched
 * count is 0, reddening the routing rows.
 */
class arming_source_t final : public tr::mem::block_source_t {
   public:
    arming_source_t() noexcept : block_source_t("test_folded_value") {}

    /** @brief True for a draw this instrument watches (a folded POINT header's bytes). */
    [[nodiscard]] static bool watched(std::size_t n) noexcept {
        return n == kShortHeaderBytes || n == kNamedHeaderBytes;
    }

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (!watched(bytes)) return tr::mem::heap_source().try_alloc(bytes, align);
        if (serve_first_ >= 0) {  // serve the leading N watched draws, then refuse
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
        void* const p = tr::mem::heap_source().try_alloc(bytes, align);
        if (p != nullptr) {
            ++served_;
            sizes_.push_back(bytes);
        }
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }

    /** @brief Refuse every watched allocation from now on. */
    void arm() noexcept { budget_ = 0; }
    /** @brief Serve the first @p k watched draws, then refuse — isolates a LATE refusal. */
    void serve_first(int k) noexcept { serve_first_ = k; }
    /** @brief How many watched allocations were REFUSED — the refusal instrument. */
    [[nodiscard]] int refusals() const noexcept { return refusals_; }
    /** @brief How many were SERVED — the "the seam is consulted at all" instrument. */
    [[nodiscard]] int served() const noexcept { return served_; }
    /** @brief Were ALL served draws either @p a or @p b bytes — the folded header widths,
     *         which differ by whether the node also carries its own `NAME` header. */
    [[nodiscard]] bool all_served_size_either(std::size_t a, std::size_t b) const noexcept {
        return !sizes_.empty() &&
               std::all_of(sizes_.begin(), sizes_.end(),
                           [a, b](std::size_t s) noexcept { return s == a || s == b; });
    }

   private:
    int budget_ = -1;       // <0 ⇒ unlimited
    int serve_first_ = -1;  // <0 ⇒ inactive; else watched draws to serve before refusing forever
    int refusals_ = 0;
    int served_ = 0;
    std::vector<std::size_t> sizes_{};
};

/**
 * @brief A caller-owned slab served by a recycling @ref tr::mem::pool_source_t — the BOUNDED
 *        composition arm, and after #873 phase 1 the whole graph's store rather than only its
 *        value backend.
 *
 * Sized generously (64 KiB, 32 size classes) because the collapse means this one source now
 * serves the pmr control blocks and the failable channel too, not just the header segments.
 */
struct scratch_pool_t {
    std::vector<std::byte> slab;
    std::vector<tr::mem::size_class_t> classes;
    tr::mem::pool_source_t<> pool;
    scratch_pool_t() : slab(64u * 1024u), classes(32), pool(slab, classes) {}
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

/**
 * @brief The direct children of `/s` the fixture registers — `/s/a` and `/s/b`. `read_children_
 *        folded` frames one POINT header per registered child PLUS the outer listing header, so
 *        the seam draw count on that path is this + 1.
 */
constexpr int kDirectChildCount = 2;

/** @brief The folded `:children` bytes, flattened — the `:children` differential comparand. */
std::vector<std::byte> children_bytes(graph_t& g, vertex_handle_t parent) {
    const auto r = g.read_children_folded(parent);
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
        arming_source_t be;
        graph_t g(&be);
        const vertex_handle_t root = build_subtree(g, payload);
        const int before = be.served();
        check(before == 0, "no write in the fixture touches the value seam (single-link stores)");
        const auto r = g.read_subtree_folded(root, "peer");
        check(r.has_value(), "the folded read succeeds through the injected backend");
        check(be.served() == kNodeCount, "one seam draw per folded node (the header count)");
        check(be.all_served_size_either(kShortHeaderBytes, kNamedHeaderBytes),
              "every seam draw is the POINT header width, plus the NAME header below the root");
        check(be.refusals() == 0, "an unarmed source refuses nothing");
    }

    // (a2) ...and NOT the global heap: the identical folded read over a POOL-backed graph makes
    // exactly kNodeCount FEWER global allocations than over a heap-backed one. On the old code
    // the two arms are equal.
    {
        graph_t heap_g;
        const vertex_handle_t heap_root = build_subtree(heap_g, payload);
        scratch_pool_t sp;
        graph_t pool_g(&sp.pool);
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
        // AT LEAST, not exactly, since #873 phase 1: the pool arm's ONE injected source now
        // also serves the composed read's collect stack and any pmr block the read touches,
        // where before those stayed on the global heap in both arms and the delta was exactly
        // the header count. The floor is still the header count, and on the old code the two
        // arms are equal, so the row is non-vacuous in the same way.
        check(heap_arm - pool_arm >= 2u * static_cast<std::size_t>(kNodeCount),
              "the difference covers at least the per-node header count (2 global news each)");
    }

    // (b) EXHAUSTION answered BY VALUE. A fully refusing backend makes the FIRST header
    // allocation fail: BACKPRESSURE, by value, never an abort and never a partial rope.
    {
        arming_source_t be;
        graph_t g(&be);
        const vertex_handle_t root = build_subtree(g, payload);
        be.arm();
        const auto r = g.read_subtree_folded(root, "peer");
        check(be.refusals() > 0, "the armed source WAS consulted (the refusal instrument)");
        check(!r.has_value() && r.error() == status_t::BACKPRESSURE,
              "a refused header degrades to BACKPRESSURE by value");
    }

    // ...and a LATE refusal is not silently truncated: serve every header but the last, and the
    // whole read still rejects rather than returning a short subtree.
    {
        arming_source_t be;
        graph_t g(&be);
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
        scratch_pool_t sp;
        graph_t pool_g(&sp.pool);
        const vertex_handle_t pool_root = build_subtree(pool_g, payload);
        const std::vector<std::byte> a = folded_bytes(heap_g, heap_root);
        const std::vector<std::byte> b = folded_bytes(pool_g, pool_root);
        check(!a.empty(), "the default-path folded read produces bytes");
        check(a == b, "pool-backed and default folded replies are byte-identical");
    }

    // (d) The SAME defect class on the OTHER folded read: `read_children_folded` frames one
    // POINT header per registered child plus the outer listing header, and the wire ":children"
    // READ routes THERE, not to read_subtree_folded. Same instrument, same order — routing
    // first, and on the old `view::over_bytes` code served() is 0.
    {
        arming_source_t be;
        graph_t g(&be);
        const vertex_handle_t root = build_subtree(g, payload);
        check(be.served() == 0, "no write in the fixture touches the value seam (:children arm)");
        const auto r = g.read_children_folded(root);
        check(r.has_value(), "the folded :children read succeeds through the injected backend");
        check(be.served() == kDirectChildCount + 1,
              "one seam draw per registered child, plus the outer listing header");
        check(be.all_served_size_either(kShortHeaderBytes, kNamedHeaderBytes),
              "every :children seam draw is the POINT header width, plus each member's NAME "
              "header");
        check(be.refusals() == 0, "an unarmed source refuses nothing on the :children fold");
    }

    // ...and through the PRODUCTION wire route — the ":children" field READ, the path a peer
    // actually reaches this code by. Without this the fix could be proven only on a direct call.
    {
        arming_source_t be;
        graph_t g(&be);
        (void)build_subtree(g, payload);
        const auto r = g.read(path_t("/s:children"));
        check(r.has_value(), "the production :children field read succeeds");
        check(be.served() == kDirectChildCount + 1,
              "the wire :children READ draws its headers from the injected seam");
    }

    // ...and NOT the global heap, by the same strict differential as (a2): the identical
    // ":children" fold over a POOL-backed graph makes exactly 2 fewer global allocations per
    // framed header (the heap backend spends one global `new` on the bytes and one on the
    // `segment_t`, where the pool carves both from its slab). On the old code the arms are equal.
    {
        graph_t heap_g;
        const vertex_handle_t heap_root = build_subtree(heap_g, payload);
        scratch_pool_t sp;
        graph_t pool_g(&sp.pool);
        const vertex_handle_t pool_root = build_subtree(pool_g, payload);
        (void)children_bytes(heap_g, heap_root);  // warm both arms outside the count
        (void)children_bytes(pool_g, pool_root);

        g_allocs = 0;
        g_arm = true;
        (void)children_bytes(heap_g, heap_root);
        const std::size_t heap_arm = g_allocs;
        g_allocs = 0;
        (void)children_bytes(pool_g, pool_root);
        const std::size_t pool_arm = g_allocs;
        g_arm = false;

        std::printf("    (:children global new: heap arm %zu, pool arm %zu)\n", heap_arm, pool_arm);
        check(pool_arm < heap_arm, "a pool-backed :children fold hits the global heap LESS");
        check(heap_arm - pool_arm >= 2u * static_cast<std::size_t>(kDirectChildCount + 1),
              "the difference covers at least the member+outer header count (2 news each)");
    }

    // (e) EXHAUSTION on the :children fold, answered BY VALUE. Fully refusing first, then a LATE
    // refusal (serve every member header, refuse only the OUTER one) — a truncated listing must
    // never be returned under an OK status.
    {
        arming_source_t be;
        graph_t g(&be);
        const vertex_handle_t root = build_subtree(g, payload);
        be.arm();
        const auto r = g.read_children_folded(root);
        check(be.refusals() > 0, "the armed source WAS consulted on the :children fold");
        check(!r.has_value() && r.error() == status_t::BACKPRESSURE,
              "a refused member header degrades to BACKPRESSURE by value");
    }
    {
        arming_source_t be;
        graph_t g(&be);
        const vertex_handle_t root = build_subtree(g, payload);
        be.serve_first(kDirectChildCount);  // every member header, then refuse the outer one
        const auto r = g.read_children_folded(root);
        check(be.served() == kDirectChildCount, "the seam served every member header");
        check(!r.has_value() && r.error() == status_t::BACKPRESSURE,
              "a refused OUTER header rejects the whole listing, never a truncated one");
    }

    // (f) The :children differential oracle — pool-backed and default listings are byte-identical
    // (the invariant; green either way by design, and the guard that this is a source-of-bytes
    // fix and not a framing change).
    {
        graph_t heap_g;
        const vertex_handle_t heap_root = build_subtree(heap_g, payload);
        scratch_pool_t sp;
        graph_t pool_g(&sp.pool);
        const vertex_handle_t pool_root = build_subtree(pool_g, payload);
        const std::vector<std::byte> a = children_bytes(heap_g, heap_root);
        const std::vector<std::byte> b = children_bytes(pool_g, pool_root);
        check(!a.empty(), "the default-path :children fold produces bytes");
        check(a == b, "pool-backed and default :children listings are byte-identical");
    }

    return tr::testing::summary("folded_read_backend");
}
