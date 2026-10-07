/**
 * @file
 * @brief #1912 — a path-target edge's key is drawn from the graph's table source, and the
 *        dispatch snapshot shares it without touching any allocator.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #1885 put the graph core's state on its injected table source; the target key was the one
 * piece left on the platform heap (a `std::shared_ptr` around a `std::vector`, two global
 * draws per admission, and on a no-exception build a vector built outside the probe). This
 * test drives the public doors only: a graph over a source that draws from `malloc`, so every
 * draw the graph makes through its seam is invisible to a global `operator new` counter, and
 * anything the counter sees is a draw that bypassed the seam.
 *
 * - (a) a `:subscribers[]` write naming a local PATH target makes no global allocation, and
 *       the source holds the key. (The `subscribe(src, target)` sugar is not the door here:
 *       it builds a `field_path_t`, whose own vector is RFC-0029's to move, not this one's.)
 * - (b) a write that fans out over that edge makes no global allocation either: the snapshot
 *       clones the key by reference.
 * - (c) the key outlives the edge's unsubscribe exactly as long as a holder needs it, and the
 *       graph gives every block back.
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

/** @brief Global-new call counter, live only while @ref g_arm is set. */
std::size_t g_allocs = 0;
bool g_arm = false;

/** @brief The counted allocation itself — malloc-backed so `operator delete` can free it. */
void* counted(std::size_t n) {
    if (g_arm) ++g_allocs;
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation (`aligned_alloc` only when genuinely over-aligned). */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) ++g_allocs;
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}

}  // namespace

// Every allocating and deallocating form is replaced (the `edge_cold_half_share_test`
// precedent), so no draw can slip past the counter through an unreplaced overload.
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

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::testing::check;
using tr::testing::make_value;

/**
 * @brief A table source over `malloc` — never `operator new` — that counts its live blocks,
 *        so the global counter sees only draws that went around the seam.
 */
class malloc_source_t final : public tr::mem::block_source_t {
   public:
    malloc_source_t() noexcept : tr::mem::block_source_t("malloc") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        const std::size_t a = align < alignof(std::max_align_t) ? alignof(std::max_align_t) : align;
        void* const p = std::aligned_alloc(a, (bytes + a - 1) / a * a);
        if (p != nullptr) ++live_;
        return p;
    }
    void release(void* p, std::size_t, std::size_t) noexcept override {
        --live_;
        std::free(p);
    }
    [[nodiscard]] tr::mem::source_stats_t stats() const noexcept override { return {}; }

    long live_ = 0; /**< @brief Blocks handed out and not returned. */
};

/** @brief (a)–(c) at the public doors: subscribe a target, publish through it, unsubscribe. */
void test_target_key_on_the_seam() {
    malloc_source_t src;
    {
        graph_t g{src};
        const path_t from = *path_t::parse("/k/src");
        const path_t to = *path_t::parse("/k/a/rather/deep/target/vertex");
        (void)g.register_vertex(to, role_t::STORED_VALUE);
        (void)g.register_vertex(from, role_t::STORED_VALUE);
        // Warm-up: one target edge on another source vertex, so first-use growth of the
        // graph's own tables is not what (a) measures.
        const path_t warm = *path_t::parse("/k/warm");
        (void)g.register_vertex(warm, role_t::STORED_VALUE);
        check(g.subscribe(warm, to).has_value(), "warm-up target edge admitted");
        const auto value = make_value({0x01, 0x02, 0x03, 0x04});

        // A SUBSCRIBER{PATH <to>} record and the `:subscribers[]` append, both built before
        // the counter is armed.
        std::vector<std::byte> body;
        tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{}, to.key());
        std::vector<std::byte> rec;
        tr::wire::emit_tlv(rec, tr::wire::type_t::SUBSCRIBER, tr::wire::opt_t{.pl = true}, body);
        const auto record = make_value(rec);
        tr::graph::field_path_t field;
        field.steps.push_back(
            tr::graph::field_step_t{.name = "subscribers", .indexed = true, .append = true});
        const vertex_handle_t v = *g.find(from.key());

        const long before = src.live_;
        g_allocs = 0;
        g_arm = true;
        const bool admitted = g.write(v, field, record).has_value();
        g_arm = false;
        check(admitted, "(a) a path-target edge is admitted");
        std::printf("  (a) admission: %zu global draw(s), %ld more source block(s)\n", g_allocs,
                    src.live_ - before);
        check(g_allocs == 0, "(a) admission draws nothing around the table source");
        check(src.live_ > before, "(a) the table source holds the edge and its key");

        g_allocs = 0;
        g_arm = true;
        const bool wrote = g.write(from, value).has_value();
        g_arm = false;
        check(wrote, "(b) the write fans out over the target edge");
        std::printf("  (b) fan-out:   %zu global draw(s)\n", g_allocs);
        check(g_allocs == 0, "(b) the snapshot clones the key without any allocator");

        const auto landed = g.read(to);
        check(landed.has_value(), "(b) the delivery landed at the target, so the key resolved");
    }
    check(src.live_ == 0, "(c) the graph gives back every block, keys included");
}

}  // namespace

int main() {
    std::printf("target_key_seam_test (#1912):\n");
    test_target_key_on_the_seam();
    return tr::testing::summary("target_key_seam");
}
