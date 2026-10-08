/**
 * @file
 * @brief #1991 — a vertex name longer than `path_key_t`'s inline bytes is drawn from the
 *        graph's injected source, so a registration takes nothing from the global heap.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The name record of `/net/tcp/conn-192-168-1-50-47301` is 24 bytes, past the 16 a key holds
 * in place, and it used to spill to a bare `new std::byte[]`: the one 24 B block per
 * registration `bench_forward_heap`'s `reg_escape` row read. The tests drive public doors
 * only, over a source that draws from `malloc`, so a global `operator new` counter sees only
 * what went around the seam.
 *
 * - (a) `path_key_t::try_make` puts a long key on the given source and a short one in place;
 *       a copy draws from the same source; the source gets every block back.
 * - (b) `try_make` answers BACKPRESSURE when the source refuses, and holds nothing.
 * - (c) a runtime registration with a long name makes no global allocation, and the graph
 *       gives every block back at teardown.
 * - (d) every refusal a registration can meet, the name's draw included, is a BACKPRESSURE
 *       value that registers nothing and leaks nothing.
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <vector>

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
using tr::graph::path_key_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::testing::check;

/**
 * @brief A source over `malloc` — never `operator new` — that counts its live blocks and
 *        refuses every draw once @ref budget_ is spent.
 */
class malloc_source_t final : public tr::mem::block_source_t {
   public:
    malloc_source_t() noexcept : tr::mem::block_source_t("malloc") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (budget_ == 0) return nullptr;
        if (budget_ > 0) --budget_;
        const std::size_t a = align < alignof(std::max_align_t) ? alignof(std::max_align_t) : align;
        void* const p = std::aligned_alloc(a, (bytes + a - 1) / a * a);
        if (p != nullptr) ++live_;
        return p;
    }
    void release(void* p, std::size_t, std::size_t) noexcept override {
        --live_;
        std::free(p);
    }

    long live_ = 0;    /**< @brief Blocks handed out and not returned. */
    long budget_ = -1; /**< @brief Draws left before refusing; negative means unlimited. */
};

/** @brief The canonical key of @p text (built before any counter is armed). */
std::vector<std::byte> key_of(const char* text) {
    const auto p = path_t::parse(text);
    return {p->key().begin(), p->key().end()};
}

/** @brief The bytes of @p s as a byte span. */
std::span<const std::byte> bytes_of(const char* s) {
    return std::as_bytes(std::span<const char>{s, std::strlen(s)});
}

/** @brief (a) and (b): the key itself, at its factory. */
void test_key_on_the_seam() {
    malloc_source_t src;
    const char* const long_name = "conn-192-168-1-50-47301";  // 23 B > kInlineBytes
    const char* const short_name = "temp";
    {
        g_allocs = 0;
        g_arm = true;
        auto longk = path_key_t::try_make(bytes_of(long_name), src);
        auto shortk = path_key_t::try_make(bytes_of(short_name), src);
        g_arm = false;
        check(longk.has_value() && shortk.has_value(), "(a) both keys are made");
        check(g_allocs == 0, "(a) neither key draws from the global heap");
        check(src.live_ == 1, "(a) the long key holds one source block, the short one none");
        check(std::ranges::equal(longk->bytes(), bytes_of(long_name)), "(a) long key bytes");
        check(std::ranges::equal(shortk->bytes(), bytes_of(short_name)), "(a) short key bytes");

        g_arm = true;
        const path_key_t copy = *longk;
        const path_key_t moved = std::move(*longk);
        g_arm = false;
        check(g_allocs == 0, "(a) copy and move draw nothing from the global heap");
        check(src.live_ == 2, "(a) the copy draws from the original's source; a move draws none");
        check(copy == moved && longk->empty(), "(a) copy equals, moved-from reads empty");
    }
    check(src.live_ == 0, "(a) every spill block goes back to its source");

    src.budget_ = 0;
    const auto refused = path_key_t::try_make(bytes_of(long_name), src);
    check(!refused && refused.error() == status_t::BACKPRESSURE,
          "(b) a refused spill draw is BACKPRESSURE");
    const auto inline_ok = path_key_t::try_make(bytes_of(short_name), src);
    check(inline_ok.has_value(), "(b) an inline key needs no draw, so a dry source still makes it");
    check(src.live_ == 0, "(b) a refused key holds nothing");
}

/** @brief (c): a runtime registration with a long name, at the graph's public door. */
void test_registration_on_the_seam() {
    malloc_source_t src;
    {
        graph_t g{src};
        (void)g.register_vertex(*path_t::parse("/net/tcp"), role_t::STORED_VALUE);
        // Warm-up sibling: the parent's first-child table is first-use growth, not this.
        (void)g.register_vertex(*path_t::parse("/net/tcp/conn-192-168-1-50-00000"),
                                role_t::STORED_VALUE);
        const std::vector<std::byte> key = key_of("/net/tcp/conn-192-168-1-50-47301");
        const long before = src.live_;
        g_allocs = 0;
        g_arm = true;
        const bool ok = g.register_vertex_key(key, role_t::STORED_VALUE).has_value();
        g_arm = false;
        check(ok, "(c) the long-named vertex registers");
        std::printf("  (c) registration: %zu global draw(s), %ld more source block(s)\n", g_allocs,
                    src.live_ - before);
        check(g_allocs == 0, "(c) a registration draws nothing around the injected source");
        check(src.live_ - before >= 2, "(c) the source holds the vertex and its name");
        check(g.find(key).has_value(), "(c) the vertex is found by its long name");
    }
    check(src.live_ == 0, "(c) the graph gives back every block, names included");
}

/** @brief (d): every refusal point of a long-named registration answers by value. */
void test_every_refusal_is_a_value() {
    const std::vector<std::byte> key = key_of("/net/tcp/conn-192-168-1-50-47301");
    bool registered_once = false;
    for (long budget = 0; budget < 32 && !registered_once; ++budget) {
        malloc_source_t src;
        {
            graph_t g{src};
            (void)g.register_vertex(*path_t::parse("/net/tcp"), role_t::STORED_VALUE);
            src.budget_ = budget;
            const auto r = g.register_vertex_key(key, role_t::STORED_VALUE);
            src.budget_ = -1;
            if (r) {
                registered_once = true;
            } else {
                check(r.error() == status_t::BACKPRESSURE, "(d) a refusal is BACKPRESSURE");
                // `find` sees registered vertices only; placeholders a descent left are
                // invisible, which is the registration contract.
                check(!g.find(key).has_value(), "(d) a refusal registers nothing");
            }
        }
        check(src.live_ == 0, "(d) a refused registration leaks nothing");
    }
    check(registered_once, "(d) a large enough budget registers the vertex");
}

}  // namespace

int main() {
    std::printf("path_key_seam_test (#1991):\n");
    test_key_on_the_seam();
    test_registration_on_the_seam();
    test_every_refusal_is_a_value();
    return tr::testing::summary("path_key_seam");
}
