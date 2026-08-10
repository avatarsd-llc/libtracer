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
#include <vector>

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

    return tr::testing::summary("byteorder");
}
