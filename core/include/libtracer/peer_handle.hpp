/**
 * @file
 * @brief The per-peer link identity's link-side constant, without the transport seam that mints it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The per-peer LINK IDENTITY's link-side constant, in a header of its own so a consumer can
 * name it without depending on the transport seam that mints the identity. `transport.hpp`
 * includes this and re-exports every name below.
 *
 * The identity itself, `tr::net::peer_handle_t`, is defined in `pair.hpp` (#1700), as a
 * distinct type over the PAIR: it is carried THROUGH the L4 resolve seam —
 * `graph::op_resolver_t::resolve` takes one, and the operation's ACL subject is derived from
 * it at the terminus (#375 Part 2 / #1266, ADR-0082) — so a graph header names it through the
 * wire-layer PAIR header and includes nothing from the net plane. So does `kPeerNameChars`, the
 * subject scratch the terminus derives into. This header includes that one, so every
 * spelling that reached it here still does.
 */
#pragma once

#include "libtracer/pair.hpp"

namespace tr::net {

/**
 * @brief The handle a link with no meaningful per-peer identity mints at link-up (#1294
 *        ruling 3) — one constant peer, valid for the link's whole life.
 *
 * A point-to-point kind exposing the bus facet for one peer, and a test double that carries
 * exactly one far side, hand this down rather than an invalid handle: the seam is UNIVERSAL,
 * so the "no peer identity here" case costs one constant at link-up instead of a branch in
 * every consumer.
 */
inline constexpr peer_handle_t kSolePeerHandle{0, 1};

}  // namespace tr::net
