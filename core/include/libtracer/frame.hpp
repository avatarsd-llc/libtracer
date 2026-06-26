// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// The frame codec: decode wire bytes into a borrowed (zero-copy) TLV tree, and
// encode a TLV tree back to bytes. Decoding never copies payload bytes — they
// are std::span views into the caller's input buffer, which must outlive the
// returned tlv_t. See docs/reference/01-data-format.md + 05-protocol-tlvs.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/tlv.hpp"
#include "libtracer/view.hpp"

namespace tr::wire {

// Decode failures, named after the tr:: built-in error concepts (RFC-0002).
// The full ERROR *wire* shape is RFC-0002 (gated); this is the local reason.
enum class error_t {
    FRAME_TRUNCATED,       // tr::frame::truncated  — ran out of bytes
    FRAME_INVALID,         // tr::frame::invalid    — reserved bit, type 0x00, bad structure
    FRAME_CRC_FAIL,        // tr::frame::crc_fail   — trailer CRC mismatch
    TLV_NESTING_TOO_DEEP,  // tr::tlv::nesting_too_deep — depth cap exceeded
};

struct crc_t {
    enum class width_t { CRC32C, CRC16_CCITT };
    width_t width{};
    std::uint32_t value{};  // 16-bit values are zero-extended
    constexpr bool operator==(const crc_t&) const noexcept = default;
};

struct timestamp_t {
    bool relative = false;  // false = absolute u64 ns; true = relative i32 ns
    std::int64_t value = 0;
    constexpr bool operator==(const timestamp_t&) const noexcept = default;
};

struct trailer_t {
    std::optional<timestamp_t> ts;
    std::optional<crc_t> crc;
    constexpr bool operator==(const trailer_t&) const noexcept = default;
};

// A decoded TLV. For opaque TLVs (opt.PL=0) `payload` holds the bytes; for
// structured TLVs (opt.PL=1) `children` holds the parsed sub-TLVs and `payload`
// is empty. `payload` (and child payloads) borrow the input buffer.
struct tlv_t {
    type_t type{};
    opt_t opt{};
    std::span<const std::byte> payload{};
    std::vector<tlv_t> children{};
    std::optional<trailer_t> trailer{};
};

// The iterative-parser depth cap (docs/reference/01 §two parser contexts).
inline constexpr std::size_t kMaxDepth = 32;

// Structural + byte-content equality (spans compared by content, recursively).
[[nodiscard]] bool equal(const tlv_t& a, const tlv_t& b) noexcept;

// Decode exactly one TLV that fills `input`; trailing bytes => FrameInvalid.
[[nodiscard]] std::expected<tlv_t, error_t> decode(std::span<const std::byte> input);

// Encode a TLV (recomputing the trailer CRC from the body when opt.CR is set).
[[nodiscard]] std::vector<std::byte> encode(const tlv_t& tlv);

/**
 * @brief Cast a flat view to a TLV (the M1 decoder, zero-copy).
 *
 * The L1↔L2 cast — "a TLV is a cast from a view." It lives at L2 (it produces a
 * @ref tlv_t) and consumes an L1 @ref view::view_t. The returned `tlv_t` borrows
 * the view's bytes, so keep the view — and thus its segment — alive while using
 * it.
 */
[[nodiscard]] inline std::expected<tlv_t, error_t> view_as_tlv(const view::view_t& v) {
    return decode(v.bytes());
}

}  // namespace tr::wire
