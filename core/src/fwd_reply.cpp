/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "fwd_reply.hpp"

#include <array>
#include <cstring>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/error.hpp"

/**
 * @file
 * @brief The one definition of the `FWD{REPLY}` head grammar and its `kind=ERROR` tail (#887).
 */

namespace tr::graph {

using view::rope_t;
using view::segment_ptr_t;
using view::view_t;
using wire::opt_t;
using wire::type_t;

namespace {

/**
 * @brief Map an L4 status_t to its registered tr:: error code (RFC-0002 §D registry, wire::err_t) —
 *        the u16 the kind=ERROR reply's ERROR{VALUE} identity carries.
 *
 * @section error_code_exhaustive Why there is no fall-through
 *
 * This is the L4→wire cast point, and the two enums either side of it are deliberately separate
 * registries: `err_t` is the WIRE registry (`tr::wire`, L2/L3), `status_t` is L4 graph
 * vocabulary, and L4 does not bind wire values (STYLE.md — dependencies point up the layers
 * only). Keeping them apart means the mapping is hand-written, and a hand-written mapping needs
 * an instrument that notices when it falls behind its input.
 *
 * The instrument is the compiler. The switch carries neither a `default:` label nor a
 * fall-through tail, so `-Wswitch` — an error under the `-Werror=switch` this library compiles
 * with — names this switch the moment an enumerator is added to `status_t` without an arm here.
 * The retired tail was `return wire::err_t::PATH_NOT_FOUND;`, which made an unmapped status a
 * *silent wire mislabel* instead: a future transport-down or version-mismatch status would have
 * gone out as `tr::path::not_found` (0x0020), telling the peer its ADDRESS was wrong and
 * inverting the retry disposition it reads off the registry (#876).
 *
 * @warning @p s must be a `status_t` enumerator, so the end of this function is unreachable by
 *          construction: a status is minted from the enumerators alone, and no path casts an
 *          integer — least of all a wire byte — into one.
 */
[[nodiscard]] wire::err_t error_code(status_t s) noexcept {
    switch (s) {
        case status_t::NOT_FOUND:
            return wire::err_t::PATH_NOT_FOUND;
        case status_t::PERMISSION_DENIED:
            return wire::err_t::ACCESS_DENIED;
        case status_t::INVALID_PATH:
            return wire::err_t::PATH_INVALID;
        case status_t::TYPE_MISMATCH:
            return wire::err_t::SCHEMA_TYPE_MISMATCH;
        case status_t::BACKPRESSURE:
            return wire::err_t::FLOW_BACKPRESSURE;
        case status_t::TIMEOUT:
            return wire::err_t::FLOW_TIMEOUT;
        case status_t::SCHEMA_NOT_FOUND:
            return wire::err_t::SCHEMA_NOT_FOUND;
        case status_t::PATH_IN_USE:
            return wire::err_t::PATH_IN_USE;
        case status_t::TRANSPORT_DOWN:
            return wire::err_t::TRANSPORT_DOWN;
    }
    std::unreachable();
}

constexpr std::size_t kU8ValueLen = 5;  // 4-byte VALUE header + 1 payload byte

}  // namespace

rope_t assemble_reply(std::span<const std::byte> reply_dst_wire,
                      std::span<const std::byte> reply_src_wire, reply_kind_t kind,
                      std::span<const std::byte> inline_tail, std::span<const view_t> shared,
                      std::size_t shared_len, mem::mem_backend_t& egress,
                      std::span<const std::byte> trailing) {
    const std::size_t children_len = kU8ValueLen + reply_dst_wire.size() + reply_src_wire.size() +
                                     kU8ValueLen + inline_tail.size();
    const std::size_t body_len = children_len + shared_len + trailing.size();
    const bool ll = body_len > 0xFFFFu;
    const std::size_t head_len = (ll ? 6u : 4u) + children_len;

    rope_t rope;
    // Reserve the reply chain (head + one link per shared payload view) up front so the
    // appends below never spill through a throwing std::vector growth — an abort() under
    // -fno-exceptions on a fragmented heap. On failure return an EMPTY rope; resolve_node's
    // or_backpressure wrapper turns an empty success reply into an addressed BACKPRESSURE
    // error (the client falls back on the same link rather than presuming it dead).
    if (!rope.try_reserve(1 + shared.size() + (trailing.empty() ? 0u : 1u))) return rope_t{};
    // The reply head draws from the injected egress backend (#795), not the global heap: this
    // is the last peer-drivable terminus allocation, sized by the swapped route bytes, and a
    // bounded node bounds it by pointing `egress` at its slab. `segment_alloc` is the nothrow
    // form — a refusal is a null handle, degraded below, never a throw.
    segment_ptr_t seg = view::segment_alloc(egress, head_len);
    // A head-alloc failure invalidates the WHOLE reply: the shared payload views WITHOUT
    // the FWD header are a headerless, unroutable frame that the send site's
    // link_count() > 0 guard would wave through as garbage. Return an EMPTY rope instead
    // (or_backpressure → addressed BACKPRESSURE), never a malformed frame.
    if (!seg) return rope_t{};
    emit_cursor_t out{seg->bytes.data()};
    out.struct_header(type_t::FWD, ll, body_len);
    out.u8_value(std::to_underlying(fwd_op_t::REPLY));
    out.tlv_sliced(reply_dst_wire);
    out.tlv_sliced(reply_src_wire);
    out.u8_value(std::to_underlying(kind));
    out.raw(inline_tail);
    rope.append(view_t::over(std::move(seg)));
    for (const view_t& v : shared) rope.append(v);  // refcount clone — no byte copy
    // The minted `PATH_REF` goes LAST — after the payload, as the reply's final child
    // (RFC-0024 §7.1). Last rather than beside `kind` so a positional reader of an ordinary
    // reply is untouched: it reads op / dst / src / kind / payload and stops, and only an
    // origin that ASKED for a mint reads past. Its own segment, because the head is sized
    // exactly and the payload's links sit between — one small allocation on the mint path
    // and none on any other.
    if (!trailing.empty()) {
        // The minted PATH_REF draws from the SAME egress backend (#795): a fixed 12 B, but on
        // the reply path all the same, so a bounded node bounds it too. A refusal is not an
        // error — the operation already succeeded — so the plain reply is rebuilt below.
        segment_ptr_t mint_seg = view::segment_alloc(egress, trailing.size());
        if (mint_seg) {
            std::memcpy(mint_seg->bytes.data(), trailing.data(), trailing.size());
            rope.append(view_t::over(std::move(mint_seg)));
            return rope;
        }
        // A mint that cannot be allocated is not an error: the operation already succeeded and
        // its reply is complete without it, so answer the plain reply and let the origin stay
        // canonical — the same degrade a saturated generation takes. Rebuilt rather than
        // returned short, because a header whose body_len counts bytes that are not there is
        // unparseable, not merely unhelpful.
        return assemble_reply(reply_dst_wire, reply_src_wire, kind, inline_tail, shared, shared_len,
                              egress);
    }
    return rope;
}

rope_t assemble_error_reply(std::span<const std::byte> reply_dst_wire,
                            std::span<const std::byte> reply_src_wire, status_t status,
                            mem::mem_backend_t& egress) {
    const std::uint16_t code = std::to_underlying(error_code(status));
    const std::array<std::byte, 14> tail{
        static_cast<std::byte>(std::to_underlying(type_t::STATUS)),
        static_cast<std::byte>(opt_t{.pl = true}.encode()),
        std::byte{10},
        std::byte{0},  // STATUS length = one 10-byte ERROR child
        static_cast<std::byte>(std::to_underlying(type_t::ERROR)),
        static_cast<std::byte>(opt_t{.pl = true}.encode()),
        std::byte{6},
        std::byte{0},  // ERROR length = one 6-byte VALUE identity child
        static_cast<std::byte>(std::to_underlying(type_t::VALUE)),
        std::byte{0},
        std::byte{2},
        std::byte{0},  // VALUE length = 2 (the u16 LE registered code)
        static_cast<std::byte>(code & 0xFFu),
        static_cast<std::byte>(code >> 8),
    };
    return assemble_reply(reply_dst_wire, reply_src_wire, reply_kind_t::ERROR, tail, {}, 0, egress);
}

}  // namespace tr::graph
