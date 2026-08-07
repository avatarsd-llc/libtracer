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

    if (h.opt.ts || h.opt.cr) {
        trailer_t trailer;
        if (h.opt.ts) {
            timestamp_t t;
            t.relative = h.opt.tf;
            if (h.opt.tf) {
                t.value = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(read_le(bytes, h.header + h.length, 4)));
            } else {
                t.value = static_cast<std::int64_t>(read_le(bytes, h.header + h.length, 8));
            }
            trailer.ts = t;
        }
        if (h.opt.cr) {
            // CRC already verified in parse_header; read the stored value to model it.
            const std::size_t crc_off = h.header + h.length + h.ts_size;
            crc_t c;
            if (h.opt.cw) {
                c.width = crc_t::width_t::CRC16_CCITT;
                c.value = static_cast<std::uint32_t>(read_le(bytes, crc_off, 2));
            } else {
                c.width = crc_t::width_t::CRC32C;
                c.value = static_cast<std::uint32_t>(read_le(bytes, crc_off, 4));
            }
            trailer.crc = c;
        }
        tlv.trailer = trailer;
    }

    if (!h.opt.pl) tlv.payload = bytes.subspan(h.header, h.length);
    return tlv;
}

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

}  // namespace

std::expected<tlv_t, err_t> decode(std::span<const std::byte> input) {
    // The one structural descent lives in grammar::walk (ADR-0048 §1); this sink
    // only builds the owning tree. The walk stack starts in these inline slots
    // (a tuning knob sized for the typical FWD nesting, ~3-4 levels) and spills
    // to the nothrow heap source for deeper frames — an owning-tree decode
    // already allocates on the heap, so its RFC-0006 depth bound is the heap.
    // (#588: the spill used to be a throwing pmr allocate.)
    owning_sink sink;
    std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;
    grammar::walk_stack_t<grammar::span_cursor> stack(slots, &mem::heap_source());
    const auto r = grammar::walk(grammar::span_cursor{input}, sink, stack);
    if (!r) return std::unexpected(r.error());
    return std::move(sink.result_);
}

std::vector<std::byte> encode(const tlv_t& tlv) {
    // Symmetry with decode (#886). `path_ref_body_valid` is the ONE home of the grammar's only
    // per-type structural rule (RFC-0024 §4.2/§4.3) and `grammar::parse_header` has always
    // consulted it; this door did not, so a caller-built PATH_REF with `opt.pl`, `opt.ll`, or a
    // body that is not a whole number of 8-byte elements serialized to bytes this very library
    // answers with `tr::frame::invalid`. The guarded emitters (`emit_path_ref`) satisfy the rule
    // by construction — they take a typed element array — which left `encode` as the only door
    // that could mint a self-rejected frame. A PATH_REF body is never structured, so `payload`
    // IS the body length here: an `opt.pl` PATH_REF fails the PL clause before the children
    // branch below ever runs. Refusing costs one predicted-not-taken compare per TLV.
    if (tlv.type == type_t::PATH_REF &&
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

    std::vector<std::byte> out;
    // The header byte layout has one home (ADR-0048 §3) and now so does the LENGTH-WIDTH
    // POLICY (#924): emit_tlv widens to the u32 LL form when the body exceeds 0xFFFF, so a
    // tlv_t built programmatically with a default opt (ll = false) over an oversize body can
    // no longer serialize a length silently truncated to `size & 0xFFFF`. A body at or under
    // 0xFFFF — and a tlv_t that already carries opt.ll — emits byte-identical bytes; the widen
    // costs one predicted-not-taken compare per TLV and allocates nothing.
    wire::emit_tlv(out, tlv.type, tlv.opt, body);

    std::vector<std::byte> ts_bytes;
    if (tlv.opt.ts) {
        const timestamp_t t = (tlv.trailer && tlv.trailer->ts)
                                  ? *tlv.trailer->ts
                                  : timestamp_t{.relative = tlv.opt.tf, .value = 0};
        if (tlv.opt.tf) {
            write_le(ts_bytes, static_cast<std::uint32_t>(static_cast<std::int32_t>(t.value)), 4);
        } else {
            write_le(ts_bytes, static_cast<std::uint64_t>(t.value), 8);
        }
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
    // The canonical PATH-payload key = the concatenated NAME-child encodings. Emit each
    // NAME TLV in place (wire::emit_name appends `02 00 <len> <bytes>` directly) instead
    // of encode()-per-child into a temporary vector — one reserve + N appends, no per-
    // segment allocation. A PATH's children are plain NAMEs (opt 0, no trailer), so this
    // is byte-identical to encode(name); and it matches what path_t/register_vertex store
    // (which also use emit_name), so the vertex-map key round-trips exactly.
    //
    // The child-type check is the point of the two-pass shape: the loop below used to trust
    // "a PATH's children are plain NAMEs", which is true of paths THIS node emits and not of
    // paths a PEER sends. Rejecting BEFORE the first append means a malformed route never
    // produces a partial key either (#681). `path_lookup_key` in the arena tier has carried
    // the same check since #436; this locus was missed.
    std::size_t total = 0;
    for (const tlv_t& name : path.children) {
        if (name.type != type_t::NAME) return std::nullopt;
        total += 4 + name.payload.size();
    }
    std::vector<std::byte> key;
    key.reserve(total);
    for (const tlv_t& name : path.children) wire::emit_name(key, name.payload);
    return key;
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
