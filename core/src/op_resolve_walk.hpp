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
    [[nodiscard]] view_t own_wire(mem::mem_backend_t& flat) const {
        return view::over_bytes(wire(), flat).value_or(view_t{});
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
     * (opt-in, size, trailer-less) is `own_or_ref_tlv`'s — this only produces the rope.
     */
    [[nodiscard]] std::optional<rope_t> pin_wire(const view_t* frame_view) const {
        if (frame_view == nullptr) return std::nullopt;
        const std::span<const std::byte> w = node().wire;
        const std::size_t off = static_cast<std::size_t>(w.data() - frame_view->bytes().data());
        return rope_t(frame_view->subview(off, w.size()));
    }

    /**
     * @brief The bytes a pin would KEEP ALIVE (RFC-0022 §3.D's `segment_bytes`): the owning
     *        segment's ALLOCATED size, not the delivered frame view's length.
     *
     * A subview shares the segment's refcount, so the segment is freed only when the last
     * subview dies — the held quantity is `owner->bytes.size()`, whatever window the transport
     * narrowed the frame to. `udp_transport_t` makes the gap concrete: it receives into a
     * `kMaxDatagram` (64 KB) segment and delivers a length-`n` window, so a 200-byte datagram's
     * payload pins 64 KB. 0 when there is no owning segment (a borrowed, span-delivered frame),
     * which the predicate reads as "cannot pin" without ever consulting the ratio.
     */
    [[nodiscard]] std::size_t segment_bytes(const view_t* frame_view) const noexcept {
        if (frame_view == nullptr || !frame_view->owner) return 0;
        return frame_view->owner->bytes.size();
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
    /** @brief `op` bit 7 was set — the origin asked for a bound-path mint (RFC-0024 §7.5). */
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
    std::uint64_t await_timeout = 0; /**< AWAIT only */
    bool has_await_timeout = false;
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
            p.has_await_timeout = true;
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
 * @brief Decode a FIELD selector node into the graph's field_path_t.
 *
 * Each level is a
 * NAME followed by 0/1/2 VALUE children: 0 => SCALAR; 1 => index_mode only
 * (ELEMENT append "[]" or WILDCARD "[*]"); 2 => [index u32, index_mode u8]
 * ("[N]"). `wildcard_seen` is set if any level carries index_mode=WILDCARD.
 */
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
            default:
                // A wire index_mode byte outside {SCALAR,ELEMENT,WILDCARD} is malformed.
                // Without this the switch would fall through and silently DROP the decoded
                // index (step keeps its non-indexed defaults) — reject it, matching the
                // kMaxFieldDepth guard below and the sibling INVALID_PATH sites (#437).
                return std::unexpected(status_t::INVALID_PATH);
        }
        fp.steps.push_back(std::move(step));
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
[[nodiscard]] view_t own_tlv(const N& node, mem::mem_backend_t& flat) {
    view_t v = node.own_wire(flat);  // owned, trailer-excluded; empty view on alloc failure
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
 * @brief The RFC-0022 §3.D stored-value decision: PIN the payload as a subrope of the owning
 *        delivery (refcount, zero copy) iff `payload_bytes * K >= segment_bytes`, the payload is
 *        trailer-less, AND the reader can pin (`pin_wire`) — the span tier pins a subview of the
 *        contiguous owning `frame_view`, the rope tier a subrope of its own scatter-gather
 *        segments. Otherwise the ADR-0041 §2 one-copy `own_tlv`.
 *
 * @param k The amplification ratio (`config_t::kPinPayloadRatio`, or a per-vertex override while
 *          RFC-0022 §6's measurement runs). @ref tr::graph::kPinNever disables pinning outright
 *          and short-circuits before the segment size is even asked for.
 *
 * @section pin_ratio_why Why a ratio and not an absolute threshold
 *
 * The two branches are asymmetric in RAM, not in correctness: a copy holds the payload, a pin
 * holds the whole owning **segment** for the value's lifetime. An absolute byte threshold
 * (`store_ref_min_bytes`, as this knob was named before RFC-0022 §3.D) prices the payload and
 * never looks at what is held, so a 4 KB payload
 * in a 4 KB frame and the same 4 KB payload in a 256 KB frame — opposite trades — satisfy it
 * identically, and the waste is bounded not at all. The ratio bounds it at `(K-1)x` the payload
 * using two quantities already in hand three lines from the branch.
 *
 * @section pin_ratio_segment What `segment_bytes` is
 *
 * `N::segment_bytes` answers the ALLOCATED size of the segment(s) a pin would keep alive, not
 * the length of the delivered frame view. On a real transport those differ by orders of
 * magnitude: `udp_transport_t` receives every datagram into a `kMaxDatagram`-sized segment and
 * delivers a length-`n` window over it, so pinning a 1 KB datagram's payload holds 64 KB. Pricing
 * the view length instead would measure a cost nobody pays.
 *
 * The eligibility test lives HERE, one locus for both readers; each reader only produces its
 * pinned rope and answers for its own segment shape. Returns a rope so a multi-link pinned
 * payload keeps its segments.
 */
template <class N>
[[nodiscard]] rope_t own_or_ref_tlv(const N& node, const view_t* frame_view, std::uint32_t k,
                                    mem::mem_backend_t& flat) {
    if (k != tr::graph::kPinNever && trailer_less(node)) {
        // 64-bit product: `payload * k` overflows 32 bits at k = pin-always on any real payload,
        // and an overflowed product decides the branch backwards.
        const std::uint64_t payload = node.wire_size();
        const std::uint64_t segment = node.segment_bytes(frame_view);
        if (segment != 0 && payload * std::uint64_t{k} >= segment) {
            if (std::optional<rope_t> pinned = node.pin_wire(frame_view)) {
                LIBTRACER_TICK_PIN();
                return std::move(*pinned);
            }
            LIBTRACER_TICK_PIN_REFUSED();
            return rope_t(own_tlv(node, flat));
        }
    }
    LIBTRACER_TICK_COPY();
    return rope_t(own_tlv(node, flat));
}

/**
 * @brief A kind=RESULT reply whose payload children are a stored rope value's links (ADR-0053 §6):
 *        a single-link value contributes one payload child (the trivial case, identical to a view
 *        read); a multi-link stored value ropes ALL its links into the reply zero-copy — no
 *        flatten.
 */
[[nodiscard]] rope_t assemble_result_rope(const reply_route_t& route, const rope_t& payload,
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
[[nodiscard]] rope_t or_backpressure(rope_t reply, const reply_route_t& route,
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
template <class N>
[[nodiscard]] result_t<rope_t> apply_op(
    graph_t& graph, const parsed_fwd_t<N>& req, vertex_handle_t v, std::string_view inbound_link,
    const view_t* frame_view, mem::mem_backend_t& flat, mem::mem_backend_t& egress,
    const reply_route_t& route, const field_path_t& field, bool has_field,
    op_resolver_t::reverse_ref_fn_t reverse_ref_fn = nullptr, void* reverse_ref_ctx = nullptr,
    op_resolver_t::path_label_fn_t path_label_fn = nullptr, void* path_label_ctx = nullptr) {
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
    std::array<std::byte, wire::path_ref_wire_bytes(1)> mint_buf{};
    std::span<const std::byte> mint;
    if (req.mint_request) {
        if (const std::optional<vertex_slot_t> slot = graph.vertex_slot(v)) {
            const wire::path_ref_element_t e{.index = slot->index, .generation = slot->generation};
            if (wire::emit_path_ref_into(mint_buf,
                                         std::span<const wire::path_ref_element_t>(&e, 1)))
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
        // §11.2's mutual exclusion, structural at the mint site exactly as car 4 made it on the
        // forwarding half. A `PATH_REF` dst is already one compression of this address and a
        // mint-flagged request is ASKING for one, so neither leg may reach the label mint and
        // the two forms never meet on one frame. Both are argument-shaped rather than
        // flag-shaped: there is no runtime switch here that could be forgotten.
        if (path_label_fn == nullptr || req.dst_bound || req.mint_request) return route;
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

    switch (req.op) {
        case fwd_op_t::READ: {
            if (has_field && is_subscribers_array(field)) {
                result_t<std::vector<view_t>> subs = graph.read_subscribers(v, inbound_link);
                if (!subs) return assemble_error_reply(route, subs.error(), egress);
                std::size_t sub_len = 0;
                for (const view_t& s : *subs) sub_len += s.length;
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
                                   std::span<const std::byte>(wrapper.data(), wll ? 6u : 4u), *subs,
                                   sub_len, egress, mint),
                    ok, egress);
            }
            // A `:field` read composes a value and is rope-valued; a plain value read hands
            // back a REFERENCE to the published one. Both feed the same reply assembly, which
            // only ever reads the rope, so bind whichever arrived without copying it.
            result_t<value_ref_t> r = has_field ? [&] {
                auto composed = graph.read(v, field, inbound_link);
                return composed ? result_t<value_ref_t>{value_ref_t::composed(std::move(*composed))}
                                : result_t<value_ref_t>{std::unexpected(composed.error())};
            }()
                                                : graph.read(v, inbound_link);
            if (!r) return assemble_error_reply(route, r.error(), egress);
            // The composed-root case: graph.read may SUCCEED (a folded ~hundreds-of-links
            // snapshot) yet the reply's own link-table reserve fail on the fragmented heap.
            // or_backpressure keeps that from becoming a silent drop (the dead-web-ui bug).
            const reply_route_t ok = labelled_route();
            return or_backpressure(assemble_result_rope(ok, **r, egress, mint), ok, egress);
        }
        case fwd_op_t::WRITE: {
            if (!req.payload.has_value())
                return assemble_error_reply(route, status_t::TYPE_MISMATCH, egress);
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
                const view_t sub_value = own_tlv(payload_node, flat);
                if (sub_value.empty())
                    return assemble_error_reply(route, status_t::BACKPRESSURE, egress);
                // The ONE route copy of the subscription's life (ADR-0041 §2), into a
                // refcounted segment — every later delivery clones the refcount.
                const view_t return_route = own_tlv(req.src, flat);
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
                view_t reverse_route{};
                if (req.reverse && reverse_ref_fn != nullptr) {
                    const std::span<const std::byte> rbody = req.reverse->body();
                    const std::size_t n = wire::path_ref_element_count(rbody.size());
                    if (n >= 1 && rbody.size() % wire::kPathRefElementBytes == 0 &&
                        n + 1 <= wire::kMaxPathRefElements && req.reverse->spans_intact()) {
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
                                if (wire::emit_path_ref_into(
                                        out, std::span<const wire::path_ref_element_t>(&*own, 1))) {
                                    // emit_path_ref_into wrote a 1-element header; widen the
                                    // length to cover the appended hop elements too.
                                    const std::size_t body_len =
                                        wire::kPathRefElementBytes + rbody.size();
                                    out[2] = static_cast<std::byte>(body_len & 0xFFu);
                                    out[3] = static_cast<std::byte>((body_len >> 8) & 0xFFu);
                                    std::memcpy(out.data() + 4 + wire::kPathRefElementBytes,
                                                rbody.data(), rbody.size());
                                    reverse_route = view_t::over(std::move(seg));
                                }
                            }
                        }
                    }
                }
                // ADR-0049: the wire append enters the graph's single admission door
                // (subscribe_wire → admit_subscriber) — the SUBSCRIBER TLV is parsed
                // ONCE there (delivery_compact included), so no parallel parse here.
                result_t<void> w =
                    graph.subscribe_wire(v, sub_value, return_route, std::string(inbound_link),
                                         std::move(reverse_route));
                if (!w) return assemble_error_reply(route, w.error(), egress);
                const reply_route_t ok = labelled_route();
                return or_backpressure(
                    assemble_reply(ok, reply_kind_t::RESULT, {}, {}, 0, egress, mint), ok,
                    egress);  // OK, empty payload
            }

            // The stored written value: ADR-0041 §2 one ownership copy, trailer-sliced by
            // construction (§4 — an arriving CRC/TS trailer is NOT stored; stored TLVs are
            // trailer-less at rest, ADR-0035) — or, when RFC-0022 §3.D's amplification ratio
            // says the payload dominates the segment it would pin, an ADR-0042 §3 pinned
            // subrope of the frame (refcount, zero copy; multi-link on the rope tier). An
            // empty rope is an allocation failure.
            //
            // K comes from the vertex's owner-declared u32 when set and from
            // `config_t::kPinPayloadRatio` otherwise. The per-vertex override exists ONLY so
            // RFC-0022 §6's arms rotate inside ONE process: measuring them as separate
            // binaries is what produced the 2.8x swing on identical code that the standing
            // interleave rule was written against. §3.D's landing form is the config constant
            // alone, and the sentinel is both defaults — so this branch changes no observable
            // behaviour by itself. Whether the override survives at all is a live maintainer
            // question (#774); the name now at least says what the value is.
            const std::uint32_t pin_k = graph.pin_payload_ratio(v) != 0
                                            ? graph.pin_payload_ratio(v)
                                            : tr::graph::kPinPayloadRatio;
            const rope_t value = own_or_ref_tlv(payload_node, frame_view, pin_k, flat);
            if (value.total_length() == 0)
                return assemble_error_reply(route, status_t::BACKPRESSURE, egress);

            result_t<void> w =
                graph.write(v, has_field ? field : field_path_t{}, value, inbound_link);
            if (!w) return assemble_error_reply(route, w.error(), egress);
            const reply_route_t ok = labelled_route();
            return or_backpressure(
                assemble_reply(ok, reply_kind_t::RESULT, {}, {}, 0, egress, mint), ok,
                egress);  // OK, empty payload
        }
        case fwd_op_t::AWAIT: {
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
            if (has_field) return assemble_error_reply(route, status_t::SCHEMA_NOT_FOUND, egress);
            const std::chrono::nanoseconds timeout =
                req.has_await_timeout ? std::chrono::nanoseconds(req.await_timeout)
                                      : kDefaultAwaitTimeout;
            result_t<value_ref_t> r = graph.await(v, timeout, inbound_link);
            if (!r)
                return assemble_error_reply(route, r.error(),
                                            egress);  // TIMEOUT => tr::flow::timeout
            const reply_route_t ok = labelled_route();
            return or_backpressure(assemble_result_rope(ok, **r, egress, mint), ok, egress);
        }
        case fwd_op_t::REPLY:
            break;  // unreachable — handled above
    }
    return std::unexpected(status_t::INVALID_PATH);
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
[[nodiscard]] result_t<rope_t> resolve_node(
    graph_t& graph, const N& root, std::string_view inbound_link, const view_t* frame_view,
    mem::mem_backend_t& flat, mem::mem_backend_t& egress,
    op_resolver_t::reverse_ref_fn_t reverse_ref_fn = nullptr, void* reverse_ref_ctx = nullptr,
    op_resolver_t::path_label_fn_t path_label_fn = nullptr, void* path_label_ctx = nullptr) {
    result_t<parsed_fwd_t<N>> parsed = parse_fwd(root);
    if (!parsed) return std::unexpected(parsed.error());
    const parsed_fwd_t<N>& req = *parsed;
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

    // The pre-dispatch error reply (#766): every "the frame says something illegal" verdict
    // below is derived from a SPAN read, and on the rope tier a refused flatten hands the
    // reader an empty span — which reads as a malformed selector or an unaddressable key.
    // Reporting that as INVALID_PATH would blame the peer's frame for this node's memory
    // state, so a refusal re-labels the verdict BACKPRESSURE (the reply route bytes are known
    // good — guard 1 — so it is addressable either way).
    const auto reply_error = [&](status_t s) {
        return assemble_error_reply(route, req.dst.spans_intact() ? s : status_t::BACKPRESSURE,
                                    egress);
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

    // Decode the optional :field selector and the wildcard deferral: a [*] level
    // on a non-subscriber-path target is rejected with INVALID_PATH.
    field_path_t field;
    const bool has_field = req.selector.has_value();
    if (has_field) {
        bool wildcard = false;
        result_t<field_path_t> f = selector_to_field(*req.selector, wildcard);
        if (!f) return reply_error(status_t::INVALID_PATH);
        field = std::move(*f);
        if (wildcard && (field.steps.empty() || field.steps[0].name != "subscribers"))
            return reply_error(status_t::INVALID_PATH);
    }

    // The BOUND form (RFC-0024 §5). A `PATH_REF` dst is not a key and is not resolved — it is
    // DEREFERENCED. The grammar has already settled the body's shape; what is left is the
    // §5.1 check (bounds, generation, then the op's own per-operation ACL at the dereferenced
    // vertex) and the §5.3 rule that governs every way it can fail.
    //
    // **Failure is a DROP, never a mis-route.** Each `unexpected` below leaves the frame
    // unforwarded and unapplied, which is what the router turns a by-value error into — no
    // re-resolution, no nearest match, no retry against a different vertex. The origin still
    // holds the canonical path the binding was minted from, and re-resolving canonically and
    // re-minting is its recovery, not this node's. (§5.3's NACK carrying the failing hop
    // index is still deferred: §9.2's spelling question is open, and a drop is already the
    // conformant behaviour — the NACK only makes the origin's recovery faster.)
    //
    // **This is the TERMINUS tier, and the forwarder hop is not here.** A residual longer
    // than one element is a hop, and a hop needs a LINK — which this tier does not have and
    // must not grow, because it is instantiated for a graph with no transports at all
    // (`op_resolver_t` is the local op applier). The hop therefore lives one layer out, in
    // `fwd_router_t::route_bound_forward`, which owns the child registry and consumes the
    // element before the frame ever reaches this call. A long residual arriving HERE means it
    // came from a caller that is not the router — a direct resolve, a test, an embedder's own
    // sink — and for that caller the answer is unchanged and correct: this node is not a
    // forwarder for the frame, so it drops it rather than guessing which element is its own.
    if (req.dst_bound) {
        if (!req.dst.spans_intact()) return std::unexpected(status_t::BACKPRESSURE);
        const std::span<const std::byte> elems = req.dst.body();
        // Exactly one element reaches a terminus: each hop consumes element 0 and forwards the
        // remainder (§4.1), so what is left here is the last element — this node's own
        // reference to the target vertex. A longer residual is a hop the router already took
        // (see above); an empty one is a route with no hops, which the codec deliberately
        // admits and the router refuses (§9.4 `ref-empty`).
        if (wire::path_ref_element_count(elems.size()) != 1)
            return std::unexpected(status_t::INVALID_PATH);
        const wire::path_ref_element_t e = wire::path_ref_element_at(elems, 0);
        const std::optional<vertex_handle_t> bound = graph.deref_vertex_slot(e.index, e.generation);
        if (!bound) return std::unexpected(status_t::NOT_FOUND);
        // No write-creates on a bound dst, deliberately: `ensure_vertex` mkdir-p's an ADDRESS,
        // and an element is not one. A vref names a vertex that existed when it was minted, so
        // "it is not there any more" is exactly the stale case the deref just refused.
        return apply_op(graph, req, *bound, inbound_link, frame_view, flat, egress, route, field,
                        has_field, reverse_ref_fn, reverse_ref_ctx, path_label_fn, path_label_ctx);
    }

    // dst resolution is the router's PATH-keyed dispatch — span-aliased for a
    // canonical PATH (ADR-0041 §3: the frame IS the key). Local-only: a dst
    // naming a transport child / unknown path is not local => ERROR(NOT_FOUND).
    // A body that does not tile into literal packed records makes the dst unaddressable, not
    // merely unknown: it is a malformed address, so it answers INVALID_PATH rather than
    // NOT_FOUND (#436, and RFC-0018's escape-in-key-context rule). The distinction outlives
    // the write-creates arm this used to guard (#1139): the two refusals carry different
    // dispositions, and a malformed address must not be reported as an address that merely
    // does not exist yet and might on the next retry.
    const result_t<std::span<const std::byte>> dst_key_r = path_lookup_key(req.dst);
    if (!dst_key_r) return reply_error(dst_key_r.error());
    const std::span<const std::byte> dst_key = *dst_key_r;
    // #766, guard 2 of 2 — everything the walk READ before it touches the graph: the op
    // discriminant, the `:field` selector's names and indices, and the dst lookup key are all
    // span reads, and on the rope tier a refused flatten answers them EMPTY. An empty key
    // finds the wrong vertex (or none); an empty selector name addresses the wrong field. So
    // check once, here, after every pre-dispatch read and before the first graph call — the
    // reply route bytes are known good by guard 1, so this refusal is ADDRESSABLE and answers
    // as the same kind=ERROR BACKPRESSURE an OOM'd reply assembly does (the client falls back
    // on the same link rather than presuming the node dead).
    if (!req.dst.spans_intact()) return reply_error(status_t::BACKPRESSURE);
    const std::optional<vertex_handle_t> found = graph.find(dst_key);
    // An unresolved dst answers NOT_FOUND for EVERY op, the fieldless WRITE included
    // (RFC-0005 amendment 1, #1139). This arm used to write-create: a remote data WRITE
    // mkdir-p'd its target and every missing level above it, consulting no type catalog,
    // counting nothing, bounded by no depth, and — where the graph held no ancestor at all
    // — gated by no ACL, since the CREATE check is on the nearest EXISTING ancestor and a
    // brand-new top-level subtree has none. Creation from a peer now goes through the
    // ADR-0059 creator endpoint, where it is typed, catalogued and ACL-gated; the caller
    // backs off and retries until whoever owns that structure establishes it.
    //
    // The appearance mechanism RFC-0005 §1 hangs on this survives the change, because a
    // create through the creator endpoint IS a write to a vertex and bubbles to the parent
    // subscriber exactly as before — only the ORIGIN of an appearance moves, from "any peer
    // writing any address" to "a create the device's own catalog admitted".
    //
    // The LOCAL `graph_t::write` overload keeps write-creating, deliberately: the in-process
    // caller is the node's own trusted code and owns its graph's structure. The asymmetry is
    // the point of the amendment, not an oversight left in it.
    if (!found) return assemble_error_reply(route, status_t::NOT_FOUND, egress);
    return apply_op(graph, req, *found, inbound_link, frame_view, flat, egress, route, field,
                    has_field, reverse_ref_fn, reverse_ref_ctx, path_label_fn, path_label_ctx);
}

}  // namespace

}  // namespace tr::graph
