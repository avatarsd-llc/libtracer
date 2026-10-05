/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#pragma once

/**
 * @file
 * @brief HOST TEST SUPPORT: an owning `tlv_t` tree read off a validated frame (#1829).
 *
 * The library reads frames in place (`wire::tlv_node_t::over` + `children()`) and no longer
 * ships the allocating `decode` that returned a `tlv_t` with vector children. Tests and host
 * benches still want a whole tree to compare (`wire::equal`) or to re-encode, so this header
 * builds one from the walker. It is not part of the library and nothing under core/src or
 * core/include includes it.
 *
 * Acceptance is the walker's by construction: the frame is validated once by
 * `tlv_node_t::over` (same grammar, same inline walk slots, same spill seam), then copied into
 * the tree. The tree's payload spans borrow @p input, as before.
 */

#include <cstddef>
#include <expected>
#include <span>

#include "libtracer/frame.hpp"

namespace tr::wire {

/** @brief Copy a validated node and its descendants into an owning `tlv_t` tree. */
[[nodiscard]] inline tlv_t to_tree(const tlv_node_t& n) {
    tlv_t t;
    t.type = n.type();
    t.opt = n.opt();
    t.payload = n.payload();
    t.trailer = n.trailer();
    for (const tlv_node_t c : n.children()) t.children.push_back(to_tree(c));
    return t;
}

/** @brief Validate exactly one TLV filling @p input and copy it into an owning tree. */
[[nodiscard]] inline std::expected<tlv_t, err_t> decode(
    std::span<const std::byte> input, mem::block_source_t& spill = mem::heap_source()) {
    const auto root = tlv_node_t::over(input, spill);
    if (!root) return std::unexpected(root.error());
    return to_tree(*root);
}

/** @brief @ref decode over a flat view's bytes; the tree borrows them. */
[[nodiscard]] inline std::expected<tlv_t, err_t> decode(
    const view::view_t& v, mem::block_source_t& spill = mem::heap_source()) {
    return decode(v.bytes(), spill);
}

}  // namespace tr::wire
