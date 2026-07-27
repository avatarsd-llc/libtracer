/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/tlv_arena.hpp"

#include <array>

#include "libtracer/grammar.hpp"

namespace tr::wire {
namespace {

/**
 * @brief A bare canonical NAME: type 0x02 with opt byte 0x00 — exactly the `02 00 <u16 len>` header
 *        path_key emits, so a PATH made only of these has a body byte-identical to its canonical
 *        vertex-map key (ADR-0041 §3).
 */
bool is_canonical_name(const grammar::header_t& h) noexcept {
    return h.type == type_t::NAME && h.opt == opt_t{};
}

/**
 * @brief The terminus-arena sink for grammar::walk (ADR-0048 §1): appends pre-order arena nodes as
 *        the shared descent visits them.
 *
 * Each node's `wire` is
 * header+body (trailer excluded, so a whole-TLV copy is trailer-less at rest);
 * a structured node's subtree extent (`end`) and its ADR-0041 §3 canonical-PATH
 * flag are sealed on close. The descent logic (pos/total, depth cap, when to
 * descend) lives in the walk; this is the pre-order twin of frame.cpp's
 * owning_sink. Its own open-node stack draws from the arena resource so a
 * slab-bound decode stays heap-free.
 */
struct arena_sink {
    mem::block_array_t<arena_tlv_t>& nodes_;
    struct open_t {
        std::uint32_t index = 0;
        bool names_only = true; /**< ADR-0041 §3 canonical-PATH property over direct children */
    };
    mem::block_array_t<open_t> open_;
    /**
     * @brief Set by the first draw the source refused (#588).
     *
     * The walk's sink hooks return void, so exhaustion cannot propagate through them;
     * it is latched here and read once the walk returns. The walk itself keeps running
     * on a full sink, which is harmless — every later push is refused too, and the
     * frame is rejected before any node is read.
     */
    bool exhausted_ = false;

    arena_sink(mem::block_array_t<arena_tlv_t>& nodes, mem::block_source_t& src)
        : nodes_(nodes), open_(src) {
        // Typical FWD nesting; deeper frames grow (bounded by `src`). A refusal here is
        // latched like any other — the frame is rejected, not aborted.
        if (!open_.reserve(8)) exhausted_ = true;
    }

    void push(const grammar::header_t& h, std::span<const std::byte> bytes) {
        // Written through the slot, not built as a temporary and copied: an `arena_tlv_t`
        // is 48 bytes, and materializing one per node cost ~45 % of this decode (#588).
        arena_tlv_t* n = nodes_.push_slot();
        if (n == nullptr) {
            exhausted_ = true;
            return;
        }
        n->type = h.type;
        n->opt = h.opt;
        n->wire = bytes.first(h.header + h.length);
        n->body = bytes.subspan(h.header, h.length);
        n->end = static_cast<std::uint32_t>(nodes_.size());  // opaque default: own index + 1
        n->canonical_path = false;
    }
    /**
     * @brief A node that is a direct child of the currently-open parent (if any) breaks the
     *        parent's canonical-PATH property unless it is a bare NAME.
     */
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
        if (!open_.push_back(open_t{.index = index, .names_only = true})) exhausted_ = true;
    }
    void on_close() {
        // One branch, not three: `on_open` latches `exhausted_` if EITHER of its two
        // pushes was refused, so `!exhausted_` already implies both stacks hold the entry
        // this close expects. Checking `open_.empty()` and the index bound as well costs
        // two more loads per structured node on the terminus path and proves nothing the
        // flag does not.
        if (exhausted_) return;
        arena_tlv_t& node = nodes_[open_.back().index];
        node.end = static_cast<std::uint32_t>(nodes_.size());
        node.canonical_path = node.type == type_t::PATH && open_.back().names_only;
        open_.pop_back();
    }
};

}  // namespace

std::expected<tlv_arena_t, err_t> decode_into(std::span<const std::byte> input,
                                              mem::block_source_t& src) {
    // The one structural descent lives in grammar::walk (ADR-0048 §1); this sink
    // appends the pre-order arena nodes. The walk stack spills to `src` past its
    // inline slots, and the sink's open-node stack draws from `src` too, so a
    // slab-bound terminus decode stays heap-free (ADR-0041 terminus-arena span
    // contract) and the caller's source is the nesting-depth bound (RFC-0006).
    //
    // Every one of those three draws is NOTHROW and guarded (#588): this runs on the
    // wire RX path behind no ACL, and a peer chooses both the nesting depth and the
    // node count. Exhaustion is TLV_NESTING_TOO_DEEP — the status RFC-0006 already
    // specifies for "exceeds this receiver's decode resources" — not an abort.
    tlv_arena_t arena(src);
    // Pre-size the node vector, as the sink already does for its open-node stack just below.
    // Without this a 7-node FWD request walked 1->2->4->8, i.e. THREE realloc-and-copy passes
    // to reach a capacity it could have started at — 720 of the terminus decode's 784 bytes
    // were that growth curve rather than the nodes themselves. A capacity hint, not a bound:
    // a deeper frame still grows, bounded only by `mr` (RFC-0006/0007, ADR-0051). The 8 is the
    // same typical-FWD-nesting figure already used twice in this function, so it introduces no
    // new number to keep in sync.
    if (!arena.nodes_.reserve(8)) return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);
    arena_sink sink(arena.nodes_, src);
    std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;
    grammar::walk_stack_t<grammar::span_cursor> stack(slots, &src);
    const auto r = grammar::walk(grammar::span_cursor{input}, sink, stack);
    if (!r) return std::unexpected(r.error());
    // The sink's own draws cannot report through the void hooks, so they latch instead.
    if (sink.exhausted_) return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);
    return arena;
}

}  // namespace tr::wire
