/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/*
 * The ONE `FWD{REPLY}` assembler (#887). The reply grammar — `FWD{ VALUE op=REPLY,
 * PATH dst=req.src, PATH src=req.dst, VALUE kind, <payload> }` (RFC-0004 §D) — and
 * the `kind=ERROR` `STATUS{ ERROR{ VALUE u16 LE code } }` tail built on it are
 * declared here and defined ONCE, in fwd_reply.cpp.
 *
 * They used to live in `op_resolve_walk.hpp`'s anonymous namespace, so the byte
 * layout was compiled THREE times: an internal-linkage copy in each of the walk's two
 * instantiation TUs (op_resolve.cpp, op_resolve_view.cpp) plus a divergent
 * hand-rolled one in `fwd_router.cpp`'s bus-NAME-hop rejection. The router's copy
 * re-encoded its route nodes with `wire::encode`, which PRESERVES trailer bytes,
 * while the resolver's copies trailer-slice them (ADR-0041 §4) — a live drift
 * surface on the one field the two paths did not agree on.
 *
 * The seam is BYTE SPANS, not nodes: a caller hands the two route TLVs' whole-TLV,
 * trailer-EXCLUDED wire bytes, so the assembler is decoupled from whichever node
 * model (arena span / lazy view / decoded `tlv_t`) produced them (ADR-0053 §7).
 *
 * NB this is a PRIVATE header — core/src only, never installed. `reply_kind_t`,
 * `status_t` and the wire bytes themselves are the public surface.
 */
#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/status.hpp"
#include "libtracer/tlv.hpp"

/**
 * @file
 * @brief The one externally-linked `FWD{REPLY}` / `kind=ERROR` reply assembler (#887).
 */

namespace tr::graph {

/**
 * @brief The opt byte an ADR-0041 §4 trailer-sliced copy carries: the structural bits (PL, LL)
 *        survive; the trailer bits (TS, CR, CW, TF) are cleared so the copied TLV — whose bytes
 *        exclude the trailer by construction (`node.wire`) — stays self-consistent and trailer-less
 *        at rest.
 *
 * Cleared through `opt_t` (not a raw
 * `& 0x48` mask) so there is one representation of the opt bitfield.
 */
[[nodiscard]] constexpr std::byte struct_opt(std::byte opt_byte) noexcept {
    return static_cast<std::byte>(
        wire::opt_t::decode(std::to_integer<std::uint8_t>(opt_byte)).without_trailer().encode());
}

/**
 * @brief Byte-identical to the retired `& 0x48` mask for any validated opt byte (0x7E = every non-
 *        reserved bit set → PL|LL == 0x48; a plain opaque VALUE 0x00 → 0x00).
 */
static_assert(struct_opt(std::byte{0x7E}) == std::byte{0x48});
static_assert(struct_opt(std::byte{0x00}) == std::byte{0x00});

/**
 * @brief A tiny cursor writer over a preallocated, exactly-sized head segment — the direct-emit
 *        that replaces the old 4-stage (encode → children → head → segment) staging: every length
 *        is known from the arena spans up front, so the reply head is ONE allocation and each route
 *        byte is copied exactly once.
 *
 * Header-inline, unlike the assembler it serves, because the resolve walk ALSO writes a bare
 * `POINT` wrapper header with it on the `:subscribers[]` read — a payload-framing site, not a
 * reply-grammar one. Inlining there is what keeps that stack-buffer emit free of a call.
 */
struct emit_cursor_t {
    std::byte* p; /**< @brief The write cursor; advanced by every emitter below. */

    /** @brief A structured (PL) header: type, opt, then a u16 (or u32 when @p ll) LE length. */
    void struct_header(wire::type_t type, bool ll, std::size_t body_len) {
        *p++ = static_cast<std::byte>(std::to_underlying(type));
        *p++ = static_cast<std::byte>(wire::opt_t{.pl = true, .ll = ll}.encode());
        detail::store_le(std::span<std::byte>(p, ll ? 4u : 2u),
                         static_cast<std::uint32_t>(body_len), ll ? 4u : 2u);
        p += ll ? 4u : 2u;
    }
    /** @brief A 1-byte `VALUE` TLV — the `op` / `kind` discriminants. */
    void u8_value(std::uint8_t v) {
        *p++ = static_cast<std::byte>(std::to_underlying(wire::type_t::VALUE));
        *p++ = std::byte{0};
        *p++ = std::byte{1};
        *p++ = std::byte{0};
        *p++ = std::byte{v};
    }
    /** @brief A whole-TLV copy whose opt byte is trailer-sliced on the way in (ADR-0041 §4). */
    void tlv_sliced(std::span<const std::byte> wire) {
        std::memcpy(p, wire.data(), wire.size());
        p[1] = struct_opt(p[1]);
        p += wire.size();
    }
    /** @brief Verbatim bytes (the assembled status tail). */
    void raw(std::span<const std::byte> bytes) {
        if (!bytes.empty()) std::memcpy(p, bytes.data(), bytes.size());
        p += bytes.size();
    }
};

/**
 * @brief Assemble the FWD{REPLY} rope: one exactly-sized head segment (FWD header + op=REPLY +
 *        dst=req.src + src=req.dst + kind + @p inline_tail) prepended to @p shared (refcount
 *        clones of the stored payload view(s)).
 *
 * The head is the only
 * allocation; the route bytes are copied ONCE (trailer-sliced); @p shared is never
 * copied — RFC-0004 §D / ADR-0035 zero-copy reply rule, ADR-0041 §5 direct emit.
 * @p reply_dst_wire (the request's `src`) and @p reply_src_wire (the request's `dst`,
 * the responder endpoint) are the trailer-excluded whole-TLV `wire` spans of those two
 * PATH nodes — passed in rather than read off the arena so the reply builder is
 * decoupled from the request's node model (ADR-0053 §7: a node-reader-agnostic seam,
 * and where ⑤'s scatter-gather reply emission will hook). Both MUST be non-empty: an
 * empty route span would emit a header whose length counts bytes no address occupies.
 *
 * @p egress is the byte backend the head segment — and, on a mint, the trailing `PATH_REF`
 * segment — draw from (#795, ADR-0074). It is the reply's EGRESS-construction seam,
 * distinct from the flatten seam: its size tracks the swapped ROUTE bytes plus the inline tail,
 * not the payload bytes `flat` is sized against. A bounded node points it at its own slab so
 * this reply-egress allocation stays inside the node's memory bound; the default is the global
 * heap. A refusal returns an EMPTY rope, which `resolve_node`'s `or_backpressure` turns into an
 * addressed `kind=ERROR STATUS{BACKPRESSURE}` — exhaustion answered by value, never an abort.
 *
 * @param reply_dst_wire The request's `src` route, trailer-excluded whole-TLV bytes.
 * @param reply_src_wire The request's `dst` route, trailer-excluded whole-TLV bytes.
 * @param kind           The reply discriminant (RESULT / ERROR).
 * @param inline_tail    Bytes emitted into the head after `kind` (the ERROR status tail).
 * @param shared         Payload views roped on after the head — never copied.
 * @param shared_len     Total byte length of @p shared (the header's length field needs it).
 * @param egress         The byte backend the head (and mint) segments draw from.
 * @param trailing       An optional minted `PATH_REF` emitted as the reply's LAST child.
 * @return The reply rope, or an EMPTY rope when @p egress refuses.
 */
[[nodiscard]] view::rope_t assemble_reply(std::span<const std::byte> reply_dst_wire,
                                          std::span<const std::byte> reply_src_wire,
                                          reply_kind_t kind, std::span<const std::byte> inline_tail,
                                          std::span<const view::view_t> shared,
                                          std::size_t shared_len, mem::mem_backend_t& egress,
                                          std::span<const std::byte> trailing = {});

/**
 * @brief A kind=ERROR reply carrying STATUS{ ERROR{ VALUE u16 LE code } } (RFC-0004 §D with the
 *        RFC-0002 §C registered-code identity) — a fixed 14-byte tail, built on the stack.
 *
 * The ONE home of the addressed-error frame: the terminus resolver's refusals and
 * `fwd_router.cpp`'s bus-NAME-hop rejection (ADR-0073 §3 / RFC-0020) both land here, so the
 * two cannot drift on the status tail, on the swap order, or on the route bytes' trailer bits.
 *
 * @param reply_dst_wire The request's `src` route, trailer-excluded whole-TLV bytes.
 * @param reply_src_wire The request's `dst` route, trailer-excluded whole-TLV bytes.
 * @param status         The L4 outcome; mapped to its registered wire code (RFC-0002 §D).
 * @param egress         The byte backend the reply head draws from.
 * @return The reply rope (exactly one link), or an EMPTY rope when @p egress refuses.
 */
[[nodiscard]] view::rope_t assemble_error_reply(std::span<const std::byte> reply_dst_wire,
                                                std::span<const std::byte> reply_src_wire,
                                                status_t status, mem::mem_backend_t& egress);

}  // namespace tr::graph
