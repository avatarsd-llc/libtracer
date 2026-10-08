/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/frame.hpp"

#include <algorithm>
#include <array>
#include <memory_resource>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/crc.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::wire {
namespace {

/** @brief Read `n` little-endian bytes at `off` (a thin span adaptor over detail::load_le). */
std::uint64_t read_le(std::span<const std::byte> b, std::size_t off, std::size_t n) noexcept {
    return detail::load_le(b.subspan(off, n));
}

/**
 * @brief Read a validated TLV's trailer values (timestamp, CRC) out of its bytes: the one
 *        reader, behind `tlv_node_t::trailer`.
 *
 * @param opt        The TLV's opt bits (which trailer fields exist, and their widths).
 * @param bytes      The TLV's own bytes.
 * @param trailer_at Offset of the first trailer byte: header + body length.
 */
std::optional<trailer_t> read_trailer(opt_t opt, std::span<const std::byte> bytes,
                                      std::size_t trailer_at) noexcept {
    if (!opt.ts && !opt.cr) return std::nullopt;
    trailer_t trailer;
    std::size_t at = trailer_at;
    if (opt.ts) {
        timestamp_t t;
        t.relative = opt.tf;
        if (opt.tf) {
            t.value = static_cast<std::int32_t>(static_cast<std::uint32_t>(read_le(bytes, at, 4)));
            at += 4;
        } else {
            t.value = static_cast<std::int64_t>(read_le(bytes, at, 8));
            at += 8;
        }
        trailer.ts = t;
    }
    if (opt.cr) {
        // CRC already verified by the grammar; the stored value is read only to report it.
        crc_t c;
        if (opt.cw) {
            c.width = crc_t::width_t::CRC16_CCITT;
            c.value = static_cast<std::uint32_t>(read_le(bytes, at, 2));
        } else {
            c.width = crc_t::width_t::CRC32C;
            c.value = static_cast<std::uint32_t>(read_le(bytes, at, 4));
        }
        trailer.crc = c;
    }
    return trailer;
}

/**
 * @brief The validate-only sink for grammar::walk: `tlv_node_t::over` builds nothing, so the
 *        walk's own inline stack is the whole cost of validating a frame (#1648). It keeps the
 *        root's header, which the walk always visits first.
 */
struct validate_sink {
    grammar::header_t root_{}; /**< @brief The root header, once visited. */
    bool seen_ = false;        /**< @brief Set by the first (root) visit. */

    void keep(const grammar::header_t& h) noexcept {
        if (seen_) return;
        root_ = h;
        seen_ = true;
    }
    void on_leaf(const grammar::header_t& h, const grammar::span_cursor&) noexcept { keep(h); }
    void on_open(const grammar::header_t& h, const grammar::span_cursor&) noexcept { keep(h); }
    void on_close() noexcept {}
};

}  // namespace

std::expected<tlv_node_t, err_t> tlv_node_t::over(std::span<const std::byte> input,
                                                  mem::block_source_t& spill) {
    // The grammar's walk with a sink that keeps only the root header; the conformance runner
    // reads the whole vector corpus through this door.
    //
    // The one structural descent lives in grammar::walk (ADR-0048 §1). The walk stack starts in
    // these inline slots (a tuning knob sized for the typical FWD nesting, ~3-4 levels) and
    // spills to `spill` for deeper frames — the INJECTED source since #873, defaulted to the
    // process heap. The RFC-0006 depth bound is therefore the caller's to set:
    // `mem::null_source()` refuses the first spill and the walk answers TLV_NESTING_TOO_DEEP.
    validate_sink sink;
    std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;
    grammar::walk_stack_t<grammar::span_cursor> stack(slots, &spill);
    const auto r = grammar::walk(grammar::span_cursor{input}, sink, stack);
    if (!r) return std::unexpected(r.error());
    return tlv_node_t(sink.root_, input);
}

void tlv_children_t::iterator::load() noexcept {
    if (rest_.empty()) return;
    const auto h = grammar::parse_header(grammar::span_cursor{rest_}, grammar::crc_check_t::DEFER);
    // Unreachable for a region `tlv_node_t::over` accepted; ending the walk keeps a broken
    // invariant from reading past the region.
    if (!h) {
        rest_ = {};
        return;
    }
    front_ = tlv_node_t(*h, rest_);
}

std::optional<trailer_t> tlv_node_t::trailer() const noexcept {
    return read_trailer(opt_, bytes_, header_ + length_);
}

std::vector<std::byte> encode(const tlv_t& tlv) {
    // The vector door is the core-array door plus one copy (#1781): ONE encoder, so the two
    // cannot disagree on a refusal, and an empty vector is the refusal, as it always was.
    mem::bytes_t out(mem::heap_source());
    if (!encode(tlv, out)) return {};
    return std::vector<std::byte>(out.begin(), out.end());
}

std::optional<std::span<const std::byte>> path_key(const tlv_node_t& path) {
    // The canonical PATH-payload key IS the PATH body (RFC-0018): a packed sequence of
    // `[u8 len][bytes]` records with `opt.PL = 0`, so a PATH node carries it in
    // `payload()` and there is nothing to re-assemble from children. No copy at all (#1885),
    // and byte-identical to what `path_t::parse` / `register_vertex`
    // store — the vertex-map key round-trips exactly.
    //
    // What this VALIDATES is the whole reason it is still fallible. The pre-RFC-0018 shape
    // was two passes over the children so a non-`NAME` child could be refused before the
    // first append (#681 / #436) — the bug where a peer's `PATH{VALUE "sensor"}` bound a
    // label to `/sensor`. A packed body cannot mistype a child, because it has none; what
    // it CAN carry is a ragged length or the `len == 0` escape, and this is **canonical /
    // key context** (this function's callers are the ADVERTISE route resolve and the
    // SUBSCRIBER target), where RFC-0018 §5.4 rejects the escape. Refusing here means a
    // malformed route still produces no key at all rather than a partial one.
    if (path.opt().pl) return std::nullopt;
    const std::span<const std::byte> body = path.payload();
    if (!wire::packed_path_valid_key(body)) return std::nullopt;
    return body;
}

namespace {

/** @brief Wire bytes a CRC trailer takes under @p opt: 0, 2 (CRC-16) or 4 (CRC-32C). */
[[nodiscard]] std::size_t crc_bytes(opt_t opt) noexcept {
    if (!opt.cr) return 0;
    return opt.cw ? 2u : 4u;
}

[[nodiscard]] std::optional<std::size_t> tlv_bytes(const tlv_t& tlv) noexcept;

/**
 * @brief @p tlv's body length, or `nullopt` for every refusal `encode` makes: an ill-formed
 *        `PATH_REF`, a timestamp bit with no coherent value, or a refused child.
 *
 * - `PATH_REF` (and the reverse list, 0x15) is symmetric with the reader (#886):
 *   `path_ref_body_valid` is the one home of the grammar's only per-type structural rule
 *   (RFC-0024 §4.2/§4.3), so a caller-built `tlv_t` with `opt.pl`, `opt.ll` or a body that is
 *   not whole 8-byte elements never serializes to bytes this library would answer with
 *   `tr::frame::invalid`. `wire::emit_tlv` stays a documented raw seam that can still mint one.
 * - The trailer timestamp is LOUD (#1109): `opt.ts` with no value, or with a `relative` flag
 *   that contradicts `opt.tf`, refuses rather than emitting a silently-zero 1970 stamp.
 * - A refused child refuses the parent: dropping it would emit a frame that decodes one
 *   component short, a silent truncation worse than emitting nothing.
 */
[[nodiscard]] std::optional<std::size_t> body_bytes(const tlv_t& tlv) noexcept {
    if (is_path_ref_type(tlv.type) &&
        !path_ref_body_valid(tlv.opt.pl, tlv.opt.ll, tlv.payload.size()))
        return std::nullopt;
    if (tlv.opt.ts && (!tlv.trailer || !tlv.trailer->ts || tlv.trailer->ts->relative != tlv.opt.tf))
        return std::nullopt;
    if (!tlv.opt.pl) return tlv.payload.size();
    std::size_t n = 0;
    for (const tlv_t& child : tlv.children) {
        const std::optional<std::size_t> c = tlv_bytes(child);
        if (!c) return std::nullopt;
        n += *c;
    }
    return n;
}

/** @brief @p tlv's whole encoded length — header, body and trailer — or `nullopt` if refused. */
[[nodiscard]] std::optional<std::size_t> tlv_bytes(const tlv_t& tlv) noexcept {
    const std::optional<std::size_t> body = body_bytes(tlv);
    if (!body) return std::nullopt;
    opt_t opt = tlv.opt;
    if (*body > 0xFFFFu) opt.ll = true;
    const std::size_t ts = tlv.opt.ts ? trailer_ts_bytes(tlv.opt.tf) : 0u;
    return header_bytes(opt) + *body + ts + crc_bytes(tlv.opt);
}

/**
 * @brief Append @p tlv (already sized and checked by @ref tlv_bytes) to @p out, whose capacity
 *        was reserved for it — so no append below can move the block the CRC reads.
 */
void put_tlv(const tlv_t& tlv, mem::bytes_t& out) noexcept {
    const std::size_t body = *body_bytes(tlv);
    opt_t opt = tlv.opt;
    if (body > 0xFFFFu) opt.ll = true;
    (void)emit_header(out, tlv.type, opt, body);
    const std::size_t at = out.size();
    if (tlv.opt.pl) {
        for (const tlv_t& child : tlv.children) put_tlv(child, out);
    } else {
        (void)out.append(tlv.payload.data(), tlv.payload.size());
    }
    if (tlv.opt.ts) (void)emit_trailer_ts(out, tlv.opt.tf, tlv.trailer->ts->value);
    if (!tlv.opt.cr) return;
    const std::span<const std::byte> covered(out.data() + at, out.size() - at);
    const std::uint32_t crc = tlv.opt.cw ? crc::crc16_ccitt(covered) : crc::crc32c(covered);
    (void)detail::append_le(out, crc, crc_bytes(tlv.opt));
}

}  // namespace

bool encode(const tlv_t& tlv, mem::bytes_t& out) noexcept {
    const std::optional<std::size_t> n = tlv_bytes(tlv);
    if (!n || !out.reserve(out.size() + *n)) return false;
    put_tlv(tlv, out);
    return true;
}

bool equal(const tlv_t& a, const tlv_t& b) noexcept {
    if (a.type != b.type || a.opt != b.opt || a.trailer != b.trailer) return false;
    if (!std::ranges::equal(a.payload, b.payload)) return false;
    if (a.children.size() != b.children.size()) return false;
    for (std::size_t i = 0; i < a.children.size(); ++i) {
        if (!equal(a.children[i], b.children[i])) return false;
    }
    return true;
}

}  // namespace tr::wire
