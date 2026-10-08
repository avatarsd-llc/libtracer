/**
 * @file
 * @brief The MCU default root: a static arena carved into size-classed free lists, with no heap
 *        behind it (#1783, ADR-0083 Decisions 4 and 6).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The host build's default root is the slab pool, so the arena is tested here directly, over a
 * local region, with the same types the MCU default root is made of:
 *   - a request takes the smallest class that holds it, at the alignment it asked for, and a
 *     returned block is the next one its class hands out;
 *   - the arena is carved front to back and never given back: a class keeps what it carved;
 *   - exhaustion, an oversize request and an over-aligned one are counted refusals, never an
 *     abort, and the census reports in-use bytes and the high-water mark;
 *   - the root derives three sub-pools that account separately and share one region, and
 *     serves table blocks itself.
 */

#include "libtracer/mem_arena.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "libtracer/guard.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

/** @brief A small table: four classes. */
constexpr std::array<std::size_t, 4> kClasses{16, 64, 256, 1024};

/** @brief The pool under test: unguarded, four classes. */
using pool_t = tr::mem::arena_pool_t<tr::no_guard_t, kClasses.size()>;
/** @brief Its arena. */
using arena_t = tr::mem::arena_t<tr::no_guard_t>;
/** @brief A root over the same table. */
using root_t = tr::mem::arena_root_t<tr::no_guard_t, kClasses.size()>;

/** @brief Whether @p p is aligned to @p a. */
bool aligned(const void* p, std::size_t a) { return reinterpret_cast<std::uintptr_t>(p) % a == 0; }

void classes_and_reuse() {
    std::printf("a request takes its class, and a returned block is reused:\n");
    alignas(64) std::array<std::byte, 4096> region{};
    std::array<void*, kClasses.size()> heads{};
    arena_t arena(region);
    pool_t pool("values", kClasses, heads, arena);

    check(pool.class_of(1, 16) == 0 && pool.class_of(16, 16) == 0 && pool.class_of(17, 16) == 1 &&
              pool.class_of(1024, 16) == 3,
          "the smallest class that holds the request");
    check(pool.class_of(1025, 16) == pool_t::kNoClass && pool.class_of(16, 128) == pool_t::kNoClass,
          "past the last row (oversize), or past kMaxAlign: no class");
    check(pool.class_of(16, 64) == 1, "an alignment the row does not divide moves up a class");

    void* const a = pool.try_alloc(40, 16);
    check(a != nullptr && aligned(a, 64), "a 40 B request is a 64 B block, carved at 64");
    check(pool.stats().in_use == 64 && arena.used() >= 64, "in_use counts the class bytes");
    pool.release(a, 40, 16);
    check(pool.stats().in_use == 0 && pool.stats().peak == 64, "released; the peak stays");
    const std::size_t carved = arena.used();
    void* const b = pool.try_alloc(64, 64);
    check(b == a && arena.used() == carved, "the class hands the returned block back, no carve");
    void* const c = pool.try_alloc(200, 16);
    check(c != nullptr && c != b && aligned(c, 64), "another class carves its own block");
    pool.release(b, 64, 64);
    pool.release(c, 200, 16);
}

void exhaustion_is_a_counted_refusal() {
    std::printf("the arena runs out, and says so by value:\n");
    alignas(64) std::array<std::byte, 2048> region{};
    std::array<void*, kClasses.size()> heads{};
    arena_t arena(region);
    pool_t pool("tables", kClasses, heads, arena);

    void* const big = pool.try_alloc(1024, 16);
    check(big != nullptr, "the first 1 KiB block fits");
    void* const second = pool.try_alloc(1000, 16);
    check(second != nullptr, "and the second");
    check(pool.try_alloc(900, 16) == nullptr, "the third does not: nullptr, no abort");
    check(pool.try_alloc(4096, 16) == nullptr, "an oversize request the arena cannot hold too");
    check(pool.try_alloc(16, 256) == nullptr, "and an over-aligned one");
    const tr::mem::source_stats_t s = pool.stats();
    check(s.refused == 3 && s.largest_refused == 4096, "three refusals, the largest 4096 B");
    check(s.in_use == 2048 && s.peak == 2048, "two blocks out");

    pool.release(big, 1024, 16);
    check(pool.try_alloc(16, 16) == nullptr,
          "a freed 1 KiB block does not serve another class: the arena never trims");
    void* const again = pool.try_alloc(512, 16);
    check(again == big, "it serves its own class");
    pool.release(again, 512, 16);
    pool.release(second, 1000, 16);
}

void oversize_is_recycled_by_size() {
    std::printf("a request past the last row is carved at its size and reused at that size:\n");
    alignas(64) std::array<std::byte, 8192> region{};
    std::array<void*, kClasses.size()> heads{};
    arena_t arena(region);
    pool_t pool("net", kClasses, heads, arena);

    void* const a = pool.try_alloc(2000, 16);
    check(a != nullptr && aligned(a, 64) && pool.stats().in_use == 2000,
          "a 2000 B request is its own 2000 B block");
    pool.release(a, 2000, 16);
    const std::size_t carved = arena.used();
    void* const b = pool.try_alloc(3000, 16);
    check(b != nullptr && b != a, "a different size does not take it");
    void* const c = pool.try_alloc(1990, 16);
    check(c == a, "the same rounded size does (1990 B rounds to 2000 B)");
    check(arena.used() <= carved + 64 + 3008, "only the new size was carved");
    pool.release(b, 3000, 16);
    pool.release(c, 1990, 16);
    check(pool.stats().in_use == 0, "both back");
}

void root_derives_three_sub_pools() {
    std::printf("the root: three sub-pools over one region:\n");
    alignas(64) std::array<std::byte, 4096> region{};
    std::array<void*, root_t::kHeads> heads{};
    root_t root(region, kClasses, heads);

    void* const v = root.values().try_alloc(100, 16);
    void* const t = root.try_alloc(100, 16);
    void* const n = root.net().try_alloc(10, 16);
    check(v != nullptr && t != nullptr && n != nullptr && v != t && t != n, "each draws its own");
    check(root.values().stats().in_use == 256 && root.tables().stats().in_use == 256 &&
              root.net().stats().in_use == 16,
          "and accounts its own; the root serves from the table sub-pool");
    const tr::mem::source_stats_t r = root.stats();
    check(r.capacity == region.size() && r.in_use >= 256 + 256 + 16 && r.peak == r.in_use,
          "the root's census is the arena: its size and the bytes carved");
    root.values().release(v, 100, 16);
    check(root.tables().try_alloc(100, 16) != v,
          "a value block returned is the value sub-pool's, not the tables'");
    root.release(t, 100, 16);
    root.net().release(n, 10, 16);
}

}  // namespace

int main() {
    classes_and_reuse();
    exhaustion_is_a_counted_refusal();
    oversize_is_recycled_by_size();
    root_derives_three_sub_pools();
    return tr::testing::summary("mem_arena");
}
