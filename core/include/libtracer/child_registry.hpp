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
 * back. Lookups are lock-free.
 *
 * **Mutation model (#494).** The table was add-only, which left a retired link's
 * `name → transport_t*` resident and dangling. @ref erase closes that, and it does so by
 * **tombstoning in place** — the slot's `link` is nulled and its NAME kept — never by
 * erasing from `children_`. That is deliberate: shifting or reallocating the vector under
 * a concurrent lock-free reader is a hard use-after-free, whereas a tombstone leaves every
 * slot address stable and a racing reader sees either the old pointer or `nullptr`. A
 * later @ref add of the SAME name reuses its tombstone, so create/remove churn on a stable
 * name set does not grow the table; a genuinely new name still appends, so the table's high
 * -water mark is the count of DISTINCT names ever registered. Compaction (and the full
 * mutation-vs-forward concurrency contract) lands with the RFC-0014 S5 liveness engine,
 * where the TSan gate and a safe reclamation scheme arrive together — see ADR-0061
 * (`docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md`).
 */
class child_registry_t {
   public:
    /**
     * @brief Register the link addressed by @p name.
     *
     * Reuses @p name's tombstone if it has one (the create → remove → re-create path),
     * otherwise appends. A control-plane call: not for the forward hot path.
     */
    void add(std::string name, transport_t& link) {
        for (child_t& c : children_) {
            if (c.link == nullptr && c.name == name) {
                c.link = &link;
                return;
            }
        }
        children_.push_back({std::move(name), &link});
    }

    /**
     * @brief Tombstone the link addressed by @p name — it stops resolving.
     *
     * Call in the same step the link/vertex is torn down, and BEFORE the `transport_t`
     * is destroyed, so no forward can resolve a freed object. The slot itself is kept
     * (see the class docs).
     * @return true if @p name named a live child, false if it named none.
     */
    bool erase(std::string_view name) {
        for (child_t& c : children_) {
            if (c.link != nullptr && c.name == name) {
                c.link = nullptr;
                return true;
            }
        }
        return false;
    }

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
            if (c.link != nullptr && c.name == name) return c.link;
        }
        for (const child_t& c : children_) {
            if (c.link == nullptr) continue;  // tombstone (#494) — no link to ask
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

    /** @brief Number of slots — live children PLUS tombstones (test introspection). */
    [[nodiscard]] std::size_t size() const noexcept { return children_.size(); }

    /** @brief Number of children that still resolve (test introspection). */
    [[nodiscard]] std::size_t live_size() const noexcept {
        std::size_t n = 0;
        for (const child_t& c : children_) {
            if (c.link != nullptr) ++n;
        }
        return n;
    }

   private:
    struct child_t {
        std::string name;
        transport_t* link;  // nullptr ⇒ tombstone (#494): the slot is dead but stays put
    };
    // Slots are stable for this object's lifetime: only appended to, never erased or
    // reordered, so a lock-free reader's iteration stays valid across a control-plane
    // erase. No lock on the hot path.
    std::vector<child_t> children_;
};

}  // namespace tr::net
