/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Emit one TLV as raw wire bytes — header (type, opt, little-endian length) plus
 * body — without building a `tlv_t` model object. The structural byte-builders
 * (PATH canonical keys, ROUTER envelopes, :schema POINT descriptors) all share
 * this instead of each hand-rolling the header. For decoding, and for emitting a
 * full `tlv_t` value (payload/children/trailers), use `%frame.hpp`'s encode/decode.
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
#include "libtracer/path_ref.hpp"
#include "libtracer/tlv.hpp"

namespace tr::wire {

/**
 * @brief Append just a TLV header: `<type> <opt> <length>` — the ONE representation of the
 *        header byte layout (ADR-0048 §3).
 *
 * Length is u16 LE, or u32 LE when the `opt.ll` bit is set. The width follows `opt.ll`
 * verbatim; the caller owns the LL decision (so `encode`, which respects a `tlv_t`'s
 * existing `opt.ll`, and `emit_tlv`, which auto-widens for an oversize body, share this
 * without either changing behavior).
 */
inline void emit_header(std::vector<std::byte>& out, type_t type, opt_t opt, std::size_t body_len) {
    out.push_back(static_cast<std::byte>(std::to_underlying(type)));
    out.push_back(static_cast<std::byte>(opt.encode()));
    detail::append_le(out, static_cast<std::uint32_t>(body_len), opt.ll ? 4u : 2u);
}

/**
 * @brief Append one TLV: `<type> <opt> <length> <body>`.
 *
 * Length is u16 LE, widening to u32 LE (the LL bit set) when @p body exceeds 0xFFFF. @p opt
 * carries the structural bits — pass `opt_t{.pl = true}` for a structured (list) payload.
 */
inline void emit_tlv(std::vector<std::byte>& out, type_t type, opt_t opt,
                     std::span<const std::byte> body) {
    if (body.size() > 0xFFFFu) opt.ll = true;
    emit_header(out, type, opt, body.size());
    out.insert(out.end(), body.begin(), body.end());
}

/** @brief Append a NAME TLV over opaque bytes — the PATH-segment / metadata-tag workhorse. */
inline void emit_name(std::vector<std::byte>& out, std::span<const std::byte> name) {
    emit_tlv(out, type_t::NAME, opt_t{}, name);
}

/**
 * @brief Append a `PATH_REF` TLV over @p elements — the bound-path form (RFC-0024 §4).
 *
 * Emits the 4-byte envelope (`0x14`, `opt = 0x00` — `PL = 0` and `LL = 0` are both MUSTs of
 * §4.2) followed by the bare 8-byte element array, in route order, with no per-element
 * framing and no count field: the count IS `length / 8`.
 *
 * @return False — emitting nothing — when @p elements exceeds @ref kMaxPathRefElements, the
 *         §4.3 bound. A caller with more hosts than that has no bound spelling and falls back
 *         to the canonical `PATH`, which is the mint key and the fallback by construction.
 */
[[nodiscard]] inline bool emit_path_ref(std::vector<std::byte>& out,
                                        std::span<const path_ref_element_t> elements) {
    if (elements.size() > kMaxPathRefElements) return false;
    const std::size_t body_len = elements.size() * kPathRefElementBytes;
    emit_header(out, type_t::PATH_REF, opt_t{}, body_len);
    const std::size_t base = out.size();
    out.resize(base + body_len);
    for (std::size_t i = 0; i < elements.size(); ++i) {
        path_ref_store_element(std::span<std::byte>(out).subspan(base + i * kPathRefElementBytes,
                                                                 kPathRefElementBytes),
                               elements[i]);
    }
    return true;
}

/** @brief Append a NAME TLV over a text segment (no temporary buffer). */
inline void emit_name(std::vector<std::byte>& out, std::string_view name) {
    emit_name(out, std::span<const std::byte>(reinterpret_cast<const std::byte*>(name.data()),
                                              name.size()));
}

}  // namespace tr::wire
