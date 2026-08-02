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
#include "libtracer/config.hpp"
#include "libtracer/error.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/op_resolve.hpp"
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
 * @brief The opt byte an ADR-0041 §4 trailer-sliced copy carries: the structural bits (PL, LL)
 *        survive; the trailer bits (TS, CR, CW, TF) are cleared so the copied TLV — whose bytes
 *        exclude the trailer by construction (`node.wire`) — stays self-consistent and trailer-less
 *        at rest.
 *
 * Cleared through `opt_t` (not a raw
 * `& 0x48` mask) so there is one representation of the opt bitfield.
 */
[[nodiscard]] constexpr std::byte struct_opt(std::byte opt_byte) noexcept {
    return static_cast<std::byte>(
        opt_t::decode(std::to_integer<std::uint8_t>(opt_byte)).without_trailer().encode());
}

/**
 * @brief Byte-identical to the retired `& 0x48` mask for any validated opt byte (0x7E = every non-
 *        reserved bit set → PL|LL == 0x48; a plain opaque VALUE 0x00 → 0x00).
 */
static_assert(struct_opt(std::byte{0x7E}) == std::byte{0x48});
static_assert(struct_opt(std::byte{0x00}) == std::byte{0x00});

/**
 * @brief Map an L4 status_t to its registered tr:: error code (RFC-0002 §D registry, wire::err_t) —
 *        the u16 the kind=ERROR reply's ERROR{VALUE} identity carries.
 */
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
    [[nodiscard]] bool canonical_path() const noexcept { return node().canonical_path; }

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
     * CANNOT resolve either shape: identical `origin/main` source compiled at two different
     * build paths differs by +1.7 % to +6.7 % on this leg (12 interleaved rounds, ranges
     * disjoint), which is larger than anything either shape does and has the same
     * frame-size profile. Any cross-worktree A/B of this leg measures code layout. The
     * evidence that the un-injected path is unchanged is the object-file `cmp`, not a
     * stopwatch.
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
     * `kMaxDatagram` (64 KB) segment and delivers `subview(0, n)`, so a 200-byte datagram's
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
    N dst{};                         /**< forward route (a PATH node) */
    std::optional<N> selector{};     /**< optional :field (a FIELD node) */
    N src{};                         /**< accumulated return route (a PATH node) */
    std::optional<N> payload{};      /**< WRITE only (the value node) */
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
 * delivers a `subview(0, n)` of it, so pinning a 1 KB datagram's payload holds 64 KB. Pricing
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
 * @brief A tiny cursor writer over a preallocated, exactly-sized head segment — the direct-emit
 *        that replaces the old 4-stage (encode → children → head → segment) staging: every length
 *        is known from the arena spans up front, so the reply head is ONE allocation and each route
 *        byte is copied exactly once.
 */
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

/**
 * @brief Assemble the FWD{REPLY} rope: one exactly-sized head segment (FWD header + op=REPLY +
 *        dst=req.src + src=req.dst + kind + `inline_tail`) prepended to `shared` (refcount clones
 *        of the stored payload view(s)).
 *
 * The head is the only
 * allocation; the route bytes are copied ONCE (trailer-sliced); `shared` is never
 * copied — RFC-0004 §D / ADR-0035 zero-copy reply rule, ADR-0041 §5 direct emit.
 * @p reply_dst_wire (the request's `src`) and @p reply_src_wire (the request's `dst`,
 * the responder endpoint) are the trailer-excluded whole-TLV `wire` spans of those two
 * PATH nodes — passed in rather than read off the arena so the reply builder is
 * decoupled from the request's node model (ADR-0053 §7: a node-reader-agnostic seam,
 * and where ⑤'s scatter-gather reply emission will hook).
 */
[[nodiscard]] rope_t assemble(std::span<const std::byte> reply_dst_wire,
                              std::span<const std::byte> reply_src_wire, reply_kind_t kind,
                              std::span<const std::byte> inline_tail,
                              std::span<const view_t> shared, std::size_t shared_len) {
    const std::size_t children_len = kU8ValueLen + reply_dst_wire.size() + reply_src_wire.size() +
                                     kU8ValueLen + inline_tail.size();
    const std::size_t body_len = children_len + shared_len;
    const bool ll = body_len > 0xFFFFu;
    const std::size_t head_len = (ll ? 6u : 4u) + children_len;

    rope_t rope;
    // Reserve the reply chain (head + one link per shared payload view) up front so the
    // appends below never spill through a throwing std::vector growth — an abort() under
    // -fno-exceptions on a fragmented heap. On failure return an EMPTY rope; resolve_node's
    // or_backpressure wrapper turns an empty success reply into an addressed BACKPRESSURE
    // error (the client falls back on the same link rather than presuming it dead).
    if (!rope.try_reserve(1 + shared.size())) return rope_t{};
    segment_ptr_t seg = view::heap_alloc(head_len);
    // A head-alloc failure invalidates the WHOLE reply: the shared payload views WITHOUT
    // the FWD header are a headerless, unroutable frame that the send site's
    // link_count() > 0 guard would wave through as garbage. Return an EMPTY rope instead
    // (or_backpressure → addressed BACKPRESSURE), never a malformed frame.
    if (!seg) return rope_t{};
    emit_cursor_t out{seg->bytes.data()};
    out.struct_header(type_t::FWD, ll, body_len);
    out.u8_value(std::to_underlying(fwd_op_t::REPLY));
    out.tlv_sliced(reply_dst_wire);
    out.tlv_sliced(reply_src_wire);
    out.u8_value(std::to_underlying(kind));
    out.raw(inline_tail);
    rope.append(view_t::over(std::move(seg)));
    for (const view_t& v : shared) rope.append(v);  // refcount clone — no byte copy
    return rope;
}

/**
 * @brief A kind=ERROR reply carrying STATUS{ ERROR{ VALUE u16 LE code } } (RFC-0004 §D with the
 *        RFC-0002 §C registered-code identity) — a fixed 14-byte tail, built on the stack.
 */
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

/**
 * @brief A kind=RESULT reply whose payload children are a stored rope value's links (ADR-0053 §6):
 *        a single-link value contributes one payload child (the trivial case, identical to a view
 *        read); a multi-link stored value ropes ALL its links into the reply zero-copy — no
 *        flatten.
 */
[[nodiscard]] rope_t assemble_result_rope(std::span<const std::byte> reply_dst_wire,
                                          std::span<const std::byte> reply_src_wire,
                                          const rope_t& payload) {
    // The links span feeds assemble directly — no heap copy of the link table. The
    // old std::vector staging copy was a per-reply transient that scaled with the
    // stored value's link count and ABORTED on heap exhaustion under -fno-exceptions
    // (the throwing std::allocator has no failure path on an MCU) — observed as an
    // OOM abort on a composed-root read's ~288-link reply. `payload` outlives the
    // call, so borrowing its span is safe.
    return assemble(reply_dst_wire, reply_src_wire, reply_kind_t::RESULT, {}, payload.links(),
                    payload.total_length());
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
[[nodiscard]] rope_t or_backpressure(rope_t reply, std::span<const std::byte> reply_dst_wire,
                                     std::span<const std::byte> reply_src_wire) {
    if (reply.link_count() == 0)
        return assemble_error(reply_dst_wire, reply_src_wire, status_t::BACKPRESSURE);
    return reply;
}

/**
 * @brief The vertex-map key for an arena-decoded PATH: span-aliased when canonical (ADR-0041 §3 —
 *        the PATH body IS the key, zero materialization), re-emitted into `fallback` otherwise (a
 *        foreign encoder's LL-widened / trailer-carrying NAMEs).
 *
 * Our own encoders always produce the canonical form.
 *
 * @par Why the fallback checks the child's TYPE (#436)
 * "Non-canonical" is two different things wearing one flag. `is_canonical_name`
 * (`tlv_arena.cpp:20`) demands `type == NAME && opt == {}`, so a PATH is non-canonical
 * either because a child is a legal NAME carrying options — an LL-widened length or a
 * trailer, which is exactly what this fallback exists to normalize — or because a child is
 * **not a NAME at all**, which `reference/05` §PATH forbids: *"Each child MUST be a NAME
 * TLV; other types are invalid in PATH context"* (normative by incorporation, ADR-0007).
 *
 * Re-emitting blindly conflated the two. `wire::emit_name` was called on every child body
 * regardless of type, so an illegal `PATH{VALUE "sensor"}` was silently REWRITTEN into the
 * legal `PATH{NAME "sensor"}`'s key and resolved `/sensor` — measured, before this fix, as
 * a `RESULT` carrying the stored value rather than an error. That breaks the injectivity
 * `reference/02` depends on: two byte-different PATHs addressed one vertex, so any peer,
 * cache or router keyed on PATH bytes had two spellings for one address.
 *
 * The fallback is still needed for the first case, so the fix is not to delete it — it is
 * to reject a non-NAME child before re-emitting it.
 *
 * @retval INVALID_PATH A child is not a NAME TLV.
 */
template <class N>
[[nodiscard]] result_t<std::span<const std::byte>> path_lookup_key(
    const N& path, std::vector<std::byte>& fallback) {
    if (path.canonical_path()) return path.body();
    auto ch = path.children();
    for (std::optional<N> seg = ch.next(); seg; seg = ch.next()) {
        if (seg->type() != type_t::NAME) return std::unexpected(status_t::INVALID_PATH);
        wire::emit_name(fallback, seg->body());
    }
    return std::span<const std::byte>(fallback);
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
[[nodiscard]] result_t<rope_t> resolve_node(graph_t& graph, const N& root,
                                            std::string_view inbound_link, const view_t* frame_view,
                                            mem::mem_backend_t& flat) {
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
        return assemble_error(reply_dst_wire, reply_src_wire,
                              req.dst.spans_intact() ? s : status_t::BACKPRESSURE);
    };

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

    // dst resolution is the router's PATH-keyed dispatch — span-aliased for a
    // canonical PATH (ADR-0041 §3: the frame IS the key). Local-only: a dst
    // naming a transport child / unknown path is not local => ERROR(NOT_FOUND).
    std::vector<std::byte> key_fallback;
    // An illegal non-NAME child makes the dst unaddressable, not merely unknown: it is a
    // malformed address, so it answers INVALID_PATH rather than NOT_FOUND, and it answers
    // BEFORE the write-creates branch below — a WRITE must not mkdir-p a path the spec
    // says cannot be spelled that way (#436).
    const result_t<std::span<const std::byte>> dst_key_r = path_lookup_key(req.dst, key_fallback);
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
                return or_backpressure(
                    assemble(reply_dst_wire, reply_src_wire, reply_kind_t::RESULT,
                             std::span<const std::byte>(wrapper.data(), wll ? 6u : 4u), *subs,
                             sub_len),
                    reply_dst_wire, reply_src_wire);
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
            if (!r) return assemble_error(reply_dst_wire, reply_src_wire, r.error());
            // The composed-root case: graph.read may SUCCEED (a folded ~hundreds-of-links
            // snapshot) yet the reply's own link-table reserve fail on the fragmented heap.
            // or_backpressure keeps that from becoming a silent drop (the dead-web-ui bug).
            return or_backpressure(assemble_result_rope(reply_dst_wire, reply_src_wire, **r),
                                   reply_dst_wire, reply_src_wire);
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
                const view_t sub_value = own_tlv(payload_node, flat);
                if (sub_value.empty())
                    return assemble_error(reply_dst_wire, reply_src_wire, status_t::BACKPRESSURE);
                // The ONE route copy of the subscription's life (ADR-0041 §2), into a
                // refcounted segment — every later delivery clones the refcount.
                const view_t return_route = own_tlv(req.src, flat);
                if (return_route.empty())
                    return assemble_error(reply_dst_wire, reply_src_wire, status_t::BACKPRESSURE);
                // ADR-0049: the wire append enters the graph's single admission door
                // (subscribe_wire → admit_subscriber) — the SUBSCRIBER TLV is parsed
                // ONCE there (delivery_compact included), so no parallel parse here.
                result_t<void> w =
                    graph.subscribe_wire(v, sub_value, return_route, std::string(inbound_link));
                if (!w) return assemble_error(reply_dst_wire, reply_src_wire, w.error());
                return or_backpressure(
                    assemble(reply_dst_wire, reply_src_wire, reply_kind_t::RESULT, {}, {}, 0),
                    reply_dst_wire, reply_src_wire);  // OK, empty payload
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
                return assemble_error(reply_dst_wire, reply_src_wire, status_t::BACKPRESSURE);

            result_t<void> w =
                graph.write(v, has_field ? field : field_path_t{}, value, inbound_link);
            if (!w) return assemble_error(reply_dst_wire, reply_src_wire, w.error());
            return or_backpressure(
                assemble(reply_dst_wire, reply_src_wire, reply_kind_t::RESULT, {}, {}, 0),
                reply_dst_wire, reply_src_wire);  // OK, empty payload
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
            if (has_field)
                return assemble_error(reply_dst_wire, reply_src_wire, status_t::SCHEMA_NOT_FOUND);
            const std::chrono::nanoseconds timeout =
                req.has_await_timeout ? std::chrono::nanoseconds(req.await_timeout)
                                      : kDefaultAwaitTimeout;
            result_t<value_ref_t> r = graph.await(v, timeout, inbound_link);
            if (!r)
                return assemble_error(reply_dst_wire, reply_src_wire,
                                      r.error());  // TIMEOUT => tr::flow::timeout
            return or_backpressure(assemble_result_rope(reply_dst_wire, reply_src_wire, **r),
                                   reply_dst_wire, reply_src_wire);
        }
        case fwd_op_t::REPLY:
            break;  // unreachable — handled above
    }
    return std::unexpected(status_t::INVALID_PATH);
}

}  // namespace

}  // namespace tr::graph
