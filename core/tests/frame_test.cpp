/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Frame-codec depth-cap test. Locks the docs/reference/01 §"Iterative parsing
 * requirement": nested TLVs are parsed iteratively (no recursion), bounded by
 * kMaxDepth = 32, with deeper frames rejected as NESTING_TOO_DEEP rather than
 * overflowing a small MCU call stack. A frame nested to the deepest legal depth
 * round-trips; one level deeper is rejected cleanly (no crash).
 */

#include "libtracer/frame.hpp"

#include <cstddef>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// Build a tree whose leaf sits at `leaf_depth`: `leaf_depth` structured PATH
// wrappers (opt.PL=1) around one empty opaque VALUE leaf. The root is depth 0,
// so the leaf is at depth `leaf_depth`.
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

// Count nesting depth of a decoded tree (root = 0).
int measured_depth(const tr::wire::tlv_t& t) {
    int d = 0;
    const tr::wire::tlv_t* cur = &t;
    while (!cur->children.empty()) {
        cur = &cur->children.front();
        ++d;
    }
    return d;
}

}  // namespace

int main() {
    using namespace tr::wire;
    std::printf("Frame depth-cap (iterative parse, kMaxDepth=%zu):\n", kMaxDepth);

    const int deepest_ok = static_cast<int>(kMaxDepth) - 1;  // leaf at depth 31

    // Deepest legal frame round-trips.
    {
        const tlv_t built = build_nested(deepest_ok);
        const std::vector<std::byte> bytes = encode(built);
        const auto dec = decode(bytes);
        check(dec.has_value(), "deepest legal nesting (leaf depth 31) decodes");
        if (dec) {
            check(measured_depth(*dec) == deepest_ok, "decoded tree has depth 31");
            check(equal(*dec, built), "deepest legal frame round-trips byte-exactly");
        }
    }

    // One level deeper is rejected as NESTING_TOO_DEEP (not a crash, not silent).
    {
        const tlv_t built = build_nested(deepest_ok + 1);  // leaf at depth 32
        const std::vector<std::byte> bytes = encode(built);
        const auto dec = decode(bytes);
        check(!dec.has_value(), "over-cap nesting (leaf depth 32) is rejected");
        check(!dec.has_value() && dec.error() == tr::wire::error_t::TLV_NESTING_TOO_DEEP,
              "rejection reason is NESTING_TOO_DEEP");
    }

    std::printf(g_failures == 0 ? "\nFRAME: PASS\n" : "\nFRAME: FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
