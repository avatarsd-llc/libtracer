// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// trailer_t frame checks. CRC-32C (Castagnoli) is the default; CRC-16-CCITT is
// the opt.CW=1 variant. Header-only, constexpr tables (built at compile time).
// See docs/reference/01-data-format.md and ADR-0004.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tr::crc {

namespace detail {

// CRC-32C: reflected poly 0x82F63B78 (= reverse of 0x1EDC6F41).
constexpr std::array<std::uint32_t, 256> crc32c_table() noexcept {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        }
        t[i] = c;
    }
    return t;
}

// CRC-16-CCITT (FALSE): poly 0x1021, MSB-first, init 0xFFFF, no final xor.
constexpr std::array<std::uint16_t, 256> crc16_table() noexcept {
    std::array<std::uint16_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(i << 8);
        for (int k = 0; k < 8; ++k) {
            c = (c & 0x8000u) ? static_cast<std::uint16_t>((c << 1) ^ 0x1021u)
                              : static_cast<std::uint16_t>(c << 1);
        }
        t[i] = c;
    }
    return t;
}

inline constexpr auto kCrc32c = crc32c_table();
inline constexpr auto kCrc16 = crc16_table();

}  // namespace detail

[[nodiscard]] constexpr std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::byte b : data) {
        c = detail::kCrc32c[(c ^ std::to_integer<std::uint8_t>(b)) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

[[nodiscard]] constexpr std::uint16_t crc16_ccitt(std::span<const std::byte> data) noexcept {
    std::uint16_t c = 0xFFFFu;
    for (std::byte b : data) {
        const std::uint8_t idx =
            static_cast<std::uint8_t>((c >> 8) ^ std::to_integer<std::uint8_t>(b));
        c = static_cast<std::uint16_t>(detail::kCrc16[idx] ^ (c << 8));
    }
    return c;
}

}  // namespace tr::crc
