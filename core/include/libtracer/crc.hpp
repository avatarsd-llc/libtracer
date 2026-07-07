/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * trailer_t frame checks. CRC-32C (Castagnoli) is the default; CRC-16-CCITT is
 * the opt.CW=1 variant. Header-only, constexpr tables (built at compile time).
 * See docs/reference/01-data-format.md and ADR-0004.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

// CRC-32C (Castagnoli) IS the SSE4.2 `_mm_crc32_*` / ARMv8 `__crc32c*` instruction
// (same reflected poly 0x82F63B78). We dispatch to it at RUNTIME when the CPU has it,
// and fall back to a portable slice-by-8 table otherwise — all three paths (hardware,
// slice-by-8, byte-at-a-time Sarwate) produce byte-identical CRCs (a frozen-vector
// requirement). The intrinsics stay ISOLATED to a target-attributed function so the
// whole translation unit is NOT built for SSE4.2 (that would fault on older CPUs).
#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>  // SSE4.2 _mm_crc32_u8 / _mm_crc32_u32 / _mm_crc32_u64
#define TR_CRC_X86 1
#endif
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>  // __crc32cb / __crc32cd (compile-time feature-gated)
#define TR_CRC_ARM 1
#endif

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

// Slice-by-8 tables for the portable (no-hardware) CRC-32C path: table[0] is the
// single-byte Sarwate table; table[k][i] folds `i` forward `k` extra bytes. Built
// at compile time from the base table; used only on the runtime fallback path, so
// this costs 8 KiB of rodata that a hardware-CRC CPU never touches.
constexpr std::array<std::array<std::uint32_t, 256>, 8> crc32c_slice_tables() noexcept {
    std::array<std::array<std::uint32_t, 256>, 8> t{};
    t[0] = crc32c_table();
    for (int i = 0; i < 256; ++i) {
        std::uint32_t c = t[0][static_cast<std::size_t>(i)];
        for (int k = 1; k < 8; ++k) {
            c = t[0][c & 0xFFu] ^ (c >> 8);
            t[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)] = c;
        }
    }
    return t;
}
inline constexpr auto kCrc32cSlice = crc32c_slice_tables();

}  // namespace detail

namespace detail {

// One contiguous little-endian u32 read (byte-wise so it is endianness-agnostic; the
// compiler folds it to a single load on a little-endian target).
[[nodiscard]] inline std::uint32_t crc_load_le32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24);
}

// Portable slice-by-8 CRC-32C fold of a running state — 8 bytes per iteration off the
// table, then the trailing bytes byte-at-a-time. Byte-identical to the Sarwate loop.
[[nodiscard]] inline std::uint32_t crc32c_slice8(std::uint32_t c, const std::byte* p,
                                                 std::size_t n) noexcept {
    const auto& t = kCrc32cSlice;
    while (n >= 8) {
        const std::uint32_t lo = c ^ crc_load_le32(p);
        const std::uint32_t hi = crc_load_le32(p + 4);
        c = t[7][lo & 0xFFu] ^ t[6][(lo >> 8) & 0xFFu] ^ t[5][(lo >> 16) & 0xFFu] ^
            t[4][(lo >> 24) & 0xFFu] ^ t[3][hi & 0xFFu] ^ t[2][(hi >> 8) & 0xFFu] ^
            t[1][(hi >> 16) & 0xFFu] ^ t[0][(hi >> 24) & 0xFFu];
        p += 8;
        n -= 8;
    }
    while (n-- > 0) c = t[0][(c ^ std::to_integer<std::uint8_t>(*p++)) & 0xFFu] ^ (c >> 8);
    return c;
}

#if TR_CRC_X86
// SSE4.2 hardware CRC-32C. The `target` attribute confines the instruction to THIS
// function so the rest of the TU stays buildable-and-runnable on a non-SSE4.2 CPU.
// The instruction implements exactly the reflected CRC-32C update, so feeding the
// running state through it (8 bytes at a time on x86-64, then a byte tail) yields the
// same intermediate state as the table loop — bit-for-bit.
__attribute__((target("sse4.2"))) inline std::uint32_t crc32c_hw(std::uint32_t c,
                                                                 const std::byte* p,
                                                                 std::size_t n) noexcept {
#if defined(__x86_64__)
    std::uint64_t crc = c;
    while (n >= 8) {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        crc = _mm_crc32_u64(crc, v);
        p += 8;
        n -= 8;
    }
    c = static_cast<std::uint32_t>(crc);
#else
    while (n >= 4) {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        c = _mm_crc32_u32(c, v);
        p += 4;
        n -= 4;
    }
#endif
    while (n-- > 0) c = _mm_crc32_u8(c, std::to_integer<std::uint8_t>(*p++));
    return c;
}
#endif

#if TR_CRC_ARM
// ARMv8 CRC32 extension (compile-time gated by __ARM_FEATURE_CRC32). Same reflected
// CRC-32C update as the table loop; 8 bytes at a time, then a byte tail.
inline std::uint32_t crc32c_hw(std::uint32_t c, const std::byte* p, std::size_t n) noexcept {
    while (n >= 8) {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        c = __crc32cd(c, v);
        p += 8;
        n -= 8;
    }
    while (n-- > 0) c = __crc32cb(c, std::to_integer<std::uint8_t>(*p++));
    return c;
}
#endif

// Runtime dispatch: hardware CRC-32C where the CPU has it (selected ONCE), else the
// portable slice-by-8 fold. Never reached during constant evaluation (see the
// consteval guard in crc32c_update) so it may freely use runtime-only intrinsics.
[[nodiscard]] inline std::uint32_t crc32c_update_runtime(std::uint32_t c,
                                                         std::span<const std::byte> data) noexcept {
    const std::byte* const p = data.data();
    const std::size_t n = data.size();
#if TR_CRC_X86
    static const bool have_hw = __builtin_cpu_supports("sse4.2");
    return have_hw ? crc32c_hw(c, p, n) : crc32c_slice8(c, p, n);
#elif TR_CRC_ARM
    return crc32c_hw(c, p, n);
#else
    return crc32c_slice8(c, p, n);
#endif
}

// The per-byte feed (no init, no final xor) — the associative core shared by the
// single-span and multi-span CRCs. Exposed so a CRC over a payload-plus-trailer
// region can be computed across two spans WITHOUT concatenating them into a
// fresh buffer (the per-frame `covered` allocation the codec used to pay). Stays
// `constexpr` for compile-time CRCs (the Sarwate table loop); at RUNTIME it routes
// through the CPUID-dispatched hardware / slice-by-8 path, which is byte-identical.
[[nodiscard]] constexpr std::uint32_t crc32c_update(std::uint32_t c,
                                                    std::span<const std::byte> data) noexcept {
    if (std::is_constant_evaluated()) {
        for (std::byte b : data) {
            c = kCrc32c[(c ^ std::to_integer<std::uint8_t>(b)) & 0xFFu] ^ (c >> 8);
        }
        return c;
    }
    return crc32c_update_runtime(c, data);
}

[[nodiscard]] constexpr std::uint16_t crc16_update(std::uint16_t c,
                                                   std::span<const std::byte> data) noexcept {
    for (std::byte b : data) {
        const std::uint8_t idx =
            static_cast<std::uint8_t>((c >> 8) ^ std::to_integer<std::uint8_t>(b));
        c = static_cast<std::uint16_t>(kCrc16[idx] ^ (c << 8));
    }
    return c;
}

}  // namespace detail

/**
 * @brief A running CRC-32C accumulator — the one home of the init/final-xor constants.
 *
 * Feed the covered bytes as any number of contiguous chunks (a rope crossing link
 * boundaries, a payload-plus-trailer region), then read `value()`: byte-identical to
 * `crc32c` over the concatenation (the CRC is associative over the feed), with no
 * intermediate buffer. The single-/two-span `crc32c` below delegate to it.
 */
struct crc32c_state {
    std::uint32_t c = 0xFFFFFFFFu; /**< @brief Running state; init per RFC 3720. */
    /** @brief Feed one contiguous chunk of covered bytes. */
    constexpr void feed(std::span<const std::byte> data) noexcept {
        c = detail::crc32c_update(c, data);
    }
    /** @brief The finalized CRC-32C over everything fed so far. */
    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return c ^ 0xFFFFFFFFu; }
};

/**
 * @brief A running CRC-16-CCITT (FALSE) accumulator — the crc16 twin of @ref crc32c_state
 *        (init 0xFFFF, no final xor). Same feed-chunks-then-read-value contract.
 */
struct crc16_ccitt_state {
    std::uint16_t c = 0xFFFFu; /**< @brief Running state; init 0xFFFF, no final xor. */
    /** @brief Feed one contiguous chunk of covered bytes. */
    constexpr void feed(std::span<const std::byte> data) noexcept {
        c = detail::crc16_update(c, data);
    }
    /** @brief The finalized CRC-16-CCITT over everything fed so far. */
    [[nodiscard]] constexpr std::uint16_t value() const noexcept { return c; }
};

/** @brief CRC-32C (Castagnoli) over @p data. */
[[nodiscard]] constexpr std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
    crc32c_state s;
    s.feed(data);
    return s.value();
}

/**
 * @brief CRC-32C over the concatenation of @p a then @p b — byte-identical to
 *        `crc32c(a++b)` (CRC is associative over the feed), with no intermediate buffer.
 */
[[nodiscard]] constexpr std::uint32_t crc32c(std::span<const std::byte> a,
                                             std::span<const std::byte> b) noexcept {
    crc32c_state s;
    s.feed(a);
    s.feed(b);
    return s.value();
}

/** @brief CRC-16-CCITT (FALSE) over @p data. */
[[nodiscard]] constexpr std::uint16_t crc16_ccitt(std::span<const std::byte> data) noexcept {
    crc16_ccitt_state s;
    s.feed(data);
    return s.value();
}

/**
 * @brief CRC-16-CCITT over the concatenation of @p a then @p b — byte-identical to
 *        `crc16_ccitt(a++b)`, no intermediate buffer.
 */
[[nodiscard]] constexpr std::uint16_t crc16_ccitt(std::span<const std::byte> a,
                                                  std::span<const std::byte> b) noexcept {
    crc16_ccitt_state s;
    s.feed(a);
    s.feed(b);
    return s.value();
}

}  // namespace tr::crc
