/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Frame-codec nesting test. Locks the docs/reference/01 §"Iterative parsing
 * requirement" under RFC-0006: nested TLVs are parsed iteratively (no
 * recursion) and depth is bounded by the RECEIVER'S decode resources, never a
 * constant. A heap-resourced decode parses a frame far deeper than the old cap
 * of 32; a null-spill grammar walk whose inline slots ARE its whole budget
 * rejects a deeper frame cleanly as NESTING_TOO_DEEP ("exceeds this receiver's
 * decode resources") — no crash, no throw.
 */

#include "libtracer/frame.hpp"

#include <cstddef>
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

    std::printf(g_failures == 0 ? "\nFRAME: PASS\n" : "\nFRAME: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
