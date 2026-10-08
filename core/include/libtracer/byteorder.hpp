/**
 * @file
 * @brief Little-endian integer codec — the one place bytes become integers and back.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The v1 wire format is little-endian throughout (TLV lengths, VALUE / TIME payloads,
 * ROUTER metadata; docs/spec/v1.md). Every codec and runtime site funnels through here
 * instead of hand-rolling shift/mask loops, so the byte order lives in exactly one tested
 * place. Header-only, no heap of its own.
 *
 * The codec is public (`%tr::wire::load_le`, `%store_le`, `%append_le`, #2025), so an embedder
 * encoding its own composite payloads or wire fields uses the same three functions the
 * library does. The wire widths are `u16`, `u32` and `u64`; any unsigned type is accepted.
 * The `tr::detail` spellings are the same entities, re-exported for the existing internal
 * callers; `tr::detail` also keeps the internal-only helpers (the `std::vector` append and
 * `as_string_view`).
 */
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/mem_source.hpp"

namespace tr::wire {

/**
 * @brief Load a little-endian unsigned from the low `min(in.size(), sizeof(T))` bytes of `in`;
 *        any bytes beyond `in` read as zero.
 *
 * Tolerant by design — a payload may be narrower than the widest integer (e.g. a 1-byte
 * hop_count loaded as a u64).
 */
template <std::unsigned_integral T = std::uint64_t>
[[nodiscard]] constexpr T load_le(std::span<const std::byte> in) noexcept {
    T value = 0;
    const std::size_t n = in.size() < sizeof(T) ? in.size() : sizeof(T);
    for (std::size_t i = 0; i < n; ++i)
        value |= static_cast<T>(std::to_integer<std::uint8_t>(in[i])) << (8 * i);
    return value;
}

/**
 * @brief Store the low `width` bytes of `value`, little-endian, into `out[0..width)`.
 *
 * Preconditions: `out.size() >= width` and `width <= sizeof(T)`.
 */
template <std::unsigned_integral T>
constexpr void store_le(std::span<std::byte> out, T value, std::size_t width = sizeof(T)) noexcept {
    for (std::size_t i = 0; i < width; ++i)
        out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8 * i)));
}

/**
 * @brief Append the low `width` bytes of `value`, little-endian, to a core byte array.
 *
 * Failable like every core container (#1781): the grow draws from @p out's own source.
 * Not `constexpr`, because the grow goes through a runtime block source.
 *
 * Precondition: `width <= sizeof(T)`.
 * @retval false The source refused the grow; @p out is unchanged.
 */
template <std::unsigned_integral T>
[[nodiscard]] bool append_le(mem::bytes_t& out, T value, std::size_t width = sizeof(T)) noexcept {
    std::array<std::byte, sizeof(T)> le{};
    store_le(std::span<std::byte>(le), value, width);
    return out.append(le.data(), width);
}

}  // namespace tr::wire

namespace tr::detail {

using wire::append_le;
using wire::load_le;
using wire::store_le;

/**
 * @brief Append the low `width` bytes of `value`, little-endian, to a byte vector.
 *
 * Internal and host-only: the public form appends to a core byte array
 * (`%tr::wire::append_le`). Precondition: `width <= sizeof(T)`.
 */
template <std::unsigned_integral T>
void append_le(std::vector<std::byte>& out, T value, std::size_t width = sizeof(T)) {
    for (std::size_t i = 0; i < width; ++i)
        out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8 * i))));
}

/**
 * @brief View a byte span as a char-string view — the byte↔char-string counterpart to the
 *        integer loads above.
 *
 * One locus for the `reinterpret_cast<const char*>` idiom repeated across the codec/router
 * (NAME payloads, link names), so the aliasing cast lives in one audited place. The bytes are
 * not assumed to be NUL-terminated; the view's length is the span's length.
 */
[[nodiscard]] inline std::string_view as_string_view(std::span<const std::byte> in) noexcept {
    return std::string_view(reinterpret_cast<const char*>(in.data()), in.size());
}

}  // namespace tr::detail
