/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/*
 * The ONE templated terminus resolve walk (ADR-0053 §7): the node-reader concept
 * plus every helper the walk needs, shared by its two instantiation TUs so the
 * resolver is written once instead of forked (the drift class ADR-0048 §1
 * eliminated in the grammar). op_resolve.cpp instantiates the `arena_node` reader
 * (span tier: byte-identical, the MCU terminus + conformance oracle);
 * op_resolve_view.cpp instantiates the `tlv_view_t` reader (owning rope tier) in
 * ITS OWN TU, so a span-only target that never links the lazy tier never
 * instantiates the view walk (ADR-0048 §1 / ADR-0047 templating rule). The
 * helpers live in an anonymous namespace: each of the two includers gets its own
 * internal-linkage copy — never a public surface.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/error.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/tlv_emit.hpp"

/**
 * @file
 * @brief The shared templated terminus resolve walk + node-reader concept (ADR-0053 §7).
 */

namespace tr::graph {

using view::rope_t;
using view::segment_ptr_t;
using view::view_t;
using wire::arena_tlv_t;
using wire::opt_t;
using wire::tlv_arena_t;
using wire::type_t;

namespace {

// The opt byte an ADR-0041 §4 trailer-sliced copy carries: the structural bits
// (PL, LL) survive; the trailer bits (TS, CR, CW, TF) are cleared so the copied
// TLV — whose bytes exclude the trailer by construction (`node.wire`) — stays
// self-consistent and trailer-less at rest. Cleared through `opt_t` (not a raw
// `& 0x48` mask) so there is one representation of the opt bitfield.
[[nodiscard]] constexpr std::byte struct_opt(std::byte opt_byte) noexcept {
    return static_cast<std::byte>(
        opt_t::decode(std::to_integer<std::uint8_t>(opt_byte)).without_trailer().encode());
}

// Byte-identical to the retired `& 0x48` mask for any validated opt byte (0x7E =
// every non-reserved bit set → PL|LL == 0x48; a plain opaque VALUE 0x00 → 0x00).
static_assert(struct_opt(std::byte{0x7E}) == std::byte{0x48});
static_assert(struct_opt(std::byte{0x00}) == std::byte{0x00});

// Map an L4 status_t to its registered tr:: error code (RFC-0002 §D registry,
// wire::err_t) — the u16 the kind=ERROR reply's ERROR{VALUE} identity carries.
[[nodiscard]] wire::err_t error_code(status_t s) noexcept {
    switch (s) {
        case status_t::NOT_FOUND:
            return wire::err_t::PATH_NOT_FOUND;
        case status_t::PERMISSION_DENIED:
            return wire::err_t::ACCESS_DENIED;
        case status_t::INVALID_PATH:
            return wire::err_t::PATH_INVALID;
        case status_t::TYPE_MISMATCH:
            return wire::err_t::SCHEMA_TYPE_MISMATCH;
        case status_t::BACKPRESSURE:
            return wire::err_t::FLOW_BACKPRESSURE;
        case status_t::TIMEOUT:
            return wire::err_t::FLOW_TIMEOUT;
        case status_t::SCHEMA_NOT_FOUND:
            return wire::err_t::SCHEMA_NOT_FOUND;
        case status_t::PATH_IN_USE:
            return wire::err_t::PATH_IN_USE;
    }
    return wire::err_t::PATH_NOT_FOUND;
}

// The node-reader concept (ADR-0053 §7): the terminus resolves through ONE
// templated walk over a decoded-TLV node, so the span arena and the lazy rope
// view share the resolver instead of forking it (the drift class ADR-0048 §1
// eliminated in the grammar). A node exposes its header facts, its
// trailer-excluded whole-TLV `wire` bytes and its `body` bytes, and FORWARD
// child iteration (`children().next()`) — never random sibling access, so the
// same walk serves a forward-only rope view. `arena_node` is the span-tier
// instantiation (byte-identical, still the MCU terminus + conformance oracle);
// the `tlv_view_t` reader instantiation follows (3c).
struct arena_node {
    const tlv_arena_t* a = nullptr;
    std::uint32_t i = 0;

    [[nodiscard]] const arena_tlv_t& node() const noexcept { return (*a)[i]; }
    [[nodiscard]] type_t type() const noexcept { return node().type; }
    [[nodiscard]] opt_t opt() const noexcept { return node().opt; }
    [[nodiscard]] bool structured() const noexcept { return node().opt.pl; }
    [[nodiscard]] std::span<const std::byte> wire() const noexcept { return node().wire; }
    [[nodiscard]] std::span<const std::byte> body() const noexcept { return node().body; }
    [[nodiscard]] bool canonical_path() const noexcept { return node().canonical_path; }

    // The trailer-excluded whole TLV as a fresh OWNED segment (the ADR-0041 §2
    // ownership copy of a borrowed arena span — the span tier always copies its
    // bytes, since the arena outlives nothing). The lazy rope reader overrides this
    // to adopt a multi-link flatten instead of copying it twice (ADR-0053 ⑤).
    [[nodiscard]] view_t own_wire() const { return view::over_bytes(wire()).value_or(view_t{}); }

    // The trailer-excluded whole-TLV byte length — read WITHOUT materializing (the
    // ADR-0042 §3 store-decision size test; the rope reader answers it from its
    // header + body_size without a flatten).
    [[nodiscard]] std::size_t wire_size() const noexcept { return node().wire.size(); }

    // Pin this TLV as a subrope of the owning delivery instead of copying it (ADR-0042
    // §3): the span tier pins a subview of the contiguous @p frame_view (a single link);
    // `nullopt` when the frame is borrowed (no owning view to pin). The eligibility test
    // (opt-in, size, trailer-less) is `own_or_ref_tlv`'s — this only produces the rope.
    [[nodiscard]] std::optional<rope_t> pin_wire(const view_t* frame_view) const {
        if (frame_view == nullptr) return std::nullopt;
        const std::span<const std::byte> w = node().wire;
        const std::size_t off = static_cast<std::size_t>(w.data() - frame_view->bytes().data());
        return rope_t(frame_view->subview(off, w.size()));
    }

    // Forward-only child cursor — the shared shape of `tlv_view_t::children_t`.
    class children_cursor {
       public:
        children_cursor(const tlv_arena_t* a, std::uint32_t begin, std::uint32_t end) noexcept
            : a_(a), j_(begin), end_(end) {}
        [[nodiscard]] std::optional<arena_node> next() noexcept {
            if (j_ >= end_) return std::nullopt;
            const std::uint32_t cur = j_;
            j_ = a_->next_sibling(j_);
            return arena_node{a_, cur};
        }

       private:
        const tlv_arena_t* a_;
        std::uint32_t j_;
        std::uint32_t end_;
    };
    [[nodiscard]] children_cursor children() const noexcept {
        return children_cursor{a, tlv_arena_t::first_child(i), node().end};
    }
};

// A parsed request FWD over node HANDLES — re-readable, no bytes owned until an
// ownership copy is taken (ADR-0041 §2). Templated over the node-reader @p N so
// the arena and the lazy view produce the same parsed shape.
template <class N>
struct parsed_fwd_t {
    fwd_op_t op{};
    N dst{};                          // forward route (a PATH node)
    std::optional<N> selector{};      // optional :field (a FIELD node)
    N src{};                          // accumulated return route (a PATH node)
    std::optional<N> payload{};       // WRITE only (the value node)
    std::uint64_t await_timeout = 0;  // AWAIT only
    bool has_await_timeout = false;
};

// Parse the FWD child sequence positionally (RFC-0004 §B order: op, dst, FIELD?,
// src, [payload | await_timeout]) by FORWARD iteration over @p root's children.
// Returns INVALID_PATH for a structurally malformed frame (the resolver turns
// that into the error side, not a reply).
template <class N>
[[nodiscard]] result_t<parsed_fwd_t<N>> parse_fwd(const N& root) {
    if (root.type() != type_t::FWD || !root.structured())
        return std::unexpected(status_t::INVALID_PATH);
    auto ch = root.children();
    parsed_fwd_t<N> p;

    const std::optional<N> op = ch.next();
    if (!op || op->type() != type_t::VALUE) return std::unexpected(status_t::INVALID_PATH);
    p.op = static_cast<fwd_op_t>(detail::load_le<std::uint8_t>(op->body()));

    std::optional<N> dst = ch.next();
    if (!dst || dst->type() != type_t::PATH) return std::unexpected(status_t::INVALID_PATH);
    p.dst = *dst;

    std::optional<N> next = ch.next();
    if (next && next->type() == type_t::FIELD) {
        p.selector = *next;
        next = ch.next();
    }
    if (!next || next->type() != type_t::PATH) return std::unexpected(status_t::INVALID_PATH);
    p.src = *next;

    const std::optional<N> tail = ch.next();
    if (p.op == fwd_op_t::WRITE) {
        if (tail) p.payload = *tail;
    } else if (p.op == fwd_op_t::AWAIT) {
        if (tail && tail->type() == type_t::VALUE) {
            p.await_timeout = detail::load_le<std::uint64_t>(tail->body());
            p.has_await_timeout = true;
        }
    }
    return p;
}

// FIELD index_mode (RFC-0004 §C, the optional u8 index_mode VALUE).
enum class index_mode_t : std::uint8_t { SCALAR = 0, ELEMENT = 1, WILDCARD = 2 };

// Decode a FIELD selector node into the graph's field_path_t. Each level is a
// NAME followed by 0/1/2 VALUE children: 0 => SCALAR; 1 => index_mode only
// (ELEMENT append "[]" or WILDCARD "[*]"); 2 => [index u32, index_mode u8]
// ("[N]"). `wildcard_seen` is set if any level carries index_mode=WILDCARD.
template <class N>
[[nodiscard]] result_t<field_path_t> selector_to_field(const N& field, bool& wildcard_seen) {
    field_path_t fp;
    auto ch = field.children();
    std::optional<N> cur = ch.next();
    while (cur) {  // one level per NAME + its 0/1/2 trailing VALUEs
        if (cur->type() != type_t::NAME) return std::unexpected(status_t::INVALID_PATH);
        field_step_t step;
        step.name.assign(detail::as_string_view(cur->body()));
        std::optional<N> v0;
        std::optional<N> v1;
        std::optional<N> next = ch.next();
        if (next && next->type() == type_t::VALUE) {
            v0 = std::move(next);
            next = ch.next();
        }
        if (v0 && next && next->type() == type_t::VALUE) {
            v1 = std::move(next);
            next = ch.next();
        }
        index_mode_t mode = index_mode_t::SCALAR;
        bool has_index = false;
        std::uint32_t index = 0;
        if (v0 && v1) {
            has_index = true;
            index = detail::load_le<std::uint32_t>(v0->body());
            mode = static_cast<index_mode_t>(detail::load_le<std::uint8_t>(v1->body()));
        } else if (v0) {
            mode = static_cast<index_mode_t>(detail::load_le<std::uint8_t>(v0->body()));
        }
        switch (mode) {
            case index_mode_t::ELEMENT:
                step.indexed = true;
                if (has_index)
                    step.index = static_cast<std::uint16_t>(index);
                else
                    step.append = true;
                break;
            case index_mode_t::WILDCARD:
                step.indexed = true;
                step.wildcard = true;
                wildcard_seen = true;
                break;
            case index_mode_t::SCALAR:
                step.indexed = has_index;
                if (has_index) step.index = static_cast<std::uint16_t>(index);
                break;
        }
        fp.steps.push_back(std::move(step));
        if (fp.steps.size() > kMaxFieldDepth) return std::unexpected(status_t::INVALID_PATH);
        cur = std::move(next);  // the lookahead item is the next level's NAME (or end)
    }
    return fp;
}

// True for a whole-array ":subscribers[]" read (vs. a single "[N]" slot).
[[nodiscard]] bool is_subscribers_array(const field_path_t& fp) noexcept {
    return fp.steps.size() == 1 && fp.steps[0].name == "subscribers" &&
           (fp.steps[0].append || (!fp.steps[0].indexed && !fp.steps[0].wildcard));
}

// True for the subscribe form specifically — a ":subscribers[]" APPEND (a new edge),
// distinct from a ":subscribers[N]" clear (unsubscribe) or the whole-array read.
[[nodiscard]] bool is_subscribe_append(const field_path_t& fp) noexcept {
    return fp.steps.size() == 1 && fp.steps[0].name == "subscribers" && fp.steps[0].append;
}

// The one ADR-0041 §2 ownership copy of a whole TLV into a fresh owned segment:
// the reader's trailer-excluded `own_wire` (span tier copies its borrowed bytes;
// rope tier adopts a multi-link flatten, ADR-0053 ⑤) with the copied opt byte's
// trailer bits cleared (§4) — the stored TLV is trailer-less at rest and
// self-consistent. The opt patch lives here, ONE locus for both readers.
template <class N>
[[nodiscard]] view_t own_tlv(const N& node) {
    view_t v = node.own_wire();  // owned, trailer-excluded; empty view on alloc failure
    if (!v.empty()) v.owner->bytes[1] = struct_opt(v.owner->bytes[1]);
    return v;
}

// True iff the node's opt byte carries NO trailer bits — the reference
// implementation's ADR-0042 §3 restriction: a referenced store cannot patch the
// opt byte in a shared frame, so only an already-trailer-less payload may be
// referenced; a CRC/TS-carrying payload falls back to the trailer-sliced copy.
template <class N>
[[nodiscard]] bool trailer_less(const N& node) noexcept {
    const opt_t o = node.opt();
    return !o.ts && !o.cr && !o.cw && !o.tf;
}

// The ADR-0042 §3 stored-value decision, generalized to the rope tier (ADR-0053 ⑤):
// PIN the payload as a subrope of the owning delivery (refcount, zero copy) when the
// vertex opted in (`store_ref_min_bytes` > 0), the payload is big enough, its opt byte
// is trailer-less, AND the reader can pin (`pin_wire`) — the span tier pins a subview
// of the contiguous owning `frame_view`, the rope tier a subrope of its own
// scatter-gather segments. Otherwise the ADR-0041 §2 one-copy `own_tlv`. The
// eligibility test lives HERE, one locus for both readers; each reader only produces
// its pinned rope. Returns a rope so a multi-link pinned payload keeps its segments.
template <class N>
[[nodiscard]] rope_t own_or_ref_tlv(const N& node, const view_t* frame_view,
                                    std::uint32_t ref_min_bytes) {
    if (ref_min_bytes > 0 && node.wire_size() >= ref_min_bytes && trailer_less(node)) {
        if (std::optional<rope_t> pinned = node.pin_wire(frame_view)) return std::move(*pinned);
    }
    return rope_t(own_tlv(node));
}

// A tiny cursor writer over a preallocated, exactly-sized head segment — the
// direct-emit that replaces the old 4-stage (encode → children → head → segment)
// staging: every length is known from the arena spans up front, so the reply
// head is ONE allocation and each route byte is copied exactly once.
struct emit_cursor_t {
    std::byte* p;

    void struct_header(type_t type, bool ll, std::size_t body_len) {
        *p++ = static_cast<std::byte>(std::to_underlying(type));
        *p++ = static_cast<std::byte>(opt_t{.pl = true, .ll = ll}.encode());
        detail::store_le(std::span<std::byte>(p, ll ? 4u : 2u),
                         static_cast<std::uint32_t>(body_len), ll ? 4u : 2u);
        p += ll ? 4u : 2u;
    }
    void u8_value(std::uint8_t v) {  // a 1-byte VALUE TLV (op / kind discriminants)
        *p++ = static_cast<std::byte>(std::to_underlying(type_t::VALUE));
        *p++ = std::byte{0};
        *p++ = std::byte{1};
        *p++ = std::byte{0};
        *p++ = std::byte{v};
    }
    void tlv_sliced(std::span<const std::byte> wire) {  // trailer-sliced whole-TLV copy (§4)
        std::memcpy(p, wire.data(), wire.size());
        p[1] = struct_opt(p[1]);
        p += wire.size();
    }
    void raw(std::span<const std::byte> bytes) {
        if (!bytes.empty()) std::memcpy(p, bytes.data(), bytes.size());
        p += bytes.size();
    }
};

constexpr std::size_t kU8ValueLen = 5;  // 4-byte VALUE header + 1 payload byte

// Assemble the FWD{REPLY} rope: one exactly-sized head segment (FWD header +
// op=REPLY + dst=req.src + src=req.dst + kind + `inline_tail`) prepended to
// `shared` (refcount clones of the stored payload view(s)). The head is the only
// allocation; the route bytes are copied ONCE (trailer-sliced); `shared` is never
// copied — RFC-0004 §D / ADR-0035 zero-copy reply rule, ADR-0041 §5 direct emit.
// @p reply_dst_wire (the request's `src`) and @p reply_src_wire (the request's `dst`,
// the responder endpoint) are the trailer-excluded whole-TLV `wire` spans of those two
// PATH nodes — passed in rather than read off the arena so the reply builder is
// decoupled from the request's node model (ADR-0053 §7: a node-reader-agnostic seam,
// and where ⑤'s scatter-gather reply emission will hook).
[[nodiscard]] rope_t assemble(std::span<const std::byte> reply_dst_wire,
                              std::span<const std::byte> reply_src_wire, reply_kind_t kind,
                              std::span<const std::byte> inline_tail,
                              const std::vector<view_t>& shared, std::size_t shared_len) {
    const std::size_t children_len = kU8ValueLen + reply_dst_wire.size() + reply_src_wire.size() +
                                     kU8ValueLen + inline_tail.size();
    const std::size_t body_len = children_len + shared_len;
    const bool ll = body_len > 0xFFFFu;
    const std::size_t head_len = (ll ? 6u : 4u) + children_len;

    rope_t rope;
    segment_ptr_t seg = view::heap_alloc(head_len);
    if (seg) {
        // A head-alloc failure degrades the reply (payload views only), never crashes.
        emit_cursor_t out{seg->bytes.data()};
        out.struct_header(type_t::FWD, ll, body_len);
        out.u8_value(std::to_underlying(fwd_op_t::REPLY));
        out.tlv_sliced(reply_dst_wire);
        out.tlv_sliced(reply_src_wire);
        out.u8_value(std::to_underlying(kind));
        out.raw(inline_tail);
        rope.append(view_t::over(std::move(seg)));
    }
    for (const view_t& v : shared) rope.append(v);  // refcount clone — no byte copy
    return rope;
}

// A kind=ERROR reply carrying STATUS{ ERROR{ VALUE u16 LE code } } (RFC-0004 §D
// with the RFC-0002 §C registered-code identity) — a fixed 14-byte tail, built
// on the stack.
[[nodiscard]] rope_t assemble_error(std::span<const std::byte> reply_dst_wire,
                                    std::span<const std::byte> reply_src_wire, status_t status) {
    const std::uint16_t code = std::to_underlying(error_code(status));
    const std::array<std::byte, 14> tail{
        static_cast<std::byte>(std::to_underlying(type_t::STATUS)),
        static_cast<std::byte>(opt_t{.pl = true}.encode()),
        std::byte{10},
        std::byte{0},  // STATUS length = one 10-byte ERROR child
        static_cast<std::byte>(std::to_underlying(type_t::ERROR)),
        static_cast<std::byte>(opt_t{.pl = true}.encode()),
        std::byte{6},
        std::byte{0},  // ERROR length = one 6-byte VALUE identity child
        static_cast<std::byte>(std::to_underlying(type_t::VALUE)),
        std::byte{0},
        std::byte{2},
        std::byte{0},  // VALUE length = 2 (the u16 LE registered code)
        static_cast<std::byte>(code & 0xFFu),
        static_cast<std::byte>(code >> 8),
    };
    return assemble(reply_dst_wire, reply_src_wire, reply_kind_t::ERROR, tail, {}, 0);
}

// A kind=RESULT reply whose payload children are a stored rope value's links (ADR-0053
// §6): a single-link value contributes one payload child (the trivial case, identical to
// a view read); a multi-link stored value ropes ALL its links into the reply zero-copy —
// no flatten.
[[nodiscard]] rope_t assemble_result_rope(std::span<const std::byte> reply_dst_wire,
                                          std::span<const std::byte> reply_src_wire,
                                          const rope_t& payload) {
    const std::span<const view_t> links = payload.links();
    const std::vector<view_t> shared(links.begin(), links.end());
    return assemble(reply_dst_wire, reply_src_wire, reply_kind_t::RESULT, {}, shared,
                    payload.total_length());
}

// The vertex-map key for an arena-decoded PATH: span-aliased when canonical
// (ADR-0041 §3 — the PATH body IS the key, zero materialization), re-emitted
// into `fallback` otherwise (a foreign encoder's LL-widened / trailer-carrying
// NAMEs). Our own encoders always produce the canonical form.
template <class N>
[[nodiscard]] std::span<const std::byte> path_lookup_key(const N& path,
                                                         std::vector<std::byte>& fallback) {
    if (path.canonical_path()) return path.body();
    auto ch = path.children();
    for (std::optional<N> seg = ch.next(); seg; seg = ch.next())
        wire::emit_name(fallback, seg->body());
    return fallback;
}

// The ONE templated resolve walk (ADR-0053 §7): apply an @p N-read request FWD
// against @p graph and build the FWD{REPLY} rope. Instantiated with `arena_node`
// (span tier, byte-identical) and — 3c — the `tlv_view_t` reader (owning rope
// tier). Every frame read goes through the node-reader concept; nothing here
// names a specific decode representation.
template <class N>
[[nodiscard]] result_t<rope_t> resolve_node(graph_t& graph, const N& root,
                                            std::string_view inbound_link,
                                            const view_t* frame_view) {
    result_t<parsed_fwd_t<N>> parsed = parse_fwd(root);
    if (!parsed) return std::unexpected(parsed.error());
    const parsed_fwd_t<N>& req = *parsed;
    if (req.op == fwd_op_t::REPLY) return std::unexpected(status_t::INVALID_PATH);

    // The reply's route is the request's routes swapped: reply dst = request src (the
    // accumulated return route), reply src = request dst (this node's responder
    // endpoint). Their trailer-excluded whole-TLV `wire` bytes feed every assemble
    // below — read once here so the reply builder never reaches back into a specific
    // node model (ADR-0053 §7 node-reader seam). parse_fwd guarantees both PATH nodes.
    const std::span<const std::byte> reply_dst_wire = req.src.wire();
    const std::span<const std::byte> reply_src_wire = req.dst.wire();

    // Decode the optional :field selector and the wildcard deferral: a [*] level
    // on a non-subscriber-path target is rejected with INVALID_PATH.
    field_path_t field;
    const bool has_field = req.selector.has_value();
    if (has_field) {
        bool wildcard = false;
        result_t<field_path_t> f = selector_to_field(*req.selector, wildcard);
        if (!f) return assemble_error(reply_dst_wire, reply_src_wire, status_t::INVALID_PATH);
        field = std::move(*f);
        if (wildcard && (field.steps.empty() || field.steps[0].name != "subscribers"))
            return assemble_error(reply_dst_wire, reply_src_wire, status_t::INVALID_PATH);
    }

    // dst resolution is the router's PATH-keyed dispatch — span-aliased for a
    // canonical PATH (ADR-0041 §3: the frame IS the key). Local-only: a dst
    // naming a transport child / unknown path is not local => ERROR(NOT_FOUND).
    std::vector<std::byte> key_fallback;
    const std::span<const std::byte> dst_key = path_lookup_key(req.dst, key_fallback);
    std::optional<vertex_handle_t> found = graph.find(dst_key);
    if (!found) {
        // Write-creates (RFC-0005): a remote DATA write (no :field selector) to a
        // nonexistent path creates it, mkdir-p style, CREATE-gated on the nearest
        // existing ancestor under the inbound link's subject. Field ops and
        // read/await keep NOT_FOUND — there is no vertex to control or serve.
        if (req.op == fwd_op_t::WRITE && !has_field) {
            const result_t<vertex_handle_t> made = graph.ensure_vertex(dst_key, inbound_link);
            if (!made) return assemble_error(reply_dst_wire, reply_src_wire, made.error());
            found = *made;
        } else {
            return assemble_error(reply_dst_wire, reply_src_wire, status_t::NOT_FOUND);
        }
    }
    const vertex_handle_t v = *found;

    switch (req.op) {
        case fwd_op_t::READ: {
            if (has_field && is_subscribers_array(field)) {
                result_t<std::vector<view_t>> subs = graph.read_subscribers(v, inbound_link);
                if (!subs) return assemble_error(reply_dst_wire, reply_src_wire, subs.error());
                std::size_t sub_len = 0;
                for (const view_t& s : *subs) sub_len += s.length;
                // PL=1 wrapper (POINT) whose children are the slot SUBSCRIBER views,
                // roped on zero-copy. POINT is the structured introspection-result
                // container already used for :schema and vertex enumeration.
                std::array<std::byte, 6> wrapper;
                const bool wll = sub_len > 0xFFFFu;
                emit_cursor_t wout{wrapper.data()};
                wout.struct_header(type_t::POINT, wll, sub_len);
                return assemble(reply_dst_wire, reply_src_wire, reply_kind_t::RESULT,
                                std::span<const std::byte>(wrapper.data(), wll ? 6u : 4u), *subs,
                                sub_len);
            }
            result_t<rope_t> r =
                has_field ? graph.read(v, field, inbound_link) : graph.read(v, inbound_link);
            if (!r) return assemble_error(reply_dst_wire, reply_src_wire, r.error());
            return assemble_result_rope(reply_dst_wire, reply_src_wire, *r);
        }
        case fwd_op_t::WRITE: {
            if (!req.payload.has_value())
                return assemble_error(reply_dst_wire, reply_src_wire, status_t::TYPE_MISMATCH);
            const N& payload_node = *req.payload;

            // A remote subscribe — a `:subscribers[]` APPEND that arrived over a
            // transport (inbound_link set) carrying a SUBSCRIBER — binds a REMOTE
            // subscriber instead of a local fan-out edge (#136); its stored views
            // (source SUBSCRIBER + return route) are subscription-scoped and keep
            // the ADR-0041 one-copy behavior unconditionally (ADR-0042 §3 applies
            // to the value store only).
            const bool remote_sub = !inbound_link.empty() && has_field &&
                                    is_subscribe_append(field) &&
                                    payload_node.type() == type_t::SUBSCRIBER;

            // The remote-subscribe binding: its stored views (source SUBSCRIBER + the
            // accumulated return route) are subscription-scoped and keep the ADR-0041 §2
            // one-copy behavior unconditionally (ADR-0042 §3 pinning applies to the value
            // store only). The slot retains `src` (copied once, trailer-sliced) + the
            // inbound link so the producer fan-out delivers FWD{WRITE}/COMPACT home. A
            // wire TLV is never empty, so an empty copy is exactly an allocation failure
            // ⇒ BACKPRESSURE.
            if (remote_sub) {
                const view_t sub_value = own_tlv(payload_node);
                if (sub_value.empty())
                    return assemble_error(reply_dst_wire, reply_src_wire, status_t::BACKPRESSURE);
                // The ONE route copy of the subscription's life (ADR-0041 §2), into a
                // refcounted segment — every later delivery clones the refcount.
                const view_t return_route = own_tlv(req.src);
                if (return_route.empty())
                    return assemble_error(reply_dst_wire, reply_src_wire, status_t::BACKPRESSURE);
                // ADR-0049: the wire append enters the graph's single admission door
                // (subscribe_wire → admit_subscriber) — the SUBSCRIBER TLV is parsed
                // ONCE there (delivery_compact included), so no parallel parse here.
                result_t<void> w =
                    graph.subscribe_wire(v, sub_value, return_route, std::string(inbound_link));
                if (!w) return assemble_error(reply_dst_wire, reply_src_wire, w.error());
                return assemble(reply_dst_wire, reply_src_wire, reply_kind_t::RESULT, {}, {},
                                0);  // OK, empty payload
            }

            // The stored written value: ADR-0041 §2 one ownership copy, trailer-sliced by
            // construction (§4 — an arriving CRC/TS trailer is NOT stored; stored TLVs are
            // trailer-less at rest, ADR-0035) — or, on an owning delivery with the vertex
            // opted in, an ADR-0042 §3 pinned subrope of the frame (refcount, zero copy;
            // multi-link on the rope tier). An empty rope is an allocation failure.
            const rope_t value =
                own_or_ref_tlv(payload_node, frame_view, graph.settings(v).store_ref_min_bytes);
            if (value.total_length() == 0)
                return assemble_error(reply_dst_wire, reply_src_wire, status_t::BACKPRESSURE);

            result_t<void> w =
                graph.write(v, has_field ? field : field_path_t{}, value, inbound_link);
            if (!w) return assemble_error(reply_dst_wire, reply_src_wire, w.error());
            return assemble(reply_dst_wire, reply_src_wire, reply_kind_t::RESULT, {}, {},
                            0);  // OK, empty payload
        }
        case fwd_op_t::AWAIT: {
            const std::chrono::nanoseconds timeout =
                req.has_await_timeout ? std::chrono::nanoseconds(req.await_timeout)
                                      : kDefaultAwaitTimeout;
            result_t<rope_t> r = graph.await(v, timeout, inbound_link);
            if (!r)
                return assemble_error(reply_dst_wire, reply_src_wire,
                                      r.error());  // TIMEOUT => tr::flow::timeout
            return assemble_result_rope(reply_dst_wire, reply_src_wire, *r);
        }
        case fwd_op_t::REPLY:
            break;  // unreachable — handled above
    }
    return std::unexpected(status_t::INVALID_PATH);
}

}  // namespace

}  // namespace tr::graph
