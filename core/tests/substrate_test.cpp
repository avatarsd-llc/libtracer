// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// L0/L1 substrate tests: refcount lifetime (the canonical intrusive_ptr
// orderings), zero-copy subview/concat, rope serialization equivalence (the
// docs/reference/02 proof obligation), the view->TLV cast claim with the
// lifetime gap M1 left open now closed, and the bounded pool backend. Reuses the
// seed vectors as real TLV bytes; no JSON parser. Builds twice — once with
// atomic refcounts, once with -DLIBTRACER_NO_ATOMIC (single-threaded mode).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

std::vector<std::byte> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::ranges::transform(raw, out.begin(), [](char c) {
        return static_cast<std::byte>(static_cast<unsigned char>(c));
    });
    return out;
}

// A backend that counts how many times destroy fires and frees the control block
// it is handed. Used to assert exact refcount lifetime.
class CountingBackend final : public tr::mem::mem_backend_t {
   public:
    CountingBackend() noexcept : tr::mem::mem_backend_t("counting") {}
    void destroy(tr::view::segment_t* seg) noexcept override {
        ++destroys;
        delete seg;
    }
    int destroys = 0;
};

// Make an owned segment over `bytes` backed by `be` (refcount = 1, adopted).
tr::view::segment_ptr_t make_segment(CountingBackend& be, std::span<std::byte> bytes) {
    return tr::view::segment_ptr_t::adopt(new tr::view::segment_t(&be, bytes));
}

void test_refcount_lifetime() {
    std::printf("Refcount lifetime (intrusive_ptr orderings):\n");
    std::array<std::byte, 8> store{};
    CountingBackend be;
    {
        tr::view::segment_ptr_t a = make_segment(be, store);
        check(a.use_count() == 1, "fresh segment use_count == 1");
        tr::view::segment_ptr_t b = a;  // clone (relaxed inc)
        check(a.use_count() == 2, "clone bumps to 2 (fan-out to subscriber 1)");
        {
            tr::view::segment_ptr_t c = b;  // clone
            check(a.use_count() == 3, "second clone -> 3 (fan-out to subscriber 2)");
            check(be.destroys == 0, "no destroy while referenced");
        }
        check(a.use_count() == 2, "release of subscriber 2 -> 2");
    }
    check(be.destroys == 1, "destroy fires exactly once when the last handle drops");
}

void test_transfer_vs_clone() {
    std::printf("Ownership transfer vs clone (docs/reference/02 §delivery):\n");
    std::array<std::byte, 4> store{};
    CountingBackend be;
    tr::view::segment_ptr_t a = make_segment(be, store);
    tr::view::segment_ptr_t moved = std::move(a);  // transfer: take the existing ref
    check(moved.use_count() == 1 && !a, "move transfers ownership, no net refcount change");
    tr::view::segment_ptr_t cloned = moved;  // clone: a new ref
    check(moved.use_count() == 2, "clone adds exactly one reference");
    check(be.destroys == 0, "still alive while a handle remains");
}

void test_zero_copy_subview_concat() {
    std::printf("Zero-copy subview / concat / iovec:\n");
    std::array<std::byte, 16> store{};
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = static_cast<std::byte>(i);
    std::array<std::byte, 4> tail{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                  std::byte{0xEF}};

    tr::view::view_t whole = tr::view::view_t::over(tr::view::borrow(store));
    tr::view::view_t sub = whole.subview(4, 8);
    check(sub.bytes().data() == store.data() + 4, "subview points into the same buffer (no copy)");
    check(sub.length == 8, "subview length is exact");

    tr::view::rope_t rope(whole);
    rope.append(tr::view::view_t::over(tr::view::borrow(tail)));
    check(rope.link_count() == 2, "rope has 2 links");
    check(rope.total_length() == store.size() + tail.size(), "rope total length sums links");

    const auto iov = rope.to_iovec();
    check(iov.size() == 2 && iov[0].data() == store.data() && iov[1].data() == tail.data(),
          "to_iovec spans point into the original buffers (scatter-gather, no copy)");
}

void test_rope_equivalence(const fs::path& vroot) {
    std::printf("rope_t serialization equivalence (the docs/reference/02 proof obligation):\n");
    const std::vector<std::byte> flat = read_file(vroot / "path/path-sensor-temp" / "input.bin");
    check(!flat.empty(), "seed vector loaded");

    // Split the one TLV's bytes across two borrowed segments — a 2-link rope.
    const std::size_t cut = flat.size() / 2;
    std::vector<std::byte> part_a(flat.begin(), flat.begin() + cut);
    std::vector<std::byte> part_b(flat.begin() + cut, flat.end());
    tr::view::rope_t rope(tr::view::view_t::over(tr::view::borrow(part_a)));
    rope.append(tr::view::view_t::over(tr::view::borrow(part_b)));

    const tr::view::view_t materialized = rope.flatten();  // one copy, into a heap segment
    const auto mb = materialized.bytes();
    check(std::ranges::equal(mb, flat), "flatten(rope) reproduces the flat bytes exactly");

    const auto t_flat = tr::decode(flat);
    const auto t_rope = tr::view::view_as_tlv(materialized);
    check(t_flat.has_value() && t_rope.has_value() && tr::equal(*t_flat, *t_rope),
          "decode(flatten(rope)) == decode(flat)");
    check(t_rope.has_value() && t_rope->children.size() == 2,
          "the rope-decoded PATH has its 2 NAME children");
}

void test_cast_claim_outlives_source(const fs::path& vroot) {
    std::printf("Cast claim — tlv_t outlives its source buffer via the segment refcount:\n");
    tr::view::view_t v;
    {
        // Copy a seed vector into an OWNED heap segment, build a view, then let
        // the original buffer go out of scope. M1 alone would dangle here.
        const std::vector<std::byte> src = read_file(vroot / "crc/value-crc32c" / "input.bin");
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(src.size());
        check(static_cast<bool>(seg), "heap segment allocated");
        std::memcpy(seg->bytes.data(), src.data(), src.size());
        v = tr::view::view_t::over(std::move(seg));
    }  // `src` freed here; `v` (and its segment copy) survives

    const auto tlv = tr::view::view_as_tlv(v);
    check(tlv.has_value(), "view_as_tlv decodes after the source buffer was freed");
    check(tlv.has_value() && tlv->type == tr::type_t::VALUE, "decoded type is VALUE");
    check(tlv.has_value() && tlv->trailer && tlv->trailer->crc.has_value(),
          "CRC trailer present and verified (decode would have failed otherwise)");
}

void test_bounded_pool() {
    std::printf("Bounded pool backend (custom allocator over a fixed slab):\n");
    std::array<std::byte, 256> slab{};
    tr::mem::pool_t pool(slab, 16);
    const std::size_t cap = pool.capacity();
    check(cap > 0, "pool carved at least one slot from the slab");
    check(pool.available() == cap, "all slots free initially");

    std::vector<tr::view::segment_ptr_t> held;
    while (tr::view::segment_t* s = pool.alloc(16))
        held.push_back(tr::view::segment_ptr_t::adopt(s));
    check(held.size() == cap, "pool hands out exactly capacity slots");
    check(pool.alloc(16) == nullptr, "an exhausted pool returns nullptr (BACKPRESSURE)");
    check(pool.available() == 0, "no slots available when full");

    held.pop_back();  // releases one segment -> destroy returns its slot
    check(pool.available() == 1, "freeing a segment returns its slot to the pool");
    tr::view::segment_t* again = pool.alloc(16);
    check(again != nullptr, "alloc succeeds again after a free");
    const tr::view::segment_ptr_t reclaim = tr::view::segment_ptr_t::adopt(again);
    check(pool.alloc(17) == nullptr, "a request larger than the slot payload is refused");
}

}  // namespace

int main() {
    const fs::path vroot{LIBTRACER_VECTORS_DIR};

    test_refcount_lifetime();
    test_transfer_vs_clone();
    test_zero_copy_subview_concat();
    test_rope_equivalence(vroot);
    test_cast_claim_outlives_source(vroot);
    test_bounded_pool();

#ifdef LIBTRACER_NO_ATOMIC
    std::printf("\n(built with LIBTRACER_NO_ATOMIC — single-threaded refcount)\n");
#endif
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
