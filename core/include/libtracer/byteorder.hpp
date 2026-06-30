/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Little-endian integer (de)serialization — the one place bytes become integers
 * and back. The v1 wire format is little-endian throughout (TLV lengths, VALUE /
 * TIME payloads, ROUTER metadata; docs/spec/v1.md). Every codec and runtime site
 * funnels through here instead of hand-rolling shift/mask loops, so the byte order
 * lives in exactly one tested place. Header-only and `constexpr`; `detail` =
 * internal (not a public API surface).
 */
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace tr::detail {

// Load a little-endian unsigned from the low `min(in.size(), sizeof(T))` bytes of
// `in`; any bytes beyond `in` read as zero. Tolerant by design — a payload may be
// narrower than the widest integer (e.g. a 1-byte hop_count loaded as a u64).
template <std::unsigned_integral T = std::uint64_t>
[[nodiscard]] constexpr T load_le(std::span<const std::byte> in) noexcept {
    T value = 0;
    const std::size_t n = in.size() < sizeof(T) ? in.size() : sizeof(T);
    for (std::size_t i = 0; i < n; ++i)
        value |= static_cast<T>(std::to_integer<std::uint8_t>(in[i])) << (8 * i);
    return value;
}

// Store the low `width` bytes of `value`, little-endian, into `out[0..width)`.
// Preconditions: `out.size() >= width` and `width <= sizeof(T)`.
template <std::unsigned_integral T>
constexpr void store_le(std::span<std::byte> out, T value, std::size_t width = sizeof(T)) noexcept {
    for (std::size_t i = 0; i < width; ++i)
        out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8 * i)));
}

// Append the low `width` bytes of `value`, little-endian, to a byte vector.
// Precondition: `width <= sizeof(T)`.
template <std::unsigned_integral T>
void append_le(std::vector<std::byte>& out, T value, std::size_t width = sizeof(T)) {
    for (std::size_t i = 0; i < width; ++i)
        out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8 * i))));
}

// View a byte span as a char-string view — the byte↔char-string counterpart to
// the integer loads above. One locus for the `reinterpret_cast<const char*>`
// idiom repeated across the codec/router (NAME payloads, link names), so the
// aliasing cast lives in one audited place. The bytes are not assumed to be
// NUL-terminated; the view's length is the span's length.
[[nodiscard]] inline std::string_view as_string_view(std::span<const std::byte> in) noexcept {
    return std::string_view(reinterpret_cast<const char*>(in.data()), in.size());
}

}  // namespace tr::detail
