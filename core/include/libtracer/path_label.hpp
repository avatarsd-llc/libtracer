/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The PATH LABEL value — RFC-0027 §4's 32-bit per-host, per-path-element alias:
 * a u16 slot index in the minting host's label table plus that slot's u16
 * generation at mint time, little-endian. Host-assigned, NEVER a content hash
 * (§4.2), and node-scoped: a path label means something only on the host that
 * minted it (§4.1's label-scope rule).
 *
 * This header owns the VALUE, its byte order, and — since RFC-0027 §5.3 closed —
 * the ELEMENT SPELLING that carries it. All three of §5.3's sub-questions are ruled:
 *
 * - **Amendment 4:** a label is a self-describing element of its own, never a variant
 *   spelling of a name segment.
 * - **Amendment 5:** those bytes are RFC-0018 §8's escape record —
 *   `00 <u8 kind=0x16> <u8 len=4> <u32 LE label>`, **7 B per element**. The 8-byte
 *   `PATH_LABEL` TLV-child spelling of §5.3's candidate block is **never built**, so no
 *   type code is assigned and there is no per-element option byte to constrain. Amendment 5
 *   also rules §5.3 sub-question 3: a `PATH` carrying a label is **NOT** admissible as a
 *   `path_lookup_key` — that refusal is `packed_path_valid_key`'s, unchanged.
 * - **Amendment 6:** one label covers a hop's whole **local part** (its entire mount run),
 *   not one segment. That is a property of what a minting hop REPLACES, not of these bytes:
 *   the record is the same 7 B whether the run it stands for is one segment or five.
 *
 * What this header still does NOT do is decide **when** an element is minted or what it
 * resolves to. Emitting the record is grammar; §6.2's trigger, the reply-leg rewrite and the
 * deref are the forwarder's (later cars of #1325).
 *
 * Lives in `tr::wire` (L2/L3), beside `path_ref.hpp`, for the identical reason:
 * it turns wire bytes into a wire type and back, and knows nothing about a graph.
 * What a label MEANS — the bounds check into the minting host's table, the
 * generation compare, the ACL re-check at the dereferenced vertex (§7, §8.2) —
 * is the net plane's, and lives in `path_label_table.hpp`.
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/packed_path.hpp"

/**
 * @file
 * @brief The RFC-0027 §4 path-label value: `(u16 index, u16 generation)` as a u32 LE, and the
 *        §5.3 element that carries it — RFC-0018's escape record at `kind = 0x16`.
 */

namespace tr::wire {

/**
 * @brief Bytes one path label occupies on the wire — 4 (RFC-0027 §4.1).
 *
 * The 16/16 split is a ruling, not a candidate: §4.4 rejects a `u64` label on the cost model
 * (it doubles the cost of the one thing the design exists to make small) and a bare `u16`
 * outright (a slot index with no staleness guard is #603's misroute). What §5.3 leaves open is
 * how these four bytes are FRAMED inside a `PATH`, never how many there are.
 */
inline constexpr std::size_t kPathLabelBodyBytes = 4;

/**
 * @brief Slots a 16-bit index can name — 65 536 (RFC-0027 §4.1).
 *
 * A bound on *simultaneously minted path parts*, not on vertices, not on peers and not on
 * flows: a mint happens once per subscription's first fire, per hop (§6.2). §8.3's injected
 * capacity and its per-peer ceiling both sit below this, so the ceiling a deployment feels is
 * the one it chose — this is only where the encoding stops.
 */
inline constexpr std::size_t kPathLabelSlotSpace = 0x10000;

/**
 * @brief The largest generation a slot can reach — RFC-0027 §4.3.1's SATURATION point.
 *
 * A slot whose generation reaches this value stops advancing and is retired permanently: it
 * MUST NOT be minted into again, not after reclamation, not after a peer departs, and not
 * after the table empties. The rule preserves RFC-0024 §4.4 rule 3 (*"the generation MUST
 * saturate, never wrap"*) verbatim across both `(slot, generation)` fields of the doc set, so
 * an implementer reads one answer rather than one per type code — and it costs zero wire bits,
 * because it constrains only what a minting host does with its own table.
 */
inline constexpr std::uint16_t kPathLabelMaxGeneration = 0xFFFF;

/**
 * @brief The generation a label never carries — `0` means "no label" (RFC-0027 §4.1).
 *
 * A fresh slot is born at generation 1, so a zeroed 4-byte body is not a label anyone minted
 * and @ref path_label_t::valid rejects it without a table lookup. Same reservation, same
 * reason, as `net::peer_handle_t`'s zero generation.
 */
inline constexpr std::uint16_t kPathLabelNoGeneration = 0;

/**
 * @brief One path label: a node-scoped reference to one local PART of an address.
 *
 * The index is an ARRAY SLOT handed out by the minting host (§4.1), which is what makes the
 * value bounds-checkable — the unforgeable-validation property RFC-0024 §4.5 rejects a raw
 * pointer for. It is never a hash of the path text: a hash collision is a MIS-DELIVERY, the
 * failure class the doc set closes by construction rather than by digest width (§4.2).
 *
 * The generation is the anti-mis-route guard, compared against the slot's stamp on every use
 * and only ever moving forward, so a stale label never becomes valid by waiting (§7.1). It
 * saturates and retires its slot; it never wraps (§4.3.1).
 *
 * A label is an **address, never a capability**: §8.1 mints only post-auth, and §8.2 re-checks
 * the ACL at the dereferenced vertex on every labelled operation. A generation match says the
 * part is the same one, never that the caller may still act on it.
 */
struct path_label_t {
    /** @brief The minting host's own table slot — meaningless on any other host. */
    std::uint16_t index = 0;
    /** @brief That slot's generation at mint time (saturates, never wraps — §4.3.1). */
    std::uint16_t generation = kPathLabelNoGeneration;

    /** @brief True iff this could name a minted slot (a zero generation never does). */
    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation != kPathLabelNoGeneration;
    }

    /** @brief The whole label as the one u32 the wire carries — the form a table keys by. */
    [[nodiscard]] constexpr std::uint32_t bits() const noexcept {
        return (static_cast<std::uint32_t>(generation) << 16) | index;
    }

    /** @brief Labels compare by identity — same slot AND same generation. */
    [[nodiscard]] friend constexpr bool operator==(path_label_t, path_label_t) noexcept = default;
};

static_assert(sizeof(path_label_t) == 4, "a path label stays the 4-byte value the wire carries");

/**
 * @brief Rebuild a label from its u32 wire form — the inverse of @ref path_label_t::bits.
 */
[[nodiscard]] constexpr path_label_t path_label_from_bits(std::uint32_t bits) noexcept {
    return path_label_t{
        .index = static_cast<std::uint16_t>(bits & 0xFFFFU),
        .generation = static_cast<std::uint16_t>(bits >> 16),
    };
}

/**
 * @brief Read a label out of the 4 bytes at @p body (little-endian, both halves).
 *
 * @note Precondition: `body.size() >= 4` (debug-asserted), which a caller holds by having had
 *       the body length checked once — the same division of labour `path_ref_element_at`
 *       states, and for the same reason: re-deriving the bound per element would price a
 *       check into every hop of a forward.
 */
[[nodiscard]] constexpr path_label_t path_label_load(std::span<const std::byte> body) noexcept {
    assert(body.size() >= kPathLabelBodyBytes);
    return path_label_from_bits(
        detail::load_le<std::uint32_t>(body.subspan(0, kPathLabelBodyBytes)));
}

/**
 * @brief Write @p label into the 4 bytes at @p out (little-endian) — the inverse of
 *        @ref path_label_load.
 *
 * @note Precondition: `out.size() >= 4` (debug-asserted); the emitter sizes its buffer once,
 *       ahead of the first store.
 */
constexpr void path_label_store(std::span<std::byte> out, path_label_t label) noexcept {
    assert(out.size() >= kPathLabelBodyBytes);
    detail::store_le<std::uint32_t>(out.subspan(0, kPathLabelBodyBytes), label.bits());
}

/**
 * @brief Bytes one label ELEMENT occupies inside a packed `PATH` body — 7 (RFC-0027 §5.3.2).
 *
 * `00 <kind> <len>` framing plus the 4-byte value. The RFC's §3.3 table is re-run at this
 * width: 18 B for a direct link, 25 B with one forwarder, 32 B with two.
 */
inline constexpr std::size_t kPathLabelRecordBytes = kPackedEscapeOverhead + kPathLabelBodyBytes;

/**
 * @brief The two STRUCTURAL clauses of RFC-0027 §5.3.2's ruled element, kept apart on purpose.
 *
 * - **`kind` MUST be `0x16`** — an element is read by its kind and never by its position
 *   (§5.1). Another kind is somebody else's element: a hop steps over it by its declared
 *   length and MUST NOT read its payload as a label.
 * - **`len` MUST be exactly 4** — a payload that is not exactly one label has no reading, and a
 *   hop that implements labels refuses the address rather than reading a short or long field.
 *
 * They are two clauses and the §12.5 vectors keep them in two cases
 * (`path-label/label-foreign-kind`, `path-label/label-wrong-length`) for RFC-0024 §9.4's
 * reason: a core that drops one of two clauses sharing a vector passes it anyway.
 *
 * @note The candidate `opt.PL` / `opt.LL` clauses this predicate replaces are **unrepresentable**
 *       under the ruled spelling — an escape record has no option byte, because it is not a TLV.
 */
[[nodiscard]] constexpr bool path_label_record_valid(std::uint8_t kind,
                                                     std::size_t payload_len) noexcept {
    return kind == kPackedEscapeKindLabel && payload_len == kPathLabelBodyBytes;
}

/**
 * @brief Append one label element — `00 16 04 <u32 LE>` — to a packed `PATH` body.
 * @return False, appending NOTHING, when @p label is not a label a host ever minted (generation
 *         `0` is reserved "no label", §4.1), so the reserved value cannot reach the wire by
 *         way of a default-constructed struct.
 *
 * Writing the bytes is not §6.2's mint: this says how a label is SPELLED, never that one is due.
 */
[[nodiscard]] inline bool emit_path_label(std::vector<std::byte>& out, path_label_t label) {
    if (!label.valid()) return false;
    std::byte body[kPathLabelBodyBytes]{};
    path_label_store(body, label);
    return emit_path_escape(out, kPackedEscapeKindLabel, body);
}

/**
 * @brief Read the label element at @p at of a packed `PATH` body, or `nullopt` when the record
 *        there is not one.
 *
 * `nullopt` covers every "not a label here" case in one answer — a literal segment, a ragged
 * record, a foreign kind, a payload that is not 4 bytes, and the reserved zero generation —
 * because the reading hop's response to all of them is the same and none of them is a label.
 * What a hop DOES with the distinction (skip a foreign kind, refuse a malformed address) is the
 * forwarder's, which is why this returns a value rather than a status.
 */
[[nodiscard]] constexpr std::optional<path_label_t> path_label_at(std::span<const std::byte> body,
                                                                  std::size_t at) noexcept {
    const auto kind = packed_escape_kind(body, at);
    const auto payload = packed_escape_payload(body, at);
    if (!kind || !payload || !path_label_record_valid(*kind, payload->size())) return std::nullopt;
    const path_label_t label = path_label_load(*payload);
    if (!label.valid()) return std::nullopt;
    return label;
}

}  // namespace tr::wire
