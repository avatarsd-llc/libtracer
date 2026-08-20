/*
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

}  // namespace tr::graph
