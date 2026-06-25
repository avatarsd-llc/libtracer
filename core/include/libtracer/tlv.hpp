// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// Type codes and the `opt` options bitfield — the L2/L3 surface of the wire
// format. See docs/reference/01-data-format.md (header + opt) and
// docs/reference/05-protocol-tlvs.md (per-type layout).
#pragma once

#include <cstdint>

namespace tracer {

// Core type-code registry (0x01-0x0D). 0x05 is retired (was LIST, ADR-0003).
enum class Type : std::uint8_t {
    Value       = 0x01,
    Name        = 0x02,
    Description = 0x03,
    Subscriber  = 0x04,
    Path        = 0x06,
    Point       = 0x07,
    Error       = 0x08,
    Status      = 0x09,
    Acl         = 0x0A,
    Settings    = 0x0B,
    Time        = 0x0C,
    Router      = 0x0D,
};

// The 1-byte `opt` field. Bits, MSB->LSB: R | PL | TS | CR | LL | CW | TF | R.
// Bits 7 and 0 are reserved-MUST-be-zero (non-zero => frame::invalid).
struct Opt {
    bool pl = false;  // bit 6: payload-is-structured (children, not opaque)
    bool ts = false;  // bit 5: trailer carries a timestamp
    bool cr = false;  // bit 4: trailer carries a CRC
    bool ll = false;  // bit 3: length width (false = u16, true = u32)
    bool cw = false;  // bit 2: CRC width (false = CRC-32C, true = CRC-16-CCITT)
    bool tf = false;  // bit 1: timestamp form (false = abs u64, true = rel i32)

    static constexpr std::uint8_t kReservedMask = 0b1000'0001;  // bits 7 and 0

    [[nodiscard]] static constexpr bool reserved_set(std::uint8_t b) noexcept {
        return (b & kReservedMask) != 0;
    }

    [[nodiscard]] static constexpr Opt decode(std::uint8_t b) noexcept {
        return Opt{
            .pl = (b & 0x40) != 0,
            .ts = (b & 0x20) != 0,
            .cr = (b & 0x10) != 0,
            .ll = (b & 0x08) != 0,
            .cw = (b & 0x04) != 0,
            .tf = (b & 0x02) != 0,
        };
    }

    [[nodiscard]] constexpr std::uint8_t encode() const noexcept {
        return static_cast<std::uint8_t>(
            (pl ? 0x40 : 0) | (ts ? 0x20 : 0) | (cr ? 0x10 : 0) |
            (ll ? 0x08 : 0) | (cw ? 0x04 : 0) | (tf ? 0x02 : 0));
    }

    constexpr bool operator==(const Opt&) const noexcept = default;
};

}  // namespace tracer
