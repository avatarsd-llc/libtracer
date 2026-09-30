/**
 * @file
 * @brief `history_of` — the tests' vector-shaped view of `graph_t::history`, which fills a
 *        caller-owned span (RFC-0028 D11) rather than returning a container.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "libtracer/graph.hpp"

namespace tr::testing {

/** @brief More entries than any test's ring retains, so one call always takes the whole ring. */
inline constexpr std::size_t kHistoryProbe = 256;

/**
 * @brief The whole STREAM ring of @p v, oldest first, as a vector — or the status `history`
 *        refused with. A test convenience: the vector is the TEST's allocation, not the graph's.
 */
inline tr::graph::result_t<std::vector<tr::graph::value_ref_t>> history_of(
    const tr::graph::graph_t& g, tr::graph::vertex_handle_t v) {
    std::array<tr::graph::value_ref_t, kHistoryProbe> buf;
    const tr::graph::result_t<std::size_t> n = g.history(v, buf);
    if (!n) return std::unexpected(n.error());
    return std::vector<tr::graph::value_ref_t>(buf.begin(), buf.begin() + *n);
}

}  // namespace tr::testing
