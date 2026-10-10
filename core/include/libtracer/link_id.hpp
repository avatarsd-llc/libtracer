/**
 * @file
 * @brief The node-scoped interned link identity that keys the subscriber index, and the
 *        pairs a remote edge delivers over.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The NODE-SCOPED interned LINK IDENTITY the subscriber index is keyed by, in a header
 * of its own so the transport plane can name it without depending on `graph.hpp`, and
 * so `graph.hpp` can name it without growing a second include.
 *
 * WHY IT IS ITS OWN TYPE (#1266 / #1417). It is NOT `tr::net::peer_handle_t`. A handle is
 * meaningful only to the link that minted it, so two links each minting
 * `tr::net::kSolePeerHandle` name different peers with equal handles — which is precisely
 * why #1366 refused to key the index on one. A `link_id_t` is minted by `graph_t` from a
 * node-scoped dense slot space against the link's ADMITTED-OVER NAME, so two links always
 * get two ids and the identity means the same thing everywhere in the node.
 */
#pragma once

#include <cstdint>
#include <type_traits>

#include "libtracer/pair.hpp"

namespace tr::graph {

/**
 * @brief A link's interned identity — the token `graph_t::subscribe_wire` carries so the
 *        subscriber index never hashes a name (#1266).
 *
 * `(slot, generation)`, the same node-local-index-plus-validate-on-use-stamp primitive the
 * in-tree edge binding (#830), the RFC-0024 vref and `tr::net::peer_handle_t` already mint.
 * The slot addresses the index's dense entry directly; the stamp is what makes a token that
 * outlived its link SAFE rather than merely unlucky — a released slot's stamp moves, so a
 * stale token fails to validate and the index falls back to interning the name it was handed.
 *
 * That fallback is the whole safety argument. A carried token is an OPTIMISATION: every door
 * that takes one also takes the name, and a token that is absent, stale, or simply wrong for
 * the name in hand costs a lookup and never a wrong entry. #1071 and #943 exist to keep a
 * departing link's edges reachable, and nothing here may put one out of reach.
 *
 * A default-constructed token is the "no token, intern by name" value, which is what every
 * caller that does not carry one passes and what the index sees today.
 */
struct link_id_t {
    /** @brief The index's dense slot; meaningless without @ref generation. */
    std::uint32_t slot = 0;
    /** @brief The validate-on-use stamp; `0` is reserved to mean "no token". */
    std::uint32_t generation = 0;

    /** @brief True iff this token could name a live index entry (a zero stamp never does). */
    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }

    /** @brief The token's whole identity as one integer — the word a lock-free per-link
     *         cache publishes with a single store, so a reader can never pair one link's
     *         slot with another's stamp (the field-tearing shape #882 fixed one seam out). */
    [[nodiscard]] constexpr std::uint64_t bits() const noexcept {
        return (static_cast<std::uint64_t>(generation) << 32) | slot;
    }

    /** @brief The inverse of @ref bits. */
    [[nodiscard]] static constexpr link_id_t from_bits(std::uint64_t bits) noexcept {
        return link_id_t{static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32)};
    }

    /** @brief Tokens compare by identity — same slot AND same stamp. */
    [[nodiscard]] friend constexpr bool operator==(link_id_t, link_id_t) noexcept = default;
};

static_assert(sizeof(link_id_t) == 8, "the carried link token stays an 8-byte POD");
static_assert(std::is_trivially_copyable_v<link_id_t>);

/**
 * @brief The link a remote subscriber edge delivers over, as pairs (#1941, #1622 L6): this
 *        node's PAIR for the link's connection vertex, and the bus peer's handle under it.
 *
 * The identity the delivery path resolves, in place of the link NAME the edge used to store.
 * Resolving it is a pair deref, not a name scan: the connection vertex's index selects the
 * registry slot bound to it, and on a bus door the peer's handle selects the peer
 * (`tr::net::bus_link_t::peer_link_of`). L4 never interprets either pair; the transport plane
 * mints them at admission and reads them back at delivery.
 *
 * **What makes two of them the same link** is @ref same_link, and it ignores both
 * generations on purpose. A connection vertex is keyed by its mount and never retired while
 * the node lives (#1940), so its index stands for the mount's name for the node's lifetime.
 * A peer's index is its position under the door, which is what its NAME spells (`p<slot>`,
 * a CAN node id — ADR-0073 §2), so the generation that separates two sessions of one slot
 * never separated their edges either: the name each edge stored was the same. Comparing the
 * indexes is the name compare with the name taken out, and a departure therefore evicts
 * exactly the edges it evicted before.
 *
 * A default-constructed value names no link (@ref valid is false). A vertex PAIR's generation
 * starts at 0, so "no link" is the index @ref kNoConn rather than a zero stamp.
 */
struct link_pair_t {
    /** @brief The @ref conn index that names no connection vertex — no vertex map holds it. */
    static constexpr std::uint32_t kNoConn = 0xFFFFFFFFu;

    /** @brief This node's PAIR for the link's connection vertex; index @ref kNoConn ⇒ no link. */
    wire::pair_t conn{.index = kNoConn, .generation = 0};
    /** @brief The bus peer's handle (`tr::net::peer_handle_t`'s pair); generation `0` ⇒ the
     *         link itself, not one of its peers. */
    wire::pair_t peer{};

    /** @brief True iff this names a link. */
    [[nodiscard]] constexpr bool valid() const noexcept { return conn.index != kNoConn; }

    /** @brief True iff this names one peer of a bus link rather than the link itself. */
    [[nodiscard]] constexpr bool has_peer() const noexcept { return peer.generation != 0; }

    /** @brief True iff @p a and @p b both name a link and it is the same one — compared by
     *         the indexes alone (see above for why neither generation takes part). */
    [[nodiscard]] friend constexpr bool same_link(link_pair_t a, link_pair_t b) noexcept {
        return a.valid() && b.valid() && a.conn.index == b.conn.index &&
               a.has_peer() == b.has_peer() && a.peer.index == b.peer.index;
    }

    /** @brief Value equality over both pairs, generations included. */
    [[nodiscard]] friend constexpr bool operator==(link_pair_t, link_pair_t) noexcept = default;
};

static_assert(sizeof(link_pair_t) == 16, "a remote edge's link is two pairs and nothing else");
static_assert(std::is_trivially_copyable_v<link_pair_t>);

/**
 * @brief What the transport plane hands a remote SUBSCRIBE about the link it arrived on: the
 *        index token and the delivery pairs.
 *
 * The two answer different questions. @ref token is the subscriber index's subscript, an
 * optimisation the index validates against the name (see @ref link_id_t). @ref link is where
 * the edge delivers, and it is the edge's only record of that: an edge admitted without one
 * has nowhere to deliver and is refused.
 */
struct carried_link_t {
    link_id_t token{};  /**< @brief The index token; default ⇒ the index interns the name. */
    link_pair_t link{}; /**< @brief The pairs the edge delivers over; default ⇒ none. */
};

}  // namespace tr::graph
