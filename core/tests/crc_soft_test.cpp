/**
 * @file
 * @brief Both software CRC-32C folds against the compile-time Sarwate CRC (#2116).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `config_t::kCrc32cSlices` picks which software fold a build runs: 8 (slice-by-8, the
 * default) or 1 (the single table a NARROW node binds). The CRC is a frozen-vector
 * requirement, so both folds must agree with the constant-evaluated Sarwate loop byte for
 * byte. This test calls `detail::crc32c_soft<1>` and `<8>` directly, so it runs both folds
 * whatever the build bound and never takes the SSE4.2 or ARMv8 hardware path that
 * `crc32c_update` dispatches to on a capable host. Without it, the one-table fold would be
 * compiled in CI only by the footprint sentinels, which execute nothing.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "libtracer/crc.hpp"

namespace {

/** @brief Test bytes, generated at compile time so the reference CRCs can be too. */
constexpr std::size_t kBytes = 1100;
constexpr std::array<std::byte, kBytes> kData = [] {
    std::array<std::byte, kBytes> d{};
    std::uint32_t x = 0x2116u;
    for (auto& b : d) {
        x = x * 1103515245u + 12345u;
        b = static_cast<std::byte>(x >> 16);
    }
    return d;
}();

/** @brief Every length 0..64, then odd lengths around 1 KiB and the buffer's end. */
constexpr std::array<std::size_t, 76> kLengths = [] {
    std::array<std::size_t, 76> l{};
    std::size_t i = 0;
    for (std::size_t n = 0; n <= 64; ++n) l[i++] = n;
    constexpr std::array<std::size_t, 11> kOdd = {1015, 1017, 1021, 1023, 1024,  1025,
                                                  1027, 1031, 1033, 1039, kBytes};
    for (const std::size_t n : kOdd) l[i++] = n;
    return l;
}();

/** @brief The reference: `crc32c_update` in constant evaluation is the Sarwate table loop. */
constexpr std::array<std::uint32_t, kLengths.size()> kExpected = [] {
    std::array<std::uint32_t, kLengths.size()> e{};
    for (std::size_t i = 0; i < kLengths.size(); ++i)
        e[i] = tr::crc::detail::crc32c_update(
            0xFFFFFFFFu, std::span<const std::byte>(kData.data(), kLengths[i]));
    return e;
}();

/** @brief The check vector `"123456789"`, as bytes. */
constexpr std::array<std::byte, 9> kCheck = {std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
                                             std::byte{'4'}, std::byte{'5'}, std::byte{'6'},
                                             std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
static_assert((tr::crc::detail::crc32c_update(0xFFFFFFFFu, kCheck) ^ 0xFFFFFFFFu) == 0xE3069283u,
              "the compile-time reference is CRC-32C");

int g_failures = 0;

void check(bool ok, const char* what, std::size_t n) {
    if (!ok) {
        ++g_failures;
        std::printf("  [FAIL] %s at %zu bytes\n", what, n);
    }
}

template <std::size_t Slices>
void fold_matches_reference(const char* name) {
    const int before = g_failures;
    const std::uint32_t check_crc =
        tr::crc::detail::crc32c_soft<Slices>(0xFFFFFFFFu, kCheck.data(), kCheck.size()) ^
        0xFFFFFFFFu;
    check(check_crc == 0xE3069283u, name, kCheck.size());
    for (std::size_t i = 0; i < kLengths.size(); ++i)
        check(tr::crc::detail::crc32c_soft<Slices>(0xFFFFFFFFu, kData.data(), kLengths[i]) ==
                  kExpected[i],
              name, kLengths[i]);
    // A running state carried across a split: the codec folds payload and trailer separately.
    const std::uint32_t head = tr::crc::detail::crc32c_soft<Slices>(0xFFFFFFFFu, kData.data(), 13);
    check(tr::crc::detail::crc32c_soft<Slices>(head, kData.data() + 13, kBytes - 13) ==
              kExpected.back(),
          name, kBytes);
    std::printf("  [%s] %s: \"123456789\" and %zu lengths match the Sarwate reference\n",
                g_failures == before ? "PASS" : "FAIL", name, kLengths.size());
}

}  // namespace

int main() {
    std::printf("crc_soft_test: the software CRC-32C folds (build binds kCrc32cSlices = %zu)\n",
                tr::graph::config_t::kCrc32cSlices);
    fold_matches_reference<1>("one table (kCrc32cSlices = 1)");
    fold_matches_reference<8>("slice-by-8 (kCrc32cSlices = 8)");
    return g_failures == 0 ? 0 : 1;
}
