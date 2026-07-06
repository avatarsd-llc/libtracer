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

/**
 * @brief The core TLV type-code registry (0x01-0x10, docs/reference/05 §per-type layout).
 *
 * 0x05 is retired (was LIST, ADR-0003). 0x0E SPEC is the in-band vertex-creation spec
 * (ADR-0017); 0x0F FWD and 0x10 FIELD are the remote-operation frames (RFC-0004 / ADR-0035,
 * the v1 fast-track range 0x0F-0x1F). All are structured (opt.PL=1) and handled generically
 * by the codec. Codes 0x11-0x13 are transport-plane route-handle control frames (RFC-0004
 * §E.1, ADR-0035 slice 4): they ride a full-TLV link (ws/UDP) ALONGSIDE FWD to compact an
 * established, `delivery_compact`-flagged flow into a per-link label. They are NOT part of
 * the FWD frame and NOT cross-core conformance TLVs — a peer that ignores them simply keeps
 * the full-route delivery path — but are self-describing (opt.PL=1) so the codec parses them
 * generically.
 */
enum class type_t : std::uint8_t {
    VALUE = 0x01,       /**< @brief Opaque scalar value. */
    NAME = 0x02,        /**< @brief UTF-8 name segment. */
    DESCRIPTION = 0x03, /**< @brief Human-readable description. */
    SUBSCRIBER = 0x04,  /**< @brief Subscriber registration edge. */
    PATH = 0x06,        /**< @brief Structured path (a sequence of NAME children). */
    POINT = 0x07,       /**< @brief A point in a path/graph. */
    ERROR = 0x08,       /**< @brief Error report. */
    STATUS = 0x09,      /**< @brief Status report. */
    ACL = 0x0A,         /**< @brief Access-control list. */
    SETTINGS = 0x0B,    /**< @brief QoS settings. */
    TIME = 0x0C,        /**< @brief Time. */
    ROUTER = 0x0D,      /**< @brief Router-wrapped frame. */
    SPEC = 0x0E,        /**< @brief In-band vertex-creation spec (structured; ADR-0017). */
    FWD = 0x0F,         /**< @brief Remote-operation forward frame (RFC-0004 §B / ADR-0035). */
    FIELD = 0x10,       /**< @brief Control-plane `:field` selector (RFC-0004 §C / ADR-0035). */
    /** @brief Route-handle: VALUE label(u16) + PATH route — bind label→route, swapped per hop. */
    ADVERTISE = 0x11,
    /** @brief Route-handle: VALUE label(u16) + payload TLV — a label-compacted delivery. */
    COMPACT = 0x12,
    /** @brief Route-handle: VALUE label(u16) — stale/unknown label seen; prompts re-advertise. */
    HANDLE_NACK = 0x13,
};

/**
 * @brief The 1-byte `opt` options bitfield of a TLV header.
 *
 * Bits, MSB→LSB: R | PL | TS | CR | LL | CW | TF | R. Bits 7 and 0 are
 * reserved-MUST-be-zero (a set reserved bit ⇒ `frame::invalid`). See
 * docs/reference/01-data-format.md §header + opt.
 */
struct opt_t {
    bool pl = false; /**< @brief bit 6: payload-is-structured (children, not opaque bytes). */
    bool ts = false; /**< @brief bit 5: trailer carries a timestamp. */
    bool cr = false; /**< @brief bit 4: trailer carries a CRC. */
    bool ll = false; /**< @brief bit 3: length width (false = u16, true = u32). */
    bool cw = false; /**< @brief bit 2: CRC width (false = CRC-32C, true = CRC-16-CCITT). */
    bool tf = false; /**< @brief bit 1: timestamp form (false = abs u64, true = rel i32). */

    /** @brief The reserved-MUST-be-zero bit mask (bits 7 and 0). */
    static constexpr std::uint8_t kReservedMask = 0b1000'0001;

    /** @brief True iff a reserved bit is set in raw byte @p b (⇒ the frame is invalid). */
    [[nodiscard]] static constexpr bool reserved_set(std::uint8_t b) noexcept {
        return (b & kReservedMask) != 0;
    }

    /** @brief Unpack a raw `opt` byte @p b (reserved bits are checked separately). */
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

    /** @brief Pack back into the raw `opt` byte (reserved bits always zero). */
    [[nodiscard]] constexpr std::uint8_t encode() const noexcept {
        return static_cast<std::uint8_t>((pl ? 0x40 : 0) | (ts ? 0x20 : 0) | (cr ? 0x10 : 0) |
                                         (ll ? 0x08 : 0) | (cw ? 0x04 : 0) | (tf ? 0x02 : 0));
    }

    /**
     * @brief The same opt with the trailer bits (TS/CR/CW/TF) cleared — only the structural
     *        bits (PL/LL) survive.
     *
     * An ADR-0041 §4 trailer-sliced whole-TLV copy (op_resolve.cpp) applies this so the copy,
     * whose bytes exclude the trailer by construction, stays self-consistent — the typed
     * replacement for the raw `opt & 0x48` mask that once encoded these bits.
     */
    [[nodiscard]] constexpr opt_t without_trailer() const noexcept {
        opt_t o = *this;
        o.ts = o.cr = o.cw = o.tf = false;
        return o;
    }

    /** @brief Value equality over all option bits. */
    constexpr bool operator==(const opt_t&) const noexcept = default;
};

}  // namespace tr::wire
