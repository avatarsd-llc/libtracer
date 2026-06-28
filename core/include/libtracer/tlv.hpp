/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * type_t codes and the `opt` options bitfield — the L2/L3 surface of the wire
 * format. See docs/reference/01-data-format.md (header + opt) and
 * docs/reference/05-protocol-tlvs.md (per-type layout).
 */
#pragma once

#include <cstdint>

namespace tr::wire {

// Core type-code registry (0x01-0x10). 0x05 is retired (was LIST, ADR-0003).
// 0x0E SPEC is the in-band vertex-creation spec (ADR-0017). 0x0F FWD and 0x10
// FIELD are the remote-operation frames (RFC-0004 / ADR-0035, the v1 fast-track
// range 0x0F-0x1F). All structured (opt.PL=1) and handled generically by the
// codec — see docs/reference/05 §SPEC and RFC-0004 §B/§C.
enum class type_t : std::uint8_t {
    VALUE = 0x01,
    NAME = 0x02,
    DESCRIPTION = 0x03,
    SUBSCRIBER = 0x04,
    PATH = 0x06,
    POINT = 0x07,
    ERROR = 0x08,
    STATUS = 0x09,
    ACL = 0x0A,
    SETTINGS = 0x0B,
    TIME = 0x0C,
    ROUTER = 0x0D,
    SPEC = 0x0E,
    FWD = 0x0F,
    FIELD = 0x10,
    // Transport-plane route-handle control frames (RFC-0004 §E.1, ADR-0035 slice
    // 4). These ride a full-TLV link (ws/UDP) ALONGSIDE FWD to compact an
    // established, `delivery_compact`-flagged delivery flow into a per-link label
    // (header-elision generalized, ADR-0022). They are NOT part of the FWD frame
    // and NOT cross-core conformance TLVs — a peer that ignores them simply keeps
    // the full-route delivery path. Self-describing (opt.PL=1) so the codec parses
    // them generically. See docs/reference/05 §route-handle and RFC-0004 §E.1.
    ADVERTISE = 0x11,    // VALUE label(u16) + PATH route — bind label -> route, swapped per hop
    COMPACT = 0x12,      // VALUE label(u16) + payload TLV — a label-compacted delivery
    HANDLE_NACK = 0x13,  // VALUE label(u16) — stale/unknown label seen; prompts re-advertise
};

// The 1-byte `opt` field. Bits, MSB->LSB: R | PL | TS | CR | LL | CW | TF | R.
// Bits 7 and 0 are reserved-MUST-be-zero (non-zero => frame::invalid).
struct opt_t {
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

    [[nodiscard]] static constexpr opt_t decode(std::uint8_t b) noexcept {
        return opt_t{
            .pl = (b & 0x40) != 0,
            .ts = (b & 0x20) != 0,
            .cr = (b & 0x10) != 0,
            .ll = (b & 0x08) != 0,
            .cw = (b & 0x04) != 0,
            .tf = (b & 0x02) != 0,
        };
    }

    [[nodiscard]] constexpr std::uint8_t encode() const noexcept {
        return static_cast<std::uint8_t>((pl ? 0x40 : 0) | (ts ? 0x20 : 0) | (cr ? 0x10 : 0) |
                                         (ll ? 0x08 : 0) | (cw ? 0x04 : 0) | (tf ? 0x02 : 0));
    }

    constexpr bool operator==(const opt_t&) const noexcept = default;
};

}  // namespace tr::wire
