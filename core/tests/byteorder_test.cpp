/**
 * @file
 * @brief Unit tests for the little-endian (de)serialization primitive (byteorder.hpp).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The codec/runtime sites that funnel through it are covered end-to-end by the
 * conformance vectors; this pins the helper's own contract (LE order, the
 * short-span zero-extension tolerance, width truncation, constexpr-ness).
 */

#include "libtracer/byteorder.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "test_support.hpp"

namespace {

using namespace tr::detail;

using tr::testing::check;

std::uint8_t at(const std::vector<std::byte>& v, std::size_t i) {
    return std::to_integer<std::uint8_t>(v[i]);
}

/** @brief load_le is usable in a constant expression. */
constexpr std::array<std::byte, 4> kFour{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                         std::byte{0x04}};
static_assert(load_le<std::uint32_t>(kFour) == 0x04030201u, "load_le is constexpr + little-endian");

/** @brief The public codec (`tr::wire`) and the internal spelling are one entity, not a copy. */
static_assert(&tr::detail::load_le<std::uint32_t> == &tr::wire::load_le<std::uint32_t>);
static_assert(&tr::detail::store_le<std::uint64_t> == &tr::wire::store_le<std::uint64_t>);

/** @brief Store @p v into a `T`-wide buffer and load it back, in a constant expression. */
template <std::unsigned_integral T>
constexpr T round_trip(T v) noexcept {
    std::array<std::byte, sizeof(T)> b{};
    tr::wire::store_le(std::span<std::byte>(b), v);
    return tr::wire::load_le<T>(b);
}

/** @brief The boundary values every width must survive: zero, one, the sign bit, the maximum. */
template <std::unsigned_integral T>
constexpr bool boundaries_round_trip() noexcept {
    constexpr T kMax = std::numeric_limits<T>::max();
    constexpr T kHigh = static_cast<T>(T{1} << (8 * sizeof(T) - 1));
    for (const T v : {T{0}, T{1}, T{0xFF}, static_cast<T>(T{0xFF} << 8), kHigh,
                      static_cast<T>(kHigh - 1), static_cast<T>(kMax - 1), kMax})
        if (round_trip(v) != v) return false;
    return true;
}

static_assert(boundaries_round_trip<std::uint16_t>(), "u16 boundaries round-trip (constexpr)");
static_assert(boundaries_round_trip<std::uint32_t>(), "u32 boundaries round-trip (constexpr)");
static_assert(boundaries_round_trip<std::uint64_t>(), "u64 boundaries round-trip (constexpr)");
static_assert(noexcept(tr::wire::load_le<std::uint16_t>(std::span<const std::byte>{})));
static_assert(noexcept(tr::wire::store_le(std::span<std::byte>{}, std::uint32_t{0}, 0)));

/** @brief Append @p v to a core byte array and check the bytes, LE, and the round-trip. */
template <std::unsigned_integral T>
bool appends_le(T v) {
    tr::mem::bytes_t out(tr::mem::heap_source());
    if (!tr::wire::append_le(out, v) || out.size() != sizeof(T)) return false;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        if (std::to_integer<std::uint8_t>(out.data()[i]) != static_cast<std::uint8_t>(v >> (8 * i)))
            return false;
    return tr::wire::load_le<T>(std::span<const std::byte>(out.data(), out.size())) == v;
}

/** @brief The public codec over each wire width (u16, u32, u64). */
void public_codec() {
    using tr::testing::check;
    check(round_trip<std::uint16_t>(0xA55A) == 0xA55A, "wire::store_le/load_le round-trip (u16)");
    check(round_trip<std::uint32_t>(0xDEADBEEFu) == 0xDEADBEEFu,
          "wire::store_le/load_le round-trip (u32)");
    check(round_trip<std::uint64_t>(0x0102030405060708ull) == 0x0102030405060708ull,
          "wire::store_le/load_le round-trip (u64)");

    check(appends_le<std::uint16_t>(0) && appends_le<std::uint16_t>(0xFFFF) &&
              appends_le<std::uint16_t>(0x8001),
          "wire::append_le u16 boundaries into a core byte array");
    check(appends_le<std::uint32_t>(0) && appends_le<std::uint32_t>(0xFFFFFFFFu) &&
              appends_le<std::uint32_t>(0x80000001u),
          "wire::append_le u32 boundaries into a core byte array");
    check(appends_le<std::uint64_t>(0) && appends_le<std::uint64_t>(~0ull) &&
              appends_le<std::uint64_t>(0x8000000000000001ull),
          "wire::append_le u64 boundaries into a core byte array");

    // A partial width writes only the low bytes.
    tr::mem::bytes_t two(tr::mem::heap_source());
    check(tr::wire::append_le(two, std::uint32_t{0x00CCBBAAu}, 2) && two.size() == 2 &&
              std::to_integer<std::uint8_t>(two.data()[0]) == 0xAA &&
              std::to_integer<std::uint8_t>(two.data()[1]) == 0xBB,
          "wire::append_le honors width < sizeof(T)");

    // A refused grow is reported by value and leaves the array unchanged.
    tr::mem::bytes_t refused(tr::mem::null_source());
    check(
        !tr::wire::append_le(refused, std::uint64_t{0x1122334455667788ull}) && refused.size() == 0,
        "wire::append_le reports a refused grow and leaves the array unchanged");
}

}  // namespace

int main() {
    std::printf("byteorder — little-endian (de)serialization:\n");

    std::array<std::byte, 8> buf{};
    store_le<std::uint64_t>(buf, 0x1122334455667788ull);
    check(std::to_integer<std::uint8_t>(buf[0]) == 0x88, "store_le emits the low byte first");
    check(load_le<std::uint64_t>(buf) == 0x1122334455667788ull,
          "store_le/load_le round-trip (u64)");

    std::vector<std::byte> v16;
    append_le<std::uint16_t>(v16, 0xBEEF);
    check(v16.size() == 2 && at(v16, 0) == 0xEF && at(v16, 1) == 0xBE,
          "append_le u16 little-endian");

    const std::array<std::byte, 1> one{std::byte{0x2A}};
    check(load_le<std::uint64_t>(one) == 0x2A, "load_le tolerates a short span (zero-extends)");

    std::vector<std::byte> trunc;
    append_le<std::uint64_t>(trunc, 0xAABBCCDDull, 2);  // request only the low 2 bytes
    check(trunc.size() == 2 && at(trunc, 0) == 0xDD && at(trunc, 1) == 0xCC,
          "append_le honors width < sizeof(T)");

    public_codec();

    return tr::testing::summary("byteorder");
}
