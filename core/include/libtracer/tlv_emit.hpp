/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Emit one TLV as raw wire bytes — header (type, opt, little-endian length) plus
 * body — without building a `tlv_t` model object. The structural byte-builders
 * (PATH canonical keys, ROUTER envelopes, :schema POINT descriptors) all share
 * this instead of each hand-rolling the header. For decoding, and for emitting a
 * full `tlv_t` value (payload/children/trailers), use frame.hpp's encode/decode.
 *
 * Lives in `tr::wire` (L2/L3): it produces wire bytes from wire types (`type_t`,
 * `opt_t`), so it is a codec concern, not a layer-free `tr::detail` primitive —
 * the low-level LE byte helper it builds on (`detail::append_le`, byteorder.hpp)
 * stays in `tr::detail`.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv.hpp"

namespace tr::wire {

// Append one TLV: <type> <opt> <length> <body>, where length is u16 LE, widening
// to u32 LE (with the LL bit set) when the body exceeds 0xFFFF. `opt` carries the
// structural bits — pass `opt_t{.pl = true}` for a structured (list) payload.
inline void emit_tlv(std::vector<std::byte>& out, type_t type, opt_t opt,
                     std::span<const std::byte> body) {
    if (body.size() > 0xFFFFu) opt.ll = true;
    out.push_back(static_cast<std::byte>(std::to_underlying(type)));
    out.push_back(static_cast<std::byte>(opt.encode()));
    detail::append_le(out, static_cast<std::uint32_t>(body.size()), opt.ll ? 4u : 2u);
    out.insert(out.end(), body.begin(), body.end());
}

// Append a NAME TLV over opaque bytes — the PATH-segment / metadata-tag workhorse.
inline void emit_name(std::vector<std::byte>& out, std::span<const std::byte> name) {
    emit_tlv(out, type_t::NAME, opt_t{}, name);
}

// Append a NAME TLV over a text segment (no temporary buffer).
inline void emit_name(std::vector<std::byte>& out, std::string_view name) {
    emit_name(out, std::span<const std::byte>(reinterpret_cast<const std::byte*>(name.data()),
                                              name.size()));
}

}  // namespace tr::wire
