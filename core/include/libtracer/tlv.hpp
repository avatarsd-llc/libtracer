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
 * generically. 0x14 PATH_REF is the bound-path address form (RFC-0024 §4) and 0x15
 * PATH_REF_REVERSE the reverse-direction list a mint-flagged request accumulates (§7.1
 * amendment 2): the two types whose body is NOT self-describing — a fixed-stride 8-byte
 * record array (opt.PL=0), whose shape the grammar therefore checks by type (path_ref.hpp,
 * gated by `is_path_ref_type`).
 *
 * One code beyond the core range is named here: 0x80 BATCH, the single assignment inside the
 * user range (0x80-0xFF, RFC-0025 §4.1.2 clause 6). It is an ordinary structured TLV to the
 * codec — the enumerator exists so the one convention libtracer itself emits has a spelling,
 * not because the codec treats it specially.
 */
enum class type_t : std::uint8_t {
    VALUE = 0x01,       /**< @brief Opaque scalar value. */
    NAME = 0x02,        /**< @brief UTF-8 name segment. */
    DESCRIPTION = 0x03, /**< @brief Human-readable description. */
    SUBSCRIBER = 0x04,  /**< @brief Subscriber registration edge. */
    PATH = 0x06,        /**< @brief Path address; opaque body of packed `[u8 len][utf8]`
                             segment records (RFC-0018), NOT `NAME` children. */
    POINT = 0x07,       /**< @brief A point in a path/graph. */
    ERROR = 0x08,       /**< @brief Error report. */
    STATUS = 0x09,      /**< @brief Status report. */
    ACL = 0x0A,         /**< @brief Access-control list. */
    SETTINGS = 0x0B,    /**< @brief QoS settings. */
    /** @brief An APPLICATION-DOMAIN timestamp carried inside a structured payload — RESERVED,
     *         deliberately emitted and consumed by no core code (#1109). The wire-trailer TS
     *         (`opt.ts`) is transport-time; sample-acquisition / control-deadline time rides
     *         the payload as a TIME child instead (docs/reference/01-data-format.md
     *         §application-domain timestamps). What a TIME body means is the embedder's schema,
     *         so core assigns the code and nothing else. */
    TIME = 0x0C,
    ROUTER = 0x0D, /**< @brief Router-wrapped frame. */
    SPEC = 0x0E,   /**< @brief In-band vertex-creation spec (structured; ADR-0017). */
    FWD = 0x0F,    /**< @brief Remote-operation forward frame (RFC-0004 §B / ADR-0035). */
    FIELD = 0x10,  /**< @brief Control-plane `:field` selector (RFC-0004 §C / ADR-0035). */
    /** @brief Route-handle: VALUE label(u16) + PATH route — bind label→route, swapped per hop. */
    ADVERTISE = 0x11,
    /** @brief Route-handle: VALUE label(u16) + payload TLV — a label-compacted delivery. */
    COMPACT = 0x12,
    /** @brief Route-handle: VALUE label(u16) — stale/unknown label seen; prompts re-advertise. */
    HANDLE_NACK = 0x13,
    /** @brief Bound path: a bare array of 8-byte node-scoped vertex refs (RFC-0024 §4). */
    PATH_REF = 0x14,
    /**
     * @brief The REVERSE-direction bound path a mint-flagged request accumulates
     *        (RFC-0024 §7.1 amendment 2) — same body grammar as `PATH_REF`, different role.
     *
     * A distinct code rather than a positional rule: every other element of this grammar
     * self-describes by type, and "the only trailing child" would break the moment a future
     * RFC adds a second trailing child to a mint-flagged request. It also un-forecloses a raw
     * `PATH_REF`-typed *payload* on such a request. The code costs nothing: a reader already
     * compares the child's type byte, so a different constant is the same instruction
     * (`peek_trailing_mint`).
     */
    PATH_REF_REVERSE = 0x15,
    /**
     * @brief The ONE assigned user-range code: the BATCH record (RFC-0025 §4.1.2, Amendment 3
     *        clause 6) — a structured (`opt.PL=1`) written value whose children are the sample
     *        frames of one flush (`batch.hpp`).
     *
     * `0x80`–`0xFF` is the range the protocol does not opine on, and the assignment does not
     * change that: a deployment already using `0x80` for its own record is not made
     * non-conforming, no core-range code is minted, no `opt` bit is added, and the graph still
     * never interprets the body (claim 5). What the assignment buys is ONE number — so the
     * reference helpers, the §4.3 descriptor and the conformance vectors stop each picking
     * their own. Until Amendment 3 this code appeared only as the worked example of
     * docs/reference/05-protocol-tlvs.md §`0x0C`, which reads the same either way.
     */
    BATCH = 0x80,
};

/**
 * @brief True for either bound-path type — the two codes whose body is a fixed-stride
 *        8-byte element array rather than a self-describing payload (RFC-0024 §4.2).
 *
 * The structural rules of `path_ref_body_valid` apply to both, so every site that gates on
 * "is this an element array" asks HERE rather than spelling a two-code disjunction. `0x15`
 * was taken adjacent to `0x14` for exactly this: the test stays ONE masked compare on the
 * grammar's per-TLV path, where a disjunction would have been two.
 */
[[nodiscard]] constexpr bool is_path_ref_type(type_t t) noexcept {
    static_assert(static_cast<std::uint8_t>(type_t::PATH_REF) == 0x14);
    static_assert(static_cast<std::uint8_t>(type_t::PATH_REF_REVERSE) == 0x15);
    return (static_cast<std::uint8_t>(t) & 0xFEu) == static_cast<std::uint8_t>(type_t::PATH_REF);
}

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
