// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// The frame codec: decode wire bytes into a borrowed (zero-copy) TLV tree, and
// encode a TLV tree back to bytes. Decoding never copies payload bytes — they
// are std::span views into the caller's input buffer, which must outlive the
// returned Tlv. See docs/reference/01-data-format.md + 05-protocol-tlvs.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "libtracer/tlv.hpp"

namespace tr {

// Decode failures, named after the tr:: built-in error concepts (RFC-0002).
// The full ERROR *wire* shape is RFC-0002 (gated); this is the local reason.
enum class Error {
    FrameTruncated,     // tr::frame::truncated  — ran out of bytes
    FrameInvalid,       // tr::frame::invalid    — reserved bit, type 0x00, bad structure
    FrameCrcFail,       // tr::frame::crc_fail   — trailer CRC mismatch
    TlvNestingTooDeep,  // tr::tlv::nesting_too_deep — depth cap exceeded
};

struct Crc {
    enum class Width { Crc32c, Crc16Ccitt };
    Width width{};
    std::uint32_t value{};  // 16-bit values are zero-extended
    constexpr bool operator==(const Crc&) const noexcept = default;
};

struct Timestamp {
    bool relative = false;  // false = absolute u64 ns; true = relative i32 ns
    std::int64_t value = 0;
    constexpr bool operator==(const Timestamp&) const noexcept = default;
};

struct Trailer {
    std::optional<Timestamp> ts;
    std::optional<Crc> crc;
    constexpr bool operator==(const Trailer&) const noexcept = default;
};

// A decoded TLV. For opaque TLVs (opt.PL=0) `payload` holds the bytes; for
// structured TLVs (opt.PL=1) `children` holds the parsed sub-TLVs and `payload`
// is empty. `payload` (and child payloads) borrow the input buffer.
struct Tlv {
    Type type{};
    Opt opt{};
    std::span<const std::byte> payload{};
    std::vector<Tlv> children{};
    std::optional<Trailer> trailer{};
};

// The iterative-parser depth cap (docs/reference/01 §two parser contexts).
inline constexpr std::size_t kMaxDepth = 32;

// Structural + byte-content equality (spans compared by content, recursively).
[[nodiscard]] bool equal(const Tlv& a, const Tlv& b) noexcept;

// Decode exactly one TLV that fills `input`; trailing bytes => FrameInvalid.
[[nodiscard]] std::expected<Tlv, Error> decode(std::span<const std::byte> input);

// Encode a TLV (recomputing the trailer CRC from the body when opt.CR is set).
[[nodiscard]] std::vector<std::byte> encode(const Tlv& tlv);

}  // namespace tr
