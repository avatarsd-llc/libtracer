/**
 * @file
 * @brief #1991, #2672 — a vertex's name is drawn from the graph's injected source, into the
 *        graph's name block, so a registration takes nothing from the global heap.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A vertex references its NAME record in the graph's name block, chunks the graph carves from
 * its table source (#2672). Before, a record past 16 bytes, such as the 24 B record of
 * `/net/tcp/conn-192-168-1-50-47301`, spilled to a bare `new std::byte[]` (until #1991) and
 * then to the table source. The tests drive public doors only, over a source that draws from
 * `malloc`, so a global `operator new` counter sees only what went around the seam.
 *
 * - (a) a refused creation gives its name back: creations refused at the vertex draw, after
 *       the name was carved, leave the name block where it was instead of filling chunks.
 * - (c) a runtime registration with a long name makes no global allocation, and the graph
 *       gives every block back at teardown.
 * - (d) every refusal a registration can meet, the name's draw included, is a BACKPRESSURE
 *       value that registers nothing and leaks nothing.
 * - (e) a reader that renders keys with no lock (`key_of`) and reads the slot memo
 *       (`vertex_slot`) while a writer keeps registering, and so keeps carving new chunks, sees
 *       every name whole. Run under ThreadSanitizer, this is the proof that a name is valid
 *       under the existing reclamation rules: the bytes are written before the vertex is linked
 *       and never again.
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <thread>
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
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::testing::check;

/**
 * @brief A source over `malloc` — never `operator new` — that counts its live blocks and
 *        refuses every draw once @ref budget_ is spent, and every draw of @ref refuse_bytes_.
 */
class malloc_source_t final : public tr::mem::block_source_t {
   public:
    malloc_source_t() noexcept : tr::mem::block_source_t("malloc") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (budget_ == 0 || bytes == refuse_bytes_) return nullptr;
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
    std::size_t refuse_bytes_ = 0; /**< @brief A draw size always refused; 0 refuses none. */
};

/** @brief The canonical key of @p text (built before any counter is armed). */
std::vector<std::byte> key_of(const char* text) {
    const auto p = path_t::parse(text);
    return {p->key().begin(), p->key().end()};
}

/** @brief (a): a creation refused after its name was carved gives the name back. */
void test_refused_creation_gives_the_name_back() {
    malloc_source_t src;
    {
        graph_t g{src};
        (void)g.register_vertex(*path_t::parse("/net/tcp"), role_t::STORED_VALUE);
        (void)g.register_vertex(*path_t::parse("/net/tcp/conn-192-168-1-50-00000"),
                                role_t::STORED_VALUE);
        const long before = src.live_;
        // Only the vertex's own draw is refused, so each attempt carves its name first.
        src.refuse_bytes_ = sizeof(tr::graph::vertex_t);
        bool all_refused = true;
        for (int i = 0; i < 1000; ++i) {
            char text[64];
            std::snprintf(text, sizeof text, "/net/tcp/conn-192-168-1-50-%05d", i + 1);
            const auto r = g.register_vertex_key(key_of(text), role_t::STORED_VALUE);
            all_refused = all_refused && !r && r.error() == status_t::BACKPRESSURE;
        }
        src.refuse_bytes_ = 0;
        check(all_refused, "(a) every creation refused at the vertex draw is BACKPRESSURE");
        std::printf("  (a) 1000 refused creations: %ld more source block(s)\n", src.live_ - before);
        check(src.live_ - before <= 1,
              "(a) the refused names went back: at most one new chunk, not one per 256 B");
        check(
            g.register_vertex_key(key_of("/net/tcp/conn-192-168-1-50-00001"), role_t::STORED_VALUE)
                .has_value(),
            "(a) the same name registers once the source serves again");
    }
    check(src.live_ == 0, "(a) the graph gives back every block, names included");
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
        check(src.live_ - before >= 1, "(c) the source holds the vertex (its name, a chunk)");
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

/** @brief (e): lock-free name readers against a writer that keeps carving chunks. */
void test_readers_see_whole_names() {
    constexpr std::size_t kNames = 3000;
    graph_t g{tr::mem::heap_source()};
    (void)g.register_vertex(*path_t::parse("/n"), role_t::STORED_VALUE);
    std::vector<std::vector<std::byte>> keys(kNames);
    for (std::size_t i = 0; i < kNames; ++i) {
        char text[64];
        std::snprintf(text, sizeof text, "/n/a-name-of-some-length-%05zu", i);
        keys[i] = key_of(text);
    }
    std::vector<std::optional<tr::graph::vertex_handle_t>> handles(kNames);
    std::atomic<std::size_t> published{0};
    std::thread writer([&] {
        for (std::size_t i = 0; i < kNames; ++i) {
            const auto h = g.register_vertex_key(keys[i], role_t::STORED_VALUE);
            if (!h) return;
            handles[i] = *h;
            published.store(i + 1, std::memory_order_release);
        }
    });
    bool whole = true;
    bool slotted = true;
    std::size_t reads = 0;
    tr::mem::bytes_t out(tr::mem::heap_source());
    for (std::size_t seen = 0; seen < kNames;) {
        seen = published.load(std::memory_order_acquire);
        for (std::size_t i = seen > 64 ? seen - 64 : 0; i < seen; ++i) {
            whole = whole && g.key_of(*handles[i], out).has_value() &&
                    std::ranges::equal(tr::mem::as_span(out), keys[i]);
            slotted = slotted && g.vertex_slot(*handles[i]).has_value();
            ++reads;
        }
    }
    writer.join();
    std::printf("  (e) %zu lock-free key renders against %zu registrations\n", reads, kNames);
    check(published.load() == kNames, "(e) every name registers");
    check(whole, "(e) every key a reader rendered is the registered name, byte for byte");
    check(slotted, "(e) every slot memo a reader read resolves");
}

}  // namespace

int main() {
    std::printf("name_block_seam_test (#1991, #2672):\n");
    test_refused_creation_gives_the_name_back();
    test_registration_on_the_seam();
    test_every_refusal_is_a_value();
    test_readers_see_whole_names();
    return tr::testing::summary("name_block_seam");
}
