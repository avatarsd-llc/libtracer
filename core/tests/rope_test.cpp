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
 * accessors a contiguous-bytes consumer calls (single-link: zero copy; multi: flatten),
 * and the nothrow soft-fail growth API (try_reserve / try_to_iovec and the
 * tr::detail try_reserve / try_push_back primitives) that keeps the composed-reply path
 * from abort()ing under -fno-exceptions on a fragmented heap.
 */

#include "libtracer/rope.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/mem_borrowed.hpp"
#include "libtracer/mem_heap.hpp"
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

/**
 * @brief The nothrow soft-fail growth API: an impossible count returns false (never
 *        abort()s), a normal reservation makes the following appends non-reallocating,
 *        and try_to_iovec / the tr::detail primitives behave correctly.
 */
void test_nothrow_growth() {
    std::printf("rope_t nothrow soft-fail growth (composed-reply OOM safety):\n");
    std::array<std::byte, 12> buf{};
    for (std::size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<std::byte>(0xC0 + static_cast<int>(i));

    // An impossible link count soft-fails instead of abort()ing in a throwing reserve.
    rope_t imp;
    const std::size_t impossible = SIZE_MAX / sizeof(view_t) + 1;
    check(!imp.try_reserve(impossible), "try_reserve(impossible count) returns false (no abort)");
    check(imp.link_count() == 0, "a failed try_reserve leaves the rope untouched");

    // Perf fast path: a reservation that fits the inline small-buffer storage takes NO
    // heap allocation — the hot small-reply (assemble delivery) path stays zero-alloc.
    rope_t small;
    check(small.try_reserve(2), "try_reserve(2) on a fresh rope returns true");
    check(small.link_count() == 0 && links_inline(small),
          "try_reserve(2) leaves the rope INLINE — no heap spill (zero-alloc fast path)");
    small.append(byte_view(buf[0]));
    small.append(byte_view(buf[1]));
    check(small.link_count() == 2 && links_inline(small),
          "two appends after the inline try_reserve stay INLINE (still no allocation)");

    // A normal reservation: the reserved appends never reallocate the heap chain.
    rope_t r;
    check(r.try_reserve(buf.size()), "try_reserve(normal count) returns true");
    for (std::size_t i = 0; i < 3; ++i) r.append(byte_view(buf[i]));  // spill to the heap chain
    const view_t* anchor = r.links().data();
    for (std::size_t i = 3; i < buf.size(); ++i) r.append(byte_view(buf[i]));
    check(r.link_count() == buf.size(), "every reserved append lands");
    check(r.links().data() == anchor, "reserved appends do not reallocate the chain (nothrow)");
    bool order_ok = true;
    for (std::size_t i = 0; i < buf.size(); ++i)
        if (std::to_integer<int>(r.links()[i].bytes()[0]) != (0xC0 + static_cast<int>(i)))
            order_ok = false;
    check(order_ok, "reserved appends preserve link order and bytes");

    // try_to_iovec: one span per link, pointing INTO the original segments (no copy).
    std::vector<std::span<const std::byte>> iov;
    check(r.try_to_iovec(iov), "try_to_iovec returns true on a normal rope");
    bool spans_ok = iov.size() == r.link_count();
    for (std::size_t i = 0; i < iov.size() && spans_ok; ++i)
        if (iov[i].data() != r.links()[i].bytes().data()) spans_ok = false;
    check(spans_ok, "try_to_iovec yields one zero-copy span per link");

    // The generic nothrow vector primitives both APIs build on (tr::detail).
    std::vector<int> v;
    const std::size_t vimp = SIZE_MAX / sizeof(int) + 1;
    check(!tr::detail::try_reserve(v, vimp), "detail::try_reserve(impossible) returns false");
    check(tr::detail::try_reserve(v, 4), "detail::try_reserve(normal) returns true");
    bool push_ok = true;
    for (int i = 0; i < 10; ++i) push_ok = push_ok && tr::detail::try_push_back(v, std::move(i));
    check(push_ok && v.size() == 10, "detail::try_push_back grows past capacity, all true");
    bool vorder = true;
    for (int i = 0; i < 10; ++i)
        if (v[static_cast<std::size_t>(i)] != i) vorder = false;
    check(vorder, "detail::try_push_back preserves order across the growth");
}

}  // namespace

int main() {
    test_sbo_gate();
    test_accessors();
    test_nothrow_growth();
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
