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

#include <concepts>
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

/** @brief Wire bytes a TLV header occupies under @p opt: 4, or 6 when the `ll` bit widens the
 *         length to u32. */
[[nodiscard]] constexpr std::size_t header_bytes(opt_t opt) noexcept { return opt.ll ? 6u : 4u; }

/**
 * @brief Store a TLV header into an exactly-sized @p out span — the ONE representation of the
 *        header byte layout (ADR-0048 §3), in the form a caller composing into an OWNED SEGMENT
 *        needs.
 *
 * The span form is the primitive and `emit_header` is the container form over it, so a
 * rope-native composer (`%batch.hpp`'s `compose_batch`) and a `std::vector` byte
 * builder cannot drift into two spellings of the same four bytes. Length is u16 LE, or u32 LE
 * when `opt.ll` is set; the width follows `opt.ll` verbatim — this writes a header, it does not
 * decide one.
 *
 * Precondition: `out.size() >= header_bytes(opt)`.
 */
inline void store_header(std::span<std::byte> out, type_t type, opt_t opt,
                         std::size_t body_len) noexcept {
    out[0] = static_cast<std::byte>(std::to_underlying(type));
    out[1] = static_cast<std::byte>(opt.encode());
    detail::store_le(out.subspan(2), static_cast<std::uint32_t>(body_len), opt.ll ? 4u : 2u);
}

/**
 * @brief Append just a TLV header: `<type> <opt> <length>` — the container form of
 *        `store_header`, which is the ONE representation of the header byte layout
 *        (ADR-0048 §3).
 *
 * Length is u16 LE, or u32 LE when the `opt.ll` bit is set. The width follows `opt.ll`
 * verbatim — this writes a header, it does not decide one. The LL decision belongs one level
 * up, in `emit_tlv`, which widens for an oversize body; `frame.cpp`'s `encode` routes through
 * it (#924 — `encode` used to call this directly and truncated an oversize length to
 * `size & 0xFFFF`).
 *
 * `emit_tlv` is NOT the only home of that rule, and a caller must not assume it is: the
 * subscribe sugar in `graph.cpp` calls this directly (safe only because `kMaxPathBytes`
 * bounds the length), `emit_path_ref_into` hand-rolls its own header, and the forward plane's
 * `fwd_frame_view` / `stack_writer` tiers each carry a separate copy of the widen rule. A
 * caller reaching for `emit_header` owns the width decision itself.
 */
inline void emit_header(std::vector<std::byte>& out, type_t type, opt_t opt, std::size_t body_len) {
    const std::size_t at = out.size();
    out.resize(at + header_bytes(opt));
    store_header(std::span<std::byte>(out).subspan(at), type, opt, body_len);
}

/**
 * @brief Store trailer-timestamp bytes into an exactly-sized @p out span — the ONE
 *        representation of the trailer-TS byte layout, covering BOTH forms (#1109).
 *
 * TF=0 (`relative == false`): u64 LE nanoseconds since the Unix epoch, 8 bytes. TF=1
 * (`relative == true`): signed i32 LE nanosecond offset from the parent's stamp, 4 bytes
 * (docs/reference/01-data-format.md §timestamp form). Both forms live here so the value
 * codec (`frame.cpp`), the reply echo and a future stream shape (#879) emit identical bytes.
 *
 * Precondition: `out.size() == trailer_ts_bytes(relative)`.
 */
inline void store_trailer_ts(std::span<std::byte> out, bool relative, std::int64_t ns) noexcept {
    if (relative) {
        detail::store_le(out, static_cast<std::uint32_t>(static_cast<std::int32_t>(ns)), 4u);
    } else {
        detail::store_le(out, static_cast<std::uint64_t>(ns), 8u);
    }
}

/** @brief Trailer-timestamp width on the wire: 4 bytes relative (TF=1), 8 absolute (TF=0). */
[[nodiscard]] constexpr std::size_t trailer_ts_bytes(bool relative) noexcept {
    return relative ? 4u : 8u;
}

/** @brief Append trailer-timestamp bytes to a growing buffer — the container form of
 *         `store_trailer_ts` (same bytes by delegation, so the two cannot drift). */
inline void emit_trailer_ts(std::vector<std::byte>& out, bool relative, std::int64_t ns) {
    const std::size_t base = out.size();
    out.resize(base + trailer_ts_bytes(relative));
    store_trailer_ts(std::span<std::byte>(out).subspan(base), relative, ns);
}

/**
 * @brief Append one TLV: `<type> <opt> <length> <body>`.
 *
 * Length is u16 LE, widening to u32 LE (the LL bit set) when @p body exceeds 0xFFFF. @p opt
 * carries the structural bits — pass `opt_t{.pl = true}` for a structured (list) payload.
 *
 * The TRAILER bits of @p opt are cleared by construction (#1109): this emitter writes header
 * + body and nothing after, so an `opt.ts`/`opt.cr` passed here used to mint a frame that
 * CLAIMED a trailer it did not carry — which a receiver reads as truncation (the grammar
 * counts the trailer into `total`) or, worse, eats the last body bytes as a bogus stamp. A
 * caller with a trailer to write uses `frame.hpp`'s `encode` (values) or emits the header via
 * `emit_header` and appends `emit_trailer_ts` / CRC bytes itself (byte builders).
 */
inline void emit_tlv(std::vector<std::byte>& out, type_t type, opt_t opt,
                     std::span<const std::byte> body) {
    opt = opt.without_trailer();
    if (body.size() > 0xFFFFu) opt.ll = true;
    emit_header(out, type, opt, body.size());
    out.insert(out.end(), body.begin(), body.end());
}

/** @brief Append a NAME TLV over opaque bytes — the PATH-segment / metadata-tag workhorse. */
inline void emit_name(std::vector<std::byte>& out, std::span<const std::byte> name) {
    emit_tlv(out, type_t::NAME, opt_t{}, name);
}

/**
 * @brief Append a VALUE TLV whose body is @p value, little-endian, in @p width bytes — the
 *        PUBLIC spelling of "an integer as a VALUE".
 *
 * The u8 / u16 / u32 integer VALUE is the other half of every `(NAME key, value)` config
 * pair (`config_reader_t::u8`/`u16`/`u32` decodes it), and until #902 there was no public
 * way to write one: a consumer building a connection SPEC had to size its own buffer and
 * call `detail::store_le` — an internal primitive — or hand-roll a shift loop. This is the
 * encode counterpart of those accessors, in `tr::wire` because it produces wire bytes from a
 * wire type; the layer-free LE byte helper it builds on (`detail::append_le`,
 * byteorder.hpp) stays in `tr::detail`.
 *
 * The width is explicit and defaults to `sizeof(T)`, so the emitted payload length is the
 * caller's decision — the reader is tolerant of a narrower payload (`detail::load_le` zero-
 * fills), but the SPEC keys documented as u16/u32 are pinned by their emitted width here.
 *
 * Precondition: `width <= sizeof(T)`.
 */
template <std::unsigned_integral T>
inline void emit_value_le(std::vector<std::byte>& out, T value, std::size_t width = sizeof(T)) {
    emit_header(out, type_t::VALUE, opt_t{}, width);
    detail::append_le(out, value, width);
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
 * @param out      Destination buffer, at least `path_ref_wire_bytes(elements.size())` bytes.
 * @param elements The route, in route order.
 * @param type     Which bound-path code to head it with — `type_t::PATH_REF` (the default,
 *                 an address) or `type_t::PATH_REF_REVERSE` (the reverse list a
 *                 mint-flagged request accumulates, RFC-0024 §7.1 amendment 2). The body
 *                 grammar is identical; only the role differs, which is the whole reason the
 *                 role is spelled in the type byte rather than in a position.
 *
 * @return False — writing nothing — when @p elements exceeds @ref kMaxPathRefElements (the
 *         §4.3 bound; such a caller has no bound spelling and falls back to the canonical
 *         `PATH`), when @p out is smaller than `path_ref_wire_bytes`, or when @p type is
 *         neither bound-path code.
 */
[[nodiscard]] inline bool emit_path_ref_into(std::span<std::byte> out,
                                             std::span<const path_ref_element_t> elements,
                                             type_t type = type_t::PATH_REF) {
    if (elements.size() > kMaxPathRefElements) return false;
    if (!is_path_ref_type(type)) return false;
    const std::size_t body_len = elements.size() * kPathRefElementBytes;
    if (out.size() < path_ref_wire_bytes(elements.size())) return false;
    out[0] = static_cast<std::byte>(std::to_underlying(type));
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
 * @return False — emitting nothing — when @p elements exceeds @ref kMaxPathRefElements (the
 *         §4.3 bound) or @p type is neither bound-path code.
 */
[[nodiscard]] inline bool emit_path_ref(std::vector<std::byte>& out,
                                        std::span<const path_ref_element_t> elements,
                                        type_t type = type_t::PATH_REF) {
    if (elements.size() > kMaxPathRefElements) return false;
    if (!is_path_ref_type(type)) return false;
    const std::size_t base = out.size();
    out.resize(base + path_ref_wire_bytes(elements.size()));
    return emit_path_ref_into(std::span<std::byte>(out).subspan(base), elements, type);
}

/** @brief Append a NAME TLV over a text segment (no temporary buffer). */
inline void emit_name(std::vector<std::byte>& out, std::string_view name) {
    emit_name(out, std::span<const std::byte>(reinterpret_cast<const std::byte*>(name.data()),
                                              name.size()));
}

}  // namespace tr::wire
