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
 * A child is addressed by its **mount path** — `/net/<module>/<name>` (RFC-0014,
 * ADR-0061): the run of `dst` segments this node consumes to route onward, and the run
 * prepended to `src` on the way back. Lookups are lock-free.
 *
 * **Link identity is the QUALIFIED name `"<module>/<name>"`** — one string, so every
 * existing consumer of a link identity (route-handle label tables, subscriber-edge
 * eviction, the departure notifiers) keeps working on an opaque string and needs no
 * signature change. The demux, which holds two raw segment spans and must not allocate
 * on the hot path (`bench_forward_heap`'s `allocs=0` gate), matches through
 * @ref by_segments, which compares the two parts in place and never builds a key.
 *
 * **Shape is per-CONNECTION, not per-module** — a refinement of ADR-0061, which assumed
 * a module declares it. It cannot: `ws-server`'s `peer_named` config decides whether the
 * bus facet is exposed, so two connections in one module may differ. The shape is
 * therefore captured ONCE at @ref add time from `link.bus()` and stored on the slot, which
 * still honours the ADR's actual requirement — no `bus()` probe on the forward path.
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
     * @brief One registered child: its qualified mount name, its link, and its shape.
     *
     * `name` is `"<module>/<name>"` (RFC-0014's `/net/<module>/<name>` minus the constant
     * `net` root). `link == nullptr` marks a TOMBSTONE (#494) — the slot is dead but stays
     * put so a concurrent lock-free reader's iteration remains valid. `multi_peer` is the
     * shape captured once at @ref add time, so the forward path never probes `bus()`.
     */
    struct child_t {
        std::string name;        /**< @brief Qualified mount name, `"<module>/<name>"`. */
        transport_t* link;       /**< @brief The link; nullptr marks a tombstone (#494). */
        bool multi_peer = false; /**< @brief Shape, captured once at @ref add time. */
    };

    /**
     * @brief Register the link addressed by qualified name @p name (`"<module>/<name>"`).
     *
     * Reuses @p name's tombstone if it has one (the create → remove → re-create path),
     * otherwise appends. Captures the link's SHAPE here, once, so the forward path never
     * probes `bus()`. A control-plane call: not for the forward hot path.
     */
    void add(std::string name, transport_t& link) {
        const bool multi_peer = link.bus() != nullptr;
        for (child_t& c : children_) {
            if (c.link == nullptr && c.name == name) {
                c.link = &link;
                c.multi_peer = multi_peer;
                return;
            }
        }
        children_.push_back({std::move(name), &link, multi_peer});
    }

    /**
     * @brief The live child whose qualified name equals @p segs joined by `/` (null if none).
     *
     * The forward demux's entry point. @p segs are raw segment spans read straight out of
     * the inbound frame (`<module>`, `<name>`, and for a bus peer `<peer>`); they are
     * compared against the stored key **in place**, so no key is ever built and the hot
     * path stays allocation-free (`bench_forward_heap`'s `allocs=0` gate).
     */
    [[nodiscard]] const child_t* by_segments(std::span<const std::string_view> segs) const {
        for (const child_t& c : children_) {
            if (c.link != nullptr && matches(c.name, segs)) return &c;
        }
        return nullptr;
    }

    /**
     * @brief Resolve @p peer within THIS endpoint's own peer table (ADR-0061).
     *
     * The per-endpoint replacement for `by_name`'s global cross-bus scan: a peer segment
     * is resolved against the multi-peer child it was addressed *through*, so two servers'
     * same-named peers stay distinct and a peer is never reachable through the wrong
     * module. A point-to-point child resolves no peer at all.
     * @return The directed per-peer endpoint, or nullptr if this child has no such peer.
     */
    [[nodiscard]] static transport_t* resolve_peer(const child_t& child, std::string_view peer) {
        if (!child.multi_peer || child.link == nullptr) return nullptr;
        bus_link_t* const bus = child.link->bus();
        return bus == nullptr ? nullptr : bus->peer_link(peer);
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
    /** @brief True iff @p key equals @p segs joined by `/`, compared without allocating. */
    [[nodiscard]] static bool matches(const std::string& key,
                                      std::span<const std::string_view> segs) noexcept {
        std::size_t need = segs.empty() ? 0 : segs.size() - 1;
        for (const std::string_view s : segs) need += s.size();
        if (key.size() != need) return false;
        std::size_t at = 0;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (i != 0 && key[at++] != '/') return false;
            if (key.compare(at, segs[i].size(), segs[i]) != 0) return false;
            at += segs[i].size();
        }
        return true;
    }

    // Slots are stable for this object's lifetime: only appended to, never erased or
    // reordered, so a lock-free reader's iteration stays valid across a control-plane
    // erase. No lock on the hot path.
    std::vector<child_t> children_;
};

}  // namespace tr::net
