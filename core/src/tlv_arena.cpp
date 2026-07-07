/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/tlv_arena.hpp"

#include "libtracer/grammar.hpp"

namespace tr::wire {
namespace {

// A bare canonical NAME: type 0x02 with opt byte 0x00 — exactly the
// `02 00 <u16 len>` header path_key emits, so a PATH made only of these has a
// body byte-identical to its canonical vertex-map key (ADR-0041 §3).
bool is_canonical_name(const grammar::header_t& h) noexcept {
    return h.type == type_t::NAME && h.opt == opt_t{};
}

// The terminus-arena sink for grammar::walk (ADR-0048 §1): appends pre-order
// arena nodes as the shared descent visits them. Each node's `wire` is
// header+body (trailer excluded, so a whole-TLV copy is trailer-less at rest);
// a structured node's subtree extent (`end`) and its ADR-0041 §3 canonical-PATH
// flag are sealed on close. The descent logic (pos/total, depth cap, when to
// descend) lives in the walk; this is the pre-order twin of frame.cpp's
// owning_sink. Its own open-node stack draws from the arena resource so a
// slab-bound decode stays heap-free.
struct arena_sink {
    std::pmr::vector<arena_tlv_t>& nodes_;
    struct open_t {
        std::uint32_t index = 0;
        bool names_only = true;  // ADR-0041 §3 canonical-PATH property over direct children
    };
    std::pmr::vector<open_t> open_;

    arena_sink(std::pmr::vector<arena_tlv_t>& nodes, std::pmr::memory_resource& mr)
        : nodes_(nodes), open_(&mr) {
        open_.reserve(8);  // typical FWD nesting; deeper frames grow (bounded by the cap)
    }

    void push(const grammar::header_t& h, std::span<const std::byte> bytes) {
        nodes_.push_back(arena_tlv_t{
            .type = h.type,
            .opt = h.opt,
            .wire = bytes.first(h.header + h.length),
            .body = bytes.subspan(h.header, h.length),
            .end = static_cast<std::uint32_t>(nodes_.size() + 1),  // opaque default
            .canonical_path = false,
        });
    }
    // A node that is a direct child of the currently-open parent (if any) breaks
    // the parent's canonical-PATH property unless it is a bare NAME.
    void note_child(const grammar::header_t& h) {
        if (!open_.empty() && !is_canonical_name(h)) open_.back().names_only = false;
    }
    void on_leaf(const grammar::header_t& h, const grammar::span_cursor& node) {
        note_child(h);
        push(h, node.buf);
    }
    void on_open(const grammar::header_t& h, const grammar::span_cursor& node) {
        note_child(h);
        const auto index = static_cast<std::uint32_t>(nodes_.size());
        push(h, node.buf);
        open_.push_back(open_t{.index = index, .names_only = true});
    }
    void on_close() {
        arena_tlv_t& node = nodes_[open_.back().index];
        node.end = static_cast<std::uint32_t>(nodes_.size());
        node.canonical_path = node.type == type_t::PATH && open_.back().names_only;
        open_.pop_back();
    }
};

}  // namespace

std::expected<tlv_arena_t, err_t> decode_into(std::span<const std::byte> input,
                                              std::pmr::memory_resource& mr) {
    // The one structural descent lives in grammar::walk (ADR-0048 §1); this sink
    // appends the pre-order arena nodes. The walk's cursor stack + the sink's
    // open-node stack both draw from `mr`, so a slab-bound terminus decode stays
    // heap-free (ADR-0041 terminus-arena span contract).
    tlv_arena_t arena(mr);
    arena_sink sink(arena.nodes_, mr);
    const auto r = grammar::walk(grammar::span_cursor{input}, sink, mr, kMaxDepth);
    if (!r) return std::unexpected(r.error());
    return arena;
}

}  // namespace tr::wire
