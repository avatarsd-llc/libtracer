/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The interned per-peer LINK IDENTITY and its companion constants, in a header of
 * their own so a consumer can name the identity without depending on the transport
 * seam that mints it. `transport.hpp` includes this and re-exports every name below,
 * so nothing that already spells `tr::net::peer_handle_t` has to change.
 *
 * WHY IT IS SPLIT OUT (#375 Part 2 / #1266). The handle is carried THROUGH the L4
 * resolve seam — `graph::op_resolver_t::resolve` takes one, and the operation's ACL
 * subject is derived from it at the terminus. `tr::graph` is L4 and `tr::net` is the
 * transport plane above it (core/STYLE.md: dependencies point up the layers only), so
 * a graph header may not include `transport.hpp`. It may include this one: eight bytes
 * of POD, two constants, and no transport type at all.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

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
 * It is `(index, generation)`, the same node-local-index-plus-validate-on-use-stamp primitive
 * the in-tree edge binding (#830), the RFC-0024 vref and the ESP link's session ref already
 * mint — a 8-byte trivially-copyable POD, cheap to copy per frame and cheap to key a table by.
 * The two fields are OPAQUE to a consumer: only the minting link knows what an index means,
 * and a consumer may only compare handles, hash them, and hand them back.
 *
 * **It is not a session reference.** A session ref (`httpd_ws_link_t::session_ref_t`,
 * #1146/#1262) is ONE SUPPLIER of a handle, not the handle itself: an announce-census CAN
 * peer has no session at all and still needs a stable link key, so the handle is the general
 * concept and the session ref produces one.
 *
 * **It does not carry the subject.** A per-peer auth subject is DERIVED from the handle at
 * the terminus (`graph::op_resolver_t`'s subject seam) rather than carried in it, which is
 * what keeps the per-frame POD minimal (#1294 ruling 2).
 *
 * **It is never absent on the bus seam.** Every handle the peer-receiver seam hands down is
 * `valid()`: a link with no meaningful per-peer identity mints `kSolePeerHandle` once at
 * link-up and hands that down for every frame, so no consumer of that seam needs a "handle
 * absent" branch (#1294 ruling 3). A DEFAULT-constructed handle is still the "no peer here"
 * value, and is what a link with no per-peer identity at all reports.
 */
struct peer_handle_t {
    /** @brief The minting link's own peer INDEX — meaningless to anyone else. */
    std::uint32_t index = 0;
    /** @brief The validate-on-use stamp; `0` is reserved to mean "no peer". */
    std::uint32_t generation = 0;

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

/**
 * @brief Scratch big enough for any per-peer NAME or SUBJECT token a link resolves
 *        (`bus_link_t::peer_name`, `transport_t::peer_subject`) — `p<slot>` / `n<node>`
 *        are both far inside it.
 */
inline constexpr std::size_t kPeerNameChars = 32;

}  // namespace tr::net
