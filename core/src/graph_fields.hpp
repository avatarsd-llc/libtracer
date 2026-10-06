/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief The seam between `graph.cpp` and the `:`-field unit `graph_fields.cpp` (#1711).
 *
 * The colon-field surface lives in its own translation unit; the one thing it shares with the
 * rest of the graph runtime that is not a `graph_t` member is the SUBSCRIBER admission parse,
 * which the `:subscribers` write row and `graph_t::subscribe_wire` (in `graph.cpp`) both run.
 * It is defined beside the row, its two-of-three user, so its wire helpers compile once.
 *
 * NB this is a PRIVATE header — core/src only, never installed.
 */
#pragma once

#include "libtracer/frame.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/status.hpp"
#include "libtracer/subscriber.hpp"

namespace tr::graph {

/**
 * @brief The wire→`subscriber_t` admission parse every subscriber door shares (ADR-0049):
 *        decode and type-check the record, parse it once, and retain it zero-copy. Defined in
 *        `graph_fields.cpp`.
 *
 * @param src The graph's table source: the cold half draws from it (#1885).
 * @return TYPE_MISMATCH iff @p record is not one valid SUBSCRIBER TLV — the doors' one shared
 *         refusal of the record — and BACKPRESSURE when what the record needs could not be
 *         held.
 */
[[nodiscard]] result_t<void> parse_wire_subscriber(const view::view_t& record, subscriber_t& s,
                                                   mem::block_source_t& src);

}  // namespace tr::graph
