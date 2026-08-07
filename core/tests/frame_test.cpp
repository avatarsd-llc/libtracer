/**
 * @file
 * @brief Frame-codec nesting + length-width tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Locks the docs/reference/01 §"Iterative parsing
 * requirement" under RFC-0006: nested TLVs are parsed iteratively (no
 * recursion) and depth is bounded by the RECEIVER'S decode resources, never a
 * constant. A heap-resourced decode parses a frame far deeper than the old cap
 * of 32; a null-spill grammar walk whose inline slots ARE its whole budget
 * rejects a deeper frame cleanly as NESTING_TOO_DEEP ("exceeds this receiver's
 * decode resources") — no crash, no throw.
 *
 * Also locks the ONE length-width policy (#924): `encode` goes through
 * `emit_tlv`, so a `tlv_t` built programmatically with a default `opt`
 * (`ll = false`) over a body larger than 0xFFFF widens to the u32 LL form and
 * round-trips through `decode` instead of serializing a length truncated to
 * `size & 0xFFFF`. Bodies at or under 0xFFFF stay byte-identical — pinned.
 */

#include "libtracer/frame.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/grammar.hpp"

namespace {

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/**
 * @brief Build a tree whose leaf sits at @p leaf_depth: `leaf_depth` structured
 *        PATH wrappers (opt.PL=1) around one empty opaque VALUE leaf (root = 0).
 */
tr::wire::tlv_t build_nested(int leaf_depth) {
    tr::wire::tlv_t node;
    if (leaf_depth == 0) {
        node.type = tr::wire::type_t::VALUE;  // opaque leaf, empty payload
        return node;
    }
    node.type = tr::wire::type_t::PATH;
    node.opt.pl = true;
    node.children.push_back(build_nested(leaf_depth - 1));
    return node;
}

/** @brief Count nesting depth of a decoded tree (root = 0). */
int measured_depth(const tr::wire::tlv_t& t) {
    int d = 0;
    const tr::wire::tlv_t* cur = &t;
    while (!cur->children.empty()) {
        cur = &cur->children.front();
        ++d;
    }
    return d;
}

/** @brief A walk sink that models nothing — drives grammar::walk as a validator. */
struct null_sink_t {
    /** @brief A structured TLV opened — nothing to model. */
    void on_open(const tr::wire::grammar::header_t&, const tr::wire::grammar::span_cursor&) {}
    /** @brief An opaque TLV visited — nothing to model. */
    void on_leaf(const tr::wire::grammar::header_t&, const tr::wire::grammar::span_cursor&) {}
    /** @brief The open node's children completed — nothing to seal. */
    void on_close() {}
};

/**
 * @brief Run grammar::walk over @p bytes with a budget of exactly @p slots
 *        open-node records and NO spill — the receiver's whole decode resource.
 */
std::expected<void, tr::wire::err_t> walk_with_budget(
    const std::vector<std::byte>& bytes,
    std::span<tr::wire::grammar::walk_frame_t<tr::wire::grammar::span_cursor>> slots) {
    null_sink_t sink;
    tr::wire::grammar::walk_stack_t<tr::wire::grammar::span_cursor> stack(slots, nullptr);
    return tr::wire::grammar::walk(tr::wire::grammar::span_cursor{bytes}, sink, stack);
}

/**
 * @brief Read a frame's header length field straight off the wire — u16 LE, or u32 LE when
 *        the emitted `opt` byte carries the LL bit.
 *
 * Deliberately hand-rolled rather than routed through `grammar::parse_header`: the claim
 * under test is what `encode` WROTE, so reading it back through the decoder that also
 * interprets LL would let one bug hide the other.
 */
std::uint64_t wire_length(const std::vector<std::byte>& bytes) {
    const tr::wire::opt_t opt = tr::wire::opt_t::decode(std::to_integer<std::uint8_t>(bytes[1]));
    const std::size_t n = opt.ll ? 4u : 2u;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[2 + i])) << (8u * i);
    }
    return v;
}

/** @brief True iff the emitted `opt` byte of @p bytes has the LL (u32 length) bit set. */
bool wire_ll(const std::vector<std::byte>& bytes) {
    return tr::wire::opt_t::decode(std::to_integer<std::uint8_t>(bytes[1])).ll;
}

}  // namespace

int main() {
    using namespace tr::wire;
    std::printf("Frame nesting (iterative parse, receiver-resource-bounded — RFC-0006):\n");

    // Far deeper than the old cap of 32: a heap-resourced receiver just parses it.
    {
        constexpr int kDeep = 500;
        const tlv_t built = build_nested(kDeep);
        const std::vector<std::byte> bytes = encode(built);
        const auto dec = decode(bytes);
        check(dec.has_value(), "deep nesting (leaf depth 500) decodes on a heap-resourced host");
        if (dec) {
            check(measured_depth(*dec) == kDeep, "decoded tree has depth 500");
            check(equal(*dec, built), "deep frame round-trips byte-exactly");
        }
    }

    // A null-spill walk stack: the inline slots are the receiver's whole budget.
    // A leaf at depth N opens N nodes, so N slots accept it and reject N+1 —
    // with TLV_NESTING_TOO_DEEP ("exceeds this receiver's decode resources").
    {
        constexpr int kBudget = 4;
        grammar::walk_frame_t<grammar::span_cursor> slots[kBudget];
        const std::vector<std::byte> fits = encode(build_nested(kBudget));
        const std::vector<std::byte> deeper = encode(build_nested(kBudget + 1));
        check(walk_with_budget(fits, slots).has_value(),
              "frame at the budget (4 open nodes / 4 slots) is accepted");
        const auto rejected = walk_with_budget(deeper, slots);
        check(!rejected.has_value(), "frame past the budget is rejected (no throw, no crash)");
        check(!rejected.has_value() && rejected.error() == err_t::TLV_NESTING_TOO_DEEP,
              "rejection reason is NESTING_TOO_DEEP (exceeds decode resources)");
    }

    std::printf("\nFrame length width (one auto-widen policy for both emitters — #924):\n");

    // A small body emits exactly the bytes it always did: `01 00 04 00 de ad be ef`. This is
    // the pin that the widen did not move the ordinary case.
    {
        const std::array<std::byte, 4> pay{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                           std::byte{0xEF}};
        tlv_t small;
        small.type = type_t::VALUE;
        small.payload = pay;
        const std::vector<std::byte> got = encode(small);
        const std::array<std::uint8_t, 8> want{0x01, 0x00, 0x04, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
        const bool same =
            got.size() == want.size() &&
            std::equal(got.begin(), got.end(), want.begin(),
                       [](std::byte g, std::uint8_t w) { return std::to_integer<int>(g) == w; });
        check(same, "a 4-byte body still emits the pinned 8 bytes (u16 length, byte-identical)");
    }

    // The boundary is `>`, not `>=`: a body of EXACTLY 0xFFFF still fits a u16 and must not
    // widen — otherwise every frame within a byte of the limit would change on the wire.
    {
        const std::vector<std::byte> pay(0xFFFFu, std::byte{0x11});
        tlv_t edge;
        edge.type = type_t::VALUE;
        edge.payload = pay;
        const std::vector<std::byte> bytes = encode(edge);
        check(bytes.size() == 4u + 0xFFFFu && !wire_ll(bytes) && wire_length(bytes) == 0xFFFFu,
              "a body of exactly 0xFFFF keeps the 4-byte u16 header (widen is > not >=)");
    }

    // The defect: a default-opt tlv_t over a 70000-byte OPAQUE body used to serialize its
    // length as 70000 & 0xFFFF == 4464 — a frame a peer mis-frames. It now widens.
    {
        constexpr std::size_t kBig = 70000;  // > 0xFFFF
        std::vector<std::byte> pay(kBig);
        for (std::size_t i = 0; i < kBig; ++i) {
            pay[i] = static_cast<std::byte>((i * 31u + 7u) & 0xFFu);
        }
        tlv_t big;
        big.type = type_t::VALUE;
        big.payload = pay;  // default opt — ll = false, exactly as the defect report builds it
        const std::vector<std::byte> bytes = encode(big);

        check(wire_ll(bytes), "oversize opaque body: encode auto-set the LL bit on the wire");
        check(bytes.size() == 6u + kBig, "oversize opaque body: 6-byte header + the whole body");
        check(wire_length(bytes) == kBig,
              "oversize opaque body: the length field is 70000, not 70000 & 0xFFFF (4464)");

        // Encode/decode symmetry (the #886 sibling concern): the frame encode now emits must be
        // one its own decoder accepts and reconstructs whole.
        const auto dec = decode(bytes);
        check(dec.has_value(), "oversize opaque frame round-trips through decode");
        tlv_t widened = big;
        widened.opt.ll = true;  // the ONLY difference decode should report
        check(dec && equal(*dec, widened),
              "decoded tree == the source tlv_t with opt.ll set (payload intact, untruncated)");
    }

    // Same for a STRUCTURED body: the children each fit a u16, their concatenation does not.
    {
        constexpr std::size_t kSeg = 40000;
        const std::vector<std::byte> a(kSeg, std::byte{0xA5});
        const std::vector<std::byte> b(kSeg, std::byte{0x5A});
        tlv_t root;
        root.type = type_t::PATH;
        root.opt.pl = true;
        tlv_t first;
        first.type = type_t::NAME;
        first.payload = a;
        tlv_t second;
        second.type = type_t::NAME;
        second.payload = b;
        root.children.push_back(first);
        root.children.push_back(second);

        constexpr std::size_t kBody = 2u * (4u + kSeg);  // 80008 > 0xFFFF
        const std::vector<std::byte> bytes = encode(root);
        check(wire_ll(bytes) && wire_length(bytes) == kBody,
              "oversize structured body: LL set and the length field is 80008");
        check(bytes.size() == 6u + kBody, "oversize structured body: 6-byte header + children");

        const auto dec = decode(bytes);
        check(dec.has_value() && dec->children.size() == 2,
              "oversize structured frame decodes to its two NAME children");
        tlv_t widened = root;
        widened.opt.ll = true;
        check(dec && equal(*dec, widened),
              "structured tree round-trips (children keep their own u16 headers)");
    }

    std::printf(g_failures == 0 ? "\nFRAME: PASS\n" : "\nFRAME: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
