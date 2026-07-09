/**
 * @file
 * @brief rope_t small-buffer storage + the value-consumption accessors (ADR-0053 §6).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The acceptance gate for rope-valued vertices: a 1- or 2-link rope keeps its links
 * in INLINE storage — no heap allocation for the chain — so a rope-valued vertex slot
 * (make_shared<rope_t>) costs exactly one allocation, what the old view_t slot cost;
 * the 3rd link spills the chain to the heap. Also covers only()/materialize(), the
 * accessors a contiguous-bytes consumer calls (single-link: zero copy; multi: flatten).
 */

#include "libtracer/rope.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

#include "libtracer/mem_borrowed.hpp"
#include "libtracer/view.hpp"

namespace {

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

using tr::view::rope_t;
using tr::view::view_t;

/**
 * @brief A borrowed one-byte view over `b` (the segment wraps existing bytes; `b` must outlive the
 *        view).
 *
 * Isolates the rope's chain storage from any value allocation.
 */
view_t byte_view(std::byte& b) {
    return view_t::over(tr::view::borrow(std::span<std::byte>(&b, 1)));
}

/**
 * @brief True iff the rope keeps its links in inline small-buffer storage: the chain's data() lies
 *        within the rope object itself, so there is no heap chain allocation.
 */
bool links_inline(const rope_t& r) {
    const auto* obj = reinterpret_cast<const std::byte*>(&r);
    const auto* data = reinterpret_cast<const std::byte*>(r.links().data());
    return data >= obj && data < obj + sizeof(rope_t);
}

void test_sbo_gate() {
    std::printf("rope_t small-buffer storage (ADR-0053 §6 trivial-case cost guard):\n");
    std::array<std::byte, 4> buf{std::byte{0xA0}, std::byte{0xA1}, std::byte{0xA2},
                                 std::byte{0xA3}};

    const rope_t empty;
    check(empty.link_count() == 0 && empty.total_length() == 0, "default rope is empty");
    check(links_inline(empty), "empty rope's chain is inline (no heap alloc)");

    const rope_t one(byte_view(buf[0]));
    check(one.link_count() == 1 && one.total_length() == 1, "single-link rope has one link");
    check(links_inline(one), "single-link chain is INLINE — the trivial-case gate (no heap alloc)");

    rope_t two(byte_view(buf[0]));
    two.append(byte_view(buf[1]));
    check(two.link_count() == 2 && two.total_length() == 2, "two-link rope has two links");
    check(links_inline(two), "two-link chain is INLINE (no heap alloc)");

    rope_t three(byte_view(buf[0]));
    three.append(byte_view(buf[1]));
    three.append(byte_view(buf[2]));
    check(three.link_count() == 3 && three.total_length() == 3, "three-link rope has three links");
    check(!links_inline(three), "the third link SPILLS the chain to the heap");

    // concat crossing the inline boundary keeps every link and the right order.
    rope_t a(byte_view(buf[0]));
    rope_t b(byte_view(buf[1]));
    b.append(byte_view(buf[2]));
    a.concat(b);  // 1 + 2 = 3 links -> spills
    check(a.link_count() == 3 && !links_inline(a), "concat past two links spills to the heap");
    check(std::to_integer<int>(a.links()[0].bytes()[0]) == 0xA0 &&
              std::to_integer<int>(a.links()[2].bytes()[0]) == 0xA2,
          "spilled chain preserves link order");
}

void test_accessors() {
    std::printf("rope_t only()/materialize() (the L4 value consumption accessors):\n");
    std::array<std::byte, 4> buf{std::byte{0xB0}, std::byte{0xB1}, std::byte{0xB2},
                                 std::byte{0xB3}};

    const view_t link = byte_view(buf[0]);
    const rope_t one(link);
    check(one.only().owner.get() == link.owner.get(),
          "only() returns the sole link (zero copy, same segment)");
    const view_t m1 = one.materialize();
    check(m1.owner.get() == link.owner.get(),
          "materialize() of a single-link rope is zero-copy (same segment)");

    rope_t multi(byte_view(buf[0]));
    multi.append(byte_view(buf[1]));
    multi.append(byte_view(buf[2]));  // 3 links -> spilled, genuinely multi-link
    const view_t flat = multi.materialize();
    check(flat.owner.get() != multi.links()[0].owner.get(),
          "materialize() of a multi-link rope allocates a fresh contiguous segment (a copy)");
    check(flat.length == 3 && std::to_integer<int>(flat.bytes()[0]) == 0xB0 &&
              std::to_integer<int>(flat.bytes()[1]) == 0xB1 &&
              std::to_integer<int>(flat.bytes()[2]) == 0xB2,
          "materialized bytes are the links concatenated in order");

    // subrope narrows across links, sharing (never copying) the covered segments.
    const rope_t sub = multi.subrope(1, 2);
    check(sub.total_length() == 2 && std::to_integer<int>(sub.links().front().bytes()[0]) == 0xB1,
          "subrope covers the requested window across links");
}

}  // namespace

int main() {
    test_sbo_gate();
    test_accessors();
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
