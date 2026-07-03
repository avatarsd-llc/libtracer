/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The connection registry: this node's `NAME → transport link` table — the
 * compositor demux of ADR-0037 (a transport-vertex "resolves the next path segment
 * to a child"). It replaces `fwd_router_t`'s anonymous `children_` field with ONE
 * named, shareable owner, so the connection table is not duplicated between the
 * router and `transport_vertex_t` (Brick 3a of the #83 Stage-2 flip).
 *
 * Layering (ADR-0016): this lives in `tr::net` (L5) and holds `transport_t*` — it is
 * NOT `graph.find` against the L4 vertex map, because an L4 `vertex_t` must never
 * know about a transport (`vertex.hpp`). ADR-0037 §Stage-2 phrased the dissolution as
 * "graph.find(child)"; the layering-safe realization is this single tr::net-owned
 * registry the router consults, which achieves the same "no duplicated children-table"
 * without inverting the L4↔L5 dependency (see ADR-0038 §Brick-3a note).
 */
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

/**
 * @brief This node's `NAME → transport link` table (the compositor demux, ADR-0037).
 *
 * A child NAME is the single path segment by which this node addresses a link — the
 * segment a `dst` names to route onward, and the segment prepended to `src` on the way
 * back. Immutable after setup (populated before frames flow), so lookups are lock-free.
 */
class child_registry_t {
   public:
    /** @brief Register the link addressed by @p name. Call once per link, during setup. */
    void add(std::string name, transport_t& link) { children_.push_back({std::move(name), &link}); }

    /**
     * @brief The link addressed by @p name (nullptr if none).
     *
     * Resolution order (ADR-0044): an exact static child NAME wins; otherwise each
     * registered BUS child (a link exposing @ref transport_t::bus) is asked to
     * resolve @p name as a currently-audible peer (@ref bus_link_t::peer_link),
     * yielding a DIRECTED per-peer endpoint. So an announced bus peer's name is a
     * routable next-hop segment with no registry mutation and no stored peer state
     * — the peer table lives inside the bus transport and expires with its traffic.
     */
    [[nodiscard]] transport_t* by_name(std::string_view name) const {
        for (const child_t& c : children_) {
            if (c.name == name) return c.link;
        }
        for (const child_t& c : children_) {
            if (bus_link_t* const bus = c.link->bus()) {
                if (transport_t* const peer = bus->peer_link(name)) return peer;
            }
        }
        return nullptr;
    }

    /** @brief The link whose NAME equals the raw segment bytes @p seg (nullptr if none). */
    [[nodiscard]] transport_t* by_segment(std::span<const std::byte> seg) const {
        return by_name(detail::as_string_view(seg));
    }

    /** @brief Number of registered children (test introspection). */
    [[nodiscard]] std::size_t size() const noexcept { return children_.size(); }

   private:
    struct child_t {
        std::string name;
        transport_t* link;
    };
    std::vector<child_t> children_;  // immutable after setup — no lock on the hot path
};

}  // namespace tr::net
