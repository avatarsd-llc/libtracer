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
#include "libtracer/tlv.hpp"
#include "libtracer/view.hpp"

namespace tr::wire {

// Decode failures reuse the RFC-0002 registry codes (error.hpp) directly — the
// grammar returns `err_t` (ADR-0048), so `err_path`/severity/disposition come
// for free and there is no parallel decode-only error vocabulary. Decode only
// ever yields FRAME_TRUNCATED / FRAME_INVALID / FRAME_CRC_FAIL / TLV_NESTING_TOO_DEEP.

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

/** @brief The iterative-parser nesting-depth cap (docs/reference/01 §two parser contexts). */
inline constexpr std::size_t kMaxDepth = 32;

/** @brief Structural + byte-content equality (spans compared by content, recursively). */
[[nodiscard]] bool equal(const tlv_t& a, const tlv_t& b) noexcept;

/**
 * @brief Decode exactly one TLV that fills @p input.
 * @param input The bytes to decode — must be exactly one TLV; trailing bytes ⇒ `FrameInvalid`.
 * @return The decoded @ref tlv_t (borrowing @p input), or an @ref err_t on failure.
 */
[[nodiscard]] std::expected<tlv_t, err_t> decode(std::span<const std::byte> input);

/**
 * @brief Encode a TLV to its wire bytes (recomputing the trailer CRC when `opt.cr` is set).
 * @param tlv The TLV tree to serialize.
 * @return The encoded frame bytes.
 */
[[nodiscard]] std::vector<std::byte> encode(const tlv_t& tlv);

/**
 * @brief The canonical PATH-payload key of a decoded PATH TLV — the graph vertex-map key.
 *
 * The concatenated NAME-child encodings. One locus for what `graph_t`, `op_resolver_t`,
 * and `fwd_router_t` each previously rebuilt inline.
 * @param path A decoded PATH @ref tlv_t.
 * @return The canonical key bytes.
 */
[[nodiscard]] std::vector<std::byte> path_key(const tlv_t& path);

/**
 * @brief Cast a flat view to a TLV (the M1 decoder, zero-copy).
 *
 * The L1↔L2 cast — "a TLV is a cast from a view." It lives at L2 (it produces a
 * @ref tlv_t) and consumes an L1 @ref view::view_t. The returned `tlv_t` borrows
 * the view's bytes, so keep the view — and thus its segment — alive while using
 * it.
 */
[[nodiscard]] inline std::expected<tlv_t, err_t> view_as_tlv(const view::view_t& v) {
    return decode(v.bytes());
}

}  // namespace tr::wire
