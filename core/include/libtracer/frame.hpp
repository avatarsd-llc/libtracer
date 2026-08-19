/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The frame codec: decode wire bytes into a borrowed (zero-copy) TLV tree, and
 * encode a TLV tree back to bytes. Decoding never copies payload bytes — they
 * are std::span views into the caller's input buffer, which must outlive the
 * returned tlv_t. See docs/reference/01-data-format.md + 05-protocol-tlvs.md.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/error.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/view.hpp"

namespace tr::wire {

// Decode failures reuse the RFC-0002 registry codes (error.hpp) directly — the
// grammar returns `err_t` (ADR-0048), so `err_path`/severity/disposition come
// for free and there is no parallel decode-only error vocabulary. Decode only
// ever yields FRAME_TRUNCATED / FRAME_INVALID / FRAME_CRC_FAIL /
// TLV_NESTING_TOO_DEEP ("exceeds this receiver's decode resources", RFC-0006 —
// never reached by this heap-spilled decode, only by a null-spill grammar walk).

/** @brief A decoded trailer CRC: its width and the (zero-extended) checksum value. */
struct crc_t {
    /** @brief The CRC algorithm/width carried in the trailer. */
    enum class width_t {
        CRC32C,      /**< @brief 32-bit CRC-32C (Castagnoli). */
        CRC16_CCITT, /**< @brief 16-bit CRC-16/CCITT. */
    };
    width_t width{};       /**< @brief Which CRC algorithm produced @ref value. */
    std::uint32_t value{}; /**< @brief The checksum; 16-bit values are zero-extended. */
    /** @brief Value equality over width and value. */
    constexpr bool operator==(const crc_t&) const noexcept = default;
};

/** @brief A decoded trailer timestamp: absolute u64 ns or a relative i32 ns delta. */
struct timestamp_t {
    bool relative = false;  /**< @brief false = absolute u64 ns; true = relative i32 ns. */
    std::int64_t value = 0; /**< @brief The timestamp / delta in nanoseconds. */
    /** @brief Value equality over the relative flag and value. */
    constexpr bool operator==(const timestamp_t&) const noexcept = default;
};

/** @brief A decoded TLV trailer: an optional @ref timestamp_t and/or @ref crc_t. */
struct trailer_t {
    std::optional<timestamp_t> ts; /**< @brief The trailer timestamp, if present. */
    std::optional<crc_t> crc;      /**< @brief The trailer CRC, if present. */
    /** @brief Value equality over both optional fields. */
    constexpr bool operator==(const trailer_t&) const noexcept = default;
};

/**
 * @brief A decoded TLV — the materialized, eager representation of one wire frame node.
 *
 * For opaque TLVs (`opt.pl == 0`) @ref payload holds the bytes and @ref children is
 * empty; for structured TLVs (`opt.pl == 1`) @ref children holds the parsed sub-TLVs and
 * @ref payload is empty. @ref payload (and child payloads) BORROW the input buffer.
 */
struct tlv_t {
    type_t type{}; /**< @brief The TLV type code. */
    opt_t opt{};   /**< @brief The option bits (pl / cr / ll / …). */
    std::span<const std::byte>
        payload{};                 /**< @brief Opaque bytes (borrowed); empty when structured. */
    std::vector<tlv_t> children{}; /**< @brief Parsed sub-TLVs; empty when opaque. */
    std::optional<trailer_t> trailer{}; /**< @brief The decoded trailer, if present. */
};

/** @brief Structural + byte-content equality (spans compared by content, recursively). */
[[nodiscard]] bool equal(const tlv_t& a, const tlv_t& b) noexcept;

/**
 * @brief The INJECTED wire-time clock seam (#1109): where a stamping producer's trailer-TS
 *        nanoseconds come from.
 *
 * The library itself never reads an ambient clock — not here, not anywhere on a frame path.
 * A producer that wants wire-time stamps constructs one of these over whatever time source
 * its platform has and passes it to `stamp_ts`; a producer that does not stamp pays
 * nothing, the same per-frame opt-in shape the trailer CRC has always had.
 *
 * The contract is CONTEXT.md's `origin_timestamp`: the value MUST be per-producer MONOTONIC
 * (HLC-style — strictly increasing per origin, wall-clock-seeded where available, bumped
 * logically on coarse clocks or NTP backward jumps). Wall-clock meaning is advisory;
 * cross-producer comparison is undefined by design. On an MCU without SNTP the epoch anchor
 * may be arbitrary — that does not affect the echo/RTT use, where only the origin's own
 * clock is ever read (`RTT = now_ns() - echoed_stamp`).
 */
class wire_clock_t {
   public:
    /** @brief The producer's current wire-time in nanoseconds (TF=0 trailer units). */
    [[nodiscard]] virtual std::int64_t now_ns() noexcept = 0;

   protected:
    /** @brief Seam type: destroyed only as its concrete implementation, never through here. */
    ~wire_clock_t() = default;
};

/**
 * @brief Stamp @p tlv with an ABSOLUTE (TF=0) wire-time trailer of @p now_ns — the writer
 *        half of the trailer timestamp the decoders have always read (#1109).
 *
 * Sets `opt.ts`, clears `opt.tf`, and writes the trailer value, so a stamped TLV can never
 * reach `encode`'s loud opt-without-value refusal. Absolute-only ON PURPOSE: the relative
 * form (TF=1) is anchored to the parent's stamp and the spec's anchorless-reject rule
 * (docs/reference/01-data-format.md §relative) is a conformance gap the reference codec has
 * not closed — writing TF=1 before that check exists mints frames that decode to silent
 * near-epoch garbage on a non-validating receiver. The byte layout for both forms already
 * has one home (`wire::store_trailer_ts`), so the TF=1 writer is a small follow-up gated on
 * that check, not a redesign.
 */
inline void stamp_ts(tlv_t& tlv, std::int64_t now_ns) noexcept {
    tlv.opt.ts = true;
    tlv.opt.tf = false;
    if (!tlv.trailer) tlv.trailer.emplace();
    tlv.trailer->ts = timestamp_t{.relative = false, .value = now_ns};
}

/** @brief `stamp_ts` from the injected @p clock — the one call a stamping hot path makes. */
inline void stamp_ts(tlv_t& tlv, wire_clock_t& clock) noexcept { stamp_ts(tlv, clock.now_ns()); }

/**
 * @brief Decode exactly one TLV that fills @p input.
 *
 * The structural descent's open-node stack starts in inline slots sized for the typical
 * FWD nesting and SPILLS to @p spill for deeper frames, so the RFC-0006 decode-resource
 * bound is a property the caller injects rather than one this function fixes (#873,
 * ADR-0079). `decode(input, mem::null_source())` is the spelling of "no spill at all":
 * a frame nested past the inline slots is refused with `TLV_NESTING_TOO_DEEP` instead of
 * growing (@ref grammar::walk_stack_t).
 *
 * @note @p spill bounds the WALK STACK only. The returned tree is an OWNING `tlv_t`, whose
 *       child vectors allocate on the global heap by construction — this parameter does not
 *       make an owning decode allocation-free, and a caller that needs the whole decode on
 *       an injected store wants the terminus arena (`wire::decode_into`) instead.
 *
 * @param input The bytes to decode — must be exactly one TLV; trailing bytes ⇒ `FrameInvalid`.
 * @param spill The block source the walk stack spills into once its inline slots are full.
 *              Default: the process heap, i.e. today's behaviour unchanged.
 * @return The decoded @ref tlv_t (borrowing @p input), or an `err_t` on failure.
 */
[[nodiscard]] std::expected<tlv_t, err_t> decode(std::span<const std::byte> input,
                                                 mem::block_source_t& spill = mem::heap_source());

/**
 * @brief Encode a TLV to its wire bytes (recomputing the trailer CRC when `opt.cr` is set).
 *
 * The length width is not taken from @p tlv verbatim: a body larger than 0xFFFF widens to the
 * u32 `LL` form regardless of `tlv.opt.ll`, the same rule `wire::emit_tlv` applies (#924), so a
 * programmatically built tree can no longer serialize a length truncated to `size & 0xFFFF`. A
 * body at or under 0xFFFF is emitted unchanged; `tlv.opt.ll` is never cleared. A body over
 * 0xFFFFFFFF still truncates modulo 2^32 — the grammar has no wider length form, so that
 * residual is a wire-format limit rather than something `encode` can express.
 *
 * Encoding is SYMMETRIC with `decode`: the grammar's one per-type structural rule — a
 * `PATH_REF` body is a fixed-stride 8-byte record array, so `opt.PL` and `opt.LL` are both
 * forbidden and the length is bounded (RFC-0024 §4.2/§4.3, `wire::path_ref_body_valid`) — is
 * applied here too, so this codec cannot mint a frame it would itself reject (#886). A refused
 * TLV anywhere in the tree refuses the whole tree rather than silently dropping a component.
 *
 * The trailer timestamp is LOUD, not defaulted (#1109): a TLV whose `opt.ts` is set with no
 * `trailer->ts` value — or whose trailer value's `relative` flag contradicts `opt.tf` — is
 * refused (empty vector), never emitted with a silently-zero stamp. `stamp_ts` sets the bit
 * and the value together, so a stamped TLV cannot reach this refusal.
 *
 * @param tlv The TLV tree to serialize.
 * @return The encoded frame bytes, or an EMPTY vector when @p tlv (or any descendant) is an
 *         ill-formed `PATH_REF` or claims a timestamp it does not carry. Empty is
 *         unambiguous: a serialized TLV always carries at least its 4-byte header, so no
 *         well-formed @p tlv encodes to nothing.
 */
[[nodiscard]] std::vector<std::byte> encode(const tlv_t& tlv);

/**
 * @brief The canonical PATH-payload key of a decoded PATH TLV — the graph vertex-map key.
 *
 * The PATH body's packed `[u8 len][bytes]` segment records, copied (RFC-0018 — `opt.PL = 0`,
 * so the body IS the key). One locus for what `graph_t`, `op_resolver_t`, and `fwd_router_t`
 * each previously rebuilt inline.
 *
 * This is **canonical / key** context, so the framing is checked and the RFC-0018 §5.4
 * `len == 0` escape is REJECTED — a label is not canonical bytes. A ragged or escape-bearing
 * body yields `std::nullopt` rather than a key. Before RFC-0018 the same `nullopt` guarded a
 * non-`NAME` child: this function re-emitted ANY child's payload through `wire::emit_name`, so
 * a peer's `PATH{VALUE "sensor"}` composed byte-identical key bytes to the legal
 * `PATH{NAME "sensor"}` and resolved `/sensor` (#436's shape, one tier up, and #681). A packed
 * body has no children to mistype, so that class of divergence is structurally gone.
 *
 * @note Returning `nullopt` rather than an empty vector is load-bearing. `graph_t::find_ptr`
 *       walks segments from the root, so an EMPTY key exits its loop immediately and resolves
 *       the ROOT vertex — an empty-key rejection would convert this bug into a misroute to `/`,
 *       which is worse than the bug.
 *
 * @param path A decoded PATH @ref tlv_t.
 * @return The canonical key bytes, or `nullopt` if the body is not a run of literal packed
 *         records (ragged framing, an escape record, or a structured `opt.PL = 1` PATH).
 */
[[nodiscard]] std::optional<std::vector<std::byte>> path_key(const tlv_t& path);

/**
 * @brief Decode exactly one TLV from a flat view (the L1↔L2 cast, zero-copy).
 *
 * The L1↔L2 cast — "a TLV is a cast from a view." It lives at L2 (it produces a
 * @ref tlv_t) and consumes an L1 @ref view::view_t. The returned `tlv_t` borrows
 * the view's bytes, so keep the view — and thus its segment — alive while using
 * it.
 *
 * @param v The view whose bytes are exactly one TLV.
 * @param spill Forwarded to the span overload — the walk stack's spill source, defaulting
 *              to the process heap.
 * @return The decoded @ref tlv_t (borrowing @p v's bytes), or an `err_t` on failure.
 */
[[nodiscard]] inline std::expected<tlv_t, err_t> decode(
    const view::view_t& v, mem::block_source_t& spill = mem::heap_source()) {
    return decode(v.bytes(), spill);
}

}  // namespace tr::wire
