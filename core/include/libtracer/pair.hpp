/**
 * @file
 * @brief The owner-issued `(u32 index, u32 generation)` PAIR, its little-endian load and
 *        store, and the per-peer link handle spelled over it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ONE type for one thing (RFC-0029 §2.1, #1700). The same eight bytes were spelled as four
 * structs: the RFC-0024 path-ref element, the graph's vertex slot, the RFC-0029 PAIR element
 * and the net plane's peer handle. The first three are the SAME value — a node-scoped slot
 * index and the generation stamping that tenancy — so they are aliases of @ref tr::wire::pair_t
 * here, and every load and store of their wire form is the one pair below.
 *
 * The peer handle is the same SHAPE with a different meaning: the minting link's own peer
 * index, opaque to everyone else, and never a vertex reference. It stays a DISTINCT type
 * (`tr::net::peer_handle_t`, derived from the pair) so a vertex slot cannot be passed where a
 * peer is expected, and it lives here rather than in the net plane so a graph-layer header
 * (`op_resolve.hpp`) can name it without including any transport header (ADR-0082: the
 * subject is derived from the handle at the terminus, never carried in it).
 *
 * `tr::wire` (L2/L3): plain values and their byte form. Nothing here dereferences a pair or
 * checks a generation against a graph.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "libtracer/byteorder.hpp"

namespace tr::wire {

/**
 * @brief An owner-issued `(index, generation)` — RFC-0029's PAIR, RFC-0024 §4.4's element.
 *
 * The index is a slot in the owner's dense, append-only index; the generation is that slot's
 * retirement stamp (saturating, never wrapping). The two are read and handed out TOGETHER:
 * an index without the generation that was current when it was read is not a reference.
 */
struct pair_t {
    /** @brief The owner's slot index (u32 LE on the wire). */
    std::uint32_t index = 0;
    /** @brief That slot's generation when the pair was issued (u32 LE on the wire). */
    std::uint32_t generation = 0;

    /** @brief Value equality over both fields. */
    [[nodiscard]] friend constexpr bool operator==(pair_t, pair_t) noexcept = default;
};

static_assert(sizeof(pair_t) == 8, "a PAIR is two u32s and nothing else");
static_assert(std::is_trivially_copyable_v<pair_t>);

/** @brief Bytes a PAIR occupies on the wire: a u32 LE index, then a u32 LE generation. */
inline constexpr std::size_t kPairBytes = 8;

/**
 * @brief Read a PAIR from the first eight bytes of @p in (both fields little-endian).
 *
 * @note Precondition: `in.size() >= kPairBytes`; every caller has sized its window first.
 */
[[nodiscard]] constexpr pair_t pair_load_le(std::span<const std::byte> in) noexcept {
    return pair_t{.index = detail::load_le<std::uint32_t>(in.subspan(0, 4)),
                  .generation = detail::load_le<std::uint32_t>(in.subspan(4, 4))};
}

/**
 * @brief Write @p p into the first eight bytes of @p out (both fields little-endian).
 *
 * @note Precondition: `out.size() >= kPairBytes`.
 */
constexpr void pair_store_le(std::span<std::byte> out, pair_t p) noexcept {
    detail::store_le<std::uint32_t>(out.subspan(0, 4), p.index);
    detail::store_le<std::uint32_t>(out.subspan(4, 4), p.generation);
}

}  // namespace tr::wire

namespace tr::net {

/**
 * @brief An opaque per-peer LINK HANDLE — the identity the peer-receiver seam carries
 *        (#1294), minted once when a peer becomes audible and valid until it departs.
 *
 * The seam used to re-supply a peer NAME string on every inbound frame, which forced every
 * consumer that wanted a per-peer identity to re-derive one from that string per frame — a
 * hash and a map find on the subscribe path (#1266), and nothing at all to hang a per-peer
 * auth subject off (#375 Part 2). This handle is that identity, handed down instead.
 *
 * It is a PAIR — `(index, generation)`, the node-local-index-plus-validate-on-use-stamp
 * primitive the vertex slot, the RFC-0029 path element and the ESP link's session ref also
 * use — but a DISTINCT type over it: the index is the minting link's own peer index, not a
 * vertex slot, so a @ref tr::wire::pair_t never converts to one. The two fields are OPAQUE to
 * a consumer: only the minting link knows what an index means, and a consumer may only compare
 * handles, hash them, and hand them back.
 *
 * **It is not a session reference.** A session ref (`httpd_ws_link_t::session_ref_t`,
 * #1146/#1262) is ONE SUPPLIER of a handle, not the handle itself: an announce-census CAN
 * peer has no session at all and still needs a stable link key, so the handle is the general
 * concept and the session ref produces one.
 *
 * **It does not carry the subject.** A per-peer auth subject is DERIVED from the handle at
 * the terminus (`graph::op_resolver_t`'s subject seam) rather than carried in it, which is
 * what keeps the per-frame POD minimal (#1294 ruling 2, ADR-0082).
 *
 * **It is never absent on the bus seam.** Every handle the peer-receiver seam hands down is
 * `valid()`: a link with no meaningful per-peer identity mints `kSolePeerHandle` once at
 * link-up and hands that down for every frame, so no consumer of that seam needs a "handle
 * absent" branch (#1294 ruling 3). A DEFAULT-constructed handle is still the "no peer here"
 * value, and is what a link with no per-peer identity at all reports.
 */
struct peer_handle_t : wire::pair_t {
    /** @brief True iff this handle names a peer (a zero generation never does). */
    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }

    /** @brief The handle's whole identity as one integer — the key an interning
     *         consumer (#1266) hashes, so it never has to know the field split. */
    [[nodiscard]] constexpr std::uint64_t bits() const noexcept {
        return (static_cast<std::uint64_t>(generation) << 32) | index;
    }

    /** @brief Handles compare by identity — same index AND same generation. */
    [[nodiscard]] friend constexpr bool operator==(peer_handle_t, peer_handle_t) noexcept = default;
};

static_assert(sizeof(peer_handle_t) == 8, "the per-frame peer handle stays an 8-byte POD");
static_assert(std::is_trivially_copyable_v<peer_handle_t>);
static_assert(!std::is_convertible_v<wire::pair_t, peer_handle_t>,
              "a vertex PAIR never converts to a peer handle");

/**
 * @brief Scratch big enough for any per-peer NAME or SUBJECT token a link resolves
 *        (`bus_link_t::peer_name`, `transport_t::peer_subject`) — `p<slot>` / `n<node>`
 *        are both far inside it. Beside the handle because the terminus derives the
 *        subject into it from the handle (ADR-0082), in the graph layer.
 */
inline constexpr std::size_t kPeerNameChars = 32;

}  // namespace tr::net
