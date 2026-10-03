/**
 * @file
 * @brief The in-place TLV walker (`tr::wire::tlv_node_t` / `tlv_children_t`, #1648): it reads
 *        a frame as `decode` does and allocates nothing doing it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The corpus-wide equivalence with `decode` lives in the conformance runner; this suite pins
 * the node's own surface (spans, trailer values, descent, the range concept) and the property
 * the walker exists for: validating and walking a FWD-shaped frame makes ZERO global-heap
 * allocations. The counter is this binary's own replacement of the global `operator new` — a
 * whole-program decision, so the suite is its own executable.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <new>
#include <ranges>
#include <span>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tlv_emit.hpp"
#include "test_support.hpp"

namespace {

/** @brief Global `operator new` calls made since the process started. */
std::atomic<std::size_t> g_allocs{0};

}  // namespace

/** @brief The counting replacement: every plain and nothrow `new` funnels through here. */
void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    std::abort();
}
/**
 * @brief The nothrow form, replaced too so every block this binary frees came from `malloc`
 *        (the heap block source draws through it; ASan flags a mixed pair).
 */
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n == 0 ? 1 : n);
}
/** @brief Matching release for the counting nothrow `operator new`. */
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
/** @brief Matching release for the counting `operator new`. */
void operator delete(void* p) noexcept { std::free(p); }
/** @brief Sized release for the counting `operator new`. */
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

using tr::testing::check;
using tr::wire::opt_t;
using tr::wire::tlv_children_t;
using tr::wire::tlv_node_t;
using tr::wire::type_t;

static_assert(std::forward_iterator<tlv_children_t::iterator>,
              "the child walk is multi-pass: a forward iterator");
static_assert(std::ranges::forward_range<tlv_children_t>, "and a forward range");
static_assert(sizeof(tlv_node_t) <= 4 * sizeof(void*),
              "a node is a borrowed span plus header facts, never a tree");

/** @brief Bytes from a list of small integers. */
std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (const int b : v) out.push_back(static_cast<std::byte>(b));
    return out;
}

/** @brief Append one TLV of @p type over @p body. */
void put(std::vector<std::byte>& out, type_t type, opt_t opt, std::span<const std::byte> body) {
    tr::wire::emit_tlv(out, type, opt, body);
}

/**
 * @brief `FWD{ VALUE op, PATH dst, PATH src, VALUE payload }`, the frame shape router ingress
 *        observes: four children, one level deep.
 */
std::vector<std::byte> fwd_frame() {
    const auto op = bytes({0x01});
    const auto dst = bytes({0x03, 'o', 'u', 't', 0x06, 's', 'e', 'n', 's', 'o', 'r'});
    const auto src = bytes({0x05, 'r', 'e', 'p', 'l', 'y'});
    const auto pay = bytes({0xDE, 0xAD, 0xBE, 0xEF});
    std::vector<std::byte> body;
    put(body, type_t::VALUE, opt_t{}, op);
    put(body, type_t::PATH, opt_t{}, dst);
    put(body, type_t::PATH, opt_t{}, src);
    put(body, type_t::VALUE, opt_t{}, pay);
    std::vector<std::byte> frame;
    put(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

void test_walk_reads_the_children_in_place() {
    const std::vector<std::byte> frame = fwd_frame();
    const auto root = tlv_node_t::over(frame);
    check(root.has_value() && root->type() == type_t::FWD && root->opt().pl,
          "over() accepts a FWD and yields its structured root");
    if (!root) return;
    check(root->bytes().data() == frame.data() && root->bytes().size() == frame.size(),
          "the root borrows the whole input, no copy");
    check(root->payload().empty() && root->body().size() == frame.size() - 4,
          "a structured node has no payload; its body is the children region");

    std::vector<type_t> types;
    std::size_t covered = 0;
    for (const tlv_node_t c : root->children()) {
        types.push_back(c.type());
        check(c.bytes().data() == root->body().data() + covered,
              "each child is a window on the parent's region, in order");
        covered += c.bytes().size();
    }
    check(types == std::vector<type_t>{type_t::VALUE, type_t::PATH, type_t::PATH, type_t::VALUE},
          "the walk yields the four children in wire order");
    check(covered == root->body().size(), "the walk consumes the children region exactly");

    const tlv_node_t last = *std::ranges::next(root->children().begin(), 3);
    check(last.payload().size() == 4 && last.payload()[0] == std::byte{0xDE},
          "an opaque child's payload is its body");
    check(last.children().empty() && last.children().begin() == last.children().end(),
          "an opaque node walks no children");
}

void test_descent_and_trailers() {
    // SETTINGS{ NAME "k", VALUE 0x2A } with a CRC32C and an absolute timestamp on the VALUE,
    // inside an outer STATUS: two levels, so the descent goes through a child's own walk.
    std::vector<std::byte> value;
    {
        tr::wire::tlv_t v{.type = type_t::VALUE};
        const auto pay = bytes({0x2A});
        v.payload = pay;
        v.opt.cr = true;
        tr::wire::stamp_ts(v, 1234567);
        value = tr::wire::encode(v);
    }
    std::vector<std::byte> settings_body;
    put(settings_body, type_t::NAME, opt_t{}, bytes({'k'}));
    settings_body.insert(settings_body.end(), value.begin(), value.end());
    std::vector<std::byte> status_body;
    put(status_body, type_t::SETTINGS, opt_t{.pl = true}, settings_body);
    std::vector<std::byte> frame;
    put(frame, type_t::STATUS, opt_t{.pl = true}, status_body);

    const auto root = tlv_node_t::over(frame);
    check(root.has_value(), "a two-level frame with a CRC'd, stamped leaf validates");
    if (!root) return;
    const tlv_node_t settings = *root->children().begin();
    check(settings.type() == type_t::SETTINGS && settings.opt().pl,
          "the first level yields the structured SETTINGS");
    auto it = settings.children().begin();
    check((*it).type() == type_t::NAME, "descending walks the SETTINGS' own children");
    ++it;
    const tlv_node_t leaf = *it;
    const auto tr_ = leaf.trailer();
    check(tr_ && tr_->ts && !tr_->ts->relative && tr_->ts->value == 1234567,
          "the leaf's trailer timestamp reads back from its bytes");
    check(tr_ && tr_->crc && tr_->crc->width == tr::wire::crc_t::width_t::CRC32C,
          "the leaf's trailer CRC reads back from its bytes");
    check(leaf.wire().size() == 4 + 1 && leaf.bytes().size() == 4 + 1 + 8 + 4,
          "wire() is header + body; bytes() adds the 12 trailer bytes");
    ++it;
    check(it == settings.children().end(), "and the walk ends after the second child");
    check(!root->trailer(), "a trailer-less node reports none");
}

void test_refusals_match_decode() {
    std::vector<std::byte> frame = fwd_frame();
    frame.push_back(std::byte{0});
    const auto trailing = tlv_node_t::over(frame);
    check(!trailing && trailing.error() == tr::wire::err_t::FRAME_INVALID,
          "trailing bytes are FRAME_INVALID, as decode answers");

    std::vector<std::byte> bad = fwd_frame();
    bad[4] = std::byte{0x00};  // the first child's type byte: 0x00 is never a TLV type
    const auto deep = tlv_node_t::over(bad);
    const auto dec = tr::wire::decode(bad);
    check(!deep && !dec && deep.error() == dec.error(),
          "a malformed CHILD refuses the whole frame, with decode's error");

    // Nested past the walk's inline slots: refused under the null source, grown under the heap.
    std::vector<std::byte> nested;
    put(nested, type_t::VALUE, opt_t{}, bytes({1}));
    for (int i = 0; i < 12; ++i) {
        std::vector<std::byte> outer;
        put(outer, type_t::STATUS, opt_t{.pl = true}, nested);
        nested.swap(outer);
    }
    const auto capped = tlv_node_t::over(nested, tr::mem::null_source());
    check(!capped && capped.error() == tr::wire::err_t::TLV_NESTING_TOO_DEEP,
          "a frame deeper than the inline slots is refused when spilling is refused");
    check(tlv_node_t::over(nested).has_value(), "and accepted when the spill may grow");
}

void test_zero_allocation() {
    const std::vector<std::byte> frame = fwd_frame();
    std::size_t seen = 0;
    const std::size_t before = g_allocs.load();
    if (const auto root = tlv_node_t::over(frame, tr::mem::null_source())) {
        for (const tlv_node_t c : root->children()) seen += c.bytes().size();
    }
    const std::size_t made = g_allocs.load() - before;
    check(seen > 0, "the measured window walked the frame");
    check(made == 0, "validating and walking a FWD makes zero heap allocations");

    // The baseline the walker replaces: the owning decode builds a child vector per frame.
    const std::size_t before_dec = g_allocs.load();
    const auto dec = tr::wire::decode(frame);
    check(dec.has_value() && g_allocs.load() > before_dec,
          "(control) the owning decode of the same frame does allocate");
}

}  // namespace

int main() {
    test_walk_reads_the_children_in_place();
    test_descent_and_trailers();
    test_refusals_match_decode();
    test_zero_allocation();
    return tr::testing::summary("tlv_children");
}
