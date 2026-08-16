/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/**
 * @file
 * @brief L2/L3 (`tr::wire`) — THE packed `PATH` body grammar (RFC-0018).
 *
 * A `PATH` (`0x06`) body is `opt.PL = 0` and holds a self-delimiting sequence of
 * **segment records**, each `[u8 len][len bytes of UTF-8]`, in order. The walk is
 * `p += 1 + body[p]` — one byte load, one add, and the bounds compare the load
 * already needed. That is the whole point of the encoding: it replaces a 4-byte
 * `NAME` TLV header (type + option byte + `u16` length) whose option byte admitted
 * several legal spellings of one address, so a packed body has exactly ONE spelling
 * per address and byte-keying the vertex map is guaranteed rather than tested
 * (RFC-0018 §4, reversing ADR-0062 §"Considered options").
 *
 * `len == 0` is the **escape record** of RFC-0018 §8, normative since §5.4
 * Amendment 1: `00 <u8 kind> <u8 len> <len bytes>`, total `3 + len` bytes. It is
 * **admissible in a frame path** — a forwarder steps over one it does not implement
 * by its declared length rather than dropping a frame it is only relaying — and
 * **rejected in canonical / key context**, because a label is not canonical bytes
 * and the vertex-map key must stay pure-string for `key_view_t`'s
 * byte-prefix-implies-ancestor invariant to hold. Nothing in this core MINTS an
 * escape; `kind = 0x16` is reserved for RFC-0027's label element.
 *
 * The two contexts are two functions here, named for the rule they enforce
 * (`packed_path_valid_key` vs `packed_record_span`), so no call site has to
 * decide the question with a bool argument it might pass the wrong way.
 */

namespace tr::wire {

/** @brief Max bytes one packed segment record's payload may carry (`u8` length field). */
inline constexpr std::size_t kPackedSegMaxBytes = 255;

/**
 * @brief The escape record's length byte — RFC-0018 §8 / §5.4 Amendment 1.
 *
 * Zero is not a spellable segment (path syntax rejects an empty segment, so `/a//b` is
 * invalid rather than a spelling of `/a/b`), which is what makes it free to reserve.
 */
inline constexpr std::uint8_t kPackedEscapeLen = 0;

/** @brief Bytes of an escape record beyond its declared payload: `00`, kind, len. */
inline constexpr std::size_t kPackedEscapeOverhead = 3;

/**
 * @brief The escape `kind` reserved for RFC-0027's label element (a `u32`: u16 index,
 *        u16 generation). Declared so the SKIP path can be described; nothing mints it.
 */
inline constexpr std::uint8_t kPackedEscapeKindLabel = 0x16;

/**
 * @brief Total bytes of the packed record starting at @p at of @p body — the walk's ONE
 *        locus, in **frame-path** context (an escape record is stepped over, not refused).
 * @retval 0 The bytes at @p at are ragged: no length byte, or a record that runs past the
 *           body's end. A well-framed record is at least 2 bytes, so 0 is unambiguous.
 */
[[nodiscard]] constexpr std::size_t packed_record_span(std::span<const std::byte> body,
                                                       std::size_t at) noexcept {
    if (at >= body.size()) return 0;
    const auto len = static_cast<std::uint8_t>(body[at]);
    if (len != kPackedEscapeLen) return at + 1 + len > body.size() ? 0 : 1 + len;
    // The escape: `00 <kind> <len> <payload>`. Self-delimiting by design (RFC-0018 §8) —
    // the node least likely to implement `kind` is exactly the node that must step over it.
    if (at + kPackedEscapeOverhead > body.size()) return 0;
    const auto esc = static_cast<std::uint8_t>(body[at + 2]);
    return at + kPackedEscapeOverhead + esc > body.size() ? 0 : kPackedEscapeOverhead + esc;
}

/** @brief True iff the record at @p at of @p body is the `len == 0` escape (RFC-0018 §8). */
[[nodiscard]] constexpr bool packed_record_is_escape(std::span<const std::byte> body,
                                                     std::size_t at) noexcept {
    return at < body.size() && static_cast<std::uint8_t>(body[at]) == kPackedEscapeLen;
}

/**
 * @brief Does @p body tile exactly into packed records that are ALL literal segments —
 *        the **canonical / key** context rule (RFC-0018 §5.4).
 *
 * An escape record answers false here, and that is the whole asymmetry with
 * `packed_record_span`: a label is not canonical bytes, so a `PATH` carrying one is
 * never a `path_lookup_key` and never a vertex-map key. Keeping canonical keys
 * pure-string is what preserves `key_view_t`'s byte-prefix-implies-ancestor invariant
 * unchanged, which is RFC-0018 falsifier 4.
 *
 * An EMPTY body is valid: it is the graph root (`/`), zero segments.
 */
[[nodiscard]] constexpr bool packed_path_valid_key(std::span<const std::byte> body) noexcept {
    std::size_t at = 0;
    while (at < body.size()) {
        const auto len = static_cast<std::uint8_t>(body[at]);
        if (len == kPackedEscapeLen) return false;  // escape: admissible in a frame, not a key
        if (at + 1 + len > body.size()) return false;
        at += 1 + len;
    }
    return true;
}

/**
 * @brief Append one packed segment record `[u8 len][bytes]` for @p seg.
 * @return False, appending NOTHING, when @p seg is empty (which would spell the escape) or
 *         longer than @ref kPackedSegMaxBytes (which the length field cannot express).
 */
[[nodiscard]] inline bool emit_path_segment(std::vector<std::byte>& out,
                                            std::span<const std::byte> seg) {
    if (seg.empty() || seg.size() > kPackedSegMaxBytes) return false;
    out.push_back(static_cast<std::byte>(seg.size()));
    out.insert(out.end(), seg.begin(), seg.end());
    return true;
}

/** @brief Text overload of @ref emit_path_segment (no temporary buffer). */
[[nodiscard]] inline bool emit_path_segment(std::vector<std::byte>& out, std::string_view seg) {
    return emit_path_segment(out, std::span<const std::byte>(
                                      reinterpret_cast<const std::byte*>(seg.data()), seg.size()));
}

}  // namespace tr::wire
