/**
 * @file
 * @brief The one place the test tree knows the FWD wire layout (#875).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A hand-rolled `b_fwd` used to be defined **27 times across 23 test files**, each one
 * re-stating the same three facts about the frame: the child ORDER, that the op is the
 * FIRST child and a bare `VALUE` (not a NAME, not a FIELD), and that the envelope carries
 * `opt_t{.pl = true}`. A layout change was a 23-file synchronized edit, and the copies had
 * already drifted — not in the bytes they emitted, but in what their *parameters* meant:
 * `b_fwd(op, dst, src, X)` bound `X` to the SELECTOR in eight files and to the PAYLOAD in
 * eight others, and `fwd_frame_view_test.cpp` took `(…, payload, selector)` in the opposite
 * order from everyone else. Nothing caught it because each copy only ever met its own call
 * sites.
 *
 * This header states the layout once:
 *
 * ```
 *   FWD[.pl] { VALUE op, PATH dst, [FIELD selector], PATH src, [payload] }
 * ```
 *
 * (RFC-0004 §B child order; the selector sits between `dst` and `src` so a forwarder can
 * peek `dst` at a fixed offset and skip the rest.) Every builder below funnels through
 * @ref tr::testing::fwd_envelope, so a future layout change edits ONE function body.
 *
 * The deliberately-malformed shapes are parameters here rather than separate hand-rolls:
 * a non-structured envelope (`structured = false`, the missing `.pl` bit) and a zero-length
 * op child (@ref tr::testing::b_fwd_no_op) are both frames a test must be able to build
 * *precisely because* the decoder has to reject them.
 *
 * `tr::testing` is a tests-only namespace — it is not a layer in the L0..L5 model and
 * nothing under `core/src` or `core/include` may name it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/op_resolve.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::testing {

/** @brief Raw wire bytes under assembly — what every builder here returns. */
using bytes_t = std::vector<std::byte>;

/**
 * @brief Wrap already-assembled @p children in the FWD envelope.
 *
 * The single home of the envelope's structural bit. @p structured false clears `.pl`, which
 * makes the frame malformed (a FWD body is a child list, never an opaque payload) — the shape
 * the terminus-reject tests need.
 */
inline bytes_t fwd_envelope(std::span<const std::byte> children, bool structured = true) {
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::FWD,
                       structured ? tr::wire::opt_t{.pl = true} : tr::wire::opt_t{}, children);
    return out;
}

/** @brief The op child: one opaque `VALUE` byte, carrying opcode bits 5-0 plus any flags. */
inline bytes_t fwd_op_child(std::uint8_t op_byte) {
    bytes_t out;
    const std::byte b{op_byte};
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief An EMPTY op child — a zero-length `VALUE`, which no opcode read can satisfy. */
inline bytes_t fwd_op_child_empty() {
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>{});
    return out;
}

/**
 * @brief A `PATH` TLV over @p segs — the address form the transport tests build (#1115).
 *
 * The `b_fwd` family takes already-built `dst`/`src` TLV bytes, which suited the 23 files
 * #875 migrated because they had `path_t` bytes in hand. The transport and mount tests
 * instead spell their addresses as segment lists, and each of them re-derived the same two
 * facts to turn one into the other: a segment is a `NAME`, and the containing `PATH` carries
 * `opt_t{.pl = true}` because its body is a child list. Those are layout facts, so they
 * belong here rather than in eight test files.
 *
 * An EMPTY @p segs is the reply-route-so-far of an origin hop: a present, zero-length `PATH`
 * that grows one segment per forwarder. It is emitted, never omitted — a missing `src` child
 * is a different (malformed) frame.
 */
template <class Segs>
inline bytes_t b_path(const Segs& segs) {
    bytes_t body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    bytes_t out;
    tr::wire::emit_tlv(out, tr::wire::type_t::PATH, tr::wire::opt_t{.pl = true}, body);
    return out;
}

/** @brief @ref b_path over a braced segment list — `b_path({"node", "leaf"})`. */
inline bytes_t b_path(std::initializer_list<std::string_view> segs) {
    return b_path<std::initializer_list<std::string_view>>(segs);
}

namespace detail {

/** @brief Append @p src to @p dst — the assembly primitive every builder here shares. */
inline void append(bytes_t& dst, std::span<const std::byte> src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

/**
 * @brief Assemble the FWD body from an already-built @p op_child and emit the envelope.
 *
 * THE layout: `op, dst, [selector], src, [payload]`. Empty optional children are omitted
 * rather than emitted zero-length, which is what a real origin puts on the wire.
 */
inline bytes_t fwd_frame(std::span<const std::byte> op_child, std::span<const std::byte> dst,
                         std::span<const std::byte> selector, std::span<const std::byte> src,
                         std::span<const std::byte> payload, bool structured) {
    bytes_t body;
    append(body, op_child);
    append(body, dst);
    if (!selector.empty()) append(body, selector);
    append(body, src);
    if (!payload.empty()) append(body, payload);
    return fwd_envelope(body, structured);
}

}  // namespace detail

/**
 * @brief A FWD frame from a RAW op byte — the form that can set flag bits (RFC-0024 §7.5).
 *
 * @param op_byte   opcode bits 5-0, optionally OR'd with a flag (`kFwdOpFlagMintRequest`).
 * @param dst       the destination `PATH` TLV bytes.
 * @param src       the reply-route `PATH` TLV bytes.
 * @param selector  optional `FIELD` selector, emitted BETWEEN `dst` and `src`.
 * @param payload   optional trailing payload TLV bytes.
 */
inline bytes_t b_fwd_raw_op(std::uint8_t op_byte, std::span<const std::byte> dst,
                            std::span<const std::byte> src,
                            std::span<const std::byte> selector = {},
                            std::span<const std::byte> payload = {}) {
    return detail::fwd_frame(fwd_op_child(op_byte), dst, selector, src, payload, true);
}

/**
 * @brief A FWD frame — the canonical builder every test should reach for first.
 *
 * @param op        the operation; its opcode byte becomes the first `VALUE` child.
 * @param dst       the destination `PATH` TLV bytes.
 * @param src       the reply-route `PATH` TLV bytes.
 * @param selector  optional `FIELD` selector, emitted BETWEEN `dst` and `src`.
 * @param payload   optional trailing payload TLV bytes.
 */
inline bytes_t b_fwd(tr::graph::fwd_op_t op, std::span<const std::byte> dst,
                     std::span<const std::byte> src, std::span<const std::byte> selector = {},
                     std::span<const std::byte> payload = {}) {
    return b_fwd_raw_op(static_cast<std::uint8_t>(op), dst, src, selector, payload);
}

/**
 * @brief A FWD frame carrying the bound-path MINT REQUEST flag (RFC-0024 §7.5).
 *
 * Named rather than left to callers to OR by hand: the flag lives in the op byte, which is
 * exactly the layout knowledge this header exists to hold in one place.
 */
inline bytes_t b_fwd_mint(tr::graph::fwd_op_t op, std::span<const std::byte> dst,
                          std::span<const std::byte> src, std::span<const std::byte> selector = {},
                          std::span<const std::byte> payload = {}) {
    return b_fwd_raw_op(static_cast<std::uint8_t>(op) | tr::graph::kFwdOpFlagMintRequest, dst, src,
                        selector, payload);
}

/**
 * @brief A FWD frame whose op child is a ZERO-LENGTH `VALUE` — a malformed-frame probe.
 *
 * Well-formed in envelope and child count, unreadable in opcode: the shape that proves a
 * forwarder reads the op byte's LENGTH before its value.
 */
inline bytes_t b_fwd_no_op(std::span<const std::byte> dst, std::span<const std::byte> src,
                           std::span<const std::byte> selector = {},
                           std::span<const std::byte> payload = {}) {
    return detail::fwd_frame(fwd_op_child_empty(), dst, selector, src, payload, true);
}

}  // namespace tr::testing
