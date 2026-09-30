/**
 * @file
 * @brief RFC-0028 slice 7 — one callback idiom (D10), one read type (D11), and a HANDLER
 *        target that adopts the published value like a stored target.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The gates, each asserted here:
 *
 *   - `sizeof(handlers_t) <= 96` and `sizeof(value_handlers_t) <= 48` on a 64-bit host: six and
 *     three `{fn, ctx}` hooks, where they were `std::function`s of 32 B each (216 B / 96 B);
 *   - `graph_t::history` fills caller storage and allocates NOTHING;
 *   - a HANDLER target's `on_write` is handed the very block the source published — its
 *     address, not a clone — and K handler targets cost the publish no allocation beyond the
 *     one a zero-target publish costs;
 *   - the lifetime rule on the seam: a handler that keeps the value past its call takes
 *     `value_ref_t::keep(value)`, which SHARES a published block and COPIES the links out of
 *     stack storage. The kept values are read after the storage they came from is gone, so an
 *     ASan build turns a wrong answer into a use-after-free report rather than a lucky pass.
 *
 * Allocation counts are taken RELATIVE to a baseline publish on the same graph shape, never
 * as absolute numbers, and no assertion reads a reference count: the hazard-slot binding
 * (`LIBTRACER_LKV_SLOT=hazard_slot_t`) allocates a node per publish and defers reclamation,
 * so both would differ by slot policy while the properties under test do not.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

// --- the counting allocator (every variant, so no block escapes the count) -------------------

namespace {

std::atomic<long long> g_allocs{0};
std::atomic<bool> g_armed{false};

void* counted(std::size_t n) {
    void* p = std::malloc(n != 0 ? n : 1);
    if (p != nullptr && g_armed.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    return p;
}

}  // namespace

void* operator new(std::size_t n) {
    void* p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    void* p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new(std::size_t n, std::align_val_t) { return operator new(n); }
void* operator new[](std::size_t n, std::align_val_t) { return operator new(n); }
void* operator new(std::size_t n, std::align_val_t, const std::nothrow_t&) noexcept {
    return counted(n);
}
void* operator new[](std::size_t n, std::align_val_t, const std::nothrow_t&) noexcept {
    return counted(n);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::handlers_t;
using tr::graph::hook_t;
using tr::graph::path_t;
using tr::graph::result_t;
using tr::graph::role_t;
using tr::graph::value_ref_t;
using tr::graph::value_t;
using tr::graph::vertex_handle_t;
using tr::graph::write_ctx_t;
using tr::testing::check;
using tr::testing::make_value;

/** @brief Allocations made while @p fn runs. */
template <class Fn>
long long allocs_during(Fn&& fn) {
    g_allocs.store(0, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_seq_cst);
    fn();
    g_armed.store(false, std::memory_order_seq_cst);
    return g_allocs.load(std::memory_order_relaxed);
}

/** @brief The first byte of a one-link value (0 when it has none). */
std::uint8_t first_byte(const value_t& v) {
    if (v.link_count() == 0 || v.total_length() == 0) return 0;
    return std::to_integer<std::uint8_t>(v.links()[0].bytes()[0]);
}

// ---------------------------------------------------------------------------------------------

void test_sizes() {
    std::printf("D10 — the seam structs are hook-sized:\n");
    static_assert(sizeof(hook_t<void()>) == 2 * sizeof(void*), "a hook is {fn, ctx}");
    static_assert(std::is_trivially_copyable_v<handlers_t>, "copying seams allocates nothing");
    static_assert(std::is_trivially_destructible_v<tr::graph::value_handlers_t>,
                  "freeing a parked seam block runs no user code");
    if constexpr (sizeof(void*) == 8) {
        static_assert(sizeof(handlers_t) <= 96, "RFC-0028 §5.7: handlers_t 216 -> 96 B");
        static_assert(sizeof(tr::graph::value_handlers_t) <= 48,
                      "RFC-0028 §5.7: value_handlers_t 96 -> 48 B");
    }
    std::printf("  sizeof(handlers_t) = %zu, sizeof(value_handlers_t) = %zu\n", sizeof(handlers_t),
                sizeof(tr::graph::value_handlers_t));
    check(true, "the size gates hold at compile time");
}

void test_history_allocates_nothing() {
    std::printf("D11 — history fills caller storage with no allocation:\n");
    graph_t g;
    const vertex_handle_t s = g.register_vertex(path_t("/h/s"), role_t::STREAM);
    (void)g.set_policy(s, {.retention = tr::graph::retention_t::N, .depth = 4});
    for (std::uint8_t i = 1; i <= 6; ++i) (void)g.write(s, make_value({i}));

    std::array<value_ref_t, 4> ring;
    result_t<std::size_t> n = std::unexpected(tr::graph::status_t::NOT_FOUND);
    const long long a = allocs_during([&] { n = g.history(s, ring); });
    check(a == 0, "history() allocated nothing");
    check(n.has_value() && *n == 4, "... and filled the whole declared depth");
    check(n && first_byte(*ring[0]) == 3 && first_byte(*ring[3]) == 6,
          "... oldest first, the two oldest writes trimmed");

    std::array<value_ref_t, 2> short_span;
    const auto m = g.history(s, short_span);
    check(m && *m == 2 && first_byte(*short_span[0]) == 5 && first_byte(*short_span[1]) == 6,
          "a short span takes the NEWEST entries, still oldest first");
    check(g.history(g.register_vertex(path_t("/h/v"), role_t::STORED_VALUE), ring).error() ==
              tr::graph::status_t::SCHEMA_NOT_FOUND,
          "a non-STREAM still answers SCHEMA_NOT_FOUND");
}

/** @brief What a recording handler saw and kept. */
struct recorder_t {
    const value_t* seen = nullptr; /**< @brief The address `on_write` was handed. */
    bool seen_owned = false;       /**< @brief Whether that value had a source (a block). */
    value_ref_t kept;              /**< @brief `value_ref_t::keep` of the last value. */
    int writes = 0;                /**< @brief Deliveries received. */
};

/** @brief The recording `on_write`: note the address, keep the value the sanctioned way. */
result_t<void> record(void* ctx, const value_t& v, const write_ctx_t&) {
    auto& r = *static_cast<recorder_t*>(ctx);
    r.seen = &v;
    r.seen_owned = v.source() != nullptr;
    r.kept = value_ref_t::keep(v);
    ++r.writes;
    return {};
}

void test_handler_target_adopts() {
    std::printf("D2 for handlers — a HANDLER target is handed the published block itself:\n");
    constexpr std::size_t kHandlers = 8;
    std::array<recorder_t, kHandlers> rec{};
    graph_t g;
    const vertex_handle_t src = g.register_vertex(path_t("/a/src"), role_t::STORED_VALUE);
    const vertex_handle_t lone = g.register_vertex(path_t("/a/lone"), role_t::STORED_VALUE);
    for (std::size_t i = 0; i < kHandlers; ++i) {
        handlers_t h;
        h.on_write = {&record, &rec[i]};
        const path_t t("/a/h" + std::to_string(i));
        (void)g.register_vertex(t, role_t::HANDLER, h);
        (void)g.subscribe(path_t("/a/src"), t);
    }
    // Warm both vertices once so no one-off (edge snapshot buffers, ext blocks) is counted.
    (void)g.write(src, make_value({0x01}));
    (void)g.write(lone, make_value({0x01}));

    const tr::view::view_t v0 = make_value({0x10});
    const tr::view::view_t v1 = make_value({0x11});
    const long long bare = allocs_during([&] { (void)g.write(lone, v0); });
    const long long fanned = allocs_during([&] { (void)g.write(src, v1); });
    check(fanned == bare, "K handler targets cost the publish no allocation beyond a bare one");

    const auto published = g.read(src);
    bool all_same = published.has_value();
    for (const recorder_t& r : rec)
        all_same = all_same && r.writes == 2 && r.seen == published->get() && r.seen_owned;
    check(all_same, "every handler was handed the source's published block — its address");
    bool all_shared = true;
    for (const recorder_t& r : rec) all_shared = all_shared && r.kept == published->get();
    check(all_shared, "value_ref_t::keep on a published block SHARES it (same block)");

    // The kept references outlive the source's own reference to that block.
    (void)g.write(src, make_value({0x12}));
    (void)g.write(src, make_value({0x13}));
    check(rec[0].kept.get() != nullptr && first_byte(*rec[0].kept) == 0x13,
          "keep() tracks the latest delivery");
    value_ref_t held = rec[3].kept;
    (void)g.write(src, make_value({0x14}));
    check(first_byte(*held) == 0x13, "a reference kept past two later publishes still reads");
}

void test_local_write_keep_copies_out_of_the_stack() {
    std::printf("the lifetime rule — keep() copies the links out of stack storage:\n");
    recorder_t rec;
    graph_t g;
    handlers_t h;
    h.on_write = {&record, &rec};
    const vertex_handle_t v = g.register_vertex(path_t("/l/h"), role_t::HANDLER, h);
    // A local write to a HANDLER hands `on_write` storage on the writer's stack: the value has
    // no source, and its address is dead the moment the write returns.
    check(g.write(v, make_value({0x42, 0x43})).has_value(), "the local write lands");
    check(rec.writes == 1 && !rec.seen_owned, "the handler was handed caller-owned storage");
    check(rec.kept && rec.kept.get() != rec.seen && rec.kept->source() != nullptr,
          "keep() minted a block of its own rather than pointing at the stack");
    // Read AFTER the stack frame is gone: under ASan a kept stack address would report here.
    check(rec.kept->total_length() == 2 && first_byte(*rec.kept) == 0x42,
          "... and the kept value reads back intact after the write returned");
}

void test_thunk_forms() {
    std::printf("tr::graph::thunk — the three spellings:\n");
    graph_t g;
    // A stateless callable needs nothing alive, so a temporary is fine.
    handlers_t h;
    h.on_children = tr::graph::thunk([]() -> result_t<tr::view::view_t> {
        return std::unexpected(tr::graph::status_t::NOT_FOUND);
    });
    check(h.on_children.ctx == nullptr, "a stateless thunk carries a null ctx");
    // A capturing callable is referenced, so it must be a named object.
    int reads = 0;
    auto on_read = [&reads]() -> result_t<tr::view::rope_t> {
        ++reads;
        return tr::view::rope_t{make_value({0x07})};
    };
    h.on_read = tr::graph::thunk(on_read);
    check(h.on_read.ctx == &on_read, "a capturing thunk points at the callable");
    const vertex_handle_t v = g.register_vertex(path_t("/t/h"), role_t::HANDLER, h);
    const auto r = g.read(v);
    check(r && first_byte(**r) == 0x07 && reads == 1, "the hook calls through to the lambda");
    // And the two-word aggregate, the form a seam over an owned object uses.
    const hook_t<int(int)> twice{[](void* c, int x) { return x * *static_cast<int*>(c); }, &reads};
    check(twice(21) == 21 && static_cast<bool>(twice), "{fn, ctx} is directly callable");
}

}  // namespace

int main() {
    test_sizes();
    test_history_allocates_nothing();
    test_handler_target_adopts();
    test_local_write_keep_copies_out_of_the_stack();
    test_thunk_forms();
    return tr::testing::summary("one_idiom");
}
