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
 * the try_flatten/try_materialize outcome separation (#917 — a legitimately empty rope, an
 * allocator refusal and a DEVICE link are three different answers, not one empty view),
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
#include "test_support.hpp"

namespace {

using tr::testing::check;

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

/**
 * @brief `r.concat(r)` — the self-aliasing gate (#915), in BOTH storage modes.
 *
 * `concat` walks `other`'s links while `append` mutates that same storage when
 * `&other == this`. In INLINE mode the spill blanks every inline slot and zeroes
 * `inline_n_` mid-walk, so a naive range-for produced `[a,b,a,{}]` — a zero-length
 * link where `b` should be, i.e. wrong bytes on the wire. In HEAP mode `push_back`
 * can reallocate the vector the walk points into (a dangling span, UB — the leg an
 * ASan build reddens on). Both are checked here by the resulting link bytes, so the
 * inline corruption is caught with no sanitizer at all.
 */
void test_self_concat() {
    std::printf("rope_t self-concat aliasing (#915):\n");
    std::array<std::byte, 8> buf{std::byte{0xC0}, std::byte{0xC1}, std::byte{0xC2},
                                 std::byte{0xC3}, std::byte{0xC4}, std::byte{0xC5},
                                 std::byte{0xC6}, std::byte{0xC7}};

    // INLINE mode, inline_n_ == 2: the first append of the walk spills.
    rope_t inl(byte_view(buf[0]));
    inl.append(byte_view(buf[1]));
    check(links_inline(inl), "precondition: the source chain is INLINE with two links");
    inl.concat(inl);
    check(inl.link_count() == 4, "inline self-concat yields four links");
    check(inl.total_length() == 4, "inline self-concat yields four bytes (no empty link)");
    bool inl_order = inl.link_count() == 4;
    if (inl_order) {
        const int want[4] = {0xC0, 0xC1, 0xC0, 0xC1};
        for (std::size_t i = 0; i < 4; ++i) {
            if (inl.links()[i].length != 1 ||
                std::to_integer<int>(inl.links()[i].bytes()[0]) != want[i]) {
                inl_order = false;
            }
        }
    }
    check(inl_order, "inline self-concat is [a,b,a,b], not the corrupted [a,b,a,{}]");

    // HEAP mode: enough links that the push_back walk would outgrow the vector.
    rope_t hp(byte_view(buf[0]));
    for (std::size_t i = 1; i < 5; ++i) hp.append(byte_view(buf[i]));
    check(!links_inline(hp), "precondition: the source chain is SPILLED to the heap");
    hp.concat(hp);
    check(hp.link_count() == 10, "heap self-concat yields ten links");
    check(hp.total_length() == 10, "heap self-concat yields ten bytes");
    bool hp_order = hp.link_count() == 10;
    if (hp_order) {
        for (std::size_t i = 0; i < 10; ++i) {
            if (hp.links()[i].length != 1 ||
                std::to_integer<int>(hp.links()[i].bytes()[0]) != 0xC0 + static_cast<int>(i % 5)) {
                hp_order = false;
            }
        }
    }
    check(hp_order, "heap self-concat repeats the chain in order (no dangling-span garbage)");

    // The self-concat guard must not disturb the ordinary cross-rope case.
    rope_t lhs(byte_view(buf[0]));
    rope_t rhs(byte_view(buf[1]));
    rhs.append(byte_view(buf[2]));
    const rope_t sum = lhs + rhs;
    check(sum.link_count() == 3 && std::to_integer<int>(sum.links()[0].bytes()[0]) == 0xC0 &&
              std::to_integer<int>(sum.links()[1].bytes()[0]) == 0xC1 &&
              std::to_integer<int>(sum.links()[2].bytes()[0]) == 0xC2,
          "cross-rope operator+ is unchanged");
    const rope_t empty_src;
    rope_t keep(byte_view(buf[3]));
    keep.concat(empty_src);
    check(keep.link_count() == 1, "concat of an empty rope is a no-op");
    rope_t self_empty;
    self_empty.concat(self_empty);
    check(self_empty.link_count() == 0, "self-concat of an empty rope stays empty");
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
 * @brief A backend that refuses every allocation — the heap-exhaustion stand-in (#917).
 */
class refusing_backend_t final : public tr::mem::mem_backend_t {
   public:
    refusing_backend_t() noexcept : mem_backend_t("test_refusing") {}

    [[nodiscard]] tr::view::segment_t* alloc(std::size_t, tr::mem::alloc_hint_t) override {
        ++calls_;
        return nullptr;
    }
    void destroy(tr::view::segment_t* seg) noexcept override {
        tr::mem::heap_backend().destroy(seg);
    }

    /** @brief How many allocations were asked for — proves the seam was (or was not) consulted. */
    [[nodiscard]] int calls() const noexcept { return calls_; }

   private:
    int calls_ = 0;
};

/**
 * @brief try_flatten / try_materialize keep the THREE outcomes apart (#917).
 *
 * `flatten()` used to answer an empty view for a DEVICE-link rope, for an allocator
 * refusal, AND for a legitimately empty rope — so every caller's `empty()` test was
 * three questions at once, and a router reading it as "malformed frame" turned a local
 * OOM into a PERMANENT accusation against the peer. These pin that each outcome is now
 * distinguishable, and that the lossy `flatten()` wrapper still behaves as before.
 */
void test_flatten_verdicts() {
    std::printf("rope_t try_flatten()/try_materialize() outcome separation (#917):\n");
    std::array<std::byte, 4> buf{std::byte{0xD0}, std::byte{0xD1}, std::byte{0xD2},
                                 std::byte{0xD3}};

    // (1) A legitimately empty rope SUCCEEDS with an empty view — and never touches the
    //     backend, so an exhausted allocator cannot recolour the degenerate case as OOM.
    refusing_backend_t idle;
    const rope_t none;
    const auto empty_ok = none.try_flatten(idle);
    check(empty_ok.has_value() && empty_ok->empty() && idle.calls() == 0,
          "try_flatten of an empty rope is a SUCCESS carrying an empty view (no allocation)");
    check(none.try_materialize(idle).has_value(), "try_materialize of an empty rope succeeds");

    // (2) The allocator's refusal is NO_MEMORY — transient, retryable.
    refusing_backend_t oom;
    rope_t multi(byte_view(buf[0]));
    multi.append(byte_view(buf[1]));
    multi.append(byte_view(buf[2]));  // 3 links -> genuinely multi-link, so a flatten happens
    const auto refused = multi.try_flatten(oom);
    check(!refused.has_value() && refused.error() == tr::view::flatten_err_t::NO_MEMORY &&
              oom.calls() == 1,
          "try_flatten reports the backend's refusal as NO_MEMORY (transient backpressure)");
    const auto refused_mat = multi.try_materialize(oom);
    check(!refused_mat.has_value() && refused_mat.error() == tr::view::flatten_err_t::NO_MEMORY,
          "try_materialize of a multi-link rope forwards the NO_MEMORY refusal");

    // (3) A DEVICE link is NOT_HOST — permanent for this rope, no retry helps, and the
    //     backend is never consulted (the CPU must not read those bytes at all).
    refusing_backend_t untouched;
    rope_t dev(view_t::over(tr::view::borrow_device(std::span<std::byte>(&buf[3], 1))));
    dev.append(byte_view(buf[0]));
    const auto not_host = dev.try_flatten(untouched);
    check(!not_host.has_value() && not_host.error() == tr::view::flatten_err_t::NOT_HOST &&
              untouched.calls() == 0,
          "try_flatten reports a DEVICE link as NOT_HOST without asking the backend");

    // A SINGLE-link device rope materializes to its link, zero copy — no CPU dereference
    // happens, exactly as materialize() has always behaved.
    const rope_t dev_one(view_t::over(tr::view::borrow_device(std::span<std::byte>(&buf[3], 1))));
    const auto dev_mat = dev_one.try_materialize(untouched);
    check(dev_mat.has_value() && dev_mat->is_device(),
          "try_materialize of a single-link DEVICE rope hands back the link (zero copy)");

    // The lossy wrappers are unchanged: both refusals still collapse into the empty view.
    check(multi.flatten(oom).empty() && dev.flatten(untouched).empty(),
          "flatten() keeps its lossy empty-view contract for both refusals");
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

    // The two storage states in which the inline no-op arm must NOT fire (#1065). It is
    // guarded on "the chain is still inline AND stays inline", and each half owns a case
    // that a reservation-shaped promise is made about, so each is asserted separately.
    //
    // (a) PARTIALLY-FILLED inline chain: one link held, two more asked for. 1 + 2 > kInline,
    // so the reservation must spill and migrate NOW — returning a bare `true` would leave
    // the caller's next append to take the throwing inline->heap spill it was told it had
    // already paid for.
    rope_t part;
    part.append(byte_view(buf[0]));
    check(part.try_reserve(2), "try_reserve(2) on a 1-link inline rope returns true");
    check(!links_inline(part),
          "a reservation that will not fit inline SPILLS at reserve time, not at append");
    const view_t* part_anchor = part.links().data();
    part.append(byte_view(buf[1]));
    part.append(byte_view(buf[2]));
    check(part.link_count() == 3, "the reserved appends on a partially-filled rope all land");
    check(part.links().data() == part_anchor,
          "appends after a partially-filled-rope reservation do not reallocate");

    // (b) ALREADY-SPILLED chain: `inline_n_` is back to 0 once the chain lives on the heap,
    // so a count that would fit the small buffer must still reach the allocator — the links
    // are not in the small buffer any more.
    rope_t sp;
    for (std::size_t i = 0; i < 3; ++i) sp.append(byte_view(buf[i]));
    check(!links_inline(sp), "three appends spill the chain (precondition)");
    check(sp.try_reserve(2), "try_reserve(2) on an already-spilled rope returns true");
    const view_t* sp_anchor = sp.links().data();
    sp.append(byte_view(buf[3]));
    sp.append(byte_view(buf[4]));
    check(sp.link_count() == 5, "the reserved appends on a spilled rope all land");
    check(sp.links().data() == sp_anchor,
          "appends after a spilled-rope reservation do not reallocate the chain");

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
    test_self_concat();
    test_accessors();
    test_flatten_verdicts();
    test_nothrow_growth();
    return tr::testing::summary("rope");
}
