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
 * verbatim — this writes a header, it does not decide one. The LL DECISION has a single
 * home one level up, in `emit_tlv`, which every emitter goes through: the structural
 * byte-builders directly, and `frame.cpp`'s `encode` for a whole `tlv_t` (#924 — `encode`
 * used to call this directly and truncated an oversize length to `size & 0xFFFF`).
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
 * @brief Bytes a `PATH_REF` over @p n elements occupies — the 4-byte envelope plus the array.
 *
 * The whole size function, so a caller can size a STACK buffer for the shape it is about to
 * emit rather than discovering the size from a container that grew to hold it.
 */
[[nodiscard]] constexpr std::size_t path_ref_wire_bytes(std::size_t n) noexcept {
    return 4u + n * kPathRefElementBytes;
}

/**
 * @brief Write a `PATH_REF` TLV over @p elements into @p out — the ALLOCATION-FREE form.
 *
 * Emits the 4-byte envelope (`0x14`, `opt = 0x00` — `PL = 0` and `LL = 0` are both MUSTs of
 * §4.2) followed by the bare 8-byte element array, in route order, with no per-element
 * framing and no count field: the count IS `length / 8`.
 *
 * This is the form the reply path uses. A `PATH_REF`'s size is a pure function of its element
 * count, so the buffer can be a caller's stack array and the emit cannot fail for want of
 * memory — which matters because the mint rides a reply that has ALREADY succeeded: under
 * `-fno-exceptions` a growing container would abort the node on a fragmented heap instead of
 * degrading to the plain reply the design promises (#748's precedent, same function).
 *
 * @return False — writing nothing — when @p elements exceeds @ref kMaxPathRefElements (the
 *         §4.3 bound; such a caller has no bound spelling and falls back to the canonical
 *         `PATH`), or when @p out is smaller than `path_ref_wire_bytes`.
 */
[[nodiscard]] inline bool emit_path_ref_into(std::span<std::byte> out,
                                             std::span<const path_ref_element_t> elements) {
    if (elements.size() > kMaxPathRefElements) return false;
    const std::size_t body_len = elements.size() * kPathRefElementBytes;
    if (out.size() < path_ref_wire_bytes(elements.size())) return false;
    out[0] = static_cast<std::byte>(std::to_underlying(type_t::PATH_REF));
    out[1] = static_cast<std::byte>(opt_t{}.encode());
    out[2] = static_cast<std::byte>(body_len & 0xFFu);
    out[3] = static_cast<std::byte>((body_len >> 8) & 0xFFu);
    for (std::size_t i = 0; i < elements.size(); ++i) {
        path_ref_store_element(out.subspan(4u + i * kPathRefElementBytes, kPathRefElementBytes),
                               elements[i]);
    }
    return true;
}

/**
 * @brief Append a `PATH_REF` TLV over @p elements to a growing buffer — the container form.
 *
 * Same bytes as `emit_path_ref_into`, which it delegates to, so the two spellings cannot
 * drift. For the reply path use the span form: it does not allocate.
 *
 * @return False — emitting nothing — when @p elements exceeds @ref kMaxPathRefElements, the
 *         §4.3 bound.
 */
[[nodiscard]] inline bool emit_path_ref(std::vector<std::byte>& out,
                                        std::span<const path_ref_element_t> elements) {
    if (elements.size() > kMaxPathRefElements) return false;
    const std::size_t base = out.size();
    out.resize(base + path_ref_wire_bytes(elements.size()));
    return emit_path_ref_into(std::span<std::byte>(out).subspan(base), elements);
}

/** @brief Append a NAME TLV over a text segment (no temporary buffer). */
inline void emit_name(std::vector<std::byte>& out, std::string_view name) {
    emit_name(out, std::span<const std::byte>(reinterpret_cast<const std::byte*>(name.data()),
                                              name.size()));
}

}  // namespace tr::wire
