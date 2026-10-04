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
#include "libtracer/subscriber.hpp"

namespace tr::graph {

/**
 * @brief The wire→`subscriber_t` admission parse every subscriber door shares (ADR-0049):
 *        type-check the decoded record, then parse it once. Defined in `graph_fields.cpp`.
 *
 * @return False iff @p tlv is not a SUBSCRIBER — the doors' one shared TYPE_MISMATCH.
 */
[[nodiscard]] bool parse_wire_subscriber(const wire::tlv_node_t& tlv, subscriber_t& s);

}  // namespace tr::graph
