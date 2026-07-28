/**
 * @file
 * @brief Unit tests for the bounded RECYCLING block source (`tr::mem::pool_source_t`, #597).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The gap this closes is specific: `heap_source_t` recycles but is unbounded, and
 * `bump_source_t` is bounded but never recycles, so a LONG-LIVED seam (a graph's control
 * source, a receiver's terminus arena) could not be bounded at all. The measured symptom
 * was an 8 KiB bump source wired as a router's `rx` decoding six frames and rejecting the
 * next 194. The load-bearing test here is therefore @ref long_lived_seam_survives — the
 * one a bump source fails.
 *
 * The rest pin the properties the header-free scheme rests on: sized reclaim means a block
 * carries no header, so N blocks of size S fit in exactly N×S bytes; and a recycled block
 * is only ever handed back out for the exact `(bytes, align)` it was carved for.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/mem_source_sync.hpp"

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

using tr::mem::pool_source_t;
using tr::mem::size_class_t;

/** @brief A slab plus its class table, sized by the caller exactly as a deployment would. */
template <std::size_t Bytes, std::size_t Classes>
struct fixture_t {
    alignas(std::max_align_t) std::byte slab[Bytes]{};
    size_class_t classes[Classes]{};
    /** @brief The source under test, wired to this fixture's storage. */
    pool_source_t<> src{std::span<std::byte>(slab), std::span<size_class_t>(classes)};
};

/**
 * @brief THE regression: a long-lived seam must survive far more work than it has slab.
 *
 * A bump source of this size serves `Bytes / block` allocations and then refuses forever.
 * A recycling one serves an unbounded number, because `used()` stops growing once every
 * live block has been carved once.
 */
void long_lived_seam_survives() {
    std::printf("long-lived seam — recycling, not monotonic fill:\n");
    fixture_t<512, 4> f;

    bool all_served = true;
    for (int i = 0; i < 10000; ++i) {
        void* a = f.src.try_alloc(64, 8);
        void* b = f.src.try_alloc(64, 8);
        if (a == nullptr || b == nullptr) {
            all_served = false;
            break;
        }
        f.src.release(a, 64, 8);
        f.src.release(b, 64, 8);
    }
    check(all_served, "10,000 alloc/release rounds on a 512 B slab all served");
    check(f.src.used() == 128, "used() settled at the 2-block high-water, not 10,000 × 64");
    check(f.src.overflowed() == 0, "no block lost — one class covered the whole run");
}

/** @brief A released block is the next one handed out (LIFO reuse of the exact shape). */
void recycles_the_same_block() {
    std::printf("\nrecycling identity:\n");
    fixture_t<256, 4> f;

    void* const first = f.src.try_alloc(32, 8);
    f.src.release(first, 32, 8);
    void* const again = f.src.try_alloc(32, 8);
    check(first == again, "release then alloc of the same shape returns the same block");
    check(f.src.used() == 32, "reuse carved nothing new");

    void* const other = f.src.try_alloc(32, 8);
    check(other != nullptr && other != first, "a second live block is distinct");
}

/** @brief Sized reclaim means no per-block header: N blocks of S fit in exactly N×S. */
void carries_no_header() {
    std::printf("\nheader-free: exact packing:\n");
    fixture_t<256, 4> f;

    int served = 0;
    while (f.src.try_alloc(32, 8) != nullptr) ++served;
    check(served == 8, "a 256 B slab served exactly 8 × 32 B blocks (no header, no rounding)");
    check(f.src.used() == 256, "the slab is exactly full");
    check(f.src.try_alloc(32, 8) == nullptr, "exhaustion is nullptr, never the platform heap");
}

/** @brief Alignment is part of the class key — a block is never reused at a stricter one. */
void alignment_is_part_of_the_key() {
    std::printf("\nalignment:\n");
    fixture_t<512, 8> f;

    void* const wide = f.src.try_alloc(32, 64);
    check(wide != nullptr, "an over-aligned request is served");
    check(reinterpret_cast<std::uintptr_t>(wide) % 64 == 0, "and is actually 64-aligned");
    f.src.release(wide, 32, 64);

    void* const narrow = f.src.try_alloc(32, 8);
    check(narrow != wide,
          "the freed align=64 block is NOT recycled for an align=8 request (distinct class)");

    void* const wide_again = f.src.try_alloc(32, 64);
    check(wide_again == wide, "but the align=64 class does recycle its own block");
}

/**
 * @brief The documented trade-off, asserted rather than left to prose: classes do not share.
 *
 * This is what costs the measured +11.1 % against the peak-live floor, and it is the one
 * thing a coalescing allocator does better. Pinning it here means the next reader meets the
 * limitation as a test, not as a surprise.
 */
void classes_do_not_share() {
    std::printf("\nsize classes are segregated (the measured trade-off):\n");
    fixture_t<192, 4> f;

    void* const small = f.src.try_alloc(64, 8);
    f.src.release(small, 64, 8);
    check(f.src.try_alloc(128, 8) != nullptr, "a 128 B request is carved fresh");
    check(f.src.used() == 192, "the freed 64 B block could not serve it — both are carved");
}

/** @brief A full class table loses blocks but never corrupts, and says so. */
void class_table_overflow_is_safe() {
    std::printf("\nclass-table overflow:\n");
    fixture_t<1024, 2> f;

    void* const a = f.src.try_alloc(16, 8);
    void* const b = f.src.try_alloc(32, 8);
    void* const c = f.src.try_alloc(48, 8);
    f.src.release(a, 16, 8);
    f.src.release(b, 32, 8);
    check(f.src.classes_used() == 2, "the table filled at its injected capacity");
    check(f.src.overflowed() == 0, "and nothing was lost yet");

    f.src.release(c, 48, 8);
    check(f.src.overflowed() == 1, "the third shape is counted as lost, not written anywhere");
    check(f.src.try_alloc(16, 8) == a, "the recorded classes still recycle correctly");
    check(f.src.try_alloc(64, 8) != nullptr, "and the source keeps serving fresh blocks");
}

/** @brief A pointer this source never handed out is ignored, not linked into a free list. */
void foreign_pointer_is_ignored() {
    std::printf("\nforeign pointer:\n");
    fixture_t<128, 4> f;
    std::byte elsewhere[64]{};

    f.src.release(elsewhere, 32, 8);
    check(f.src.classes_used() == 0, "a pointer outside the slab creates no class");

    void* const p = f.src.try_alloc(32, 8);
    check(p != nullptr && p != static_cast<void*>(elsewhere),
          "and is never handed back out as if it were ours");
}

/** @brief `classes_used()` is the number a deployment sizes its span against. */
void reports_what_to_size_against() {
    std::printf("\ndiagnostics:\n");
    fixture_t<1024, 8> f;

    void* const a = f.src.try_alloc(64, 8);
    void* const b = f.src.try_alloc(128, 8);
    void* const c = f.src.try_alloc(64, 8);
    f.src.release(a, 64, 8);
    f.src.release(b, 128, 8);
    f.src.release(c, 64, 8);
    check(f.src.classes_used() == 2, "two distinct shapes => two classes, not three blocks");
    check(f.src.overflowed() == 0, "an adequately sized table loses nothing");
}

/** @brief The hosted policy compiles, guards, and leaves behaviour identical. */
void hosted_sync_policy_works() {
    std::printf("\nsynchronization policy:\n");
    alignas(std::max_align_t) std::byte slab[256]{};
    size_class_t classes[4]{};
    pool_source_t<tr::mem::sync_mutex_t> src{std::span<std::byte>(slab),
                                             std::span<size_class_t>(classes)};

    void* const p = src.try_alloc(32, 8);
    src.release(p, 32, 8);
    check(src.try_alloc(32, 8) == p, "a locking policy changes nothing about the semantics");
    check(sizeof(pool_source_t<tr::mem::sync_mutex_t>) > sizeof(pool_source_t<>),
          "and the lock is only paid for where it is asked for");
}

}  // namespace

/** @brief The default policy must cost nothing — the MCU contract for this seam. */
static_assert(std::is_empty_v<tr::mem::sync_none_t>,
              "sync_none_t must be empty so [[no_unique_address]] can erase it");

/** @brief Exhaustion is reported by value; nothing here may throw. */
static_assert(noexcept(std::declval<pool_source_t<>&>().try_alloc(1, 1)),
              "try_alloc is part of the nothrow seam");

int main() {
    std::printf("pool_source_t — bounded, recycling block source (#597)\n\n");
    long_lived_seam_survives();
    recycles_the_same_block();
    carries_no_header();
    alignment_is_part_of_the_key();
    classes_do_not_share();
    class_table_overflow_is_safe();
    foreign_pointer_is_ignored();
    reports_what_to_size_against();
    hosted_sync_policy_works();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
