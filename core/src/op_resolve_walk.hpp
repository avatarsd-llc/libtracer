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
 *
 * The FWD{REPLY} byte grammar is the one thing that does NOT live here (#887). It
 * is declared in fwd_reply.hpp and defined ONCE, in fwd_reply.cpp: an
 * internal-linkage copy per includer meant the reply layout was compiled twice
 * over, and `fwd_router.cpp`'s bus-NAME-hop rejection hand-rolled a third that had
 * already drifted on trailer bits. The walk calls `assemble_reply` /
 * `assemble_error_reply` across that TU boundary.
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

#include "fwd_reply.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/config.hpp"
#include "libtracer/error.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path_label.hpp"
#include "libtracer/pin_instrument.hpp"
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

/**
 * @brief The node-reader concept (ADR-0053 §7): the terminus resolves through ONE templated walk
 *        over a decoded-TLV node, so the span arena and the lazy rope view share the resolver
 *        instead of forking it (the drift class ADR-0048 §1 eliminated in the grammar).
 *
 * A node exposes its header facts, its
 * trailer-excluded whole-TLV `wire` bytes and its `body` bytes, and FORWARD
 * child iteration (`children().next()`) — never random sibling access, so the
 * same walk serves a forward-only rope view. A reader that has to BUILD its contiguous
 * spans (the rope tier flattens) also answers `spans_intact()`, so a reader that could not
 * build one is answered by value instead of read short (#766). `arena_node` is the span-tier
 * instantiation (byte-identical, still the MCU terminus + conformance oracle);
 * the `tlv_view_t` reader instantiation follows (3c).
 */
struct arena_node {
    const tlv_arena_t* a = nullptr;
    std::uint32_t i = 0;

    [[nodiscard]] const arena_tlv_t& node() const noexcept { return (*a)[i]; }
    [[nodiscard]] type_t type() const noexcept { return node().type; }
    [[nodiscard]] opt_t opt() const noexcept { return node().opt; }
    [[nodiscard]] bool structured() const noexcept { return node().opt.pl; }
    [[nodiscard]] std::span<const std::byte> wire() const noexcept { return node().wire; }
    [[nodiscard]] std::span<const std::byte> body() const noexcept { return node().body; }

    /**
     * @brief The decoded trailer timestamp — ROOT node only on this tier (#1109).
     *
     * The arena's node spans exclude the trailer (ADR-0041 §4), so the value is captured at
     * decode for index 0 alone — the one node the walk ever asks (the reply echo reads the
     * request's OUTER stamp). A non-root ask answers "none", honestly: the arena did not
     * keep it.
     */
    [[nodiscard]] std::optional<wire::timestamp_t> trailer_ts() const noexcept {
        return i == 0 ? a->root_trailer_ts() : std::nullopt;
    }

    /**
     * @brief Have this walk's contiguous spans all been produced successfully (#766)? Always
     *        true on the span tier — an arena span is BORROWED from the frame, so producing
     *        it cannot fail and nothing here can refuse.
     *
     * The rope tier answers `false` once its injected flatten backend refused, because a
     * refused flatten yields an EMPTY span where the frame had bytes. Constant here, so the
     * walk's two checks fold away entirely on the MCU terminus.
     *
     * It stays constant after #801 put @ref own_wire on the seam, and that is a statement
     * about spans, not about allocation. A refused ownership copy on this tier consumes no
     * span and shortens none: `wire()` still points into the frame and every value the walk
     * derived from it stays sound. The refusal is carried by the empty view `own_wire`
     * returns, through the by-value BACKPRESSURE channel `own_tlv`'s callers already read.
     * Flipping this flag instead would be strictly worse — it would condemn a walk whose
     * spans are provably intact.
     */
    [[nodiscard]] static bool spans_intact() noexcept { return true; }

    /**
     * @brief The trailer-excluded whole TLV as a fresh OWNED segment (the ADR-0041 §2 ownership
     *        copy of a borrowed arena span — the span tier always copies its bytes, since the arena
     *        outlives nothing).
     *
     * The lazy rope reader overrides this
     * to adopt a multi-link flatten instead of copying it twice (ADR-0053 ⑤).
     *
     * Through the INJECTED seam (#801). This is the arena tier's only allocating site, and
     * until #801 it was the last ownership copy in either tier still drawing from
     * `view::over_bytes`'s global heap — so a bounded node whose peer sent a CONTIGUOUS
     * terminus WRITE (the span-delivered shape a synchronous CAN/UART child hands up, and
     * the MCU terminus's ordinary case) allocated outside its own memory bound, peer-driven,
     * and an `abort()` under `-fno-exceptions`. #793 closed the same site one tier over.
     *
     * A refusal answers `std::nullopt` ⇒ the empty view, which is exactly what `own_tlv`'s
     * callers already read as BACKPRESSURE (`resolve_node`'s empty-value guards). A wire TLV
     * is never zero bytes, so `over_bytes`' engaged-empty "legitimately-empty input" outcome
     * is unreachable here and `nullopt` is unambiguously a refusal. Never an abort, never a
     * short span: see @ref spans_intact for why no flag is set.
     *
     * @section by_parameter Why the backend is a PARAMETER and not a member
     *
     * The obvious shape — give the node a pointer to the walk's seam, the way `view_node`
     * holds one — takes `arena_node` from 16 bytes to 24, and this node is copied BY VALUE
     * everywhere: `parsed_fwd_t` holds five of them and the child cursor makes one per step.
     * Passing the backend down as an argument keeps the node two words wide, so the reference
     * lives in one register for the whole walk instead of in every copy. It costs nothing to
     * choose, and it is the only one of the two shapes that cannot cost anything.
     *
     * That is a structural argument, deliberately not a measured one. `bench_terminus_tier`
     * cannot resolve either shape — one call taking one more register argument is below its
     * noise floor — so the evidence that the un-injected path is unchanged is the object-file
     * `cmp`, not a stopwatch.
     *
     * An earlier revision of this comment blamed that on BUILD LAYOUT ("identical source at
     * two build paths differs by +1.7 % to +6.7 %"). That attribution is withdrawn (#807):
     * the same commit built at two different paths produces byte-identical objects, archive
     * and executable, so there is no layout to be sensitive to. What the confounded A/B was
     * measuring was CPU placement on a heterogeneous host. The protocol that separates the two
     * lives in `docs/methodology.md`, "The A/B protocol"; do not re-derive it here.
     *
     * The rope tier keeps its seam POINTER regardless: it needs the sticky `refused` flag,
     * and its `ensure_cache` is reached from `wire()`/`body()`, which take no arguments.
     */
    [[nodiscard]] view::view_t own_wire(mem::mem_backend_t& flat) const {
        return view::over_bytes(wire(), flat).value_or(view::view_t{});
    }

    /**
     * @brief The trailer-excluded whole-TLV byte length — read WITHOUT materializing (the ADR-0042
     *        §3 store-decision size test; the rope reader answers it from its header + body_size
     *        without a flatten).
     */
    [[nodiscard]] std::size_t wire_size() const noexcept { return node().wire.size(); }

    /**
     * @brief Pin this TLV as a subrope of the owning delivery instead of copying it (ADR-0042 §3):
     *        the span tier pins a subview of the contiguous @p frame_view (a single link);
     *        `nullopt` when the frame is borrowed (no owning view to pin).
     *
     * The eligibility test
     * (size, trailer-less) is `share_or_copy_tlv`'s — this only produces the rope.
     */
    [[nodiscard]] std::optional<view::rope_t> pin_wire(const view::view_t* frame_view) const {
        // No OWNING segment — a borrowed, span-delivered frame — means nothing to share: a
        // subview of an owner-less view would store bytes whose lifetime nobody holds.
        if (frame_view == nullptr || !frame_view->owner) return std::nullopt;
        const std::span<const std::byte> w = node().wire;
        const std::size_t off = static_cast<std::size_t>(w.data() - frame_view->bytes().data());
        return view::rope_t(frame_view->subview(off, w.size()));
    }

    /**
     * @brief Copy the trailer-excluded whole TLV into @p out (exactly @ref wire_size bytes) —
     *        the copy arm's fill of an inline value (RFC-0028 §5.1). One `memcpy`: an arena
     *        span is contiguous and host-resident.
     * @retval true always on this tier.
     */
    [[nodiscard]] bool copy_wire_into(std::span<std::byte> out) const noexcept {
        const std::span<const std::byte> w = node().wire;
        std::memcpy(out.data(), w.data(), w.size());
        return true;
    }

    /** @brief Forward-only child cursor — the shared shape of `tlv_view_t::children_t`. */
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

/**
 * @brief A parsed request FWD over node HANDLES — re-readable, no bytes owned until an ownership
 *        copy is taken (ADR-0041 §2).
 *
 * Templated over the node-reader @p N so
 * the arena and the lazy view produce the same parsed shape.
 */
template <class N>
struct parsed_fwd_t {
    fwd_op_t op{};
    /**
     * @brief The masked opcode is one of the four `fwd_op_t` values (#904).
     *
     * `kFwdOpcodeMask` admits 0–63 and only 0–3 are defined, so 60 values cast to an
     * `fwd_op_t` that names nothing. @ref op is meaningless when this is false — read it
     * FIRST. Kept as a flag rather than an `optional<fwd_op_t>` so the four defined arms
     * pay nothing: every switch on @ref op is already guarded by a check of this field.
     */
    bool op_defined = true;
    /**
     * @brief `op` bit 7 was set — the origin asked for a bound-path mint (RFC-0024 §7.5).
     *
     * `resolve_node` clears it on a labelled `dst` once the no-reply check has read it: from
     * there on it means "a mint this terminus will answer" (RFC-0027 §11.2's second clause).
     */
    bool mint_request = false;
    /** @brief `dst` is a `PATH_REF` (`0x14`), not a canonical `PATH` (RFC-0024 §4). */
    bool dst_bound = false;
    N dst{};                     /**< forward route (a PATH or PATH_REF node) */
    std::optional<N> selector{}; /**< optional :field (a FIELD node) */
    N src{};                     /**< accumulated return route (a PATH node) */
    std::optional<N> payload{};  /**< WRITE only (the value node) */
    /**
     * @brief The reverse-direction `PATH_REF_REVERSE` (`0x15`) list the forwarding hops
     *        accumulated (RFC-0024 §7.1 amendments 1 and 2) — the request's trailing child,
     *        present only on a mint-flagged request that crossed at least one contributing
     *        hop, and identified by its own type rather than by its position.
     *
     * One element SHORT of the route by construction: the hop into this responder is the
     * one no peer can mint for it, so the responder completes the list with its own
     * reference before storing it (the remote-subscribe arm below).
     */
    std::optional<N> reverse{};
    /** AWAIT only: the request's `await_timeout`, or @ref kDefaultAwaitTimeout when absent. */
    std::uint64_t await_timeout = static_cast<std::uint64_t>(kDefaultAwaitTimeout.count());
    /**
     * @brief The `src` PATH is present but ZERO-LENGTH — *no reply requested*
     *        (RFC-0004 Amendment 2, #1502/#1491).
     *
     * A reply is something the origin REQUESTS by wiring a return route; an empty route is
     * the request not made. The terminus applies the WRITE and emits nothing — success,
     * refusal and denial alike, because none of them has anywhere to go.
     *
     * NOT set by @ref parse_fwd: the grammar is unchanged (the child is required and is
     * still a `PATH`), and reading its LENGTH is a body read, which on the rope tier can be
     * refused. `resolve_node` sets it once, after the guard that makes a refused flatten
     * distinguishable from a genuinely empty route — an empty span read as "unacknowledged"
     * would turn this node's memory pressure into a silently swallowed operation.
     */
    bool no_reply = false;
};

/**
 * @brief Parse the FWD child sequence positionally (RFC-0004 §B order: op, dst, FIELD?, src,
 *        [payload | await_timeout]) by FORWARD iteration over @p root's children.
 *
 * Returns INVALID_PATH for a structurally malformed frame (the resolver turns
 * that into the error side, not a reply).
 */
template <class N>
[[nodiscard]] result_t<parsed_fwd_t<N>> parse_fwd(const N& root) {
    if (root.type() != type_t::FWD || !root.structured())
        return std::unexpected(status_t::INVALID_PATH);
    auto ch = root.children();
    parsed_fwd_t<N> p;

    const std::optional<N> op = ch.next();
    if (!op || op->type() != type_t::VALUE) return std::unexpected(status_t::INVALID_PATH);
    // The opcode is `op & 0x3F`; bits 7-6 are FLAGS (RFC-0024 §7.5, normative in §9.3). The
    // raw byte is split here, once, so nothing downstream ever sees a flag mixed into the
    // discriminant — a mint-flagged READ is a READ everywhere but at the mint itself.
    const auto op_byte = detail::load_le<std::uint8_t>(op->body());
    const std::uint8_t opcode = op_byte & kFwdOpcodeMask;
    p.op = static_cast<fwd_op_t>(opcode);
    // The mask admits 0-63 and RFC-0004 §B defines 0-3, so this records whether the cast
    // above produced a real enumerator. Recording it is not the same as rejecting it here:
    // a FORWARDER must stay opcode-agnostic (an intermediate hop routes on dst and never
    // switches on op), so the reject belongs at the TERMINUS, where a return route has been
    // captured and the peer can be told. `resolve_node` is where that happens.
    p.op_defined = opcode <= static_cast<std::uint8_t>(fwd_op_t::REPLY);
    p.mint_request = (op_byte & kFwdOpFlagMintRequest) != 0;

    std::optional<N> dst = ch.next();
    // Two address forms, and the second changes nothing about the first (RFC-0024 §1): a
    // canonical PATH of packed segment records (RFC-0018), or a PATH_REF whose body shape the
    // grammar has already settled (path_ref.hpp — PL=0, LL=0, a whole number of 8-byte
    // elements, at or under the count bound). What an element MEANS is settled at the deref, in
    // resolve_node.
    if (!dst) return std::unexpected(status_t::INVALID_PATH);
    if (dst->type() == type_t::PATH_REF) {
        p.dst_bound = true;
    } else if (dst->type() != type_t::PATH) {
        return std::unexpected(status_t::INVALID_PATH);
    }
    p.dst = *dst;

    std::optional<N> next = ch.next();
    if (next && next->type() == type_t::FIELD) {
        p.selector = *next;
        next = ch.next();
    }
    if (!next || next->type() != type_t::PATH) return std::unexpected(status_t::INVALID_PATH);
    p.src = *next;

    std::optional<N> tail = ch.next();
    if (p.op == fwd_op_t::WRITE) {
        // A mint-flagged request's LAST child may be the reverse-direction list, and it is
        // told from the payload by its OWN TYPE — `PATH_REF_REVERSE` (`0x15`), never by
        // position (RFC-0024 §7.1 amendment 2). A WRITE's payload is therefore whatever
        // stands here as long as it is not that type, INCLUDING a raw `PATH_REF` VALUE:
        // amendment 1's positional reading foreclosed that shape, and amendment 2 gives it
        // back at zero cost, because this compare was always a compare.
        if (tail && tail->type() != type_t::PATH_REF_REVERSE) {
            p.payload = *tail;
            tail = ch.next();
        }
    } else if (p.op == fwd_op_t::AWAIT) {
        if (tail && tail->type() == type_t::VALUE) {
            p.await_timeout = detail::load_le<std::uint64_t>(tail->body());
            tail = ch.next();
        }
    }
    // The reverse list rides ONLY a mint-flagged request (§7.1 amendment 1); on an unflagged
    // frame a trailing `PATH_REF_REVERSE` is not licensed and stays unparsed. The flag gate
    // is kept even though the type alone is now unambiguous: the amendment licenses the child
    // on a mint-flagged request and nowhere else, and honouring an unlicensed one would bind
    // a route no hop promised to have contributed to.
    if (p.mint_request && tail && tail->type() == type_t::PATH_REF_REVERSE) p.reverse = *tail;
    return p;
}

/** @brief FIELD index_mode (RFC-0004 §C, the optional u8 index_mode VALUE). */
enum class index_mode_t : std::uint8_t { SCALAR = 0, ELEMENT = 1, WILDCARD = 2 };

/**
 * @brief Decode a FIELD selector node into the graph's field_path_t, refusing a wildcard the
 *        terminus does not serve.
 *
 * Each level is a NAME followed by 0/1/2 VALUE children: 0 => SCALAR; 1 => index_mode only
 * (ELEMENT append "[]" or WILDCARD "[*]"); 2 => [index u32, index_mode u8] ("[N]").
 *
 * The VALUEs are shifted through a two-slot register as they are read, so the LAST one read
 * is always the index_mode and the one before it the index: zero VALUEs leave both at 0
 * (SCALAR, no index) and one leaves the index at 0. One read loop then serves all three
 * shapes instead of one arm per shape.
 *
 * The `[*]` deferral lives here, with the decode that sees it: a WILDCARD level is served only
 * under a `:subscribers` head and answers INVALID_PATH anywhere else, as a malformed index_mode
 * does.
 */
template <class N>
[[nodiscard]] result_t<field_path_t> selector_to_field(const N& field) {
    field_path_t fp;
    auto ch = field.children();
    std::optional<N> cur = ch.next();
    while (cur) {  // one level per NAME + its 0/1/2 trailing VALUEs
        if (cur->type() != type_t::NAME) return std::unexpected(status_t::INVALID_PATH);
        field_step_t& step = fp.steps.emplace_back();
        step.name.assign(detail::as_string_view(cur->body()));
        // vals[1] is the index_mode, vals[0] the u32 index; each VALUE read shifts in from the
        // right. The u8 index_mode is the low byte of its u32 load (`load_le` is LE and reads
        // the absent bytes as zero), so one load width serves both.
        std::array<std::uint32_t, 2> vals{};
        std::size_t n_vals = 0;
        std::optional<N> next = ch.next();
        while (n_vals < 2 && next && next->type() == type_t::VALUE) {
            vals[0] = vals[1];
            vals[1] = detail::load_le<std::uint32_t>(next->body());
            ++n_vals;
            next = ch.next();
        }
        const bool has_index = n_vals == 2;
        const auto index = static_cast<std::uint16_t>(vals[0]);  // 0 when absent: the default
        switch (static_cast<index_mode_t>(static_cast<std::uint8_t>(vals[1]))) {
            case index_mode_t::ELEMENT:
                step.indexed = true;
                step.index = index;
                step.append = !has_index;
                break;
            case index_mode_t::WILDCARD:
                // Deferred everywhere but a `:subscribers` read: the first level names it.
                if (fp.steps[0].name != "subscribers")
                    return std::unexpected(status_t::INVALID_PATH);
                step.indexed = true;
                step.wildcard = true;
                break;
            case index_mode_t::SCALAR:
                step.indexed = has_index;
                step.index = index;
                break;
            default:
                // A wire index_mode byte outside {SCALAR,ELEMENT,WILDCARD} is malformed.
                // Without this the switch would fall through and silently DROP the decoded
                // index (step keeps its non-indexed defaults) — reject it, matching the
                // kMaxFieldDepth guard below and the sibling INVALID_PATH sites (#437).
                return std::unexpected(status_t::INVALID_PATH);
        }
        if (fp.steps.size() > kMaxFieldDepth) return std::unexpected(status_t::INVALID_PATH);
        cur = std::move(next);  // the lookahead item is the next level's NAME (or end)
    }
    return fp;
}

/** @brief True for a whole-array ":subscribers[]" read (vs. a single "[N]" slot). */
[[nodiscard]] bool is_subscribers_array(const field_path_t& fp) noexcept {
    return fp.steps.size() == 1 && fp.steps[0].name == "subscribers" &&
           (fp.steps[0].append || (!fp.steps[0].indexed && !fp.steps[0].wildcard));
}

/**
 * @brief True for the subscribe form specifically — a ":subscribers[]" APPEND (a new edge),
 *        distinct from a ":subscribers[N]" clear (unsubscribe) or the whole-array read.
 */
[[nodiscard]] bool is_subscribe_append(const field_path_t& fp) noexcept {
    return fp.steps.size() == 1 && fp.steps[0].name == "subscribers" && fp.steps[0].append;
}

/**
 * @brief The one ADR-0041 §2 ownership copy of a whole TLV into a fresh owned segment: the reader's
 *        trailer-excluded `own_wire` (span tier copies its borrowed bytes; rope tier adopts a
 *        multi-link flatten, ADR-0053 ⑤) with the copied opt byte's trailer bits cleared (§4) — the
 *        stored TLV is trailer-less at rest and self-consistent.
 *
 * The opt patch lives here, ONE locus for both readers.
 */
template <class N>
[[nodiscard]] view::view_t own_tlv(const N& node, mem::mem_backend_t& flat) {
    view::view_t v = node.own_wire(flat);  // owned, trailer-excluded; empty view on alloc failure
    if (!v.empty()) v.owner->bytes[1] = struct_opt(v.owner->bytes[1]);
    return v;
}

/**
 * @brief True iff the node's opt byte carries NO trailer bits — the reference implementation's
 *        ADR-0042 §3 restriction: a referenced store cannot patch the opt byte in a shared frame,
 *        so only an already-trailer-less payload may be referenced; a CRC/TS-carrying payload falls
 *        back to the trailer-sliced copy.
 */
template <class N>
[[nodiscard]] bool trailer_less(const N& node) noexcept {
    const opt_t o = node.opt();
    return !o.ts && !o.cr && !o.cw && !o.tf;
}

/**
 * @brief A stored WRITE value as the terminus hands it to `graph_t::write`: the rope, and —
 *        on the copy arm — the reference that keeps the inline value it names alive until
 *        the store has adopted it.
 *
 * The rope over an inline value's bytes pins the BLOCK (through the embedded segment) but not
 * the VALUE; `value_t::make` adopts the value only while a reference is still held, so the
 * terminus holds one across the write. On the share arm @ref inline_value is empty.
 */
struct stored_tlv_t {
    view::rope_t rope;        /**< @brief What `graph_t::write` stores (empty ⇒ BACKPRESSURE). */
    value_ref_t inline_value; /**< @brief The copy arm's value, held across the write. */
};

/**
 * @brief The copy arm: the trailer-excluded whole TLV copied into ONE inline `value_t` block
 *        drawn from @p source (RFC-0028 §5.1), its opt byte's trailer bits cleared (§4 — the
 *        stored TLV is trailer-less at rest and self-consistent).
 *
 * A reader that cannot copy into a contiguous host span (a rope-tier payload with a DEVICE
 * link) takes the ADR-0041 §2 `own_tlv` copy through @p flat instead — the pre-slice-5 shape,
 * and a second block. Exhaustion is the empty rope either way.
 */
template <class N>
[[nodiscard]] stored_tlv_t copy_tlv(const N& node, mem::block_source_t& source,
                                    mem::mem_backend_t& flat) {
    instrument::tick_copy();
    const std::size_t n = node.wire_size();
    value_ref_t v = value_ref_t::adopt(value_t::make_inline(n, source));
    if (!v) return {};
    const std::span<std::byte> out = const_cast<value_t*>(v.get())->inline_bytes();
    if (!node.copy_wire_into(out)) return {view::rope_t(own_tlv(node, flat)), value_ref_t{}};
    out[1] = struct_opt(out[1]);
    view::rope_t r;
    r.append(v->only());  // a segment reference: the store adopts `v` itself through it
    return {std::move(r), std::move(v)};
}

/**
 * @brief The RFC-0028 §5.3 copy-or-share decision for a stored WRITE value (D3): SHARE the
 *        payload as a subrope of the owning delivery (refcount, zero copy) iff it is at least
 *        @p threshold bytes, trailer-less, AND the reader can share (`pin_wire`) — the span
 *        tier shares a subview of the contiguous owning `frame_view`, the rope tier a subrope of
 *        its own scatter-gather segments. Otherwise the copy arm, @ref copy_tlv: one block.
 *
 * @param threshold The target vertex's `share_threshold_bytes` (`config_t::kShareThresholdBytes`
 *                  unless declared). `0` shares whatever can be shared; `SIZE_MAX` never shares.
 *
 * The measured variable is the absolute payload size (RFC-0028 R6), so that is the whole
 * predicate. What sharing HOLDS — the whole receive segment, for the value's lifetime — is the
 * retention hazard the threshold exists to price, and it is the consumer's number: a pool-backed
 * deployment sets its threshold with its pool geometry in mind (RFC-0022 Amendment 2).
 *
 * The test lives HERE, one locus for both readers; each reader only produces its shared rope or
 * its copy. Returns a rope so a multi-link shared payload keeps its segments.
 */
template <class N>
[[nodiscard]] stored_tlv_t share_or_copy_tlv(const N& node, const view::view_t* frame_view,
                                             std::size_t threshold, mem::block_source_t& source,
                                             mem::mem_backend_t& flat) {
    if (node.wire_size() >= threshold && trailer_less(node)) {
        if (std::optional<view::rope_t> shared = node.pin_wire(frame_view)) {
            instrument::tick_pin();
            return {std::move(*shared), value_ref_t{}};
        }
        instrument::tick_refused();
    }
    return copy_tlv(node, source, flat);
}

/**
 * @brief A kind=RESULT reply whose payload children are a stored rope value's links (ADR-0053 §6):
 *        a single-link value contributes one payload child (the trivial case, identical to a view
 *        read); a multi-link stored value ropes ALL its links into the reply zero-copy — no
 *        flatten.
 */
[[nodiscard]] view::rope_t assemble_result_rope(const reply_route_t& route, const value_t& payload,
                                                mem::mem_backend_t& egress,
                                                std::span<const std::byte> trailing = {}) {
    // The links span feeds assemble directly — no heap copy of the link table. The
    // old std::vector staging copy was a per-reply transient that scaled with the
    // stored value's link count and ABORTED on heap exhaustion under -fno-exceptions
    // (the throwing std::allocator has no failure path on an MCU) — observed as an
    // OOM abort on a composed-root read's ~288-link reply. `payload` outlives the
    // call, so borrowing its span is safe.
    return assemble_reply(route, reply_kind_t::RESULT, {}, payload.links(), payload.total_length(),
                          egress, trailing);
}

/**
 * @brief Guard a built SUCCESS reply against a silent drop: an empty rope means the reply
 *        assembly hit OOM (the link-table reserve or the head segment) — turn it into an
 *        ADDRESSED kind=ERROR BACKPRESSURE reply instead of letting the send site drop a
 *        `link_count() == 0` rope with no reply at all.
 *
 * The silent drop looked to a WS client like a dead session (no reply within its deadline)
 * and drove a teardown+redial churn — each redial re-primes the same large composed-root
 * snapshot and re-fails, so the page stays wedged. An addressed BACKPRESSURE lets the client
 * fall back on the SAME link (RFC-0004 §D — a reply shape it already handles). The error tail
 * is a 14-byte single-link frame whose only allocation is a tiny head segment (try_reserve(1)
 * is the inline fast path), so it succeeds on exactly the fragmented heap that could not
 * reserve the large snapshot's link table.
 */
[[nodiscard]] view::rope_t or_backpressure(view::rope_t reply, const reply_route_t& route,
                                           mem::mem_backend_t& egress) {
    if (reply.link_count() == 0) return assemble_error_reply(route, status_t::BACKPRESSURE, egress);
    return reply;
}

/**
 * @brief The vertex-map key for a decoded PATH: the body, span-aliased, ALWAYS (ADR-0041 §3 —
 *        the PATH body IS the key, zero materialization) — after the packed record framing is
 *        checked in **canonical / key** context (RFC-0018 §5.4).
 *
 * @par What RFC-0018 deleted here
 * This function used to have two arms. A `PATH` body was the key only when every child header
 * was exactly `02 00 <u16 len>`; otherwise the segments were RE-EMITTED into a caller-supplied
 * `fallback` vector, because a legal peer could spell the same address with `opt.LL = 1` or a
 * per-segment trailer and a raw byte key would then miss (ADR-0062 §"Considered options").
 * The re-emit arm is what carried #436: `wire::emit_name` ran over every child body regardless
 * of type, so an illegal `PATH{VALUE "sensor"}` was silently rewritten into the key of the
 * legal `PATH{NAME "sensor"}` and resolved `/sensor`, returning the stored value where an
 * error was owed — two byte-different PATHs addressing one vertex, against the injectivity
 * `reference/02` depends on. A packed body has no per-segment option byte and no per-segment
 * type byte, so there is exactly one spelling per address: **both** the fallback and the
 * mistype it enabled are structurally gone, and the span-alias is guaranteed rather than
 * tested. The `fallback` parameter went with them.
 *
 * @par What survives, and why it is here rather than at the frame gate
 * The `len == 0` ESCAPE (RFC-0018 §5.4 Amendment 1) is admissible in a frame path and
 * REJECTED in key context — a label is not canonical bytes, and admitting one would put a
 * non-string record inside a vertex-map key, where `key_view_t`'s
 * byte-prefix-implies-ancestor invariant is stated. So the check runs HERE, at the moment a
 * path becomes a key, not at the door: a forwarder that only relays the same frame steps over
 * the escape and never reaches this function.
 *
 * @retval INVALID_PATH The body does not tile into literal packed records — a ragged length,
 *         or an escape record in key context.
 */
template <class N>
[[nodiscard]] result_t<std::span<const std::byte>> path_lookup_key(const N& path) {
    const std::span<const std::byte> body = path.body();
    if (!wire::packed_path_valid_key(body)) return std::unexpected(status_t::INVALID_PATH);
    return body;
}

/**
 * @brief Apply a parsed request FWD's op at an ALREADY-RESOLVED vertex and build the reply.
 *
 * The one body both address forms reach (RFC-0024 §6.3): a canonical `dst` resolves to a
 * `vertex_handle_t` by key, a bound `dst` dereferences to one by element, and from here the
 * two are the SAME code — the same `graph_t` call, with the same right, the same caller
 * context, the same reply assembly. That is what makes the outcomes identical by
 * construction rather than by two policies kept in sync, and it is where the per-operation
 * ACL re-check RFC-0024 §6.2 requires actually happens: `graph_t::read` / `write` / `await`
 * evaluate `acl_allows` at @p v themselves, so neither spelling can skip a gate the other
 * takes and a generation match authorizes nothing.
 *
 * Also the one place a bound path is MINTED (RFC-0024 §7). The mint rides an operation that
 * has already passed every gate on its way here, so a vref is never produced for a
 * destination the caller could not have reached canonically — probing the bound form yields
 * exactly what probing the canonical form yields (§6.1's anti-enumeration property).
 */
template <class N, class ReplyError>
[[nodiscard]] result_t<view::rope_t> apply_op(
    graph_t& graph, const parsed_fwd_t<N>& req, vertex_handle_t v, std::string_view inbound_link,
    std::string_view subject, const view::view_t* frame_view, mem::mem_backend_t& flat,
    mem::mem_backend_t& egress, mem::mem_backend_t& retained, const reply_route_t& route,
    const ReplyError& reply_error, const field_path_t& field,
    op_resolver_t::reverse_ref_fn_t reverse_ref_fn, void* reverse_ref_ctx,
    op_resolver_t::path_label_fn_t path_label_fn, void* path_label_ctx,
    link_token_seam_t link_token, await_defer_seam_t await_defer) {
    // The mint answer (RFC-0024 §7.5): this node's own reference to the target vertex, as a
    // one-element `PATH_REF` the origin stacks under whatever it already holds for the hops
    // in front of it. 4 + 8 bytes, on the reply only, and only when asked — the request side
    // costs zero bytes, because the ask is a spare bit of an `op` byte that was already there.
    //
    // It rides SUCCESS alone. Every `assemble_error` below leaves it off, which is the
    // anti-enumeration property in code: a denied operation answers denied and nothing else,
    // never "denied, and here is a handle to the thing you may not have".
    //
    // `vertex_slot` declining is not an error either: a saturated generation makes a vertex
    // permanently unbindable (§4.4 rule 3), and the answer to that is the ordinary reply plus
    // an origin that stays on the canonical form, which always works.
    //
    // The element is written STRAIGHT into the stack buffer. It has a fixed 12-byte shape, so
    // there is nothing for a container to size — and a growing one here would abort the node
    // under `-fno-exceptions` on a fragmented heap (#748's precedent, this very function),
    // which is the opposite of the degrade this path promises: a mint that cannot happen is
    // answered with the plain reply, never with a panic.
    //
    // `vertex_slot` returns the index and the generation TOGETHER, from one lock hold. Read
    // as two calls they can straddle a retire, and the reply would then carry a well-formed
    // element naming the vertex's SUCCESSOR — the origin believing it bound the vertex its
    // operation actually reached.
    //
    // A labelled `dst` arrives here with `mint_request` already cleared: `resolve_node` decided
    // that once, at the edge (RFC-0027 §11.2's second clause).
    //
    // The ask also switches the label mint off, here and without a branch of its own: §11.2's
    // mutual exclusion. A mint-flagged request is asking for one compression of this address,
    // so it is never handed the other on the same frame.
    std::array<std::byte, wire::path_ref_wire_bytes(1)> mint_buf{};
    std::span<const std::byte> mint;
    if (req.mint_request) {
        path_label_fn = nullptr;
        if (const std::optional<vertex_slot_t> slot = graph.vertex_slot(v)) {
            wire::emit_path_ref_head(
                mint_buf, wire::type_t::PATH_REF,
                wire::path_ref_element_t{.index = slot->index, .generation = slot->generation}, 0);
            mint = std::span<const std::byte>(mint_buf);
        }
    }

    // RFC-0027 §6.1 point 3 — THE TERMINUS'S OWN REWRITE: *"the terminus does the same for the
    // residual it resolved"*. The reply's `src` IS the request's `dst` (`reply_route_t`'s
    // pre-swap), and the request's `dst` at a terminus IS the residual every forwarding hop
    // left it — so this node's local part and the region the rewrite lands in are the same
    // bytes, and the rewrite is a SUBSTITUTION of that whole region. §6.1's *"replaces, never
    // appends"* is therefore literal here rather than accounted for (erratum 2's forwarder
    // reading): the label stands where the string stood and the frame gets shorter.
    //
    // It is a LAMBDA and not a value because §8.1 is not negotiable: minting spends a table
    // slot, so it must not happen until the operation's own ACL gate has answered. RFC-0024's
    // `mint` above may be computed eagerly precisely because it spends nothing — a vertex slot
    // is read, not allocated — and it rides success by being ATTACHED only on the success arms.
    // A label cannot take that shape, so the call sites below invoke this after their
    // `graph_t` call succeeded, and never on an error arm. Exactly one arm runs per call, so
    // "at most once per resolved operation" needs no memo.
    constexpr std::size_t kPathHeadBytes = 4;  // type, opt, u16 LE length — the short envelope
    std::array<std::byte, kPathHeadBytes + wire::kPathLabelRecordBytes> label_buf{};
    const auto labelled_route = [&]() -> reply_route_t {
        // A null @p path_label_fn is the whole off switch, and every reason for it was settled
        // before this lambda can run: no injected table, a `dst` already compressed (a
        // `PATH_REF`, or a label `resolve_node` dereferenced), or a mint-flagged request (the
        // mint block above). There is no runtime flag here that could be forgotten.
        if (path_label_fn == nullptr) return route;
        // What the label ALIASES: this node's own reference to the vertex the residual
        // resolved to, read as ONE pair under one lock hold (`vertex_slot`'s whole contract —
        // an index without the generation current when it was read names a slot, not a
        // vertex). It is the identical element RFC-0024's bind mint hands back, which is what
        // lets the deref side be one implementation rather than two.
        const std::optional<vertex_slot_t> slot = graph.vertex_slot(v);
        if (!slot) return route;  // a saturated generation ⇒ unbindable ⇒ the string spelling
        const std::span<const std::byte> rec = path_label_fn(
            path_label_ctx, inbound_link,
            wire::path_ref_element_t{.index = slot->index, .generation = slot->generation});
        // An empty answer is the CONFORMANT default and covers every refusal at once (§6.3):
        // no injected table, a peer at its §8.3 ceiling, a table at capacity, a bus child this
        // car does not label. The part stays a string and nothing on the route notices.
        if (rec.size() != wire::kPathLabelRecordBytes) return route;
        // A packed `PATH` whose whole body is that one element: `opt` is 0 (PL=0 — a packed
        // body is opaque to the codec, RFC-0018) and the length is the record's, written LE
        // into the 4-byte header the reply builder then copies verbatim.
        label_buf[0] = static_cast<std::byte>(std::to_underlying(type_t::PATH));
        label_buf[1] = std::byte{0};
        label_buf[2] = static_cast<std::byte>(wire::kPathLabelRecordBytes & 0xFFu);
        label_buf[3] = static_cast<std::byte>((wire::kPathLabelRecordBytes >> 8) & 0xFFu);
        std::memcpy(label_buf.data() + kPathHeadBytes, rec.data(), rec.size());
        return reply_route_t{.dst_wire = route.dst_wire,
                             .src_wire = std::span<const std::byte>(label_buf),
                             .echo_ts = route.echo_ts};
    };

    // `resolve_node` refused REPLY and every undefined opcode before the call, so `op` is one
    // of the three request opcodes: READ, then WRITE, and AWAIT is what is left.
    if (req.op == fwd_op_t::READ) {
        if (is_subscribers_array(field)) {
            result_t<std::vector<view::view_t>> subs = graph.read_subscribers(v, subject);
            if (!subs) return assemble_error_reply(route, subs.error(), egress);
            std::size_t sub_len = 0;
            for (const view::view_t& s : *subs) sub_len += s.length;
            // PL=1 wrapper (POINT) whose children are the slot SUBSCRIBER views,
            // roped on zero-copy. POINT is the structured introspection-result
            // container already used for :schema and vertex enumeration.
            std::array<std::byte, 6> wrapper;
            const bool wll = sub_len > 0xFFFFu;
            emit_cursor_t wout{wrapper.data()};
            wout.struct_header(type_t::POINT, wll, sub_len);
            const reply_route_t ok = labelled_route();
            return or_backpressure(
                assemble_reply(ok, reply_kind_t::RESULT,
                               std::span<const std::byte>(wrapper.data(), wout.p), *subs, sub_len,
                               egress, mint),
                ok, egress);
        }
        // One read type (RFC-0028 D11): a `:field` read composes a value, a plain value
        // read hands back a REFERENCE to the published one, and both arrive as a
        // `value_ref_t` the reply assembly reads without copying.
        result_t<value_ref_t> r = graph.read(v, field, subject);  // empty field: the value
        if (!r) return assemble_error_reply(route, r.error(), egress);
        // The composed-root case: graph.read may SUCCEED (a folded ~hundreds-of-links
        // snapshot) yet the reply's own link-table reserve fail on the fragmented heap.
        // or_backpressure keeps that from becoming a silent drop (the dead-web-ui bug).
        const reply_route_t ok = labelled_route();
        return or_backpressure(assemble_result_rope(ok, **r, egress, mint), ok, egress);
    }
    if (req.op == fwd_op_t::WRITE) {
        // The WRITE arm's refusals go through @p reply_error, `resolve_node`'s one refusal
        // channel, which is silent when no reply was requested. RFC-0004 Amendment 2 (#1502):
        // an unacknowledged write's failures are DROPPED, not answered, and that is not a new
        // drop policy — it is the one a denied `COMPACT` delivery has always run under
        // (reference/05 §route-handle, the #974 ruling: *"a denied delivery is dropped like
        // any other unwritable one"*). One channel, so no arm can answer a route that is not
        // there.
        if (!req.payload.has_value()) return reply_error(status_t::TYPE_MISMATCH);
        const N& payload_node = *req.payload;

        // A remote subscribe — a `:subscribers[]` APPEND that arrived over a
        // transport (inbound_link set) carrying a SUBSCRIBER — binds a REMOTE
        // subscriber instead of a local fan-out edge (#136); its stored views
        // (source SUBSCRIBER + return route) are subscription-scoped and keep
        // the ADR-0041 one-copy behavior unconditionally (ADR-0042 §3 applies
        // to the value store only).
        const bool remote_sub = !inbound_link.empty() && is_subscribe_append(field) &&
                                payload_node.type() == type_t::SUBSCRIBER;

        // The remote-subscribe binding: its stored views (source SUBSCRIBER + the
        // accumulated return route) are subscription-scoped and keep the ADR-0041 §2
        // one-copy behavior unconditionally (ADR-0042 §3 pinning applies to the value
        // store only). The slot retains `src` (copied once, trailer-sliced) + the
        // inbound link so the producer fan-out delivers FWD{WRITE}/COMPACT home. A
        // wire TLV is never empty, so an empty copy is exactly an allocation failure
        // ⇒ BACKPRESSURE.
        if (remote_sub) {
            // A SUBSCRIBE is a standing request for future frames, so "no reply
            // requested" and "subscribe" are contradictory asks on one frame: the edge
            // this would bind has the empty route as its `target`, and every delivery
            // down it would be a `FWD{WRITE, dst=<empty>}` — the unroutable frame this
            // very amendment exists to stop, emitted forever instead of once. It is
            // MALFORMED for the same reason an empty-src READ is (RFC-0004 Amendment 2),
            // and takes the same terminus drop.
            if (req.no_reply) return std::unexpected(status_t::INVALID_PATH);
            const view::view_t sub_value = own_tlv(payload_node, retained);
            if (sub_value.empty())
                return assemble_error_reply(route, status_t::BACKPRESSURE, egress);
            // The ONE route copy of the subscription's life (ADR-0041 §2), into a
            // refcounted segment — every later delivery clones the refcount.
            const view::view_t return_route = own_tlv(req.src, retained);
            if (return_route.empty())
                return assemble_error_reply(route, status_t::BACKPRESSURE, egress);
            // The responder's COMPLETION of the reverse-direction list (RFC-0024 §7.1
            // amendment 1): the list arrives one element short — the hop into this node
            // is the one no peer can mint for it — so element 0 becomes this node's own
            // reference to the connection vertex the subscribe arrived on, supplied by
            // the injected transport-plane seam. Every failure degrades to the
            // canonical-only subscription (an EMPTY reverse view), never to an error:
            // the reverse binding is an optimisation plus a liveness check, and a
            // subscribe that cannot bind it still subscribes exactly as before.
            view::view_t reverse_route{};
            if (req.reverse && reverse_ref_fn != nullptr) {
                const std::span<const std::byte> rbody = req.reverse->body();
                const std::size_t n = wire::path_ref_element_count(rbody.size());
                // The grammar settled the body's shape on both tiers (a whole number of
                // elements, at or under the count bound), and a refused flatten reads as an
                // empty body, so the count alone decides: at least one hop element, and room
                // for this node's own in front of them.
                if (n >= 1 && n + 1 <= wire::kMaxPathRefElements) {
                    const std::optional<wire::path_ref_element_t> own =
                        reverse_ref_fn(reverse_ref_ctx, inbound_link);
                    if (own) {
                        // One owned segment for the subscription's life (the ADR-0041
                        // §2 shape `return_route` uses one field over): a fresh 4-byte
                        // header, this node's element, then the hops' elements verbatim.
                        //
                        // Headed `PATH_REF` (`0x14`), not `PATH_REF_REVERSE`: the stored
                        // form is an ADDRESS at rest — every delivery consumes element 0
                        // locally and puts elements 1.. on the wire as the delivery's
                        // bound `dst`, which is a `PATH_REF` by definition. `0x15` names
                        // the accumulating list on a request in flight, and this blob
                        // never travels in that role.
                        view::segment_ptr_t seg = view::segment_alloc(
                            flat, 4u + wire::kPathRefElementBytes + rbody.size());
                        if (seg) {
                            const std::span<std::byte> out = seg->bytes;
                            wire::emit_path_ref_head(out.first<wire::path_ref_wire_bytes(1)>(),
                                                     wire::type_t::PATH_REF, *own, rbody.size());
                            std::memcpy(out.data() + wire::path_ref_wire_bytes(1), rbody.data(),
                                        rbody.size());
                            reverse_route = view::view_t::over(std::move(seg));
                        }
                    }
                }
            }
            // ADR-0049: the wire append enters the graph's single admission door
            // (subscribe_wire → admit_subscriber) — the SUBSCRIBER TLV is parsed
            // ONCE there (delivery_compact included), so no parallel parse here.
            // The link is WHERE this edge delivers; the subject is WHO subscribed
            // (ADR-0082). They are the same string for every caller that supplied no
            // peer handle, and differ exactly when the terminus derived a per-writer
            // subject — which is what makes a FLAT listener's peers distinguishable
            // without making any of them individually routable. The subject is passed
            // as it is: `subscribe_wire` already falls back to the link for an empty
            // one, so blanking a subject equal to the link here only made it rebuild
            // the same string.
            // The carried link token (#1417), asked for HERE and only here — lazily, at
            // the one branch that can use it. `subject_for` is resolved once per resolve
            // because every op needs a subject; a token is needed by remote SUBSCRIBE
            // alone, and a control-plane saving charged to every terminus frame is the
            // mistake #1290's prototype was killed for.
            result_t<void> w = graph.subscribe_wire(
                v, sub_value, return_route, std::string(inbound_link), std::move(reverse_route),
                std::string(subject), link_token.ask());
            if (!w) return assemble_error_reply(route, w.error(), egress);
            const reply_route_t ok = labelled_route();
            return or_backpressure(
                assemble_reply(ok, reply_kind_t::RESULT, {}, {}, 0, egress, mint), ok,
                egress);  // OK, empty payload
        }

        // The stored written value, by the vertex's copy-or-share threshold (RFC-0028
        // §5.3): below it, ONE inline block holding a trailer-sliced copy (§4 — an
        // arriving CRC/TS trailer is NOT stored; stored TLVs are trailer-less at rest,
        // ADR-0035); at or above it, an ADR-0042 §3 shared subrope of the frame
        // (refcount, zero copy; multi-link on the rope tier). An empty rope is an
        // allocation failure.
        const stored_tlv_t value = share_or_copy_tlv(
            payload_node, frame_view, graph.share_threshold_bytes(v), graph.value_source(), flat);
        if (value.rope.total_length() == 0) return reply_error(status_t::BACKPRESSURE);

        // The arrival link's catalog identity (#1650) rides the token seam the walk already
        // carries into this frame — no parameter of its own, no lookup, no branch.
        result_t<void> w = graph.write(v, field, value.rope, subject, link_token.link_kind);
        // RFC-0004 Amendment 2's whole effect, in one line: the write ran (or was
        // refused by the ACL, or failed) and the terminus stays silent either way. The
        // origin loses per-write backpressure feedback — `or_backpressure` never runs on
        // this path — which is inherent to an unacknowledged flow and opt-in by wiring an
        // empty `src`; the application's own sequence counter is its loss detector.
        if (req.no_reply) return view::rope_t{};
        if (!w) return assemble_error_reply(route, w.error(), egress);
        const reply_route_t ok = labelled_route();
        return or_backpressure(assemble_reply(ok, reply_kind_t::RESULT, {}, {}, 0, egress, mint),
                               ok,
                               egress);  // OK, empty payload
    }
    // A FIELD selector has no await surface, and silently dropping it was a lie
    // (#585). The selector is decoded and validated above and was then discarded,
    // so `await <v>:<anything>` behaved exactly like `await <v>` — a peer asking
    // to be woken on one facet was instead woken on the whole vertex, or told
    // `tr::flow::timeout`, which is indistinguishable from a quiet link.
    //
    // RFC-0010 §C settles the direction rather than leaving it open: a field write
    // "does NOT wake `await` on the vertex, does not advance the vertex's write
    // sequence, and does not propagate ... `await` on a single field is
    // deliberately unsupported." Nothing can ever fire such a wait, so answering
    // it is the ENOTTY of an unsupported ioctl -- SCHEMA_NOT_FOUND, the same code
    // READ and WRITE already return for a facet they do not serve (CONTEXT.md
    // §Field-write). This holds for EVERY selector, including `:subscribers` and
    // `:acl`, which read and write fine: the field exists, the await does not.
    //
    // `graph_t::await` takes no field parameter at all, so the local API never
    // offered this -- only the wire path decoded a selector it could not honour.
    //
    // The test is the selector's PRESENCE, not `field.empty()`: a zero-level `FIELD`
    // decodes to an empty path, and answering that as a plain await would be a change
    // of behaviour this refactor does not make.
    if (req.selector.has_value())
        return assemble_error_reply(route, status_t::SCHEMA_NOT_FOUND, egress);
    const std::chrono::nanoseconds timeout(req.await_timeout);
    // ADR-0084: with a deferral sink and a caller that can send a later reply, the
    // wait leaves this thread. Blocking here for `timeout` held the receive context of
    // the link the request arrived on, and every frame queued behind it. The READ gate
    // answers first, and only then may §6.1's label mint spend a slot (§8.1), exactly
    // as on the synchronous arm below.
    if (await_defer.fn != nullptr && await_defer.deferred != nullptr) {
        if (!graph.allows(v, subject, acl_right_t::READ))
            return assemble_error_reply(route, status_t::PERMISSION_DENIED, egress);
        const reply_route_t ok = labelled_route();
        const deferred_await_t d{.vertex = v,
                                 .timeout = timeout,
                                 .subject = subject,
                                 .inbound = link_token.inbound,
                                 .dst = route.dst_wire,
                                 .src = route.src_wire,
                                 .ok_src = ok.src_wire,
                                 .echo_ts = route.echo_ts,
                                 .mint = mint};
        const result_t<void> taken = await_defer.fn(await_defer.ctx, d);
        if (!taken) return assemble_error_reply(route, taken.error(), egress);
        *await_defer.deferred = true;
        return rope_t{};
    }
    result_t<value_ref_t> r = graph.await(v, timeout, subject);
    if (!r) return assemble_error_reply(route, r.error(),
                                        egress);  // TIMEOUT => tr::flow::timeout
    const reply_route_t ok = labelled_route();
    return or_backpressure(assemble_result_rope(ok, **r, egress, mint), ok, egress);
}

/**
 * @brief The payload a creation hook is shown for a `WRITE` whose `dst` missed (RFC-0030 §7.2):
 *        null unless the request is a fieldless data write, which is the only kind that may
 *        create; else the written TLV, held in @p held.
 *
 * Shared from the frame (zero copy) where the frame has an owner; copied from the graph's
 * value source where it is borrowed, once per write. The graph asks only after the parent's
 * `CREATE` gate admitted the writer, so a denied writer provokes no copy. An empty rope is a
 * refused copy (`BACKPRESSURE`).
 */
template <class N>
[[nodiscard]] const view::rope_t* creating_payload(const parsed_fwd_t<N>& req,
                                                   const field_path_t& field,
                                                   const view::view_t* frame_view, graph_t& graph,
                                                   mem::mem_backend_t& flat,
                                                   std::optional<stored_tlv_t>& held) {
    if (!field.empty() || !req.payload) return nullptr;
    // Built once per write: K hooked levels share one copy.
    if (!held) held = share_or_copy_tlv(*req.payload, frame_view, 0, graph.value_source(), flat);
    return &held->rope;
}

/**
 * @brief The ONE templated resolve walk (ADR-0053 §7): apply an @p N-read request FWD against @p
 *        graph and build the FWD{REPLY} rope.
 *
 * Instantiated with `arena_node`
 * (span tier, byte-identical) and — 3c — the `tlv_view_t` reader (owning rope
 * tier). Every frame read goes through the node-reader concept; nothing here
 * names a specific decode representation.
 */
template <class N>
[[nodiscard]] result_t<view::rope_t> resolve_node(
    graph_t& graph, const N& root, std::string_view inbound_link, std::string_view subject,
    const view::view_t* frame_view, mem::mem_backend_t& flat, mem::mem_backend_t& egress,
    mem::mem_backend_t& retained, op_resolver_t::reverse_ref_fn_t reverse_ref_fn = nullptr,
    void* reverse_ref_ctx = nullptr, op_resolver_t::path_label_fn_t path_label_fn = nullptr,
    void* path_label_ctx = nullptr, const wire::path_ref_element_t* dst_label_target = nullptr,
    link_token_seam_t link_token = {}, await_defer_seam_t await_defer = {}) {
    result_t<parsed_fwd_t<N>> parsed = parse_fwd(root);
    if (!parsed) return std::unexpected(parsed.error());
    parsed_fwd_t<N>& req = *parsed;
    if (req.op == fwd_op_t::REPLY) return std::unexpected(status_t::INVALID_PATH);

    // The reply's route is the request's routes swapped: reply dst = request src (the
    // accumulated return route), reply src = request dst (this node's responder
    // endpoint). Their trailer-excluded whole-TLV `wire` bytes feed every assemble
    // below — read once here so the reply builder never reaches back into a specific
    // node model (ADR-0053 §7 node-reader seam). parse_fwd guarantees both PATH nodes.
    reply_route_t route{.dst_wire = req.src.wire(), .src_wire = req.dst.wire()};
    // The wire-time ECHO (#1109): a request whose OUTER FWD carries an absolute (TF=0)
    // stamp gets it echoed verbatim on the reply's own trailer — every reply below, error
    // replies included, so an origin measuring RTT gets its answer whatever the outcome.
    // This REPLACES the old behaviour of clearing every arriving trailer bit: the ADR-0041
    // §4 trailer-slice still governs the route/payload COPIES (their bytes exclude the
    // trailer, so their opt must too — `struct_opt` is unchanged), but the reply frame
    // itself now answers a stamp with a stamp. TF=1 is deliberately not echoed: a root
    // stamp has no ancestor, so a relative form there is the spec's own anchorless
    // MUST-reject case, and echoing it would propagate a meaningless value.
    if (const opt_t root_opt = root.opt(); root_opt.ts && !root_opt.tf)
        route.echo_ts = root.trailer_ts();
    // #766, guard 1 of 2 — the reply's OWN route bytes. On the rope tier these two spans are
    // materialized (a multi-link PATH pays one flatten from the injected backend), so a
    // refusal here leaves no trustworthy address to answer TO: an assemble_error built on a
    // short `src` would put a truncated route on the wire. Answer on the ERROR side instead —
    // by value, which the router turns into a drop. This is also the only guard that can fire
    // before `parse_fwd`'s op byte is acted on, so it comes first.
    if (!req.src.spans_intact()) return std::unexpected(status_t::BACKPRESSURE);

    // RFC-0004 Amendment 2 (#1502, ruling on #1491) — THE EMPTY RETURN ROUTE. A `src` whose
    // body is zero bytes is not a route: it is the origin declining to ask for a reply. The
    // encoding is the EMPTY child and never an omitted one, because this grammar is
    // positional and a WRITE payload may itself be `PATH`-typed (`parse_fwd` above) — the
    // frame shape is therefore untouched and every existing parser still reads it.
    //
    // The read is a body LENGTH, and it cannot be a refused flatten read as "no reply
    // requested" (which would apply the operation in silence, the one outcome an origin cannot
    // detect): `src` was materialized whole for the route above, and guard 1 has just vouched
    // for it.
    req.no_reply = req.src.body().empty();
    // Only an unflagged WRITE may go unacknowledged. A READ or an AWAIT produces a RESULT
    // that has nowhere to go, and a mint-flagged request is asking for a bound path that
    // rides the reply alone (RFC-0024 §7.5) — each is a request for an answer paired with a
    // refusal to receive one, so each is MALFORMED. It is dropped at the TERMINUS and not
    // NACKed, for the reason the whole clause exists: there is no route to carry a NACK.
    // An undefined opcode joins them — #904's addressed `TYPE_MISMATCH` needs an address — and
    // needs no term of its own: a masked opcode outside the four defined values never equals
    // WRITE.
    if (req.no_reply && (req.op != fwd_op_t::WRITE || req.mint_request))
        return std::unexpected(status_t::INVALID_PATH);

    // The pre-dispatch error reply. It never re-labels: guard 2 below answers a refused flatten
    // BACKPRESSURE before any verdict derived from a span read can be given, so an
    // INVALID_PATH from here always blames the peer's frame and never this node's memory
    // state (#766).
    //
    // An unacknowledged request (@ref parsed_fwd_t::no_reply) short-circuits it to the empty
    // rope the router drops: RFC-0004 Amendment 2's drop policy is the same one a denied
    // `COMPACT` delivery already runs under, and the alternative — an addressed error on a
    // zero-length route — is precisely the garbage frame the amendment exists to stop.
    const auto reply_error = [&](status_t s) -> rope_t {
        if (req.no_reply) return view::rope_t{};
        return assemble_error_reply(route, s, egress);
    };

    // An opcode outside the four defined values gets an ADDRESSED error, not a drop (#904).
    //
    // `kFwdOpcodeMask` admits 0-63; RFC-0004 §B defines 0-3. Values 4-63 used to fall through
    // `apply_op`'s caseless switch to its trailing by-value INVALID_PATH, which the router
    // turns into a silent drop (`fwd_router.cpp`, the by-value error arm) — so a peer sending
    // an opcode this build does not implement got NOTHING back, while a malformed selector, a
    // bad path or an unknown vertex all got an addressed ERROR. Silence is the one answer the
    // origin cannot act on: it is indistinguishable from a dead link, so the origin retries
    // the frame this node will never serve instead of falling back.
    //
    // RFC-0024's compatibility note already reads the intended behaviour off as a reject —
    // "a pre-amendment peer sees an unknown opcode and rejects — a clean ERROR, not a
    // mis-execution" — and §9.3's masking rule exists precisely so an unrecognised FLAG
    // degrades to the plain opcode INSTEAD of reaching this reject. Flags stay additive; a
    // genuinely unknown opcode does not.
    //
    // `TYPE_MISMATCH` (wire `tr::schema::type_mismatch`) is the code the format's own
    // forward-extension rule prescribes for the same situation one level up:
    // docs/reference/01-data-format.md §"Handling unknown type codes" — an unimplemented
    // core-range code on the outer addressed TLV answers `ERROR{tr::schema::type_mismatch}`
    // WHEN A RETURN PATH EXISTS. Guard 1 above is exactly that precondition, so the same
    // verdict is spelled the same way here. No new status code, no wire surface added.
    if (!req.op_defined) return reply_error(status_t::TYPE_MISMATCH);

    // Decode the optional :field selector (its `[*]` deferral included). A request without
    // one decodes to the empty path, which is the vertex value itself.
    const result_t<field_path_t> field =
        req.selector ? selector_to_field(*req.selector) : result_t<field_path_t>{field_path_t{}};

    // #766, guard 2 of 2 — the ONE check of everything the walk read before it touches the
    // graph. The selector's names and indices are the last span reads the walk makes (the
    // `dst` body was materialized whole with the route, and guard 1 vouched for it), and on
    // the rope tier a refused flatten answers them EMPTY: an empty selector name addresses the
    // wrong field, and an empty read reported as INVALID_PATH would blame the peer's frame for
    // this node's memory state. So the refusal is answered here, once, for every `dst`
    // spelling, and no later read can be refused. The reply route bytes are known good by
    // guard 1, so this refusal is ADDRESSABLE and answers as the same kind=ERROR BACKPRESSURE
    // an OOM'd reply assembly does (the client falls back on the same link rather than
    // presuming the node dead).
    if (!req.dst.spans_intact()) return reply_error(status_t::BACKPRESSURE);
    if (!field) return reply_error(field.error());

    // The two COMPRESSED spellings of `dst`, merged into one element and one dereference: a
    // labelled `dst` (RFC-0027 §7.2) arrives with the element its label aliases already
    // resolved by the caller (@p dst_label_target), and a bound `dst` (RFC-0024 §5) carries
    // it as its only body element. Neither is a key and neither is looked up — the element is
    // DEREFERENCED, and then the op's own per-operation ACL runs at the dereferenced vertex
    // inside `graph_t::read` / `write` / `await`, exactly as the string form's does (§8.2,
    // RFC-0024 §5.1). One implementation of that sentence cannot drift from itself.
    //
    // The label goes first because a labelled `dst` is a canonical `PATH` by type (`dst_bound`
    // is false for it) whose body `path_lookup_key` would refuse as an escape record in key
    // context. The two compressions are mutually exclusive on the wire (§11.2), so the order
    // only states which one wins if a caller ever supplies both.
    //
    // Exactly one element reaches a terminus: each hop consumes element 0 and forwards the
    // remainder (§4.1). A longer residual is a hop, and a hop needs a LINK, which this tier
    // does not have and must not grow (it is instantiated for a graph with no transports at
    // all); `fwd_router_t::route_bound_forward` consumes it before the frame gets here. A long
    // residual arriving HERE came from a caller that is not the router — a direct resolve, a
    // test, an embedder's own sink — and this node drops it rather than guess which element is
    // its own. An empty residual is a route with no hops, which the codec admits and the router
    // refuses (§9.4 `ref-empty`).
    //
    // Every failure of the compressed arm is a by-value DROP, never a mis-route (§5.3, §7.2):
    // no re-resolution, no nearest match, no fall-through to the canonical walk. The origin
    // still holds the canonical path, and re-resolving it is the origin's recovery, not this
    // node's. (§5.3's NACK carrying the failing hop index is still deferred: §9.2's spelling
    // question is open, and a drop is already conformant.) No creation either: a creation
    // hook decides a missing child by NAME (RFC-0030 §7.2), and an element is not one.
    //
    // The compressions the REPLY may carry are decided here, once. A compressed `dst` is never
    // handed a label mint: §11.2 for the bound form, and for the labelled form §6.1's own
    // arithmetic at its fixed point — the reply's `src` IS the request's `dst`, so that region
    // is already the label, and re-minting would spend a second slot on bytes the echo already
    // carries. A labelled `dst` is not handed a `PATH_REF` mint either, which is §11.2's
    // second clause: *"a host SHOULD NOT bind a `PATH_REF` over a path whose elements are
    // already labelled"*. Its label is live by construction (it just resolved), and its
    // recovery from a stale one is the canonical path it still holds.
    std::optional<vertex_handle_t> v;
    wire::path_ref_element_t bound_elem{};
    const wire::path_ref_element_t* elem = dst_label_target;
    if (elem != nullptr) {
        req.mint_request = false;
    } else if (req.dst_bound) {
        const std::span<const std::byte> elems = req.dst.body();
        if (wire::path_ref_element_count(elems.size()) != 1)
            return std::unexpected(status_t::INVALID_PATH);
        bound_elem = wire::path_ref_element_at(elems, 0);
        elem = &bound_elem;
    }
    if (elem != nullptr) {
        v = graph.deref_vertex_slot(elem->index, elem->generation);
        if (!v) return std::unexpected(status_t::NOT_FOUND);  // stale: retired since the mint
        path_label_fn = nullptr;
    } else {
        // The canonical `dst`: the router's PATH-keyed dispatch, span-aliased (ADR-0041 §3:
        // the frame IS the key). Local-only: a dst naming a transport child or an unknown path
        // is not local => ERROR(NOT_FOUND).
        //
        // A body that does not tile into literal packed records makes the dst unaddressable,
        // not merely unknown: it is a malformed address, so it answers INVALID_PATH rather than
        // NOT_FOUND (#436, and RFC-0018's escape-in-key-context rule). The distinction outlives
        // the write-creates arm this once guarded (#1139): the two refusals carry different
        // dispositions, and a malformed address must not be reported as an address that merely
        // does not exist yet and might on the next retry.
        const result_t<std::span<const std::byte>> dst_key = path_lookup_key(req.dst);
        if (!dst_key) return reply_error(dst_key.error());
        // An unresolved dst answers NOT_FOUND for every op (RFC-0030 §7.1). The one exception is
        // the one RFC-0030 §7.2 makes for every origin alike: a fieldless data WRITE below a
        // parent whose creation hook creates the target. The hook is shown the payload, built
        // only when a hook is about to decide; with the hook policy closed (the default) the
        // graph answers NOT_FOUND without asking for it, so a refusal draws nothing.
        std::optional<stored_tlv_t> shown;
        const result_t<vertex_handle_t> found =
            graph.find_or_create(*dst_key, subject, [&]() -> const view::rope_t* {
                return creating_payload(req, *field, frame_view, graph, flat, shown);
            });
        if (!found) return reply_error(found.error());
        v = *found;
    }
    return apply_op(graph, req, *v, inbound_link, subject, frame_view, flat, egress, retained,
                    route, reply_error, *field, reverse_ref_fn, reverse_ref_ctx, path_label_fn,
                    path_label_ctx, link_token, await_defer);
}

}  // namespace

}  // namespace tr::graph
