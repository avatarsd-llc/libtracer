/**
 * @file
 * @brief RFC-0028 §6.9 (#1626): the ingress loan. A receive block carries a value-header
 *        reserve, and the value stored from that block is placed IN the reserve.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Pinned here:
 * 1. `view::alloc_rx` takes the reserve only where it is free (the frame plus the reserve
 *    lands in the frame's own size class, never on the heap root), in one request, and marks
 *    the block.
 * 2. `value_t::make(rope&&)` over such a block places the header in the reserve without ever
 *    asking the graph's block source, and does so only ONCE per block. A second value over
 *    the same block draws its header from the source, as it would for any other block.
 * 3. A loaned value shares like any other published value (`keep` is a refcount), and every
 *    teardown order leaves nothing behind. Run under ASan to see this.
 * 4. End to end: a graph write of a loaned frame stores a loaned value with zero draws from
 *    the graph's value source, and the value reads back byte-exact.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::value_ref_t;
using tr::graph::value_t;
using tr::testing::check;
using tr::view::rope_t;

/** @brief A heap-backed block source that counts what it is asked for. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : block_source_t("counting") {}

    /** @brief Grants since construction (or since @ref reset). */
    [[nodiscard]] std::size_t served() const noexcept {
        return served_.load(std::memory_order_relaxed);
    }
    /** @brief Forget the count. */
    void reset() noexcept { served_.store(0, std::memory_order_relaxed); }

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        served_.fetch_add(1, std::memory_order_relaxed);
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t /*bytes*/, std::size_t align) noexcept override {
        ::operator delete(p, std::align_val_t{align}, std::nothrow);
    }

   private:
    std::atomic<std::size_t> served_{0}; /**< @brief Grants counted. */
};

/** @brief The byte a patterned frame carries at @p i. */
[[nodiscard]] std::byte pattern_at(std::size_t i) {
    return std::byte(static_cast<std::uint8_t>(i * 7u));
}

/** @brief The first frame length from @p near up whose loan reserve is @p free (or is not). */
[[nodiscard]] std::size_t len_where(std::size_t near, bool free) {
    const std::size_t align = tr::mem::heap_backend().alignment();
    while (tr::mem::rx_loan_free(near, align) != free) ++near;
    return near;
}

/** @brief A loaned receive block holding a patterned @p len-byte frame. */
[[nodiscard]] tr::view::rx_block_t loaned_block(std::size_t len) {
    tr::view::rx_block_t blk = tr::view::alloc_rx(tr::mem::heap_backend(), len);
    if (blk.seg) {
        const std::span<std::byte> f = blk.frame(len);
        for (std::size_t i = 0; i < len; ++i) f[i] = pattern_at(i);
    }
    return blk;
}

/** @brief True when @p v is one link carrying @ref loaned_block's pattern for @p len bytes. */
[[nodiscard]] bool pattern_ok(const value_t& v, std::size_t len) {
    if (v.link_count() != 1 || v.total_length() != len) return false;
    const std::span<const std::byte> b = v.links()[0].bytes();
    for (std::size_t i = 0; i < len; ++i)
        if (b[i] != pattern_at(i)) return false;
    return true;
}

/** @brief Vector 1: the reserve is taken only where it is free, and marked. */
void test_alloc_rx_shape() {
    std::printf("alloc_rx reserves only where the loan is free:\n");
    if constexpr (tr::mem::kHeapRoot) {
        const tr::view::rx_block_t b = tr::view::alloc_rx(tr::mem::heap_backend(), 5000);
        check(b.seg && b.off == 0 && b.seg->rx_loan == 0,
              "the heap root draws exact blocks: never a reserve");
        return;
    }
    const std::size_t costly = len_where(6000, false);
    const tr::view::rx_block_t plain = tr::view::alloc_rx(tr::mem::heap_backend(), costly);
    check(plain.seg && plain.off == 0 && plain.seg->rx_loan == 0,
          "a reserve that would take the next class: a plain block, the frame at offset 0");
    const std::size_t free = len_where(6000, true);
    const tr::view::rx_block_t loaned = tr::view::alloc_rx(tr::mem::heap_backend(), free);
    check(loaned.seg && loaned.off == tr::mem::kRxLoanBytes && loaned.seg->rx_loan == 1,
          "a reserve inside the frame's class: the reserve in front, the block marked");
    check(loaned.seg && loaned.seg->bytes.size() >= free + tr::mem::kRxLoanBytes,
          "the block holds the reserve plus the whole frame");
    const tr::view::rx_block_t small = tr::view::alloc_rx(tr::mem::heap_backend(), 64);
    check(small.seg &&
              (small.off == 0) == !tr::mem::rx_loan_free(64, tr::mem::heap_backend().alignment()),
          "a small frame follows the same class test");
}

/** @brief Vector 2: the value goes IN the reserve, once per block. */
void test_make_in_reserve() {
    std::printf("value_t::make places a loaned value in the block's reserve, once:\n");
    const std::size_t kLen = len_where(6000, true);
    counting_source_t src;
    tr::view::rx_block_t blk = loaned_block(kLen);
    const tr::view::segment_t* const seg = blk.seg.get();
    tr::view::view_t whole = blk.take(kLen);
    tr::view::view_t twin = whole;  // a second view over the same block

    rope_t r;
    r.append(std::move(whole));
    const value_ref_t v = value_ref_t::adopt(value_t::make(std::move(r), src));
    check(v && v->is_loaned(), "the value is loaned");
    check(src.served() == 0, "the graph's source was not asked for the header");
    check(v && reinterpret_cast<const std::byte*>(v.get()) ==
                   seg->bytes.data() + tr::mem::kRxLoanValueOffset,
          "the header sits at the reserve's value offset");
    check(v && pattern_ok(*v, kLen), "the value reads the frame byte-exact");
    check(r.link_count() == 0, "the offered rope was consumed");

    rope_t r2;
    r2.append(std::move(twin));
    const value_ref_t w = value_ref_t::adopt(value_t::make(std::move(r2), src));
    check(w && !w->is_loaned(), "a second value over the claimed block is NOT loaned");
    check(src.served() == 1, "and drew its header from the source instead");
    check(w && pattern_ok(*w, kLen), "and reads the same bytes");

    // A link that starts inside the reserve is refused the loan: the header would overlay it.
    tr::view::rx_block_t blk2 = loaned_block(kLen);
    rope_t r3;
    r3.append(tr::view::view_t{std::move(blk2.seg), 0, kLen});
    src.reset();
    const value_ref_t x = value_ref_t::adopt(value_t::make(std::move(r3), src));
    check(x && !x->is_loaned() && src.served() == 1,
          "a link reaching into the reserve is placed from the source, not loaned");
}

/** @brief Vector 3: a loaned value shares by refcount, and every teardown order is clean. */
void test_share_and_teardown() {
    std::printf("a loaned value shares by refcount and tears down in any order:\n");
    const std::size_t kLen = len_where(5000, true);
    counting_source_t src;
    for (int order = 0; order < 3; ++order) {
        tr::view::rx_block_t blk = loaned_block(kLen);
        rope_t r;
        r.append(blk.take(kLen));
        value_ref_t v = value_ref_t::adopt(value_t::make(std::move(r), src));
        value_ref_t kept = value_ref_t::keep(*v, src);
        check(kept.get() == v.get() && v->use_count() == 2, "keep shares the loaned value");
        rope_t out = v->rope();  // a sink's clone of the links: holds the block, not the value
        switch (order) {
            case 0: {  // the value references first, the rope clone last
                v = value_ref_t{};
                kept = value_ref_t{};
                const value_ref_t again = value_ref_t::adopt(value_t::make(out.links(), src));
                check(again && pattern_ok(*again, kLen),
                      "the rope clone outlives the value and still reads the frame");
                out = rope_t{};
                break;
            }
            case 1:  // the rope clone first
                out = rope_t{};
                kept = value_ref_t{};
                check(pattern_ok(*v, kLen), "the value outlives the rope clone");
                v = value_ref_t{};
                break;
            default:  // interleaved
                kept = value_ref_t{};
                out = rope_t{};
                v = value_ref_t{};
                break;
        }
    }
    check(src.served() == 1, "only the case-0 re-mint over a clone asked the source");
}

/** @brief Vector 4: a graph write of a loaned frame stores a loaned value, 0 source draws. */
void test_graph_write_stores_loaned() {
    std::printf("a graph write of a loaned frame stores a loaned value:\n");
    // The first payload whose VALUE TLV's receive block carries the reserve for free.
    const std::size_t kPayload = len_where(5004, true) - 4;
    counting_source_t src;
    graph_t g(src);
    const auto p = path_t::parse("/rx/blob");
    const auto v = g.register_vertex(*p, role_t::STORED_VALUE);

    // One VALUE TLV (a header, then the payload) in one loaned receive block, the way a
    // transport hands a frame up.
    std::vector<std::byte> tlv;
    {
        tr::net::stack_writer_t<16> h;
        h.header(tr::wire::type_t::VALUE, kPayload);
        const std::span<const std::byte> hs = h.span();
        tlv.assign(hs.begin(), hs.end());
        for (std::size_t i = 0; i < kPayload; ++i) tlv.push_back(pattern_at(i));
    }
    tr::view::rx_block_t blk = tr::view::alloc_rx(tr::mem::heap_backend(), tlv.size());
    std::memcpy(blk.frame(tlv.size()).data(), tlv.data(), tlv.size());
    rope_t r;
    r.append(blk.take(tlv.size()));

    src.reset();
    check(g.write(v, std::move(r)).has_value(), "the write lands");
    check(src.served() == 0, "the graph's value source was not asked for a record");
    const auto got = g.read(v);
    check(got.has_value() && (*got)->is_loaned(), "the stored value is the loaned one");
    check(got.has_value() && (*got)->total_length() == tlv.size() &&
              std::memcmp((*got)->links()[0].bytes().data(), tlv.data(), tlv.size()) == 0,
          "and it reads back byte-exact");
}

}  // namespace

int main() {
    test_alloc_rx_shape();
    // The heap root never loans (its blocks are exact), so the loaned-value vectors have no
    // block to run over there.
    if constexpr (!tr::mem::kHeapRoot) {
        test_make_in_reserve();
        test_share_and_teardown();
        test_graph_write_stores_loaned();
    }
    return tr::testing::summary("rx_loan");
}
