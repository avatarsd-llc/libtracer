/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/tlv_arena.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/grammar.hpp"

namespace tr::wire {
namespace {

/**
 * @brief The terminus-arena sink for grammar::walk (ADR-0048 §1): appends pre-order arena nodes as
 *        the shared descent visits them.
 *
 * Each node's `wire` is
 * header+body (trailer excluded, so a whole-TLV copy is trailer-less at rest);
 * a structured node's subtree extent (`end`) is sealed on close. The descent logic
 * (pos/total, depth cap, when to descend) lives in the walk; this is the pre-order
 * twin of frame.cpp's owning_sink. Its own open-node stack draws from the arena
 * resource so a slab-bound decode stays heap-free.
 *
 * The per-child canonical-PATH bookkeeping this sink used to carry is GONE with
 * RFC-0018: a packed `PATH` body has one spelling per address, so the body is the
 * vertex-map key unconditionally and there is nothing to observe per child. That
 * removes a compare and a store from EVERY node of every terminus decode.
 */
struct arena_sink {
    mem::block_array_t<arena_tlv_t>& nodes_;
    /**
     * @brief The innermost open node's index, or @ref kNone (#2264).
     *
     * The open-node stack is threaded through the nodes themselves: while a node is open its
     * `end` holds the index of the node that was open around it, and its close restores this
     * from there before writing the real `end`. No stack of its own, so no draw and no depth
     * bound beyond the nodes the frame already costs.
     */
    std::uint32_t open_ = kNone;
    static constexpr std::uint32_t kNone = UINT32_MAX; /**< @brief No node is open. */
    /**
     * @brief Set by the first draw the source refused (#588).
     *
     * The walk's sink hooks return void, so exhaustion cannot propagate through them;
     * it is latched here and read once the walk returns. The walk itself keeps running
     * on a full sink, which is harmless — every later push is refused too, and the
     * frame is rejected before any node is read.
     */
    bool exhausted_ = false;
    /** @brief Where the root node's trailer stamp is written, if the frame carries one (#1109). */
    std::optional<timestamp_t>* root_ts_ = nullptr;

    arena_sink(mem::block_array_t<arena_tlv_t>& nodes, std::optional<timestamp_t>* root_ts)
        : nodes_(nodes), root_ts_(root_ts) {}

    void push(const grammar::header_t& h, std::span<const std::byte> bytes) {
        // The ROOT node's trailer stamp, captured before its bytes go out of reach (#1109):
        // the node's `wire` span excludes the trailer by design (ADR-0041 §4), so this is
        // the one moment the arena can still see it. Root-only — see `root_trailer_ts`.
        if (root_ts_ != nullptr && nodes_.empty() && h.opt.ts) {
            const std::size_t off = h.header + h.length;
            timestamp_t t;
            t.relative = h.opt.tf;
            if (h.opt.tf) {
                t.value = static_cast<std::int32_t>(
                    detail::load_le<std::uint32_t>(bytes.subspan(off, 4)));
            } else {
                t.value = static_cast<std::int64_t>(
                    detail::load_le<std::uint64_t>(bytes.subspan(off, 8)));
            }
            *root_ts_ = t;
        }
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
    }
    void on_leaf(const grammar::header_t& h, const grammar::span_cursor& node) {
        push(h, node.buf);
    }
    void on_open(const grammar::header_t& h, const grammar::span_cursor& node) {
        const auto index = static_cast<std::uint32_t>(nodes_.size());
        push(h, node.buf);
        if (exhausted_) return;
        nodes_[index].end = std::exchange(open_, index);  // the enclosing open node, until close
    }
    void on_close() {
        // One branch: `on_open` latches `exhausted_` when its push was refused, so
        // `!exhausted_` already implies the node this close expects is open.
        if (exhausted_) return;
        arena_tlv_t& node = nodes_[open_];
        open_ = std::exchange(node.end, static_cast<std::uint32_t>(nodes_.size()));
    }
};

}  // namespace

std::expected<tlv_arena_t, err_t> decode_into(std::span<const std::byte> input,
                                              mem::block_source_t& src) {
    // The one structural descent lives in grammar::walk (ADR-0048 §1); this sink
    // appends the pre-order arena nodes. The walk stack spills to `src` past its
    // inline slots, and the sink's open-node stack is threaded through the nodes
    // (#2264), so a slab-bound terminus decode stays heap-free (ADR-0041
    // terminus-arena span contract) and the caller's source is the nesting-depth
    // bound (RFC-0006).
    //
    // Both of those draws are NOTHROW and guarded (#588): this runs on the
    // wire RX path behind no ACL, and a peer chooses both the nesting depth and the
    // node count. Exhaustion is TLV_NESTING_TOO_DEEP — the status RFC-0006 already
    // specifies for "exceeds this receiver's decode resources" — not an abort.
    tlv_arena_t arena(src);
    // Pre-size the node vector. Without this a 7-node FWD request walked 1->2->4->8, i.e. THREE
    // realloc-and-copy passes to reach a capacity it could have started at — 720 of the terminus
    // decode's 784 bytes were that growth curve rather than the nodes themselves. A capacity hint,
    // not a bound: a deeper frame still grows, bounded only by `mr` (RFC-0006/0007, ADR-0051). The
    // 8 is the same typical-FWD-nesting figure the walk stack's inline slots use just below.
    if (!arena.nodes_.reserve(8)) return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);
    arena_sink sink(arena.nodes_, &arena.root_ts_);
    std::array<grammar::walk_frame_t<grammar::span_cursor>, 8> slots;
    grammar::walk_stack_t<grammar::span_cursor> stack(slots, &src);
    const auto r = grammar::walk(grammar::span_cursor{input}, sink, stack);
    if (!r) return std::unexpected(r.error());
    // The sink's own draws cannot report through the void hooks, so they latch instead.
    if (sink.exhausted_) return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);
    return arena;
}

}  // namespace tr::wire
