/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0029 §4.1 / §5.1 — the PAIR path element: a vertex's owner-issued
 * `(u32 index, u32 generation)` spelled INSIDE a packed `PATH` body as the RFC-0018 escape
 * record `00 <kind = 0x16> <len = 8> <u32 LE index> <u32 LE generation>` (11 bytes).
 *
 * The two fields are RFC-0024 §4.4's, unchanged: the index is a slot in the owner's dense,
 * append-only vertex index, the generation is that vertex's retirement stamp (saturating,
 * never wrapping). The VALUE type is therefore the shared `pair_t` (`pair.hpp`) — the pair
 * RFC-0024 already carried — and only its spelling is new: one element among others in a `PATH`,
 * read by its kind and length, so NAMEs and PAIRs mix freely (RFC-0029 §4.2).
 *
 * `kind = 0x16` is shared with RFC-0027's 4-byte label until RFC-0029 slice S3 deletes that
 * form (§16 Q4 rules the reuse). The two are told apart by the declared `len`, which is the
 * only structural clause an element has besides its kind: `len = 8` is a PAIR, and any other
 * length is not one. A `kind = 0x16` record of neither ruled length refuses the ADDRESS
 * (`tr::path::invalid`), never the frame (§5.1).
 *
 * `tr::wire` (L2/L3): wire bytes to wire values and back. Nothing here dereferences a pair,
 * checks a generation against a graph or decides a route — that is the forwarder's (§6).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/packed_path.hpp"
#include "libtracer/pair.hpp"

/**
 * @file
 * @brief The RFC-0029 PAIR element (`00 16 08 <u32 index> <u32 generation>`) of a packed `PATH`.
 */

namespace tr::wire {

/**
 * @brief The value a PAIR element carries — the owner-issued `(index, generation)`.
 *
 * An alias of the shared @ref pair_t, not a new type: RFC-0029 §4.1 keeps RFC-0024's element
 * verbatim and changes only where it is spelled, so the deref (`graph_t::deref_vertex_slot`),
 * the vertex slot a mint reads and every consumer of the bound form share one struct.
 */
using path_pair_t = pair_t;

/** @brief The escape `kind` a PAIR element is spelled at (RFC-0029 §5.1, §16 Q4 — reused). */
inline constexpr std::uint8_t kPathPairKind = kPackedEscapeKindLabel;

/** @brief Payload bytes of a PAIR element — a `u32` index and a `u32` generation. */
inline constexpr std::size_t kPathPairBodyBytes = kPairBytes;

/** @brief Bytes one PAIR element occupies in a packed body — 11 (`00 <kind> <len>` + 8). */
inline constexpr std::size_t kPathPairRecordBytes = kPackedEscapeOverhead + kPathPairBodyBytes;

/**
 * @brief The two structural clauses of a PAIR element: `kind == 0x16` and `len == 8`.
 *
 * Kept as one predicate so the encoder, the decoder and the forwarder's head read agree on
 * what a PAIR is by construction rather than by three copies of two compares.
 */
[[nodiscard]] constexpr bool path_pair_record_valid(std::uint8_t kind,
                                                    std::size_t payload_len) noexcept {
    return kind == kPathPairKind && payload_len == kPathPairBodyBytes;
}

/**
 * @brief Write the 11-byte PAIR record for @p pair into @p out — the allocation-free encoder.
 *
 * Fixed size, so a caller that spells a chain on a hot or stack-only path (a delivery head, a
 * reverse route stored once) writes straight into storage it already sized.
 */
constexpr void path_pair_store(std::span<std::byte, kPathPairRecordBytes> out,
                               const path_pair_t& pair) noexcept {
    out[0] = static_cast<std::byte>(kPackedEscapeLen);
    out[1] = static_cast<std::byte>(kPathPairKind);
    out[2] = static_cast<std::byte>(kPathPairBodyBytes);
    pair_store_le(out.subspan(kPackedEscapeOverhead), pair);
}

/**
 * @brief Append one PAIR element — `00 16 08 <u32 LE index> <u32 LE generation>` — to @p out.
 *
 * Every `(index, generation)` has a spelling: validity is a property of the OWNER's index at
 * dereference time (§6 step 2), never of the bytes, so this refuses only when the source does.
 * A core byte array: no owning std type crosses the public API (#1781).
 *
 * @retval false The source refused (nothing appended).
 */
[[nodiscard]] inline bool emit_path_pair(mem::bytes_t& out, const path_pair_t& pair) noexcept {
    std::array<std::byte, kPathPairRecordBytes> rec{};
    path_pair_store(rec, pair);
    return out.append(rec.data(), rec.size());
}

/**
 * @brief Decode the 8-byte payload of a PAIR record (the inverse of @ref path_pair_store's
 *        value half).
 *
 * @note Precondition: `payload.size() == 8`, which @ref path_pair_record_valid establishes.
 */
[[nodiscard]] constexpr path_pair_t path_pair_load(std::span<const std::byte> payload) noexcept {
    return pair_load_le(payload);
}

/**
 * @brief Read the PAIR element at @p at of packed `PATH` body @p body, or `nullopt` when the
 *        record there is not one.
 *
 * `nullopt` covers a literal segment, a ragged record, a foreign kind and a `kind = 0x16`
 * record of any other length. What a hop does with the distinction (relay a foreign kind,
 * refuse a malformed address) is the element walker's (`path_element_at`).
 */
[[nodiscard]] constexpr std::optional<path_pair_t> path_pair_at(std::span<const std::byte> body,
                                                                std::size_t at) noexcept {
    const auto kind = packed_escape_kind(body, at);
    const auto payload = packed_escape_payload(body, at);
    if (!kind || !payload || !path_pair_record_valid(*kind, payload->size())) return std::nullopt;
    return path_pair_load(*payload);
}

}  // namespace tr::wire
