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

void write_le(std::vector<std::byte>& out, std::uint64_t v, std::size_t n) {
    detail::append_le(out, v, n);
}

/**
 * @brief Read a validated TLV's trailer values (timestamp, CRC) out of its bytes: the ONE reader
 *        both the owning tree (`model`) and the in-place node (`tlv_node_t::trailer`) use.
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
        // CRC already verified by the grammar; the stored value is read only to model it.
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
 * @brief Model one validated header (grammar::parse_header, ADR-0048 §1) as a tlv_t: extract the
 *        payload span for an opaque node and read the (already-verified) trailer values into the
 *        owning tree.
 *
 * `bytes` is the TLV's own bytes.
 */
tlv_t model(const grammar::header_t& h, std::span<const std::byte> bytes) {
    tlv_t tlv;
    tlv.type = h.type;
    tlv.opt = h.opt;
    tlv.trailer = read_trailer(h.opt, bytes, h.header + h.length);
    if (!h.opt.pl) tlv.payload = bytes.subspan(h.header, h.length);
    return tlv;
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

/**
 * @brief The owning-tree sink for grammar::walk (ADR-0048 §1): builds the `tlv_t` tree as the
 *        shared descent visits it.
 *
 * Opaque nodes are grafted into their parent
 * (or become the root); a structured node is held open on `open_` while its
 * children graft in, then grafted itself on close. The descent logic — pos/total
 * accounting, depth cap, when to descend — lives in the walk, not here.
 */
struct owning_sink {
    std::vector<tlv_t> open_; /**< the open structured nodes (innermost last) */
    tlv_t result_;            /**< set once, when the root node finalizes */

    void place(tlv_t node) {
        if (open_.empty())
            result_ = std::move(node);  // the root
        else
            open_.back().children.push_back(std::move(node));
    }
    void on_leaf(const grammar::header_t& h, const grammar::span_cursor& node) {
        place(model(h, node.buf));
    }
    void on_open(const grammar::header_t& h, const grammar::span_cursor& node) {
        open_.push_back(model(h, node.buf));
    }
    void on_close() {
        tlv_t done = std::move(open_.back());
        open_.pop_back();
        place(std::move(done));
    }
};

/**
 * @brief The one walk both span decoders run: `decode` (owning tree) and `tlv_node_t::over`
 *        (nothing built) differ only in @p sink, so their acceptance cannot drift.
 *
 * The one structural descent lives in grammar::walk (ADR-0048 §1). The walk stack starts in
 * these inline slots (a tuning knob sized for the typical FWD nesting, ~3-4 levels) and spills
 * to `spill` for deeper frames — the INJECTED source since #873, defaulted to the process heap
 * at both public doors. The RFC-0006 depth bound is therefore the caller's to set:
 * `mem::null_source()` refuses the first spill and the walk answers TLV_NESTING_TOO_DEEP.
 * (#588: the spill used to be a throwing pmr allocate.)
 */
template <class Sink>
std::expected<void, err_t> walk_span(std::span<const std::byte> input, Sink& sink,
                                     mem::block_source_t& spill) {
    std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;
    grammar::walk_stack_t<grammar::span_cursor> stack(slots, &spill);
    return grammar::walk(grammar::span_cursor{input}, sink, stack);
}

}  // namespace

std::expected<tlv_t, err_t> decode(std::span<const std::byte> input, mem::block_source_t& spill) {
    // Only the walk STACK moves onto the seam. The owning tlv_t tree this sink builds holds
    // std::vector children, which allocate on the global heap by construction — a caller
    // that only reads wants `tlv_node_t::over`, and one that needs a bounded tree wants
    // decode_into's arena.
    owning_sink sink;
    const auto r = walk_span(input, sink, spill);
    if (!r) return std::unexpected(r.error());
    return std::move(sink.result_);
}

std::expected<tlv_node_t, err_t> tlv_node_t::over(std::span<const std::byte> input,
                                                  mem::block_source_t& spill) {
    // `decode`'s walk with a sink that keeps only the root header: the conformance runner
    // holds the two equal over the whole vector corpus.
    validate_sink sink;
    const auto r = walk_span(input, sink, spill);
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
    // Symmetry with decode (#886). `path_ref_body_valid` is the ONE home of the grammar's only
    // per-type structural rule (RFC-0024 §4.2/§4.3) and `grammar::parse_header` has always
    // consulted it; this door did not, so a caller-built PATH_REF with `opt.pl`, `opt.ll`, or a
    // body that is not a whole number of 8-byte elements serialized to bytes this very library
    // answers with `tr::frame::invalid`. The guarded emitters (`emit_path_ref`) satisfy the rule
    // by construction — they take a typed element array — which left `encode` as the door a
    // CALLER-BUILT tlv_t reaches. It is not the last unguarded write of the 0x14 type byte:
    // `wire::emit_tlv` is public and generic, so `emit_tlv(out, type_t::PATH_REF, opt_t{.pl=true},
    // body)` still mints a self-rejected frame. No in-tree caller does, and `emit_header`'s own
    // doc makes shape the caller's problem, so that is a documented raw seam rather than a hole
    // — but it is a seam, not an absence. A PATH_REF body is never structured, so `payload`
    // IS the body length here: an `opt.pl` PATH_REF fails the PL clause before the children
    // branch below ever runs. Refusing costs one predicted-not-taken compare per TLV. The gate
    // is `is_path_ref_type` rather than one code: the reverse list (0x15) carries the identical
    // body grammar (RFC-0024 §7.1 amendment 2), so it is refused by the identical rule.
    if (is_path_ref_type(tlv.type) &&
        !path_ref_body_valid(tlv.opt.pl, tlv.opt.ll, tlv.payload.size())) {
        return {};
    }

    std::vector<std::byte> body;
    if (tlv.opt.pl) {
        for (const tlv_t& child : tlv.children) {
            const std::vector<std::byte> cb = encode(child);
            // A refused child refuses the parent. Dropping it instead would emit a frame that
            // DOES decode, one component short — a silent truncation, worse than emitting
            // nothing. An accepted TLV is never empty (`emit_tlv` always writes its 4-byte
            // header), so an empty result is an unambiguous refusal, never a legal encoding.
            if (cb.empty()) return {};
            body.insert(body.end(), cb.begin(), cb.end());
        }
    } else {
        body.assign(tlv.payload.begin(), tlv.payload.end());
    }

    // The trailer timestamp is LOUD (#1109): `opt.ts` with no trailer value used to emit a
    // silently-ZERO stamp — a frame that decodes, sorts and plots as 1970-01-01, which is
    // strictly worse than no frame. A missing value, and equally a trailer value whose
    // `relative` flag contradicts `opt.tf` (the bytes would be read in the wrong width),
    // now refuse the encode through the same unambiguous empty-vector channel the PATH_REF
    // rule uses. `stamp_ts` (frame.hpp) sets bit and value together and cannot land here.
    if (tlv.opt.ts &&
        (!tlv.trailer || !tlv.trailer->ts || tlv.trailer->ts->relative != tlv.opt.tf)) {
        return {};
    }

    std::vector<std::byte> out;
    // The header byte layout has one home (ADR-0048 §3) and now so does the LENGTH-WIDTH
    // POLICY (#924): widen to the u32 LL form when the body exceeds 0xFFFF, so a tlv_t built
    // programmatically with a default opt (ll = false) over an oversize body can no longer
    // serialize a length silently truncated to `size & 0xFFFF`. A body at or under 0xFFFF —
    // and a tlv_t that already carries opt.ll — emits byte-identical bytes; the widen costs
    // one predicted-not-taken compare per TLV and allocates nothing. Emitted via emit_header
    // rather than emit_tlv since #1109: emit_tlv now CLEARS trailer bits by construction
    // (it writes nothing after the body), while this encoder appends the trailer itself.
    opt_t opt = tlv.opt;
    if (body.size() > 0xFFFFu) opt.ll = true;
    wire::emit_header(out, tlv.type, opt, body.size());
    out.insert(out.end(), body.begin(), body.end());

    std::vector<std::byte> ts_bytes;
    if (tlv.opt.ts) {
        // Value presence + form coherence were checked above; the byte layout has ONE home
        // (wire::emit_trailer_ts, both forms — #1109's builder plumbing).
        wire::emit_trailer_ts(ts_bytes, tlv.opt.tf, tlv.trailer->ts->value);
        out.insert(out.end(), ts_bytes.begin(), ts_bytes.end());
    }
    if (tlv.opt.cr) {
        // CRC over body ++ ts_bytes via the two-span overloads — no `covered`
        // concatenation buffer (byte-identical: CRC is associative over the feed).
        if (tlv.opt.cw) {
            write_le(out, crc::crc16_ccitt(body, ts_bytes), 2);
        } else {
            write_le(out, crc::crc32c(body, ts_bytes), 4);
        }
    }
    return out;
}

std::optional<std::vector<std::byte>> path_key(const tlv_t& path) {
    // The canonical PATH-payload key IS the PATH body (RFC-0018): a packed sequence of
    // `[u8 len][bytes]` records with `opt.PL = 0`, so a decoded PATH carries it in
    // `payload` and there is nothing to re-assemble from children. One copy, no
    // per-segment append, and byte-identical to what `path_t::parse` / `register_vertex`
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
    if (path.opt.pl || !path.children.empty()) return std::nullopt;
    if (!wire::packed_path_valid_key(path.payload)) return std::nullopt;
    return std::vector<std::byte>(path.payload.begin(), path.payload.end());
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
