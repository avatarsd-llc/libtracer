/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The PATH_REF (0x14) element codec — the bound-path wire form of RFC-0024 §4.
 * A PATH_REF body is a bare array of fixed 8-byte elements, one per host on the
 * route: `(u32 vertex-map index, u32 retirement generation)`, little-endian, no
 * per-element framing. This header owns the element's byte layout and the purely
 * STRUCTURAL rules the grammar enforces (§4.2 envelope, §4.3 count bound); what an
 * element MEANS — the bounds check into a host's vertex map, the generation
 * compare, the ACL re-check at the deref'd vertex — is L4 routing and is not here.
 *
 * Lives in `tr::wire` (L2/L3): it turns wire bytes into wire types and back, and
 * knows nothing about a graph.
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "libtracer/byteorder.hpp"

/**
 * @file
 * @brief The `PATH_REF` (`0x14`) element layout and structural rules (RFC-0024 §4).
 */

namespace tr::wire {

/**
 * @brief Bytes in one `PATH_REF` element — a u32 index plus a u32 generation (RFC-0024 §4.4).
 *
 * Fixed-stride by construction: element *i* is `body[8i .. 8i+8)`, computed rather than
 * parsed, which is why the format carries no per-element TLV header (RFC-0024 §4.2).
 */
inline constexpr std::size_t kPathRefElementBytes = 8;

/**
 * @brief Max elements in a `PATH_REF` — 255 (RFC-0024 §4.3, normative bound).
 *
 * Derived, not chosen: 255 is the largest count for which every per-element quantity (the
 * count itself, the largest index, a receiver's per-element table dimension) fits a `u8`,
 * and it sits above both reachable ceilings a canonical `dst` can mint from (H ≤ 69 under
 * today's NAME encoding, H ≤ 171 packed) — so it is an encoding-independent ceiling, in the
 * discipline RFC-0023 set for the canonical segment cap.
 */
inline constexpr std::size_t kMaxPathRefElements = 255;

/**
 * @brief Max `PATH_REF` body bytes — 2040 (RFC-0024 §4.3), the count bound in bytes.
 *
 * Three orders inside a u16 length, which is why `opt.LL = 1` is never applicable to a
 * `PATH_REF` (RFC-0024 §4.2).
 */
inline constexpr std::size_t kMaxPathRefBodyBytes = kMaxPathRefElements * kPathRefElementBytes;

/**
 * @brief One bound-path element: a node-scoped reference to one host's own vertex.
 *
 * Element 0 is the origin's reference to the connection vertex for its first hop; element
 * *i* is forwarder *i*'s reference to its connection vertex for hop *i+1*; the last element
 * is the terminus host's reference to the target vertex itself. Nothing in an element means
 * anything anywhere but on the host that minted it — it is an address, never a capability
 * (RFC-0024 §4.1, §6).
 */
struct path_ref_element_t {
    /** @brief The minting host's vertex-map index (u32 LE — unreachable on both targets). */
    std::uint32_t index = 0;
    /** @brief That vertex's retirement generation at mint time (u32 LE; saturates, never wraps). */
    std::uint32_t generation = 0;
    /** @brief Value equality over both fields. */
    constexpr bool operator==(const path_ref_element_t&) const noexcept = default;
};

/**
 * @brief The element count a `PATH_REF` body of @p body_len bytes carries (RFC-0024 §4.3).
 *
 * There is no count field on the wire: the count IS `length / 8`. Only meaningful for a
 * body @ref path_ref_body_valid accepts.
 */
[[nodiscard]] constexpr std::size_t path_ref_element_count(std::size_t body_len) noexcept {
    return body_len / kPathRefElementBytes;
}

/**
 * @brief The purely STRUCTURAL `PATH_REF` rules of RFC-0024 §4.2/§4.3 — the grammar's check.
 *
 * A `PATH_REF` header that fails this is `tr::frame::invalid`, exactly as a set reserved bit
 * is. All four clauses are shape, not meaning, which is what makes them a codec rule rather
 * than a resolver one (contrast `PATH`'s child-type constraint, reference/05 §Pitfalls):
 *
 * - **`opt.PL` MUST be 0** — the body is a fixed-stride record array, not child TLVs; a
 *   generic `PL = 1` walker would read the first four body bytes as a TLV header and
 *   mis-frame the whole body.
 * - **`opt.LL` MUST be 0** — the u32 length width buys nothing under a 2040-byte cap, so it
 *   is forbidden rather than merely unused.
 * - **`length % 8 == 0`** — a body that is not a whole number of elements has no reading.
 * - **`length <= 2040`** — the §4.3 element-count bound, in bytes.
 */
[[nodiscard]] constexpr bool path_ref_body_valid(bool pl, bool ll, std::size_t body_len) noexcept {
    return !pl && !ll && (body_len % kPathRefElementBytes) == 0 && body_len <= kMaxPathRefBodyBytes;
}

/**
 * @brief Read element @p i out of a `PATH_REF` body (little-endian, both fields).
 *
 * @note Precondition: `(i + 1) * 8 <= body.size()` — i.e. @p i is below
 *       @ref path_ref_element_count of a body @ref path_ref_body_valid accepted
 *       (debug-asserted). The bound is the caller's to hold because it is free
 *       there and not here: a decode path has already had the whole body shape
 *       checked by the grammar, so re-deriving the count per element would price
 *       a division into every hop of a forward. A caller that cannot prove @p i —
 *       anything walking a *foreign* frame's elements — gates its loop on
 *       @ref path_ref_element_count first; past the end this reads whatever
 *       follows the body, it does not fail.
 */
[[nodiscard]] constexpr path_ref_element_t path_ref_element_at(std::span<const std::byte> body,
                                                               std::size_t i) noexcept {
    assert((i + 1) * kPathRefElementBytes <= body.size());
    const std::span<const std::byte> e =
        body.subspan(i * kPathRefElementBytes, kPathRefElementBytes);
    return path_ref_element_t{
        .index = detail::load_le<std::uint32_t>(e.subspan(0, 4)),
        .generation = detail::load_le<std::uint32_t>(e.subspan(4, 4)),
    };
}

/**
 * @brief Write one element into the 8 bytes at @p out (little-endian, both fields).
 *
 * @note Precondition: `out.size() >= 8` (debug-asserted). The inverse of
 *       @ref path_ref_element_at, and the same contract: the emitter sizes the buffer
 *       from the element count before the first store, so the check belongs there once
 *       rather than here per element.
 */
constexpr void path_ref_store_element(std::span<std::byte> out,
                                      const path_ref_element_t& e) noexcept {
    assert(out.size() >= kPathRefElementBytes);
    detail::store_le<std::uint32_t>(out.subspan(0, 4), e.index);
    detail::store_le<std::uint32_t>(out.subspan(4, 4), e.generation);
}

}  // namespace tr::wire
