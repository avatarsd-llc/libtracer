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
#include "libtracer/tlv_emit.hpp"

namespace tr::wire {
namespace {

// Read `n` little-endian bytes at `off` (a thin span adaptor over detail::load_le).
std::uint64_t read_le(std::span<const std::byte> b, std::size_t off, std::size_t n) noexcept {
    return detail::load_le(b.subspan(off, n));
}

void write_le(std::vector<std::byte>& out, std::uint64_t v, std::size_t n) {
    detail::append_le(out, v, n);
}

// Model one validated header (grammar::parse_header, ADR-0048 §1) as a tlv_t:
// extract the payload span for an opaque node and read the (already-verified)
// trailer values into the owning tree. `bytes` is the TLV's own bytes.
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

// The owning-tree sink for grammar::walk (ADR-0048 §1): builds the `tlv_t` tree
// as the shared descent visits it. Opaque nodes are grafted into their parent
// (or become the root); a structured node is held open on `open_` while its
// children graft in, then grafted itself on close. The descent logic — pos/total
// accounting, depth cap, when to descend — lives in the walk, not here.
struct owning_sink {
    std::vector<tlv_t> open_;  // the open structured nodes (innermost last)
    tlv_t result_;             // set once, when the root node finalizes

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
    // to the default (heap) resource for deeper frames — an owning-tree decode
    // already allocates on the heap, so its RFC-0006 depth bound is the heap.
    owning_sink sink;
    std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;
    grammar::walk_stack_t<grammar::span_cursor> stack(slots, std::pmr::get_default_resource());
    const auto r = grammar::walk(grammar::span_cursor{input}, sink, stack);
    if (!r) return std::unexpected(r.error());
    return std::move(sink.result_);
}

std::vector<std::byte> encode(const tlv_t& tlv) {
    std::vector<std::byte> body;
    if (tlv.opt.pl) {
        for (const tlv_t& child : tlv.children) {
            const std::vector<std::byte> cb = encode(child);
            body.insert(body.end(), cb.begin(), cb.end());
        }
    } else {
        body.assign(tlv.payload.begin(), tlv.payload.end());
    }

    std::vector<std::byte> out;
    // The header byte layout has one home now (ADR-0048 §3) — emit_header respects
    // tlv.opt.ll verbatim, byte-identical to the hand-rolled push it replaces.
    wire::emit_header(out, tlv.type, tlv.opt, body.size());
    out.insert(out.end(), body.begin(), body.end());

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

std::vector<std::byte> path_key(const tlv_t& path) {
    // The canonical PATH-payload key = the concatenated NAME-child encodings. Emit each
    // NAME TLV in place (wire::emit_name appends `02 00 <len> <bytes>` directly) instead
    // of encode()-per-child into a temporary vector — one reserve + N appends, no per-
    // segment allocation. A PATH's children are plain NAMEs (opt 0, no trailer), so this
    // is byte-identical to encode(name); and it matches what path_t/register_vertex store
    // (which also use emit_name), so the vertex-map key round-trips exactly.
    std::vector<std::byte> key;
    std::size_t total = 0;
    for (const tlv_t& name : path.children) total += 4 + name.payload.size();
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
