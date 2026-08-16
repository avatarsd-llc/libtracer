/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/fwd_router.hpp"

#include <array>
#include <cassert>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "fwd_reply.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/error.hpp"
#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path.hpp"
#include "libtracer/rope_decode.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tlv_view.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

using graph::fwd_op_t;
using view::segment_ptr_t;
using view::view_t;
using wire::opt_t;
using wire::tlv_t;
using wire::type_t;

namespace {

/** @brief The unsigned value of one byte. */
constexpr std::uint8_t u8(std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); }

}  // namespace

// The FWD offset-dispatch cluster — fwd_hdr_t/read_fwd_header, the forward/op/control
// peeks, stack_writer, and the shrunk-dst/grown-src head rebuild — lives in the public
// fwd_frame_view.hpp (unit-tested directly, the length_prefix_framer precedent); this
// TU delegates mechanically. Frames are byte-identical.

namespace {

/**
 * @brief Reads `dst` segments as string_views, zero-copy when the region is contiguous.
 *
 * A span source yields each segment as one sub-span, so it is referenced in place; a rope
 * source may straddle a link, so those segments are stitched into this reader's own scratch.
 * The reader must outlive every view it hands out — callers keep it on the frame that also
 * holds the rebuild, since a resolved PEER name is referenced right through `gather`.
 */
/** @brief Segment views remembered per frame — the same inline run @ref tr::net::dst_seg_walk_t
 *         caches offsets for, so the two never disagree about how far the fast path reaches. */
inline constexpr std::size_t kSegViewSlots = tr::net::kDstSegCacheSlots;

template <class Cursor>
struct seg_reader_t {
    /** @brief Bind to @p c. A CONSTRUCTOR, not aggregate init: @ref scratch is deliberately
     *         left uninitialised (nothing reads it before it is written), and naming only the
     *         cursor in a brace-init trips `-Werror=missing-field-initializers` on the ESP
     *         toolchain. */
    explicit seg_reader_t(const Cursor& c) noexcept : cur(c) {}

    const Cursor& cur;
    /**
     * @brief Two one-segment stitch buffers — TRANSIENT (slot 0) and RETAINED (slot 1).
     *
     * It used to be `kMaxSegmentBytes * kMountPeekMax`: one slot per segment the peek could
     * ever hold, because the peek materialized them all up front. The descent now reads ONE
     * segment at a time and compares it immediately, so exactly two lifetimes exist — the
     * segment being compared right now, and the resolved bus PEER name, which is referenced
     * right through `gather` and so must outlive every later read. Two slots, not W: the
     * stack cost of this reader no longer moves when a mount gets wider.
     */
    std::array<std::byte, graph::kMaxSegmentBytes * 2> scratch;

    /**
     * @brief Segment `[off, len)` as a view INTO THE SOURCE, or empty if that is impossible.
     *
     * Empty means one of two things the caller distinguishes for itself: not routable (length
     * 0, or longer than a segment may be), or straddling a rope link so that only a stitched
     * copy can name it. Both are answered by @ref read into a stitch slot, on demand.
     */
    std::string_view in_place(std::size_t off, std::size_t len) const {
        if (len == 0 || len > graph::kMaxSegmentBytes) return {};
        const std::byte* first = nullptr;
        std::size_t first_len = 0;
        bool straddles = false;
        cur.for_each_span(off, len, [&](std::span<const std::byte> s) {
            if (first == nullptr) {
                first = s.data();
                first_len = s.size();
            } else {
                straddles = true;
            }
        });
        if (straddles || first_len != len) return {};
        return std::string_view(reinterpret_cast<const char*>(first), len);
    }

    /** @brief Segment `[off, len)` as a view in stitch slot @p slot, or empty if unroutable. */
    std::string_view read(std::size_t off, std::size_t len, std::size_t slot) {
        if (len == 0 || len > graph::kMaxSegmentBytes) return {};
        // The contiguous fast path IS `in_place` — this used to restate its span scan
        // byte for byte (#888). Past the length guard above, `in_place` can only come back
        // empty because the segment does not sit in one span, which is precisely the case
        // the stitch slot below exists for, so delegating changes no answer.
        if (const std::string_view v = in_place(off, len); !v.empty()) return v;
        std::size_t w = slot * graph::kMaxSegmentBytes;
        const std::size_t base = w;
        cur.for_each_span(off, len, [&](std::span<const std::byte> s) {
            for (const std::byte b : s) scratch[w++] = b;
        });
        // A feed clamped at the window edge (#986) stitches FEWER bytes than asked for,
        // and naming `len` of them anyway would hand the router a segment whose tail is
        // whatever the slot held before. Short ⇒ unroutable, the same answer `in_place`
        // gives for the straddle it could not name.
        if (w - base != len) return {};
        return std::string_view(reinterpret_cast<const char*>(scratch.data() + base), len);
    }

    /** @brief Read into the transient slot — valid only until the NEXT transient read. */
    std::string_view transient(std::size_t off, std::size_t len) { return read(off, len, 0); }
    /** @brief Read into the retained slot — the resolved PEER name, alive through `gather`. */
    std::string_view retained(std::size_t off, std::size_t len) { return read(off, len, 1); }
};

/** @brief What the mount descent resolved: where to send, and how much of `dst` it ate. */
struct mount_hit_t {
    transport_t* link = nullptr; /**< @brief The egress link, or nullptr ⇒ this node terminates. */
    std::string_view peer;       /**< @brief The resolved bus PEER segment, if any. */
    std::size_t strip_k = 0;     /**< @brief Leading dst segments this hop consumes. */
    /** @brief The link's REGISTRY identity — the qualified `"<module>/<name>"` of the matched
     *         child, or the bare peer segment when the hop resolved a bus peer. This is what
     *         `by_name` round-trips, so it is the key the label tables must store: the forward
     *         path never needs it, the control plane (ADVERTISE/COMPACT swaps) always does. */
    std::string_view link_name;
    /** @brief The next hop is a bus link's own NAME with a residual below it — REJECTED
     *         (ADR-0073 §3 / RFC-0020). A `dst` is a directed route to ONE terminus; the
     *         only routable segments below a multi-peer mount are its peer names, so a
     *         residual that resolves no peer must never fall through to the bus
     *         transport's `send()` (which broadcasts) nor to the local terminus, which
     *         would answer for an address that is not this node's to answer for. (Until
     *         RFC-0005 amendment 1 the terminus leg was worse still: a WRITE there
     *         mkdir-p'd a shadow vertex under the connection.) */
    bool rejected = false;
};

/**
 * @brief Resolve a `dst`'s leading segments against the registry — ONE registry pass (#523).
 *
 * Matches LONGEST-FIRST — the widest registered mount that prefixes the address wins, so a
 * more specific mount always beats a shorter one, and a node still carrying flat
 * single-segment children (the pre-RFC-0014 shape, and what the benches register) resolves
 * through the same code. That contract is unchanged; what changed is how it is reached.
 *
 * **There is no width loop and no width bound.** The descent used to try key widths `W..1`,
 * rescanning the whole registry at each — O(W×N) slot visits — and could only ever match the
 * widths a compile-time constant enumerated, so a mount wider than that registered happily and
 * resolved for *nothing* (#523: `size()` and `live_size()` reported it healthy while every
 * forward to it fell through to the terminus with no error anywhere). It is now ONE pass over
 * the registry, each slot matched against the prefix of ITS OWN `seg_count`
 * (@ref child_registry_t::longest_prefix): O(N), independent of width, and a mount of any
 * width — 1, 4, 33 — registers and resolves identically. The only remaining bound on mount
 * width is the path-depth budget every address spends from.
 *
 * When the matched child is multi-peer and another segment follows, that segment is resolved
 * in THAT endpoint's own peer table (never across buses) and the hop eats one more segment;
 * a residual segment that resolves NO current peer marks the hit `rejected` (ADR-0073 §3 /
 * RFC-0020 — the bus link's own NAME is not a routable next-hop, and the fall-through to
 * the bus transport's broadcasting `send()` is exactly what the ruling forbids).
 *
 * Segment-SOURCE-agnostic rather than segment-LIST-based, so the two planes share ONE descent
 * without either paying for the other's shape: the forward path feeds it a lazy walker over
 * the frame's `dst` (no array, nothing materialized in advance), and the control plane
 * (@ref fwd_router_t::on_advertise, @ref fwd_router_t::subscribe_toward) feeds it the NAME
 * children it already holds. Those two used to resolve by different rules — the control plane
 * still resolved a single BARE segment (#516) — which silently absorbed every advertise
 * addressed to a `/net/<module>/<name>` mount at the first intermediate node.
 *
 * @tparam SegAt  `std::optional<std::string_view>(std::size_t)` — segment `i`, `nullopt` past
 *                the end, EMPTY when present but not routable (over-long / unreadable). Read

 * @tparam Retain `std::string_view(std::size_t)` — segment `i` in storage that outlives the
 *                whole hop. Called at most ONCE, for a resolved bus peer, whose name is
 *                referenced right through the egress `gather`; @p SegAt's result need not
 *                survive the next call to it, which is what lets the forward arm stitch a
 *                rope-straddling segment into one reusable buffer instead of W of them.
 */
template <class SegAt, class Retain>
[[nodiscard]] mount_hit_t resolve_mount_by(const child_registry_t& registry, SegAt&& at,
                                           Retain&& retain) {
    const child_registry_t::child_t* const c = registry.longest_prefix(at);
    if (c == nullptr) return {};
    const std::size_t k = c->seg_count;
    // A dst that names the mount EXACTLY addresses the connection vertex itself — its own
    // `:children[]`, `:settings`, liveness value — so it terminates HERE. Only a dst with
    // something BELOW the mount is a forward (ADR-0038 §3a: a local dst descends to a local
    // vertex and terminates). A bus PEER is the exception: it has no vertex, so naming a peer
    // exactly still forwards, with an empty residual.
    const std::optional<std::string_view> next = at(k);
    if (!next) return {};
    // ONE snapshot of link + shape (#882). Read as two fields, a reconnect rebind that FLIPS
    // this name's shape could pair a stale point-to-point shape with the fresh BUS link and
    // send a directed request over the bus's broadcasting `send()` — the very fall-through
    // the rejected branch below exists to stop.
    const child_registry_t::egress_t eg = c->egress();
    if (eg.multi_peer) {
        if (!next->empty()) {
            if (transport_t* const p = child_registry_t::resolve_peer(*c, *next)) {
                const std::string_view peer = retain(k);
                return mount_hit_t{p, peer, k + 1, peer};
            }
        }
        // ADR-0073 §3 (RFC-0020): the bus link's own NAME is not a routable next-hop.
        // Falling through to the slot's link here egressed over the bus transport's `send()`,
        // which fans out to EVERY open peer — one directed request drew N replies and
        // scrambled FIFO reply correlation (#409). Only the link's peer names route;
        // fan-out belongs to the subscription plane.
        mount_hit_t rej;
        rej.rejected = true;
        return rej;
    }
    return mount_hit_t{eg.link, {}, k, c->name};
}

/**
 * @brief @ref resolve_mount_by over a ready-made segment list — the control plane's form, and
 *        the forward path's fast arm once a `dst` has been read into locals.
 */
[[nodiscard]] mount_hit_t resolve_mount_segs(const child_registry_t& registry,
                                             std::span<const std::string_view> seg) {
    return resolve_mount_by(
        registry,
        [seg](std::size_t i) -> std::optional<std::string_view> {
            if (i >= seg.size()) return std::nullopt;
            return seg[i];
        },
        [seg](std::size_t i) -> std::string_view {
            return i < seg.size() ? seg[i] : std::string_view{};
        });
}

/**
 * @brief The UNBOUNDED-width arm of the descent: walk `dst` lazily, one segment at a time.
 *
 * Reached only when the address does not fit the shallow arm's locals — a `dst` deeper than
 * @ref kSegViewSlots, or one whose segments straddle rope links and so cannot be named by a
 * view into the frame. Every answer is identical to the shallow arm's; this is the arm that
 * makes the width unbounded, which is why the shallow one is an OPTIMISATION and never a cap.
 *
 * `noinline` ON PURPOSE, and it is the fix for the measured regression this design shipped
 * with. Inlined, the walker, its lambdas and `resolve_mount_by`'s second instantiation all
 * landed in `on_frame_impl` — 3476 → 8080 bytes of code and +144 B of stack frame — and the
 * SHALLOW path, which touches none of it, paid for the register pressure: +14 ns per frame at
 * W = 1 rising to +25 ns at W = 3, on shapes whose per-slot work is identical. Out of line,
 * the hot function is the pre-lift one again and this arm costs a call it was always going to
 * be worth.
 *
 * The walker and the two stitch slots are the WHOLE per-frame state here: nothing is sized by
 * the mount width, so the router's stack frame is the same for a 1-segment mount and a
 * 33-segment one. (The measured alternative — a W-sized peek array — cost 592 B of rv32 stack
 * at W=4 and 2912 B at W=33, paid per rope frame. That is the design this replaces.)
 */
template <class Cursor>
[[gnu::noinline]] [[nodiscard]] mount_hit_t resolve_mount_deep(const child_registry_t& registry,
                                                               const Cursor& cur,
                                                               seg_reader_t<Cursor>& rd,
                                                               fwd_pre_t& pre) {
    dst_seg_walk_t<Cursor> walk(cur, pre);
    walk.prefill();
    // Every segment is read into the TRANSIENT stitch slot and compared immediately: the
    // descent never holds two segment views at once (`matches_prefix` folds each into its
    // memcmp before asking for the next), which is exactly what lets one buffer serve a `dst`
    // of any depth instead of one buffer per segment.
    const auto read_seg = [&](std::size_t i) -> std::optional<std::string_view> {
        const auto s = walk.at(i);
        if (!s) return std::nullopt;
        return rd.transient(s->first, s->second);
    };
    mount_hit_t hit = resolve_mount_by(registry, read_seg, [&](std::size_t i) {
        // The resolved PEER name is referenced right through the egress `gather`, so it is
        // read into the slot that outlives every later read.
        const auto s = walk.at(i);
        return s ? rd.retained(s->first, s->second) : std::string_view{};
    });
    // The walk just visited these segments; record where the consumed run ENDS so the rebuild
    // does not walk them again. Only meaningful once the descent has chosen `strip_k`.
    if (hit.link != nullptr && hit.strip_k > 0) {
        if (const std::optional<std::size_t> e = walk.end_of(hit.strip_k - 1)) {
            pre.strip_at = *e;
        } else {
            pre.valid = false;
        }
    } else {
        pre.valid = false;  // nothing to hand over — the rebuild parses for itself
    }
    return hit;
}

/**
 * @brief The forward path's entry to the descent — a SHALLOW arm in locals, a deep arm out of
 *        line.
 *
 * Reads the leading `dst` segments into plain locals in ONE tight loop — the exact shape of
 * the fixed peek this replaced, minus the fixed BOUND — and hands the descent a plain span. No
 * walker, no callable, no `std::optional` on any per-segment step. That is what the pre-lift
 * forward hop compiled to, and it is what 99% of the traffic (a 2- or 3-segment RFC-0014
 * mount) must keep costing.
 *
 * `kSegViewSlots` is a CACHE length, NOT a width bound, and the difference is observable: an
 * address longer than it, or one a rope splits, takes @ref resolve_mount_deep and resolves
 * identically — no mount is unreachable at any value, including the structural floor of two.
 * `kMountPeekMax` decided which mounts could resolve at all; this decides only which of two
 * code paths, with one answer between them, a frame takes.
 *
 * Segment 0 is not parsed here: @ref peek_fwd_dst already read that header to gate the frame
 * and now hands the offsets over (@ref fwd_pre_t::seg0_off), so a forward hop parses each
 * header exactly once (ADR-0038 inv. #1).
 *
 * `flatten` for the reason `rebuild_fwd_forward` carries it — the loop below is header reads,
 * and `read_fwd_header` returns six words that an out-of-line call hands back through memory.
 * It does NOT reach @ref resolve_mount_deep: `flatten` respects `noinline`, which is what lets
 * the two attributes state the two halves of the same decision.
 */
template <class Cursor>
[[gnu::flatten]] [[nodiscard]] mount_hit_t resolve_mount_at(const child_registry_t& registry,
                                                            const Cursor& cur,
                                                            seg_reader_t<Cursor>& rd,
                                                            fwd_pre_t& pre) {
    if (!pre.valid) return {};
    std::array<std::string_view, kSegViewSlots> seg;
    std::array<std::size_t, kSegViewSlots> seg_end;
    // Segment 0 came free with the peek's gate — the same header, read once.
    seg[0] = rd.in_place(pre.seg0_off, pre.seg0_len);
    seg_end[0] = pre.seg0_off + pre.seg0_len;  // a packed record's payload is its tail
    bool all_in_place = !seg[0].empty();
    std::size_t n = 1;
    std::size_t pos = seg_end[0];
    bool ends_in_run = true;
    while (n < kSegViewSlots) {
        if (pos >= pre.dst_end) break;  // the address ended inside the run
        const auto rec = read_packed_seg(cur, pos, pre.dst_end);
        // Ragged framing is where an ADDRESS stops — the run ends here exactly as the
        // walker's `at` would report it.
        if (!rec) break;
        pos += rec->total;
        // The RFC-0018 §5.4 escape is STEPPED OVER, not resolved: nothing mints one, and a
        // forwarder relays a record whose `kind` it does not implement rather than dropping
        // a frame it is only carrying. It occupies no segment index, so `strip_k` still
        // counts literal segments and `seg_end` still bounds the consumed run.
        if (rec->escape) continue;
        // Empty means "not addressable as a view into the frame" — unroutable, OR straddling a
        // rope link. Either way the deep arm re-reads it and answers correctly.
        seg[n] = rd.in_place(rec->body_off, rec->body_len);
        if (seg[n].empty()) all_in_place = false;
        seg_end[n] = pos;
        ++n;
    }
    // Filled the run with bytes still to come: the address MAY continue past the locals, so it
    // is the deep arm's. (It may also stop at a non-NAME child one header later — testing that
    // would cost a header parse on every frame to save a cold call on almost none.)
    if (n == kSegViewSlots && pos < pre.dst_end) ends_in_run = false;
    if (!all_in_place || !ends_in_run) return resolve_mount_deep(registry, cur, rd, pre);

    const mount_hit_t hit =
        resolve_mount_segs(registry, std::span<const std::string_view>(seg.data(), n));
    // Record where the consumed run ENDS so the rebuild does not walk these segments again.
    // Only meaningful once the descent has chosen `strip_k`, which is why it is filled here
    // rather than in the peek.
    if (hit.link != nullptr && hit.strip_k > 0 && hit.strip_k <= n) {
        pre.strip_at = seg_end[hit.strip_k - 1];
    } else {
        pre.valid = false;  // nothing to hand over — the rebuild parses for itself
    }
    return hit;
}

/**
 * @brief Emit `COMPACT{ VALUE label(u16), payload }` over @p down by SCATTER-GATHER — the
 *        steady-state egress, allocating NOTHING in the router.
 *
 * A COMPACT is a 6-byte frame header, a 6-byte label child, and a payload that is ALREADY
 * contiguous in the caller's storage. The built encoders nonetheless assembled the whole frame
 * into two `std::vector`s, copying the payload twice to produce bytes the transport was about
 * to gather anyway. Here the 12-byte head is written on the stack and the payload is REFERENCED,
 * so the router's per-frame allocation count on this leg is zero. Since #885 this is the ONLY
 * COMPACT emission in the router: the writer thread's `deliver_remote` reaches it too.
 *
 * The bytes are unchanged: `stack_writer::header` and `wire::emit_header` share the `> 0xFFFF`
 * LL-widening rule and the same little-endian order, and the label child comes from
 * @ref tr::net::label_tlv, the same locus the built encoders use. Frames carry no trailer here
 * (no emitter in this family sets the CR bit), so nothing is left uncomputed.
 *
 * What this does NOT promise is zero allocations in the TRANSPORT. A link that overrides the
 * gather form (tcp, udp, ws-server) writes these spans straight to the socket; one that does
 * not (can, loopback, ws-client) falls into `transport_t::send(iov)`'s default concatenation,
 * which allocates once. That is still strictly better than the two allocations and two payload
 * copies it replaces, so no transport regresses.
 *
 * @param down    The downstream link to emit over.
 * @param label   The out-label for that link.
 * @param payload A complete payload TLV's bytes; must outlive the call (every in-tree
 *                transport either writes synchronously or gather-copies before returning).
 */
void emit_compact(transport_t& down, std::uint16_t label, std::span<const std::byte> payload) {
    const std::array<std::byte, 6> lbl = tr::net::label_tlv(label);
    stack_writer<12> head;  // COMPACT header (<=6) + the 6-byte label child
    head.header(type_t::COMPACT, lbl.size() + payload.size());
    head.raw(lbl);
    if (!head.ok()) return;  // cannot happen at N=12; drop rather than emit a truncated frame
    const std::array<std::span<const std::byte>, 2> iov{head.span(), payload};
    down.send(std::span<const std::span<const std::byte>>(iov));
}

/**
 * @brief Emit `ADVERTISE{ VALUE label(u16), PATH route }` over @p link by SCATTER-GATHER —
 *        the ADVERTISE half of @ref emit_compact, allocating NOTHING in the router.
 *
 * The four production ADVERTISE emissions used to reach for the THROWING
 * `tr::net::encode_advertise`, which built a body vector and a frame vector and copied the
 * route into both (#885). THREE of the four run on a transport RECEIVE thread and are entirely
 * peer-provoked — the forwarding hop's re-advertise in @ref fwd_router_t::on_advertise and the
 * re-advertise in @ref fwd_router_t::on_nack — so on the shipping `-fno-exceptions` profile
 * each was a peer-reachable `abort()`. The route TLV is already contiguous at every one of
 * them (a stripped re-encode the hop is about to store, the caller's own span at the producer
 * door, the owning copy `route_handle_t::egress_route` hands back on a NACK), so the ONLY
 * bytes that needed building were the 12-byte head — which fits on the stack.
 *
 * The bytes are unchanged for the same reason @ref emit_compact's are: `stack_writer::header`
 * and `wire::emit_header` share the `> 0xFFFF` LL-widening rule and the little-endian order,
 * and the label child comes from @ref tr::net::label_tlv, the same locus the built encoder
 * uses. `compact_cache_test` pins the concatenation against `encode_advertise` across the
 * widening boundary, driving the real router door.
 *
 * As with COMPACT, this promises nothing about the TRANSPORT: a link that overrides the gather
 * form writes the two spans straight out, one that does not falls into
 * `transport_t::send(iov)`'s default concatenation — ONE allocation, and a NOTHROW one
 * (#848). Either way no throwing allocation remains on the path.
 *
 * @param link       The link to emit over.
 * @param label      The out-label being advertised on that link.
 * @param route_path A complete PATH TLV's bytes; must outlive the call (every in-tree
 *                   transport either writes synchronously or gather-copies before returning).
 */
void emit_advertise(transport_t& link, std::uint16_t label, std::span<const std::byte> route_path) {
    const std::array<std::byte, 6> lbl = tr::net::label_tlv(label);
    stack_writer<12> head;  // ADVERTISE header (<=6) + the 6-byte label child
    head.header(type_t::ADVERTISE, lbl.size() + route_path.size());
    head.raw(lbl);
    if (!head.ok()) return;  // cannot happen at N=12; drop rather than emit a truncated frame
    const std::array<std::span<const std::byte>, 2> iov{head.span(), route_path};
    link.send(std::span<const std::span<const std::byte>>(iov));
}

/**
 * @brief Emit `HANDLE_NACK{ VALUE label(u16) }` over @p link — a FIXED 10-byte frame written
 *        entirely on the stack, so the stale-label answer allocates NOTHING anywhere.
 *
 * The one arm this router runs on a peer-provoked receive thread that is now allocation-free
 * END TO END: @ref fwd_router_t::on_compact's unknown/stale-label case reads a trivially
 * copyable resolution, compares one generation, finds the inbound link by name, and emits
 * these ten bytes. It used to build them with the throwing `tr::net::encode_handle_nack`
 * (two `std::vector`s for a frame whose size is a compile-time constant), which made the
 * cheapest possible answer to a hostile peer — "I do not know that label" — the one that
 * could `abort()` the node under `-fno-exceptions` (#885).
 *
 * A NACK has no variable-length child, so unlike ADVERTISE and COMPACT there is nothing to
 * gather: the whole frame is contiguous in the stack buffer and goes out through the plain
 * span `send`, exactly as the built form did. No transport sees a shape it did not before.
 *
 * @param link  The link the stale COMPACT arrived on — the NACK goes back the way it came.
 * @param label The unknown/stale label that prompted it.
 */
void emit_handle_nack(transport_t& link, std::uint16_t label) {
    const std::array<std::byte, 6> lbl = tr::net::label_tlv(label);
    stack_writer<12> frame;  // HANDLE_NACK header (4) + the 6-byte label child
    frame.header(type_t::HANDLE_NACK, lbl.size());
    frame.raw(lbl);
    if (!frame.ok()) return;  // cannot happen at N=12; drop rather than emit a truncated frame
    link.send(frame.span());
}

/**
 * @brief The trailer-EXCLUDED whole-TLV wire bytes of a decoded route node — the span shape
 *        @ref tr::graph::assemble_error_reply copies into the reply head (ADR-0041 §4).
 *
 * `wire::encode` re-emits a trailer whenever the node carries one, so a peer that timestamped
 * or CRC'd its route TLV had those bytes echoed back INSIDE the reply's address. That is
 * exactly where the router's retired hand-rolled encoder diverged from the resolver, whose
 * route copies are trailer-sliced at rest (#887). Clearing the trailer BITS and dropping the
 * trailer VALUES is one operation, never two: a copy whose opt byte claims bytes the copy no
 * longer carries is unparseable.
 *
 * The trailer-less fast path is not a guard: it is the universal case answered without
 * deep-copying a peer-sized subtree. The two arms are not byte-identical in general — the
 * fast arm gates on `!ts && !cr` and returns the opt byte as-is, while `without_trailer()`
 * also clears CW and TF — so a route with opt `0x44` (PL|CW) and no trailer bytes takes the
 * fast arm and keeps the CW bit the slow arm would drop. That difference is unobservable
 * downstream because the assembler re-slices the route before it reaches the wire, but the
 * arms must not be described as interchangeable.
 *
 * @param route A decoded route node (a `PATH`).
 * @return Its whole-TLV bytes with the OUTER trailer removed, or an EMPTY vector when
 *         `wire::encode` refuses the node (an ill-formed `PATH_REF` descendant).
 */
[[nodiscard]] std::vector<std::byte> route_wire_trailer_less(const tlv_t& route) {
    if (!route.opt.ts && !route.opt.cr) return wire::encode(route);
    tlv_t sliced = route;
    sliced.opt = sliced.opt.without_trailer();
    sliced.trailer.reset();
    return wire::encode(sliced);
}

/**
 * @brief Answer a bus-NAME-hop rejection (ADR-0073 §3 / RFC-0020) with an ADDRESSED
 *        `FWD{REPLY, kind=ERROR, STATUS{ERROR{tr::path::invalid}}}` over the inbound link.
 *
 * The rejection shape follows the terminus resolver's own split (`op_resolve_walk.hpp` /
 * `fwd_terminus_reject_test.cpp`): a frame malformed at the level that carries `src` drops
 * BY VALUE (nowhere to reply to); a well-formed frame ANSWERS, so the peer learns its route
 * was refused instead of seeing a silent timeout. This frame is well-formed — its dst simply
 * names a hop the ruling forbids — so it answers. A `REPLY` is never answered with a reply
 * (the resolver's own rule), and this is a COLD path, so the owning `wire::decode` is
 * within the ADR-0039 allocation budget exactly as the control-frame decodes above are.
 *
 * The reply bytes are not mirrored from the resolver's grammar — they ARE the resolver's
 * grammar (#887): @ref tr::graph::assemble_error_reply is the one definition of
 * `FWD{ VALUE op=REPLY, PATH dst=req.src, PATH src=req.dst, VALUE kind=ERROR,
 * STATUS{ ERROR{ VALUE u16 LE code } } }` (RFC-0004 §D with the RFC-0002 §C registered-code
 * identity, `tr::path::invalid` = 0x0021). This function's job is reduced to the two things
 * only it knows: which frame earns a rejection, and where the two route TLVs come from.
 *
 * @warning Sharing the encoder does NOT make this path nothrow. The owning `wire::decode`
 *          on the first line still allocates through a throwing `std::vector` on the same
 *          peer-provoked path; the RX-thread allocation policy is #885's, not this
 *          function's. What is now shared is the SHAPE.
 *
 * @param registry     The child registry the answer is routed back through.
 * @param inbound_name This node's name for the link the refused frame arrived on.
 * @param frame        The refused frame's bytes.
 * @param egress       The byte backend the reply head draws from (#795, ADR-0074) — the
 *                     router's own egress seam, so a bounded node bounds this reply too.
 */
void reject_bus_name_hop(const child_registry_t& registry, std::string_view inbound_name,
                         std::span<const std::byte> frame, mem::mem_backend_t& egress,
                         graph::status_t status) {
    const auto dec = wire::decode(frame);
    if (!dec) return;  // malformed ⇒ drop by value
    const tlv_t* op = nullptr;
    const tlv_t* dst = nullptr;
    const tlv_t* src = nullptr;
    // The `dst` slot takes either routable form (RFC-0024 §4): a canonical `PATH`, or the
    // BOUND `PATH_REF` a reverse-list delivery is addressed by (§7.1 amendment 1) — the
    // refusal of a bound delivery echoes the refused `PATH_REF` exactly as the canonical
    // reject echoes the refused route, and the producer's step-5 reclaim correlates either
    // (`evict_route_edges` classifies the echo's type byte). The `src` slot stays `PATH`
    // only: a request's return route is always canonical (`05-protocol-tlvs.md` hop rules).
    // The scan stops at `src`, so a mint-flagged request's TRAILING reverse list (which sits
    // after `src`) can never be mistaken for the address being refused — and since §7.1
    // amendment 2 that list is `PATH_REF_REVERSE` (`0x15`), which this scan does not accept
    // as a `dst` at all, so the guarantee no longer rests on the scan's stopping point alone.
    for (const tlv_t& c : dec->children) {
        if (dst == nullptr && (c.type == type_t::PATH || c.type == type_t::PATH_REF)) {
            dst = &c;
        } else if (dst != nullptr && c.type == type_t::PATH) {
            src = &c;
        } else if (c.type == type_t::VALUE && op == nullptr) {
            op = &c;
        }
        if (src != nullptr) break;
    }
    // No op / dst / src ⇒ nowhere trustworthy to reply to ⇒ drop by value.
    if (op == nullptr || op->payload.size() != 1 || dst == nullptr || src == nullptr) return;
    // Never answer a REPLY with a reply (the resolver rejects a REPLY by value too): an
    // unroutable reply hop erroring BACK would ping-pong between two confused nodes.
    //
    // MASKED, like every other op-byte read (RFC-0024 §9.3 — "a forwarder MUST mask rather
    // than switch on the raw byte"). Unmasked, a REPLY carrying any flag bit is not
    // recognised as a REPLY at all and this guard waves it through, so the node answers a
    // reply with an addressed error reply — the exact frame the line above forbids.
    if (static_cast<fwd_op_t>(u8(op->payload[0]) & graph::kFwdOpcodeMask) == fwd_op_t::REPLY)
        return;

    // Reply routes swapped, as the resolver assembles them: reply dst = request src (the
    // accumulated return route), reply src = request dst (the refused spelling — what the
    // peer asked for, echoed so it can correlate).
    const std::vector<std::byte> rdst = route_wire_trailer_less(*src);
    const std::vector<std::byte> rsrc = route_wire_trailer_less(*dst);
    // A route `wire::encode` refuses is no address at all, and a head sized around an empty
    // span would announce a body length no bytes occupy. Drop by value, as for a missing src.
    if (rdst.empty() || rsrc.empty()) return;
    graph::reply_route_t route{.dst_wire = rdst, .src_wire = rsrc};
    // The wire-time echo (#1109) rides rejections too: an origin probing RTT against a route
    // this node refuses still gets its stamp back with the addressed error, so the same
    // frame answers both questions. TF=1 is not echoed — anchorless at the root, the spec's
    // own MUST-reject case (the terminus resolver applies the identical filter).
    if (dec->opt.ts && !dec->opt.tf && dec->trailer && dec->trailer->ts)
        route.echo_ts = dec->trailer->ts;
    // The status is the CALLER's, because the two refusals a hop makes on its own are different
    // answers to different questions: a bus NAME with a residual is a malformed address
    // (`tr::path::invalid`), while a path label this node cannot validate is an address that
    // does not resolve HERE (`tr::path::not_found`, RFC-0027 §7.2 — "an unresolvable address is
    // exactly what `tr::path::not_found` already means, and a label is an address"). Everything
    // else about the refusal — the swapped routes, the echoed spelling the sender correlates
    // on, the wire-time stamp echo, the egress seam and the bus-peer fallback — is identical,
    // which is why this is one function with a status rather than two that drift.
    const view::rope_t reply = graph::assemble_error_reply(route, status, egress);
    // One link — plus the echo trailer link on a stamped request — by construction: an error
    // reply carries no shared payload and no mint trailer. A zero-link rope is the egress
    // refusal; drop rather than emit a headerless frame. The echoed shape is two links, so
    // this COLD path materializes rather than growing a scatter-gather send it never needed.
    if (reply.link_count() == 0) return;
    // `by_name` includes the bus-peer fallback, so a frame that arrived FROM a peer (whose
    // inbound_name is the peer's own name) answers back over that peer's directed endpoint —
    // the same lookup resolve_terminus uses for its replies.
    if (transport_t* const up = registry.by_name(inbound_name)) {
        if (reply.link_count() == 1) {
            up->send(reply.links()[0].bytes());
        } else {
            // Honest failure channel (#917): a REFUSED materialize (an OOM on this cold
            // error path) drops the rejection reply — it is not an empty frame to send.
            const std::expected<view::view_t, view::flatten_err_t> flat = reply.try_materialize();
            if (flat) up->send(flat->bytes());
        }
    }
}

/** @brief The reply-`src` window @ref peek_refused_route hands back — offsets, not a span,
 *         because the source may be a rope the caller re-slices from its own cursor. */
struct refused_src_t {
    std::size_t off = 0; /**< @brief Absolute offset of the echoed route's PATH TLV. */
    std::size_t len = 0; /**< @brief Its total size (header + body). */
};

/**
 * @brief Is this terminating `FWD{REPLY}` an addressed RFC-0020 refusal — and if so, where
 *        is the refused route it echoes? (#1223 step 5, the producer-side half.)
 *
 * `reject_bus_name_hop` above answers a delivery whose residual names a departed session
 * with `FWD{ REPLY, dst=req.src, src=req.dst, kind=ERROR, STATUS{ERROR{tr::path::invalid}} }`
 * — the request's routes swapped, "the refused spelling ... echoed so it can correlate".
 * This peek is that correlation, on the receiving side: it recognizes the exact shape by
 * OFFSET (five header reads through the one grammar, no decode, no allocation — the warm
 * read-reply path bails at the first non-matching child) and hands back the `src` window,
 * which is the refused route byte-for-byte as this node stored and emitted it.
 *
 * Strict on the shape, deliberately: a reply with a FIELD child, a non-ERROR kind, or any
 * status but `tr::path::invalid` (0x0021) is NOT a route refusal and must not evict — a
 * TIMEOUT or BACKPRESSURE names a live route having a bad day.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 * @retval std::nullopt Not an addressed `tr::path::invalid` refusal.
 */
template <class Cursor>
[[nodiscard]] std::optional<refused_src_t> peek_refused_route(const Cursor& cur) {
    const auto outer = read_fwd_header(cur, 0);
    if (!outer || outer->type != wire::type_t::FWD || !outer->opt.pl) return std::nullopt;
    const std::size_t end = outer->body_off + outer->body_len;
    // Child 1 — VALUE op, one byte, masked REPLY (RFC-0024 §9.3: mask, never the raw byte).
    std::size_t pos = outer->body_off;
    const auto op = read_fwd_header(cur, pos);
    if (!op || op->type != wire::type_t::VALUE || op->body_len != 1) return std::nullopt;
    if (static_cast<graph::fwd_op_t>(cur.byte_at(op->body_off) & graph::kFwdOpcodeMask) !=
        graph::fwd_op_t::REPLY)
        return std::nullopt;
    pos += op->total;
    if (pos >= end) return std::nullopt;
    // Child 2 — PATH dst: the reply's consumed way home; not read further.
    const auto rdst = read_fwd_header(cur, pos);
    if (!rdst || rdst->type != wire::type_t::PATH) return std::nullopt;
    pos += rdst->total;
    if (pos >= end) return std::nullopt;
    // Child 3 — the refused route, echoed whole: a PATH, or (RFC-0024 §7.1 amendment 1)
    // the `PATH_REF` a reverse-list delivery was refused as. Non-empty by the same rule as
    // the eviction it feeds (an empty route names nothing and matches nothing).
    const auto rsrc = read_fwd_header(cur, pos);
    if (!rsrc || (rsrc->type != wire::type_t::PATH && rsrc->type != wire::type_t::PATH_REF) ||
        rsrc->body_len == 0)
        return std::nullopt;
    const refused_src_t out{.off = pos, .len = rsrc->total};
    pos += rsrc->total;
    if (pos >= end) return std::nullopt;
    // Child 4 — VALUE kind == ERROR.
    const auto kind = read_fwd_header(cur, pos);
    if (!kind || kind->type != wire::type_t::VALUE || kind->body_len != 1) return std::nullopt;
    if (cur.byte_at(kind->body_off) !=
        static_cast<std::uint8_t>(std::to_underlying(graph::reply_kind_t::ERROR)))
        return std::nullopt;
    pos += kind->total;
    if (pos >= end) return std::nullopt;
    // Child 5 — STATUS{ ERROR{ VALUE u16 LE } }, and the code is tr::path::invalid.
    const auto st = read_fwd_header(cur, pos);
    if (!st || st->type != wire::type_t::STATUS || !st->opt.pl) return std::nullopt;
    const auto err = read_fwd_header(cur, st->body_off);
    if (!err || err->type != wire::type_t::ERROR || !err->opt.pl) return std::nullopt;
    const auto code = read_fwd_header(cur, err->body_off);
    if (!code || code->type != wire::type_t::VALUE || code->body_len != 2) return std::nullopt;
    if (cur.load_le(code->body_off, 2) != std::to_underlying(wire::err_t::PATH_INVALID))
        return std::nullopt;
    return out;
}

/**
 * @brief Can any address that exists have @p name as its mount prefix?
 *
 * The ONE always-on bound left on a mount name once the width cap is gone (#523), and it is
 * derived, not chosen: a mount is reached by a `dst` that carries at least its own segments,
 * a path carries at most `graph::kMaxSegments` of them (RFC-0023), and no `dst` segment is
 * empty. A name that fails any of those is not "wide" — it is unaddressable, and registering
 * it would report a healthy child that every forward misses, which is exactly the silent
 * misroute #523 was filed about.
 */
[[nodiscard]] bool routable_mount_name(std::string_view name) noexcept {
    if (name.empty()) return false;
    std::size_t segs = 1;
    std::size_t seg_len = 0;
    for (const char c : name) {
        if (c == '/') {
            if (seg_len == 0) return false;  // an empty segment matches no dst segment
            ++segs;
            seg_len = 0;
        } else {
            ++seg_len;
        }
    }
    if (seg_len == 0) return false;  // trailing '/'
    return segs <= graph::kMaxSegments;
}

}  // namespace

void fwd_router_t::reclaim_refused_route(std::string_view inbound_name,
                                         std::span<const std::byte> frame) {
    const wire::grammar::span_cursor cur{frame};
    const std::optional<refused_src_t> ref = peek_refused_route(cur);
    if (!ref) return;
    // COLD past the peek by construction: only an addressed refusal reaches the walk. The
    // count is not surfaced, matching the link_down seam — eviction seams report nothing.
    (void)graph_.evict_route_edges(inbound_name, frame.subspan(ref->off, ref->len));
}

bool fwd_router_t::add_child(std::string name, transport_t& link, mem::block_source_t* rx) {
    // ADR-0063 §3: serialize control-plane writers. The registry's scan-then-append and the
    // child_rx_ deque's emplace_back are both non-atomic, and two creates arriving on two
    // different transports' receive threads are genuinely concurrent. Readers take nothing.
    const std::lock_guard ctl(ctl_m_);
    // ALWAYS-ON, and only for a bound that is REAL (#523). The width bound this replaced was
    // a debug `assert` of "1..3 segments" — which, compiled out under NDEBUG, meant every
    // release build had no bound at all while the descent could only reach three. The width
    // itself is now unbounded: the descent matches each registry slot against the prefix of
    // its own width, so a mount of any width resolves and there is nothing left to assert.
    //
    // What survives is the bound the addressing model actually has. A mount can only be
    // ADDRESSED by a `dst` that carries at least its own segments, and a path may carry at
    // most `graph::kMaxSegments` of them (RFC-0023). A name wider than that — or with no
    // segments, or with an EMPTY segment, which no `dst` segment may be — can never be the
    // prefix of any address that exists, so registering it is the same silent misroute #523
    // was filed about, one step further out. It is refused instead, by value.
    if (!routable_mount_name(name)) return false;
    // Populate the registry BEFORE wiring the receiver: an async transport (UDP/ws) may
    // already have a live recv thread, so `set_receiver` is the publish point — once the
    // callback is installed, on_frame can read the registry on that thread. Adding the
    // child first ensures the entry is visible before any inbound frame can resolve it
    // (the set_receiver mutex provides the release/acquire fence). No lock is taken on the
    // read hot path.
    // A registry that could not grow registers NOTHING, and this is the only place that can
    // tell the caller so. Refusing HERE — before `set_receiver` — is what keeps the failure
    // total: nothing is wired, so there is no ghost child audible on its transport but
    // resolvable by no `dst` and removable by no `remove_child`.
    if (!registry_.add(name, link)) return false;
    // Capability-matched receiver (ADR-0042 §1 / ADR-0044): a BUS link delivers
    // frames tagged with the SENDING peer's name, which becomes the hop's inbound
    // NAME — so the `src` grown on a forward (and the link a terminus reply goes
    // back over) names the bus PEER, and the registry's peer fallback turns that
    // name into a directed send. An owning-delivery link funnels through the same
    // routing with the frame view alongside; a span link keeps the borrowed-span
    // path. No adapter wraps a span into a lying view.
    if (bus_link_t* const bus = link.bus()) {
        // A reassembling bus that delivers ropes (ADR-0053 §5) hands its group up
        // as-is — zero-copy; a span-only bus keeps the borrowed peer-named path.
        // A bus frame arrives tagged with the sending peer's name — but a PEER has no
        // registry entry, so the peer name alone cannot say which mount it hangs under.
        // The receiver therefore carries the same stable per-child ctx the point-to-point
        // path uses, holding the child's QUALIFIED name (#510): a forward hop then grows
        // `src` by the full `net/<module>/<name>/<peer>` path rather than a bare peer
        // segment, which is what keeps two buses' same-named peers distinct on the way back.
        // Departure seam (RFC-0009 §D extended): a bus peer that hangs up carries its
        // own name, and label state is keyed by that name, so link_down still takes the peer.
        // No `conn_slot` is recorded for a BUS child, and that is a refusal rather than an
        // omission. TWO INDEPENDENT facts, each with its own citation (#1223 — this used to
        // cite "the RFC-0024 §5 refusal", which says nothing about buses, mounts or peers,
        // and derived the second fact from the first, which does not follow):
        //   1. The MOUNT is not a bindable next-hop. A bus link's own NAME is not a routable
        //      next-hop (ADR-0073 §3 — its `send()` BROADCASTS), and RFC-0020 §3 makes it a
        //      MUST that a residual below a bus mount is never resolved against the local
        //      graph. So no element may name the mount as the hop's egress. This is also what
        //      `bound_egress` enforces per frame, via the `eg.multi_peer` guard below —
        //      that guard protects the MOUNT, not a peer.
        //   2. A bus PEER has no vertex to name — for an ANNOUNCE-CENSUS peer. That is
        //      ADR-0044 §Decision 1, as scoped by its 2026-08-13 amendment (#1223): announced
        //      peers create no vertices, so there is nothing an element could reference. It is
        //      NOT entailed by fact 1 — a bus peer has a perfectly well-defined DIRECTED,
        //      pointer-stable endpoint (`bus_link_t::peer_link`, transport.hpp — "sends to
        //      THAT peer only", valid for the link's lifetime). An ACCEPTED ws/tcp session is
        //      outside that scope and may hold a vertex; when it does, this seam is what has
        //      to grow a slot for it.
        // A bound route over a bus therefore fails validation at this hop and the origin falls
        // back to canonical, which is exactly what the canonical spelling already does with the
        // bus link's own name.
        child_rx_ctx_t& bctx = acquire_ctx(name, rx);
        // The bus facet the frame paths resolve a peer handle's NAME through (#1294) —
        // recorded before publication, like every other resolved-once fact on this ctx.
        bctx.bus.store(bus, std::memory_order_relaxed);
        publish_ctx(bctx);
        // The session-identity pair (#1223 step 2). Arrival is a NEW seam and only an
        // ACCEPTING listener fires it — `set_peer_up_notifier` is what turns ADR-0044's
        // amended scope into a capability rather than a kind check here: an announce-census
        // bus never calls it, so a CAN peer still grows no vertex. Departure keeps doing
        // exactly what it did (`link_down`), with the anchor's retire in front of it.
        // The lifecycle seams carry the peer's HANDLE beside its name (#1294). The router
        // itself still keys its anchor and its eviction by NAME — a graph key is a path
        // segment — so it takes the name and lets the handle go; the consumers the handle
        // exists for (#1266's intern slot, #375 Part 2's subject) bind it at these two
        // sites, which is where it is minted and retired.
        bus->set_peer_up_notifier(
            [](void* c, peer_handle_t, std::string_view peer) {
                auto* const cc = static_cast<child_rx_ctx_t*>(c);
                cc->self->bus_peer_up(*cc, peer);
            },
            &bctx);
        bus->set_peer_down_notifier(
            [](void* c, peer_handle_t, std::string_view peer) {
                auto* const cc = static_cast<child_rx_ctx_t*>(c);
                cc->self->bus_peer_down(*cc, peer);
            },
            &bctx);
        if (bus->delivers_ropes()) {
            bus->set_peer_rope_receiver(
                [](void* c, peer_handle_t peer, view::rope_t frame) {
                    auto* const cc = static_cast<child_rx_ctx_t*>(c);
                    cc->self->on_frame_rope_bus(*cc, peer, std::move(frame));
                },
                &bctx);
        } else {
            bus->set_peer_receiver(
                [](void* c, peer_handle_t peer, std::span<const std::byte> frame) {
                    auto* const cc = static_cast<child_rx_ctx_t*>(c);
                    cc->self->on_frame_bus(*cc, peer, frame);
                },
                &bctx);
        }
        return true;
    }
    // Point-to-point: the inbound NAME is fixed per child, carried by a stable
    // per-child ctx (child_rx_ holds the address for the transport's lifetime).
    child_rx_ctx_t& ctx = acquire_ctx(name, rx);
    // No bus facet on this tenancy — CLEARED rather than left, because a re-add rebinds
    // this ctx (#884) and a name that used to be a bus mount must not keep answering with
    // the facet it no longer has (#1294).
    ctx.bus.store(nullptr, std::memory_order_relaxed);
    // The bound-path join (RFC-0024 §5.1), resolved ONCE per registration: the child's mount
    // run IS the canonical key of its connection vertex, so this is one map lookup of bytes
    // already in hand. A child whose vertex does not exist yet keeps `kNoConnSlot` and is
    // simply not bindable — a bound route through it fails validation and the origin falls
    // back, which is the degrade every other refusal in this design takes.
    //
    // Re-resolved on a re-add and not inherited (#884): the vertex a name denotes is a
    // property of the CURRENT tenancy. A child registered before its connection vertex
    // existed and re-added after it exists must come back bindable, and the reverse
    // direction — a name re-added as a BUS mount, which takes no `conn_slot` at all — must
    // not keep answering with the point-to-point slot it used to have.
    if (const std::optional<graph::vertex_handle_t> v = graph_.find(ctx.mount_tlv)) {
        if (const std::optional<graph::vertex_slot_t> slot = graph_.vertex_slot(*v))
            ctx.conn_slot.store(slot->index, std::memory_order_relaxed);
    }
    // LAST: every field a lock-free reader may look at is written above, and this is the
    // release edge that makes the node visible to one (see `child_rx_ctx_t::next`).
    publish_ctx(ctx);
    // Departure seam (RFC-0009 §D extended): the same stable ctx carries the child's
    // NAME to link_down when the transport reports its one connection dead.
    link.set_down_notifier(
        [](void* c) {
            auto* const cc = static_cast<child_rx_ctx_t*>(c);
            cc->self->link_down(cc->name);
        },
        &ctx);
    if (link.delivers_ropes()) {
        link.set_rope_receiver(
            [](void* c, view::rope_t frame) {
                auto* const cc = static_cast<child_rx_ctx_t*>(c);
                cc->self->on_frame_rope_impl(cc->name, std::move(frame), cc, false);
            },
            &ctx);
    } else {
        link.set_receiver(
            [](void* c, std::span<const std::byte> frame) {
                auto* const cc = static_cast<child_rx_ctx_t*>(c);
                cc->self->on_frame_impl(cc->name, frame, nullptr, cc, false);
            },
            &ctx);
    }
    return true;
}

bool fwd_router_t::remove_child(std::string_view name) {
    const std::lock_guard ctl(ctl_m_);  // pairs with add_child (ADR-0063 §3)
    // Stop resolving FIRST: once the entry is tombstoned no forward can reach the link,
    // so the caller is free to destroy the transport as soon as this returns. Then the
    // ordinary departure eviction reclaims the graph edges and label state — the same
    // work link_down does, reused rather than duplicated.
    if (!registry_.erase(name)) return false;
    // TOMBSTONE the receiver ctx (#884) — it stays on the published chain, because a lock-free
    // reader may be standing on it right now and the transport still holds its address, but it
    // stops answering `ctx_by_name`/`ctx_by_conn_slot`. Leaving it RESOLVING was the defect:
    // `add_child` of the same name appended a second ctx, and the name-keyed lookups answer
    // with the FIRST match, so every `connection_ref`/`hop_mint`/`adopt_binding` after a
    // re-add resolved the DEAD context — bound routes for the re-created child were minted
    // against the retired tenancy and never validated. A `release` store, paired with the
    // acquire each walk performs, so a reader either skips this node or has already passed it.
    if (child_rx_ctx_t* const ctx = ctl_ctx_by_name(name)) {
        ctx->retired.store(true, std::memory_order_release);
        // RFC-0027 §7.1's departure bump. The vertex this child's path label resolved to is
        // gone, so the label the peer still holds must stop validating — and the whole
        // mechanism for that is the slot's generation, which `release` advances (retiring the
        // slot permanently when it saturates, §4.3.1). No withdraw frame, no unbind, no lease
        // and no TTL (§7.3): the peer's next frame answers `NOT_FOUND` and it falls back to the
        // full-string path it still holds, re-minting from the reply after that.
        release_child_label(*ctx);
    }
    link_down(name);
    return true;
}

// The five sink setters. `sink_m_` serializes SETTERS against each other — the slot
// publishes for racing readers but does not arbitrate two concurrent publishes (#914).
// No reader ever takes it, so the frame path stays lock-free.

void fwd_router_t::on_reply(reply_fn_t fn, void* ctx) noexcept {
    const std::lock_guard lock(sink_m_);
    reply_.set(fn, ctx);
}

void fwd_router_t::on_inbound(inbound_fn_t fn, void* ctx) noexcept {
    const std::lock_guard lock(sink_m_);
    inbound_.set(fn, ctx);
}

void fwd_router_t::on_raw(raw_fn_t fn, void* ctx) noexcept {
    const std::lock_guard lock(sink_m_);
    raw_.set(fn, ctx);
}

void fwd_router_t::on_compact_delivery(compact_delivery_fn_t fn, void* ctx) noexcept {
    const std::lock_guard lock(sink_m_);
    delivery_.set(fn, ctx);
}

void fwd_router_t::on_stale_label(stale_label_fn_t fn, void* ctx) noexcept {
    const std::lock_guard lock(sink_m_);
    stale_.set(fn, ctx);
}

void fwd_router_t::clear_link(std::string_view link_name) { handles_.clear_link(link_name); }

/**
 * @brief The link-departure hook body: graph eviction first (deliveries to the dead
 *        session stop and its per-edge state is reclaimed), then the label-state drop
 *        (@ref fwd_router_t::clear_link — reused, not duplicated). See the header doc
 *        for the seam and threading contract.
 */
void fwd_router_t::link_down(std::string_view link_name) {
    graph_.evict_link_edges(link_name);
    clear_link(link_name);
}

std::string fwd_router_t::session_anchor_id(std::string_view mount, std::string_view peer) {
    std::string id;
    id.reserve(mount.size() + peer.size() + 2);
    id += ':';
    id += mount;
    id += '/';
    id += peer;
    return id;
}

void fwd_router_t::bus_peer_up(const child_rx_ctx_t& ctx, std::string_view peer) {
    // A tombstoned ctx names no live child (#884), so a late arrival from a transport whose
    // mount was already removed anchors nothing — the same skip every name-keyed consumer
    // makes. Read `retired` the way the frame paths do.
    if (ctx.retired.load(std::memory_order_acquire)) return;
    // PATH_IN_USE (the session is already anchored) is the only error this can answer, and
    // the right response to it is to keep the anchor that exists. Discarded by value.
    (void)graph_.register_session_anchor(session_anchor_id(ctx.name, peer));
}

void fwd_router_t::bus_peer_down(const child_rx_ctx_t& ctx, std::string_view peer) {
    // Retire BEFORE the eviction. Retirement is what bumps the saturating generation
    // (RFC-0024 §4.4 rule 3), and the slot this session sat in can be handed to the next
    // dialer the moment the poll thread returns from here — so the stamp has to have moved
    // by then, or the successor revives at a generation an element minted against the DEAD
    // session still matches. `find_session_anchor` answers nullopt for an announce-census
    // peer (which was never anchored) and for a double departure, both of which then take
    // the unchanged eviction-only path.
    if (const std::optional<graph::vertex_handle_t> anchor =
            graph_.find_session_anchor(session_anchor_id(ctx.name, peer)))
        (void)graph_.retire(*anchor);
    link_down(peer);
}

std::uint16_t fwd_router_t::advertise(std::string_view link_name,
                                      std::span<const std::byte> route_path) {
    transport_t* const link = registry_.by_name(link_name);
    if (link == nullptr) return 0;
    // ONE label per (link, route), never one per CALL (#913): this door IS the documented
    // self-heal, so a producer calls it on every (re)connect, and minting unconditionally
    // burned a label and leaked an egress entry per cycle. `ensure_egress` reuses the label
    // bound to an identical route, mints only for a new one, and returns 0 when the link's
    // space is exhausted or its egress table is full (#603) — nothing recorded, no frame.
    const std::uint16_t label = handles_.ensure_egress(link_name, route_path).first;
    if (label == 0) return 0;  // no label, no binding, no frame — the full-route form instead
    // The producer-side door shares the router's gather locus with the forwarding hop, exactly
    // as `send_compact` below does — so the public API and the peer-provoked re-advertise emit
    // the same bytes by construction and neither builds a frame (#885).
    emit_advertise(*link, label, route_path);
    return label;
}

void fwd_router_t::send_compact(std::string_view link_name, std::uint16_t label,
                                std::span<const std::byte> payload) {
    // The producer-side door shares the router's gather locus, so the public API and the
    // forwarding hop emit the same bytes by construction and neither allocates here.
    if (transport_t* const link = registry_.by_name(link_name)) emit_compact(*link, label, payload);
}

// --- bound paths (RFC-0024) and path labels (RFC-0027) -----------------------

namespace {
/**
 * @brief Rebuild a @ref peer_handle_t from the `bits()` a receiver ctx stores it as.
 *
 * The handle is kept as ONE `u64` on the ctx so a frame path loads it with a single relaxed
 * atomic rather than two that could straddle a control-plane rebind and pair one tenancy's
 * index with another's generation — the field-tearing shape #882 fixed one seam out.
 */
[[nodiscard]] constexpr peer_handle_t peer_handle_from_bits(std::uint64_t bits) noexcept {
    return peer_handle_t{.index = static_cast<std::uint32_t>(bits),
                         .generation = static_cast<std::uint32_t>(bits >> 32)};
}
}  // namespace

fwd_router_t::child_rx_ctx_t* fwd_router_t::ctl_ctx_by_name(std::string_view name) {
    // The owning deque, not the chain: this is the control plane, it holds `ctl_m_`, and it
    // must see a TOMBSTONE — which is exactly what the published walks refuse to return.
    for (child_rx_ctx_t& c : child_rx_)
        if (c.name == name) return &c;
    return nullptr;
}

fwd_router_t::child_rx_ctx_t& fwd_router_t::acquire_ctx(const std::string& name,
                                                        mem::block_source_t* rx) {
    if (child_rx_ctx_t* const hit = ctl_ctx_by_name(name)) {
        // One ctx per NAME, for the router's life — `child_registry_t::add`'s rule, one layer
        // out and for its two reasons (#884). A second ctx SHADOWS the first on every
        // name-keyed lookup (first match wins, and the first match is the older tenancy), and
        // churn on a stable name set would grow the chain every walk pays for, unboundedly.
        //
        // Hide it FIRST, so the rewrite below is never half-visible: a reader either sees the
        // outgoing tenancy whole, skips the node, or sees the incoming one whole. Relaxed —
        // nothing is being published here, and the publication edge is `publish_ctx`'s
        // release-store of `false`.
        hit->retired.store(true, std::memory_order_relaxed);
        // Rewound to "no connection vertex": the caller re-resolves it for the new tenancy,
        // and a BUS re-add of a formerly point-to-point name deliberately leaves it here.
        hit->conn_slot.store(kNoConnSlot, std::memory_order_relaxed);
        hit->rx.store(rx, std::memory_order_relaxed);
        // RFC-0027 §7.1: the tenancy this ctx's path label stood for is the one just hidden, so
        // the label goes with it — released (its slot's generation bumps, saturating and
        // retiring per §4.3.1) and forgotten. Nothing is told: there is no withdraw frame and
        // no lease (§7.3), and the peer discovers it on its next frame.
        release_child_label(*hit);
        // The per-peer identity advances with the tenancy, which is the second, independent
        // stamp: even inside the window before the slot's own bump is visible, a label minted
        // for the PREVIOUS tenancy of this name is owned by a handle that no longer compares
        // equal, so `path_label_table_t::lookup` refuses it on the owner check (§4.1's
        // node-scope rule). A departed tenancy and a departed vertex are different departures
        // and each gets its own stamp.
        hit->label_peer.store(next_label_peer_bits(), std::memory_order_relaxed);
        // `entry` is NOT rewritten, because it cannot have changed: a registry slot is keyed
        // by name and reused for that name forever (`erase` tombstones in place, `add`
        // rebinds the tombstone), and `add_child` has already re-added by the time we run.
        // The assert is where that coupling is pinned rather than assumed.
        assert(hit->entry == registry_.entry_by_name(name));
        // `name` and `mount_tlv` are immutable after the first publish, for the reason
        // `child_registry_t::add` gives about its own `mount_tlv`: a reader on a receive
        // thread may be holding either right now, and the bytes a rewrite would store are
        // identical anyway — the mount run is a pure function of the name.
        return *hit;
    }
    child_rx_ctx_t& fresh = child_rx_.emplace_back(this, name, registry_.mount_run_for(name), rx);
    fresh.entry = registry_.entry_by_name(name);
    // The §8.3 ceiling needs an identity a single far side cannot spend on everyone else's
    // behalf, and a point-to-point child has none of its own (#1294 mints handles for a bus
    // facet only). `kSolePeerHandle` is one constant for the whole node, so every child would
    // share one budget — precisely the case the ceiling exists to prevent. One handle per
    // child, minted here, is that doctrine applied at the granularity that makes it true.
    fresh.label_peer.store(next_label_peer_bits(), std::memory_order_relaxed);
    return fresh;
}

std::uint64_t fwd_router_t::next_label_peer_bits() noexcept {
    // Control-plane only, under `ctl_m_`. The generation half advances per REGISTRATION and
    // never returns to a value a peer might still hold a label against — the same
    // saturate-forward discipline as every other stamp in the doc set, and the reason a
    // re-added child's predecessor cannot be impersonated.
    const std::uint32_t gen = label_peer_seq_++;
    return (static_cast<std::uint64_t>(gen) << 32);
}

void fwd_router_t::publish_ctx(child_rx_ctx_t& ctx) noexcept {
    // Release-published either way: a reader that reaches the node sees a fully built one.
    if (ctx.linked) {
        // A revived tombstone is already reachable, so clearing the flag IS the publish —
        // linking it a second time would splice the chain onto itself and lose every node
        // between here and the tail.
        ctx.retired.store(false, std::memory_order_release);
        return;
    }
    if (rx_tail_ == nullptr) {
        rx_head_.store(&ctx, std::memory_order_release);
    } else {
        rx_tail_->next.store(&ctx, std::memory_order_release);
    }
    rx_tail_ = &ctx;
    ctx.linked = true;
}

const fwd_router_t::child_rx_ctx_t* fwd_router_t::ctx_by_name(std::string_view link_name) const {
    // The tombstone test comes FIRST and acquires: it is the edge that orders this node's
    // `conn_slot`/`rx` against the control thread's rewrite, so reading them before it would
    // be reading a tenancy that may already be half-replaced (#884).
    for (const child_rx_ctx_t* c = rx_head_.load(std::memory_order_acquire); c != nullptr;
         c = c->next.load(std::memory_order_acquire))
        if (!c->retired.load(std::memory_order_acquire) && c->name == link_name) return c;
    return nullptr;
}

const fwd_router_t::child_rx_ctx_t* fwd_router_t::ctx_by_conn_slot(std::uint32_t index) const {
    // A linear pass over the CHILDREN — one `u32` compare each, and the table is the node's
    // link count, not its traffic. It replaces the canonical descent's registry scan on this
    // frame rather than adding to it: a bound hop runs this instead of `resolve_mount_at`, not
    // as well as, so the per-hop work is strictly the smaller of the two (RFC-0024 §3.4).
    //
    // Walked over the PUBLISHED chain, never the owning deque: `add_child` appends from a
    // control thread while this runs on a receive thread, and iterating the deque under no
    // lock is a race on its chunk map (ADR-0063 erratum 3's ruling, applied to a container).
    //
    // A TOMBSTONED node is skipped before its slot is even compared (#884), and the acquire on
    // that test is what orders the compare against a concurrent rebind. Skipping is not
    // belt-and-braces here: it is what keeps this walk's length the LIVE child count under
    // create/remove churn, which is the property RFC-0024 §3.4 prices the bound hop against.
    if (index == kNoConnSlot) return nullptr;
    for (const child_rx_ctx_t* c = rx_head_.load(std::memory_order_acquire); c != nullptr;
         c = c->next.load(std::memory_order_acquire))
        if (!c->retired.load(std::memory_order_acquire) &&
            c->conn_slot.load(std::memory_order_relaxed) == index)
            return c;
    return nullptr;
}

std::optional<wire::path_ref_element_t> fwd_router_t::connection_ref(
    std::string_view link_name) const {
    // The walk skips tombstones (#884), so a removed child has no reference to give and a
    // re-added one gives its CURRENT tenancy's — never the dead context's, which is what made
    // a re-created child permanently unbindable.
    const child_rx_ctx_t* const ctx = ctx_by_name(link_name);
    if (ctx == nullptr) return std::nullopt;
    const std::uint32_t conn = ctx->conn_slot.load(std::memory_order_relaxed);
    if (conn == kNoConnSlot) return std::nullopt;
    const std::optional<graph::vertex_slot_t> slot = graph_.vertex_slot_at(conn);
    if (!slot) return std::nullopt;  // saturated ⇒ permanently unbindable (§4.4 rule 3)
    return wire::path_ref_element_t{.index = slot->index, .generation = slot->generation};
}

std::optional<wire::path_ref_element_t> fwd_router_t::hop_mint(
    std::string_view inbound_name, const child_rx_ctx_t* inbound_ctx) const {
    // The link a REPLY arrived on IS the link its request went out on, so the connection
    // vertex this hop selected on the way in is the one it answers with on the way back —
    // read from the frame's own arrival, never from a per-flow table (there is none).
    const child_rx_ctx_t* const ctx =
        inbound_ctx != nullptr ? inbound_ctx : ctx_by_name(inbound_name);
    // The tombstone is tested HERE as well as inside the walk, because a frame can still
    // arrive through the ctx a removed child left behind — the transport holds its address
    // and may not have been torn down yet. Minting for it would answer the origin with a
    // route through a link this node no longer has (#884).
    if (ctx == nullptr || ctx->retired.load(std::memory_order_acquire)) return std::nullopt;
    const std::uint32_t conn = ctx->conn_slot.load(std::memory_order_relaxed);
    if (conn == kNoConnSlot) return std::nullopt;
    const std::optional<graph::vertex_slot_t> slot = graph_.vertex_slot_at(conn);
    if (!slot) return std::nullopt;
    return wire::path_ref_element_t{.index = slot->index, .generation = slot->generation};
}

std::optional<wire::path_ref_element_t> fwd_router_t::reverse_hop_ref(
    std::string_view inbound_name, const child_rx_ctx_t* inbound_ctx, bool from_peer) const {
    /*
     * This hop's REVERSE-direction element (RFC-0024 §7.1 amendment 1): a reference for the
     * identity the request ARRIVED on. Point-to-point, that is the same connection vertex
     * the reply-direction mint answers with — one identity, two directions, one supplier.
     */
    if (!from_peer) return hop_mint(inbound_name, inbound_ctx);
    /*
     * Bus-session arrival: the accepted session's identity vertex — the #1254 anchor the
     * ADR-0044 amendment licensed. `inbound_name` is the session's routable name and the
     * ctx names the mount (`on_frame_bus` wiring), which is exactly `session_anchor_id`'s
     * key. An announce-census peer (CAN) was never anchored, answers nullopt here, and the
     * rebuild then STRIPS — CAN stays out of scope by construction, not by a special case.
     */
    if (inbound_ctx == nullptr || inbound_ctx->retired.load(std::memory_order_acquire))
        return std::nullopt;
    const std::optional<graph::vertex_handle_t> anchor =
        graph_.find_session_anchor(session_anchor_id(inbound_ctx->name, inbound_name));
    if (!anchor) return std::nullopt;
    const std::optional<graph::vertex_slot_t> slot = graph_.vertex_slot(*anchor);
    if (!slot) return std::nullopt;  // saturated => permanently unbindable (§4.4 rule 3)
    return wire::path_ref_element_t{.index = slot->index, .generation = slot->generation};
}

transport_t* fwd_router_t::bound_egress(wire::path_ref_element_t e, std::string_view caller,
                                        graph::acl_right_t right) const {
    // §5.1 in order: bounds, generation (both inside `deref_vertex_slot`, which also refuses a
    // saturated element), then the ACL at the DEREFERENCED vertex. Order matters only in that
    // the authorization comes last and is never skipped — a generation match authorizes
    // nothing (§6.2).
    const std::optional<graph::vertex_handle_t> v = graph_.deref_vertex_slot(e.index, e.generation);
    if (!v) return nullptr;
    if (!graph_.allows(*v, caller, right)) return nullptr;
    const child_rx_ctx_t* const ctx = ctx_by_conn_slot(e.index);
    if (ctx == nullptr) return nullptr;  // a vertex, but not one of this node's egresses
    // The child's registry slot, cached at registration — no per-frame name scan. The slot's
    // ADDRESS is fixed for the registry's lifetime; its CONTENTS are not, so the link and its
    // shape are still read atomically — as ONE word, so this reader cannot pair one
    // publication's shape with another's link (#882) — and a tombstone still drops the frame.
    const child_registry_t::child_t* const child = ctx->entry;
    if (child == nullptr) return nullptr;
    const child_registry_t::egress_t eg = child->egress();
    if (eg.multi_peer) return nullptr;
    return eg.link;  // null ⇒ tombstoned ⇒ drop
}

bool fwd_router_t::adopt_binding(graph::path_t& path, std::string_view link_name,
                                 const wire::tlv_t& reply) {
    // The mint answer is the reply's LAST child (RFC-0024 §7.1), so a reply that carries none
    // ends here and the path stays canonical — a request is a hint, never an obligation.
    if (reply.children.empty()) return false;
    const tlv_t& last = reply.children.back();
    if (last.type != type_t::PATH_REF) return false;
    const std::size_t n = wire::path_ref_element_count(last.payload.size());
    if (n == 0) return false;
    // Element 0 is this node's own, and nobody else could have written it: the hop out of the
    // origin is the one hop no peer ever sees (§4.1).
    const std::optional<wire::path_ref_element_t> own = connection_ref(link_name);
    if (!own) return false;
    std::vector<wire::path_ref_element_t> elements;
    elements.reserve(n + 1);
    elements.push_back(*own);
    for (std::size_t i = 0; i < n; ++i)
        elements.push_back(wire::path_ref_element_at(last.payload, i));
    return path.bind(elements);
}

std::optional<fwd_router_t::bound_dispatch_t> fwd_router_t::bound_dispatch(
    const graph::path_t& path, graph::acl_right_t right) const {
    const graph::path_binding_t& b = path.binding();
    if (!b.bound || b.elements.empty()) return std::nullopt;
    // The origin's caller is local, so the subject context is empty — the trusted-caller
    // convention every other local API here uses. The check still runs: it is the same one
    // line a forwarder runs, and having ONE of them is what keeps the two from drifting.
    transport_t* const link = bound_egress(b.elements.front(), {}, right);
    if (link == nullptr) return std::nullopt;
    bound_dispatch_t out;
    out.link = link;
    if (!wire::emit_path_ref(out.dst,
                             std::span<const wire::path_ref_element_t>(b.elements).subspan(1)))
        return std::nullopt;
    return out;
}

template <class Cursor, class Reject>
bool fwd_router_t::route_bound_session_delivery(std::string_view inbound_name,
                                                const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                                const Cursor& cur, const fwd_pre_t& pre,
                                                Reject&& reject) {
    // Only a WRITE can be a delivery (delivery-is-a-write, RFC-0004 §D); every other op with
    // a one-element residual keeps its bound-terminus meaning untouched. Read off the offset
    // the peek carried, masked (§9.3), exactly as route_bound_forward reads it.
    if (pre.op_body_len == 0) return false;  // malformed => the terminus tier's refusal stands
    if (static_cast<fwd_op_t>(cur.byte_at(pre.op_body_off) & graph::kFwdOpcodeMask) !=
        fwd_op_t::WRITE)
        return false;
    const wire::path_ref_element_t e = read_path_ref_element(cur, pre.dst_body_off);
    // §5.1 bounds + generation. THIS is the disclosure fix (#1223): a dead session's element
    // carries the generation its anchor was retired at, the recycled slot's revived anchor
    // reads one higher, and the delivery for the DEAD session refuses here instead of
    // reaching the unrelated successor. §5.3 requires the failure be a drop plus a NACK —
    // and the NACK is load-bearing, not politeness: it is the addressed refusal the
    // producer's step-5 reclaim (#1258) correlates to retire the stale edge on first use.
    const std::optional<graph::vertex_handle_t> v = graph_.deref_vertex_slot(e.index, e.generation);
    if (!v) {
        reject(graph::status_t::INVALID_PATH);
        return true;
    }
    const std::optional<graph::graph_t::session_anchor_route_t> ar =
        graph_.session_anchor_route(*v);
    if (!ar) return false;  // an ordinary vertex: the bound TERMINUS path, unchanged
    // §6.2's re-check at the dereferenced vertex, per delivery, under the inbound link's
    // subject — a generation match authorizes nothing. A denial is a plain drop (the
    // anti-enumeration rule: denied answers denied-shaped silence on this data-plane leg).
    if (!graph_.allows(*v, inbound_name, graph::acl_right_t::WRITE)) return true;
    // The egress is the SESSION itself: the anchor's key names mount and peer, and the
    // directed per-peer endpoint is resolved against that mount alone (`resolve_peer` —
    // never the cross-bus scan, so two servers' same-named peers stay distinct). A session
    // that departed between the deref and this lookup is a refusal like any other.
    const child_registry_t::child_t* const entry = registry_.entry_by_name(ar->mount);
    transport_t* const session =
        entry != nullptr ? child_registry_t::resolve_peer(*entry, ar->peer) : nullptr;
    if (session == nullptr) {
        reject(graph::status_t::INVALID_PATH);
        return true;
    }
    // Forward through the ONE rebuild locus: consume the element (the peek already set
    // `strip_at` one element in), and re-head the emptied `dst` as a canonical PATH — the
    // peer behind an accepted session is an ORIGIN, which never speaks the bound form, so
    // the frame it receives is byte-identical to the canonical delivery it always got.
    fwd_pre_t session_pre = pre;
    session_pre.dst_to_path = true;
    route_fwd_forward(inbound_name, inbound_ctx, from_peer, 0, cur, *session, &session_pre);
    return true;
}

template <class Cursor, class Reject>
bool fwd_router_t::route_label_forward(std::string_view inbound_name,
                                       const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                       const Cursor& cur, const fwd_pre_t& pre, Reject&& reject) {
    // The `dst` body window the peek already opened — no header is re-read to find it.
    if (pre.dst_body_off >= pre.dst_end) return false;
    const std::size_t body_len = pre.dst_end - pre.dst_body_off;
    if (body_len < wire::kPathLabelRecordBytes) return false;  // too short to BE a label

    // Only the FIRST element can be this hop's own local part: a path is read left to right and
    // every hop reads the element that stands where its mount run stood (§5.2's rule that an
    // element self-describes by kind and is read by the hop whose part it is — never by
    // position within somebody else's part). Copy just the record's bytes off the cursor, which
    // is the one place a rope may straddle a link; seven bytes on the stack, no allocation.
    std::array<std::byte, wire::kPathLabelRecordBytes> head{};
    for (std::size_t i = 0; i < head.size(); ++i)
        head[i] = static_cast<std::byte>(cur.byte_at(pre.dst_body_off + i));
    const wire::path_element_t el = wire::path_element_at(head, 0);
    // NOT a label ⇒ this is the overwhelmingly common case and it must cost nothing. A literal
    // segment, a foreign escape and a ragged record all answer the same way: false, and the
    // caller runs the canonical mount descent exactly as it did before labels existed. This is
    // where `dispatch_edge_target`'s fall-through shape lives — the branch a string path takes.
    if (el.kind != wire::path_element_kind_t::LABEL) return false;

    // From here the address IS labelled, and §7.2 governs every exit. A host with no table
    // never minted this label, so it cannot validate it and MUST NOT guess: the answer is the
    // same `NOT_FOUND`-class refusal a stale label gets, which is exactly right — "I did not
    // mint this" and "I no longer honour this" are one case to the sender, and its recovery
    // from both is the full-string path it still holds.
    // The peer a label was minted FOR is the far side of the link it arrived on, and the
    // identity of that link is its ctx (resolved by name when the frame came in through the
    // public `on_frame`, exactly as the mint side resolves it). `lookup`'s owner check is half
    // of §4.1's node-scope rule: a label leaked to a different link buys one `NOT_FOUND`.
    const child_rx_ctx_t* const ctx =
        inbound_ctx != nullptr ? inbound_ctx : ctx_by_name(inbound_name);
    const peer_handle_t peer =
        ctx != nullptr ? peer_handle_from_bits(ctx->label_peer.load(std::memory_order_relaxed))
                       : peer_handle_t{};
    const std::optional<path_label_target_t> target =
        labels_ != nullptr && peer.valid() ? labels_->lookup(peer, el.label) : std::nullopt;
    if (!target) {
        // Counted, per `dispatch_edge_target`'s discipline: a refused deref is a fact about the
        // route, and a bound that silently discards work is indistinguishable from one never
        // reached. NO repair of any kind — no re-resolution against a nearest match, no retry
        // against another slot, no guessing, and no fall-through to the canonical walk, because
        // the label REPLACED the string bytes and there is nothing left to walk.
        label_not_found_.fetch_add(1, std::memory_order_relaxed);
        reject(graph::status_t::NOT_FOUND);
        return true;
    }

    // The op's own right at the dereferenced vertex — §8.2, and the reading is RFC-0024 §6.2's
    // unchanged, because it is the same question. A REPLY carries no right of its own and a
    // labelled REPLY is not a shape a hop emits toward a peer, so it is refused rather than
    // guessed at; an opcode this build cannot name a right for is refused for the reason
    // `route_bound_forward` gives at length — guessing a right is how a write-like future
    // opcode crosses a READ-only gate.
    const auto op = static_cast<fwd_op_t>(cur.byte_at(pre.op_body_off) & graph::kFwdOpcodeMask);
    graph::acl_right_t right = graph::acl_right_t::READ;
    switch (op) {
        case fwd_op_t::READ:
        case fwd_op_t::AWAIT:
            right = graph::acl_right_t::READ;
            break;
        case fwd_op_t::WRITE:
            right = graph::acl_right_t::WRITE;
            break;
        default:
            label_not_found_.fetch_add(1, std::memory_order_relaxed);
            reject(graph::status_t::NOT_FOUND);
            return true;
    }
    // §8.2's re-check runs inside `bound_egress`, and the reuse is the POINT: a labelled
    // operation evaluates `acl_allows` at the dereferenced vertex, for that operation's own
    // right, through the identical code the bound form runs — a generation match authorizes
    // nothing. One implementation is what makes the string and label spellings' outcomes
    // byte-identical by construction rather than by assertion, which is the property
    // `acl/label-vs-string-allow` and `acl/label-vs-string-deny` exist to pin.
    transport_t* const link = bound_egress(*target, inbound_name, right);
    if (link == nullptr) {
        label_not_found_.fetch_add(1, std::memory_order_relaxed);
        reject(graph::status_t::NOT_FOUND);
        return true;
    }
    // Consume the label element and forward the residual, through the ONE rebuild locus. One
    // label covers the hop's WHOLE local part (§5.3.3), so what is stripped is one element
    // standing for a whole mount run — `strip_at` is where that element ends, and `strip_k` is
    // 1 because the rebuild counts ELEMENTS of the body it re-heads, not segments of a name.
    fwd_pre_t label_pre = pre;
    label_pre.strip_at = pre.dst_body_off + el.bytes;
    label_pre.valid = true;
    label_resolves_.fetch_add(1, std::memory_order_relaxed);
    // No mint on this leg, and it is not an omission: a labelled `dst` on a REPLY was already
    // refused by the opcode switch above, and §6.1 mints on the reply and only the reply. A
    // reply's `dst` is the request's ACCUMULATED `src`, which grows in mount runs on request
    // legs, so a labelled reply-`dst` is not a shape this design produces.
    route_fwd_forward(inbound_name, inbound_ctx, from_peer, 1, cur, *link, &label_pre);
    return true;
}

template <class Cursor, class Reject>
bool fwd_router_t::route_bound_forward(std::string_view inbound_name,
                                       const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                       const Cursor& cur, const fwd_pre_t& pre,
                                       std::size_t element_count, Reject&& reject) {
    // 0 elements: this node is the terminus.
    if (element_count == 0) return false;
    // EXACTLY one element: usually the bound terminus — but a WRITE whose one element
    // dereferences to a SESSION ANCHOR is the reverse-list delivery's last hop (RFC-0024
    // §7.1 amendment 1, #1223 step 4), and the ANSWER to a failed validation is §5.3's
    // NACK, which is what lets the producer's step-5 reclaim retire the stale edge.
    if (element_count == 1)
        return route_bound_session_delivery(inbound_name, inbound_ctx, from_peer, cur, pre,
                                            std::forward<Reject>(reject));
    // The op's own right, at the dereferenced vertex (§6.2). AWAIT reads, so it asks for READ;
    // a REPLY carries no right of its own — it is the answer to an op already authorized at
    // every gate on the way in — and a bound REPLY is not a shape this node ever emits, so it
    // is refused rather than guessed at. An opcode this build does not know is refused for the
    // same reason and NOT charged the READ right it happens to have initialized: guessing a
    // right for an unknown operation is how a write-like future opcode would cross a
    // READ-only gate. A hop that cannot name the right an operation carries cannot evaluate
    // §6.2 for it, so it does not forward it.
    //
    // Read off the offset the peek already carried, never through `peek_fwd_op`: that would
    // re-parse the FWD and op headers a third time on a frame whose whole cost story is how
    // few times its headers are read. Masked, because bits 7-6 are flags (§9.3).
    if (pre.op_body_len == 0) return true;  // no op byte at all ⇒ malformed ⇒ drop
    const auto op = static_cast<fwd_op_t>(cur.byte_at(pre.op_body_off) & graph::kFwdOpcodeMask);
    graph::acl_right_t right = graph::acl_right_t::READ;
    switch (op) {
        case fwd_op_t::READ:
        case fwd_op_t::AWAIT:
            right = graph::acl_right_t::READ;
            break;
        case fwd_op_t::WRITE:
            right = graph::acl_right_t::WRITE;
            break;
        case fwd_op_t::REPLY:
            return true;  // drop
        default:
            return true;  // an opcode with no known right ⇒ drop
    }
    const wire::path_ref_element_t e = read_path_ref_element(cur, pre.dst_body_off);
    transport_t* const child = bound_egress(e, inbound_name, right);
    // §5.3: no re-resolution, no nearest match, no retry against a different vertex, and NO
    // fall-through to the terminus — a bound frame this node cannot route is dropped, and the
    // origin's recovery is the canonical path it still holds.
    //
    // Honest about what this line is: today it is a REDUNDANT EARLY-OUT, not a proven guard.
    // Ablated to `return false`, no test moves, because the terminus tier refuses a residual
    // that is not exactly one element and drops it too (`op_resolve_walk.hpp`) — the same
    // outcome by a longer road. It is written this way so the POLICY lives where the decision
    // is made rather than being inherited from a downstream refusal that is free to change,
    // and nothing may cite it as a measured guard. The refusals it reports — the deref, the
    // ACL, the egress lookup — are each pinned by ablation in `bound_forward_test`.
    if (child == nullptr) return true;
    route_fwd_forward(inbound_name, inbound_ctx, from_peer, 0, cur, *child, &pre);
    return true;
}

void fwd_router_t::on_frame(std::string_view inbound_name, std::span<const std::byte> frame) {
    on_frame_impl(inbound_name, frame, nullptr);
}

std::string_view fwd_router_t::resolve_peer_name(const child_rx_ctx_t& ctx, peer_handle_t peer,
                                                 std::span<char> scratch) {
    bus_link_t* const bus = ctx.bus.load(std::memory_order_relaxed);
    if (bus == nullptr) return {};
    return bus->peer_name(peer, scratch);
}

void fwd_router_t::on_frame_bus(const child_rx_ctx_t& ctx, peer_handle_t peer,
                                std::span<const std::byte> frame) {
    // The handle is the seam's identity; the routing plane's is the NAME, so it is resolved
    // here, once, on the transport's own receive thread — the scratch outlives the whole
    // routing call below, which is what the resolved view may point into.
    std::array<char, kPeerNameChars> scratch{};
    const std::string_view name = resolve_peer_name(ctx, peer, scratch);
    if (name.empty()) return;  // no bus facet, or a handle this kind does not name
    on_frame_impl(name, frame, nullptr, &ctx, true);
}

void fwd_router_t::on_frame_rope(std::string_view inbound_name, view::rope_t frame) {
    on_frame_rope_impl(inbound_name, std::move(frame), nullptr, false);
}

void fwd_router_t::on_frame_rope_bus(const child_rx_ctx_t& ctx, peer_handle_t peer,
                                     view::rope_t frame) {
    std::array<char, kPeerNameChars> scratch{};
    const std::string_view name = resolve_peer_name(ctx, peer, scratch);
    if (name.empty()) return;  // see on_frame_bus
    on_frame_rope_impl(name, std::move(frame), &ctx, true);
}

template <class Cursor, class Observe, class Reject, class Terminus, class Reply>
bool fwd_router_t::route_fwd_ingress(std::string_view inbound_name, const Cursor& cur,
                                     const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                     Observe&& observe, Reject&& reject, Terminus&& terminus,
                                     Reply&& reply) {
    // The classification gate is the frame's own TYPE byte, and that is the whole of it: a
    // FWD frame is data-plane, and no peek failure below can turn it back into a control
    // frame. The rope arm used to gate on `peek_fwd_dst_any` and then on `peek_fwd_op`
    // answering, and fell through to the control sink when either refused — where
    // `peek_control` rejects a FWD outright, so the frame was dropped. The span arm reached
    // its terminus instead. The two peeks do not agree on an EMPTY op VALUE (`peek_fwd_dst_any`
    // accepts it — see `fwd_pre_t::op_body_len`, which records that reading deliberately —
    // and `peek_fwd_op` answers nullopt), so a bound-`dst` FWD carrying one was resolved
    // contiguously and vanished when the same bytes arrived as a multi-link rope. The span
    // arm's disposition is the one kept: FWD-classified ⇒ terminus, never control.
    if (static_cast<type_t>(cur.byte_at(0)) != type_t::FWD) return false;
    // Read-only observer (tests/ACL seam) — wants the tree. Span-only: no rope caller has
    // a contiguous frame to decode here, so its instantiation of this driver passes a no-op.
    observe();
    // ONE peek classifies the `dst` and opens its window: canonical PATH, bound PATH_REF, or
    // neither. Its verdict is kept, because `resolve_mount_at` overwrites `pre.valid` to mean
    // something else entirely once the descent has run ("nothing to hand the rebuild"), and
    // only the peek says which form the `dst` was.
    //
    // It also serves both jobs at once on the rope tier: deciding the frame carries a
    // descendable address, and opening the `dst` window the mount descent then walks. It used
    // to be two there — the gate peeked and threw the result away, then `resolve_mount` walked
    // the same TLV headers again, and on a multi-link rope those headers may straddle links,
    // so the second walk was the expensive kind.
    fwd_pre_t pre;
    std::size_t ref_count = 0;
    const fwd_dst_kind_t kind = peek_fwd_dst_any(cur, pre, ref_count);
    // RFC-0027 §7.2's label branch, BESIDE the mount descent and ahead of it. Ahead, because a
    // labelled first element is not a name and folding a digest chain over it would be reading
    // somebody's slot index as UTF-8; and gated on the same peek verdict the bound arm is
    // gated on, because only a canonical `PATH` has elements at all.
    //
    // The cost to a string-only node is one test of a member pointer against null. A node with
    // no injected table (§6.3's conformant default) never enters, never reads the `dst` body's
    // first bytes, and reaches `resolve_mount_at` having executed one not-taken branch — which
    // is the whole of what the plain-string forwarding path pays for this RFC.
    if (labels_ != nullptr && kind == fwd_dst_kind_t::PATH_LABEL &&
        route_label_forward(inbound_name, inbound_ctx, from_peer, cur, pre, reject))
        return true;
    {
        // The reader outlives the forward hop on purpose: a resolved bus PEER name is a view
        // into its retained stitch slot and is referenced right through the egress `gather`.
        seg_reader_t<Cursor> rd{cur};
        // Resolve the mount prefix (ADR-0061 strip-K, single-pass since #523). Segments are
        // walked lazily and read in place when the source keeps them contiguous, stitched
        // into the reader's slot when they straddle a rope link; an over-long segment is not
        // routable ⇒ fall to the terminus.
        const mount_hit_t hit = kind == fwd_dst_kind_t::PATH
                                    ? resolve_mount_at(registry_, cur, rd, pre)
                                    : mount_hit_t{};
        if (hit.link != nullptr) {
            // §11.2, and §6.1's mint decision, made HERE rather than inside the hop. The
            // address is a canonical `PATH` and not a `PATH_REF`, so this leg MAY mint — it is
            // the leg RFC-0027 exists for, the first string-spelled walk of a route whose reply
            // carries the mint home. Every other call site passes `{}`, so §11.2's mutual
            // exclusion is structural: a bound leg cannot reach the mint at all.
            //
            // It is computed here and not in `route_fwd_forward` for a MEASURED reason. That
            // hop is `bench/symbol_ratchet.json`-pinned and `rebuild_fwd_forward` is
            // `[[gnu::flatten]]`, so anything the hop decides is decided three times over in
            // the flattened body: carrying the decision there cost **+688 B** on
            // `route_fwd_forward<rope_cursor>`, and carrying it as a lazy closure still cost
            // +512 B. Reduced to a span the hop only forwards, the pinned symbol stays flat and
            // the branch lands here, on the driver that was already switching on `kind`.
            const std::span<const std::byte> reply_label =
                labels_ != nullptr
                    ? label_src_prefix(inbound_name, inbound_ctx, cur, &pre, hit.link_name)
                    : std::span<const std::byte>{};
            route_fwd_forward(inbound_name, inbound_ctx, from_peer, hit.strip_k, cur, *hit.link,
                              &pre, reply_label);
            return true;
        }
        if (hit.rejected) {  // bus NAME + residual: never broadcast, never terminus
            reject(graph::status_t::INVALID_PATH);
            return true;
        }
        // A BOUND `dst` with a residual longer than one element: this node is a FORWARDER for
        // it (RFC-0024 §4.1). Tried before the terminus conclusion below, because a bound
        // forward and a bound terminus are told apart by the element COUNT and by nothing
        // else — and getting that wrong the other way would apply a passing operation here.
        //
        // Gated on the PEEK's verdict, and the gate is a cost decision as much as a
        // correctness one: a frame whose `dst` is a canonical PATH of NAMEs cannot be a bound
        // hop, and the classification the peek already made is what says so — the hop re-reads
        // no header to find out, which is what keeps a bound terminus from costing more than
        // the canonical terminus it is supposed to beat.
        if (kind == fwd_dst_kind_t::PATH_REF &&
            route_bound_forward(inbound_name, inbound_ctx, from_peer, cur, pre, ref_count, reject))
            return true;
    }
    // The `dst` names no mount here and no bound hop took it ⇒ this node is its terminus.
    // The `PATH` arm above is the mount descent's gate, not a frame classifier: it says "this
    // frame has an address this node can descend", and a BOUND dst (`PATH_REF`, RFC-0024 §5)
    // has no NAME to descend on.
    if (peek_fwd_op(cur) == fwd_op_t::REPLY) {
        // The accumulated return route is fully consumed — this node is the originator.
        reply();
        return true;
    }
    terminus();
    return true;
}

void fwd_router_t::on_frame_rope_impl(std::string_view inbound_name, view::rope_t frame,
                                      const child_rx_ctx_t* inbound_ctx, bool from_peer) {
    // Single-link (every current producer): the link's bytes span feeds the SAME
    // routing as the borrowed path — the forward hop below never touches the
    // refcount (zero-heap, ADR-0038); only the terminus sees the owner, for the
    // ADR-0042 §3 referenced store. This is the pre-ADR-0053 view path, unchanged.
    if (frame.link_count() == 1) {
        const view_t& v = frame.links()[0];
        if (v.is_device()) return;  // CPU routing cannot read a DEVICE frame
        on_frame_impl(inbound_name, v.bytes(), &v, inbound_ctx, from_peer);
        return;
    }
    // Multi-link: route a FORWARD hop directly over the rope — NO flatten
    // (ADR-0053 ④b). The forward-vs-terminus split is read through the link-walking
    // grammar cursor and the egress scatter-gathers the untouched links; a header or
    // trailer straddling a link boundary is stitched by the cursor (grammar.hpp). Only
    // a terminus / reply / control frame — which still needs a contiguous decode (the
    // rope-aware sink is the migration ⑤/⑥ follow-on) — pays the one flatten fallback.
    // A device link cannot be CPU-read, so a non-all-host rope skips straight to the
    // fallback (flatten drops it, as before).
    if (frame.total_length() >= 4 && frame.all_host()) {
        const wire::grammar::rope_cursor cur{frame};
        if (route_fwd_ingress(
                inbound_name, cur, inbound_ctx, from_peer,
                /* observe */ [] {},
                /* reject */
                [&](graph::status_t status) {
                    // The rejection reply needs a contiguous decode; this is a COLD error
                    // path, so the one flatten is the ADR-0052 legitimate kind (exactly the
                    // control-plane precedent). Through the injected byte backend (#730), so
                    // a bounded node's memory bound covers this flatten too.
                    const std::expected<view_t, view::flatten_err_t> flat =
                        frame.subrope(0, frame.total_length()).try_materialize(*flat_);
                    // Flatten REFUSED ⇒ drop the frame. This arm is all-host-guarded above,
                    // so the refusal is the OOM — named by the error channel now rather than
                    // inferred from an empty view a zero-byte success could fake (#917). Still
                    // a REDUNDANT EARLY-OUT, like the ADVERTISE arm's: `reject_bus_name_hop`
                    // opens with a `wire::decode`, an empty span does not decode, and it
                    // returns without replying — so deleting this line changes no observable
                    // behaviour. Kept so the reason is the OOM and not the codec's leniency.
                    // The SEAM on the line above is the testable part, and
                    // `fwd_flatten_backend_test` pins it.
                    if (!flat) return;
                    reject_bus_name_hop(registry_, inbound_name, flat->bytes(), *egress_, status);
                },
                /* terminus */
                [&] {
                    // A request FWD is resolved straight off the rope (ADR-0053 3c-iii — NO
                    // flatten, verify-at-access §4).
                    resolve_terminus_rope(inbound_name, std::move(frame));
                },
                /* reply */
                [&] {
                    // The step-5 reclaim (#1223) peeks the rope IN PLACE; only an actual
                    // RFC-0020 refusal pays the one cold flatten (the reject arm's ADR-0052
                    // precedent, same injected backend), because the route compare needs the
                    // echoed src contiguous.
                    if (peek_refused_route(cur)) {
                        const std::expected<view_t, view::flatten_err_t> flat =
                            frame.subrope(0, frame.total_length()).try_materialize(*flat_);
                        if (flat) reclaim_refused_route(inbound_name, flat->bytes());
                    }
                    // A REPLY that reaches its originator here is handed to the sink
                    // rope-native (ADR-0055): NO flatten — the sink materializes on demand.
                    // Absent sink ⇒ dropped (as the flatten path would, into a no-op decode).
                    if (const auto sink = reply_.get(); sink.fn != nullptr)
                        sink.fn(sink.ctx, frame);
                }))
            return;
    }
    // Control frame (or a device/short rope): served rope-native (ADR-0055 §2/§3). The
    // route-handle sinks read the label off the rope and materialize only the sub-rope
    // they need contiguous — the interim whole-frame flatten is gone (ADR-0053 ⑥).
    on_control_rope(inbound_name, std::move(frame));
}

void fwd_router_t::on_frame_impl(std::string_view inbound_name, std::span<const std::byte> frame,
                                 const view_t* frame_view, const child_rx_ctx_t* inbound_ctx,
                                 bool from_peer) {
    if (const auto sink = raw_.get(); sink.fn != nullptr) sink.fn(sink.ctx, inbound_name, frame);
    if (frame.size() < 4) return;

    // The FWD plane never builds a tlv_t (ADR-0038 inv. #1 / ADR-0041 §5): the
    // forward-vs-terminus split and the op discriminant are read by OFFSET; a
    // forward hop scatter-gathers with zero heap; a terminus request decodes into
    // the pmr arena. Only the originator REPLY sink and the control frames below
    // keep the owning wire::decode (test/SDK-facing and flow-setup paths, allowed
    // to allocate per ADR-0039).
    const wire::grammar::span_cursor cur{frame};
    if (route_fwd_ingress(
            inbound_name, cur, inbound_ctx, from_peer,
            /* observe */
            [&] {
                if (const auto sink = inbound_.get(); sink.fn != nullptr) {
                    if (const auto dec = wire::decode(frame); dec && dec->opt.pl)
                        sink.fn(sink.ctx, inbound_name, *dec);
                }
            },
            /* reject */
            [&](graph::status_t status) {
                reject_bus_name_hop(registry_, inbound_name, frame, *egress_, status);
            },
            /* terminus */ [&] { resolve_terminus(inbound_name, frame, frame_view, inbound_ctx); },
            /* reply */
            [&] {
                // The step-5 reclaim (#1223) runs BEFORE the sink and without one: an
                // addressed RFC-0020 refusal evicts the edge that stored the refused route,
                // whether or not anything else is listening for replies. Not a refusal ⇒
                // the peek bails allocation-free.
                reclaim_refused_route(inbound_name, frame);
                // Hand the FWD{REPLY} to the sink rope-native (ADR-0055): NO decode. A
                // view-delivered frame ropes zero-copy off its owning view; a borrowed span is
                // copied once into an owned segment (the copy the old decode-then-consumer-
                // encode round-trip already paid).
                if (const auto sink = reply_.get(); sink.fn != nullptr) {
                    if (frame_view != nullptr) {
                        sink.fn(sink.ctx, view::rope_t(*frame_view));
                    } else if (view_t owned = view::over_bytes(frame).value_or(view_t{});
                               !owned.empty()) {
                        sink.fn(sink.ctx, view::rope_t(std::move(owned)));
                    }
                }
            }))
        return;

    // Control frames are read BY OFFSET — the span arm was the last reader in the ingress
    // plane still building an owning `tlv_t`.
    //
    // That owning decode was justified by ADR-0041 §5 / ADR-0055 §3 as a flow-setup cost,
    // "allowed to allocate per ADR-0039". ADR-0062 invalidated that premise: a warm COMPACT
    // is now the steady-state per-sample data frame, not setup. It cost 3 allocations for
    // the tree spine plus 5 more re-encoding a payload that is ALREADY contiguous in
    // `frame` — together ~55-63% of a warm terminus frame.
    //
    // The child window is ALREADY contiguous on this tier, so the make-contiguous seam the
    // switch takes is a plain `subspan` — no copy, and the rope tier's OOM arms are inert
    // here because a subspan of a non-empty window cannot come back empty.
    dispatch_control(inbound_name, cur, [frame](std::size_t off, std::size_t total) {
        return frame.subspan(off, total);
    });
}

std::span<const std::byte> fwd_router_t::child_label_record(std::string_view inbound_name,
                                                            const child_rx_ctx_t& ctx,
                                                            peer_handle_t peer) {
    if (!peer.valid()) return {};  // no egress identity to own the label ⇒ stays a string
    // Already minted, FOR THIS PEER: the steady state, and it never touches the table. Acquire,
    // so the seven encoded bytes read below are the ones the minting thread finished writing.
    // A label minted for a different origin is deliberately not reused — see `path_label_for`:
    // handing it over would shorten a frame on behalf of a peer whose `lookup` must refuse it.
    if (const std::uint32_t bits = ctx.path_label.load(std::memory_order_acquire); bits != 0)
        return ctx.path_label_for.load(std::memory_order_relaxed) == peer.bits()
                   ? std::span<const std::byte>(ctx.path_label_tlv)
                   : std::span<const std::byte>{};

    // §6.2's trigger fires HERE and nowhere else: the first reply this hop relays over this
    // child. No use counter, no hit threshold, no hotness estimate, no timer, no aging — the
    // condition is that the hop is already holding the resolution and has not yet spelled it.
    // What the label ALIASES is the connection vertex of the link the reply came back over —
    // the identical element `hop_mint` gives RFC-0024's reverse list, and the identical element
    // `bound_egress` consumes on the way back in. The two directions compose because they name
    // the same thing: what this hop mints on a reply is what it dereferences on the next
    // request. A node with nothing to give answers nullopt and leaves the part a string.
    const std::optional<wire::path_ref_element_t> target = hop_mint(inbound_name, &ctx);
    if (!target) return {};
    // §8.1 is structural at this point and stated so it is not lost: the mint rides an
    // operation that already ran every gate on its way to the terminus, so no label is ever
    // minted for a destination an ancestor ACL hides. Probing a labelled route yields what
    // probing the string form yields — exists + denied, never exists + here is a handle.
    const std::optional<wire::path_label_t> label = labels_->mint(peer, *target);
    if (!label) return {};  // §8.3 ceiling/capacity/all-retired ⇒ refuse-new, NOT an error

    // Encode once, into the ctx, so every later reply is the load above. `emit_path_label`
    // cannot fail on a label the table minted (a minted label is always `valid()`), and the
    // vector is local because the ctx's storage is a fixed 7-byte array.
    std::vector<std::byte> rec;
    if (!wire::emit_path_label(rec, *label) || rec.size() != wire::kPathLabelRecordBytes) {
        (void)labels_->release(*label);  // spelling refused ⇒ do not strand the slot
        return {};
    }
    auto& ctx_mut = const_cast<child_rx_ctx_t&>(ctx);
    std::copy(rec.begin(), rec.end(), ctx_mut.path_label_tlv.begin());
    ctx_mut.path_label_for.store(peer.bits(), std::memory_order_relaxed);
    // Release-store LAST: the bytes AND the owning peer are whole before anything can observe
    // the label as present, so a reader that sees it never pairs it with the wrong owner.
    ctx_mut.path_label.store(label->bits(), std::memory_order_release);
    return std::span<const std::byte>(ctx.path_label_tlv);
}

std::span<const std::byte> fwd_router_t::terminus_label_record(std::string_view inbound_link,
                                                               path_label_target_t target) {
    if (labels_ == nullptr) return {};
    // The reply leaves over the link the request arrived on, so ONE ctx answers both halves of
    // "who owns this label" and "where is it cached". A frame delivered through the public
    // `on_frame` carries no ctx down here at all — the terminus is reached through the
    // resolver, which knows only the link's NAME — so this is always the by-name lookup, which
    // is `hop_mint`'s own fallback and correct for the same reason: the answer is a property
    // of the LINK, not of how the frame reached the router.
    const child_rx_ctx_t* const ctx = ctx_by_name(inbound_link);
    if (ctx == nullptr || ctx->retired.load(std::memory_order_acquire)) return {};
    // A BUS arrival is left a string, for the reason `label_src_prefix` gives at length: this
    // ctx's `label_peer` is one identity for the whole child, so a label cached on it would be
    // presentable by EVERY peer behind the bus — the owner check that makes §4.1's node-scope
    // rule bite would be checking the wrong thing. §6.3 makes the refusal free.
    if (ctx->bus.load(std::memory_order_relaxed) != nullptr) return {};
    const peer_handle_t peer =
        peer_handle_from_bits(ctx->label_peer.load(std::memory_order_relaxed));
    if (!peer.valid()) return {};  // no identity to own the label ⇒ the residual stays a string
    // The residual, packed into one word so the steady state is one relaxed load and one
    // compare. Both halves are load-bearing: an index without the generation current when it
    // was read names a slot, not a vertex.
    const std::uint64_t want = (static_cast<std::uint64_t>(target.index) << 32) | target.generation;
    // Already minted, for THIS peer and THIS residual: the steady state, and it never touches
    // the table. Acquire, so the seven encoded bytes read below are the ones the minting thread
    // finished writing.
    if (const std::uint32_t bits = ctx->terminus_label.load(std::memory_order_acquire); bits != 0)
        return ctx->terminus_label_for.load(std::memory_order_relaxed) == peer.bits() &&
                       ctx->terminus_label_target.load(std::memory_order_relaxed) == want
                   ? std::span<const std::byte>(ctx->terminus_label_tlv)
                   : std::span<const std::byte>{};  // one label per child — see the ctx field

    // §6.2's trigger fires HERE and nowhere else on this half: the first operation this node
    // terminates for this child. No use counter, no hit threshold, no hotness estimate, no
    // timer, no aging — the condition is that the node is already holding the resolution and
    // has not yet spelled it.
    const std::optional<wire::path_label_t> label = labels_->mint(peer, target);
    if (!label) return {};  // §8.3 ceiling/capacity/all-retired ⇒ refuse-new, NOT an error
    std::vector<std::byte> rec;
    if (!wire::emit_path_label(rec, *label) || rec.size() != wire::kPathLabelRecordBytes) {
        (void)labels_->release(*label);  // spelling refused ⇒ do not strand the slot
        return {};
    }
    auto& ctx_mut = const_cast<child_rx_ctx_t&>(*ctx);
    std::copy(rec.begin(), rec.end(), ctx_mut.terminus_label_tlv.begin());
    ctx_mut.terminus_label_for.store(peer.bits(), std::memory_order_relaxed);
    ctx_mut.terminus_label_target.store(want, std::memory_order_relaxed);
    // Release-store LAST: the bytes, the owning peer and the residual it stands for are all
    // whole before anything can observe the label as present.
    ctx_mut.terminus_label.store(label->bits(), std::memory_order_release);
    return std::span<const std::byte>(ctx->terminus_label_tlv);
}

void fwd_router_t::release_child_label(child_rx_ctx_t& ctx) noexcept {
    // BOTH halves of §6.1's rewrite hang off this child and both depart with it — the
    // forwarding label that aliases the connection vertex behind it, and the terminus label
    // that aliases the vertex a residual resolved to. Releasing one and not the other would
    // leave a live slot owned by a peer identity that is about to be re-stamped.
    const std::uint32_t term_bits = ctx.terminus_label.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t bits = ctx.path_label.exchange(0, std::memory_order_acq_rel);
    if (labels_ == nullptr) return;
    // The generation bump IS the invalidation (§7.1) — there is no withdraw frame, no unbind,
    // no lease and no TTL (§7.3). The peer's next frame discovers it, answers NOT_FOUND, and
    // the peer falls back to the string path it still holds. A bump that saturates retires the
    // slot permanently (§4.3.1); that is the table's rule, not this call site's.
    if (term_bits != 0) (void)labels_->release(wire::path_label_from_bits(term_bits));
    if (bits != 0) (void)labels_->release(wire::path_label_from_bits(bits));
}

template <class Cursor>
[[gnu::noinline]] std::span<const std::byte> fwd_router_t::label_src_prefix(
    std::string_view inbound_name, const child_rx_ctx_t* inbound_ctx, const Cursor& cur,
    const fwd_pre_t* pre, std::string_view outbound_name) {
    // §11.2's mutual exclusion, at the call site the RFC assigns it to: a host SHOULD NOT mint
    // a path label into an address already spelled as a `PATH_REF`. `may_mint` is false from
    // every bound leg, so the two compressions of one address never meet on one frame. See the
    // header for why this arm refuses unconditionally while RFC-0024's shipped `bind` is left
    // exactly as it is.
    if (pre == nullptr || pre->op_body_len == 0) return {};
    // §6.1: minting rides the REPLY and only the reply. Not a preference — a request leg has
    // not yet passed the terminus's gates, so minting there would break §8.1's post-auth rule.
    // The op byte is read HERE rather than by the caller so a node with no table never reads it
    // at all; masked, because bits 7-6 are flags (§9.3).
    if (static_cast<fwd_op_t>(cur.byte_at(pre->op_body_off) & graph::kFwdOpcodeMask) !=
        fwd_op_t::REPLY)
        return {};
    // A frame delivered through the public `on_frame` carries no ctx (tests, SDK hosts, a link
    // wired outside `add_child`), so it resolves by name — `hop_mint`'s own fallback, for its
    // reason: the answer is a property of the LINK, not of how the frame reached the router.
    const child_rx_ctx_t* const ctx =
        inbound_ctx != nullptr ? inbound_ctx : ctx_by_name(inbound_name);
    if (ctx == nullptr || ctx->retired.load(std::memory_order_acquire)) return {};
    // A BUS arrival is deliberately left a string, and it is a scope decision rather than a
    // limitation of the design. A bus child's local part is its mount run PLUS the peer segment
    // resolved per frame (`extra_seg`), so one label per child would stand for a different
    // address for every peer behind the bus — the mis-delivery class this doc set closes by
    // construction. Labelling it correctly needs one slot per (child, peer), keyed on the
    // handle the seam already mints (#1294), and that is a table shape this car does not build.
    // §6.3 makes the refusal free: the part stays a string and nothing on the route notices.
    //
    // Read off the CTX rather than the caller's `from_peer` flag, so the answer is a property
    // of the child and not of one frame: a bus child relaying a frame with no resolvable peer
    // must still not acquire a label standing for one peer's address.
    if (ctx->bus.load(std::memory_order_relaxed) != nullptr) return {};
    // WHO the label is minted for: the peer this reply is being relayed OUT to, because that
    // origin is the one that will present the label back. Minting it for the link the reply
    // ARRIVED on would produce a label whose owner can never present it — a slot spent on
    // nobody, and a shortened frame the far side must refuse. The two ctxs are different
    // things and this is the distinction: the inbound ctx is what the label ALIASES, the
    // outbound one is who OWNS it.
    const child_rx_ctx_t* const egress =
        outbound_name.empty() ? nullptr : ctx_by_name(outbound_name);
    if (egress == nullptr) return {};
    return child_label_record(
        inbound_name, *ctx,
        peer_handle_from_bits(egress->label_peer.load(std::memory_order_relaxed)));
}

template <class Cursor>
void fwd_router_t::route_fwd_forward(std::string_view inbound_name,
                                     const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                     std::size_t strip_k, const Cursor& cur_src, transport_t& child,
                                     const fwd_pre_t* pre, std::span<const std::byte> reply_label) {
    // All offsets, no decoded tree: the shrunk-dst / grown-src head rebuild lives in
    // fwd_frame_view.hpp (rebuild_fwd_forward — unit-tested directly); this hop only
    // resolves the child and scatter-gathers the result. Reads AND the egress go
    // through the grammar `Cursor` seam (ADR-0053 ④b): the same code serves a
    // contiguous `span_cursor` (each region is one sub-span) and a link-walking
    // `rope_cursor` (a region yields one sub-span per link it crosses).
    // Grow `src` by the inbound link's FULL mount path, so the reply resolves through the
    // same strip-K descent a forward does (the ADR-0061 erratum). Two shapes:
    //
    //   - point-to-point — the child IS the inbound name, and its PRE-ENCODED mount run
    //     (#508) makes the grown prefix one iov entry with no per-segment encoding;
    //   - bus PEER — the child is `bus_child` (carried in from the per-child receiver ctx,
    //     #510) and the peer segment, known only now that the frame has arrived, rides as
    //     the one `extra_seg`. So `src` grows by `net/<module>/<name>/<peer>`.
    //
    // Before #510 a peer grew `src` by the BARE peer name: a return route that could not
    // distinguish two buses' same-named peers — the reply-direction twin of exactly the
    // collision per-module scoping exists to prevent.
    // The mount run comes off the LINK's own receiver ctx — no registry lookup at all. That
    // removes one of the two per-frame linear scans a hop used to pay (`entry_by_name`,
    // fetching this same run out of the table); `docs/performance.md` §2b measures it. The
    // ctx is created once per child in add_child and lives in a deque, so its address and
    // its bytes are stable for the link's lifetime — unlike a registry SLOT pointer, which
    // an append would invalidate (#521). A frame delivered through the public on_frame (no
    // ctx: tests, SDK hosts, a link wired outside add_child) still resolves by name.
    const std::span<const std::byte> mount =
        inbound_ctx != nullptr ? std::span<const std::byte>(inbound_ctx->mount_tlv)
                               : std::span<const std::byte>{};
    // RFC-0027 §6.1's reply-leg rewrite, and the ONE branch a string-only hop pays for it: a
    // null `labels_` is the conformant default (§6.3), it is a member already in this router's
    // first cache line, and the not-taken branch is what every deployment that ships today
    // takes. Everything the rewrite needs — the op-byte read, the table call, the encode — is
    // behind it and out of line, so the plain-string forwarding path reads no byte it did not
    // read before and `fwd_rebuild_t` gains no field (its 256-byte ratchet is MEASURED, #1235).
    //
    // The result rides as its OWN parameter rather than by substituting `mount`, so a REQUEST
    // leg is provably untouched: `reply_label` is consulted only on the `is_reply` arm, which
    // no request reaches, and `mount_tlv` keeps its exact meaning on the arm that does.
    const child_registry_t::child_t* const inbound =
        inbound_ctx != nullptr ? nullptr : registry_.entry_by_name(inbound_name);
    // This hop's mint contribution, on a forwarded REPLY that already carries a `PATH_REF`
    // (RFC-0024 §7.1 step 2): this node's own reference to the connection vertex for the link
    // the reply came back over. Handed to the rebuild as a CLOSURE, not a value, and the
    // laziness is the measurement: deciding here whether to mint means asking whether the
    // frame is a REPLY, and the rebuild is about to read that same op byte anyway. Asking
    // twice put a second `byte_at` on every forwarded frame — on a multi-link rope a cursor
    // walk, not a load — which is exactly the shape `bench_forward_rope` measures. The rebuild
    // calls this at most once, and only after it knows the frame carries a mint answer it can
    // extend; a node with nothing to give answers `nullopt` and the rebuild then STRIPS the
    // answer rather than relaying a list that skips a hop (§7.1 erratum 1).
    const auto mint_fn = [&]() -> std::optional<wire::path_ref_element_t> {
        return hop_mint(inbound_name, inbound_ctx);
    };
    // The REVERSE-direction twin (§7.1 amendment 1), equally lazy: called at most once, and
    // only after the rebuild has read the op byte it was going to read anyway and found the
    // mint flag set on a request. An unflagged frame never evaluates it.
    const auto reverse_fn = [&]() -> std::optional<wire::path_ref_element_t> {
        return reverse_hop_ref(inbound_name, inbound_ctx, from_peer);
    };
    const auto rebuilt =
        !mount.empty()
            ? rebuild_fwd_forward(cur_src, mount, from_peer ? inbound_name : std::string_view{},
                                  strip_k, pre, mint_fn, reverse_fn, reply_label)
        : inbound != nullptr && !inbound->mount_tlv.empty()
            ? rebuild_fwd_forward(cur_src, std::span<const std::byte>(inbound->mount_tlv),
                                  std::string_view{}, strip_k, pre, mint_fn, reverse_fn,
                                  reply_label)
            : rebuild_fwd_forward(cur_src, std::span<const std::byte>{}, inbound_name, strip_k, pre,
                                  mint_fn, reverse_fn, reply_label);
    if (!rebuilt) return;        // not a forwardable FWD ⇒ drop (callers pre-peeked)
    if (!rebuilt->ok()) return;  // malformed oversized op ⇒ drop, no overrun

    // Scatter-gather egress: the small stack heads interleaved with the untouched inbound
    // regions (remaining dst, selector, original src bytes, payload) — no payload copy. The
    // gather is written ONCE over the cursor seam (fwd_rebuild_t::gather); each region is
    // emitted via `for_each_span`, which yields exactly one sub-span for a contiguous source
    // and one per straddled link for a rope. So the container is the only thing that varies:
    // a stack `std::array` for the span path (ZERO heap, ADR-0038 inv. #2) vs a
    // `mem::block_array_t` over the failable byte seam (#596) for the rope path (a link
    // count is only known at run time).
    if constexpr (std::is_same_v<Cursor, wire::grammar::span_cursor>) {
        // Contiguous source: each region is a single sub-span — a stack array sized by
        // kFwdMaxIov, which is COUNTED from gather's emit sequence (see its docs), not a chosen
        // budget. The write is bounds-guarded regardless: this array was previously a bare 6
        // with an unchecked `iov[n++]`, so any growth in the region count was a silent overrun.
        //
        // Overflow DROPS the frame, matching the rope arm below rather than diverging from it.
        // This guard used to truncate — it filled what fitted and sent `n` spans — which put a
        // TRUNCATED frame on the wire, the precise outcome the rope arm's own comment calls
        // "worse than none" two branches down. Two arms of one hop disagreeing on a drop policy
        // is the shape of #673 (the arms disagreed on CRC verification) and of #516, so the
        // policy is now stated once and implemented identically on both sides.
        //
        // Unreachable today: gather emits at most kFwdMaxIov regions for a contiguous source by
        // construction. It is a guard against a future region being added without the count
        // moving, and in that event a counted drop is recoverable where a corrupt frame is not.
        std::array<std::span<const std::byte>, kFwdMaxIov> iov;
        std::size_t n = 0;
        bool ok = true;
        rebuilt->gather(cur_src, [&](std::span<const std::byte> s) {
            if (n < iov.size()) {
                iov[n++] = s;
            } else {
                ok = false;
            }
        });
        // ... and the same drop for a region that overshot the source window (#986): a
        // clamped feed emits FEWER bytes than the rebuilt head declares, which is the
        // truncated-frame-on-the-wire outcome this arm already refuses one line up.
        if (!ok || cur_src.poisoned()) return;
        child.send(std::span<const std::span<const std::byte>>(iov.data(), n));
    } else {
        // Rope source: a region may cross several links, so the sub-span count is only known
        // at run time — gather into a NOTHROW block array drawn from the failable seam (#596).
        // A `std::pmr::vector` here was a peer-reachable abort(): the element count is chosen
        // by the sender (link count x region count), the growth is `operator new`, and on
        // -fno-exceptions the throw is a reboot. This is the FORWARD path — it sits behind no
        // ACL, exactly like the RX decode (#588). The reply path immediately below already
        // refused by value via `try_to_iovec`; this closes the asymmetry.
        //
        // Exhaustion drops the frame. That is the correct answer for a forward hop: FWD is
        // not delivery-guaranteed, the sender retries, and emitting a partial iov would put a
        // TRUNCATED frame on the wire — worse than none.
        mem::block_array_t<std::span<const std::byte>> iov{rx_for(inbound_ctx)};
        bool ok = true;
        rebuilt->gather(cur_src, [&](std::span<const std::byte> s) {
            if (ok && !iov.push_back(s)) ok = false;
        });
        // Same #986 window-overshoot drop as the span arm: both arms state one policy.
        if (!ok || cur_src.poisoned()) return;
        child.send(std::span<const std::span<const std::byte>>(iov.data(), iov.size()));
    }
}

void fwd_router_t::resolve_terminus(std::string_view inbound_name, std::span<const std::byte> frame,
                                    const view_t* frame_view, const child_rx_ctx_t* inbound_ctx) {
    // Local request terminus (ADR-0041 §5): arena-decode straight from the
    // node's injected resource (ADR-0039 §1) — the library keeps no buffer of
    // its own; a bounded host injects a pool resource over its slab and the
    // terminus allocates nothing from the global heap. The arena is released
    // before this call returns. Apply the op and route the FWD{REPLY} back over
    // the link the request arrived on (its dst is the request's accumulated
    // src). The inbound link makes a `:subscribers[]` WRITE bind a REMOTE
    // subscriber whose deliveries route back over it (#136); the latch
    // (transient-local) fires inside resolve.
    const auto arena = wire::decode_into(frame, rx_for(inbound_ctx));
    if (!arena) return;  // malformed frame ⇒ drop
    auto reply = resolver_.resolve(*arena, inbound_name, frame_view);
    if (!reply) return;                    // structurally non-request ⇒ drop
    if (reply->link_count() == 0) return;  // assemble OOM ⇒ empty rope ⇒ drop (no garbage frame)
    if (transport_t* in = registry_.by_name(inbound_name)) {
        // Nothrow scatter-gather egress: the span table growth would abort() under
        // -fno-exceptions on a fragmented heap — drop the reply instead (the client retries).
        std::vector<std::span<const std::byte>> iov;
        if (!reply->try_to_iovec(iov)) return;
        in->send(std::span<const std::span<const std::byte>>(iov));
    }
}

void fwd_router_t::resolve_terminus_rope(std::string_view inbound_name, view::rope_t frame) {
    // ADR-0053 3c-iii: the multi-link request terminus, resolved straight off the
    // rope — the interim flatten is gone. Adopt the frame as a lazy view: over()
    // anchors the root header + total-size bounds, and verify() adds the root
    // trailer-CRC linear scan. Ingress checks END there (CONTEXT.md §Validation
    // timing) — no whole-tree walk: a malformed or CRC-failing interior TLV
    // surfaces its error where the resolver CONSUMES that level (per-TLV
    // verify-at-access, ADR-0053 §4).
    const auto view = wire::tlv_view_t::over(std::move(frame));
    if (!view) return;                        // malformed root ⇒ drop (as a decode error)
    if (!view->verify().has_value()) return;  // root CRC failure ⇒ drop
    // No frame_view is threaded, and the rope tier does not need one: unlike the arena's
    // pin_wire — which requires a contiguous frame view and is gated on it —
    // view_node::pin_wire IGNORES the argument and subropes the payload directly
    // (`op_resolve_view.cpp:113-135`), so the ADR-0042 §3 referenced store works here with
    // ZERO copy, retaining only the payload's links rather than the whole frame. (This
    // comment previously read as though the pin were unavailable on the rope path; it is
    // the reverse, and that misreading nearly justified deleting the tier.) Reply routes
    // back over the inbound link, its dst the request's accumulated src, as at the arena
    // terminus.
    auto reply = resolver_.resolve(*view, inbound_name, nullptr);
    if (!reply) return;                    // structurally non-request ⇒ drop
    if (reply->link_count() == 0) return;  // assemble OOM ⇒ empty rope ⇒ drop (no garbage frame)
    if (transport_t* in = registry_.by_name(inbound_name)) {
        // Nothrow scatter-gather egress (see resolve_terminus): drop rather than abort on
        // a span-table growth failure.
        std::vector<std::span<const std::byte>> iov;
        if (!reply->try_to_iovec(iov)) return;
        in->send(std::span<const std::span<const std::byte>>(iov));
    }
}

// --- route-handle (ws delivery-compaction, RFC-0004 §E.1) --------------------

template <class Cursor, class Contig>
void fwd_router_t::dispatch_control(std::string_view inbound_name, const Cursor& cur,
                                    Contig&& contig) {
    // `crc_check_t::VERIFY` is passed explicitly and is load-bearing on BOTH tiers: a control
    // frame MUTATES routing state, so it is applied only after the trailer proves the bytes
    // intact (CONTEXT.md §Frame integrity, ADR-0041 §1). `peek_control` defaults to DEFER
    // because every forward-hop caller wants that — a hop relays bytes it never interprets —
    // so the default is right and the explicit argument is what carries the policy. On the
    // span tier the owning `wire::decode` this replaced verified every node's CRC, so
    // deferring here would silently start ACCEPTING a COMPACT whose root trailer says its
    // payload is corrupt. The cost is zero allocations and, on our own traffic, zero cycles:
    // `emit_compact` emits no CR bit. A peer may legally set one, which is exactly why the
    // check must be explicit. Fragmenting a frame must not change whether it is applied.
    const auto head = peek_control(cur, wire::grammar::crc_check_t::VERIFY);
    if (!head) return;  // malformed / not a control frame / CRC failure ⇒ drop
    switch (head->type) {
        case type_t::HANDLE_NACK:
            on_nack(inbound_name, head->label);  // label only — no materialize at all
            return;
        case type_t::ADVERTISE: {
            if (head->child1_off == 0) return;
            // The route is the one child that genuinely needs a tree: on_advertise walks its
            // NAME segments and re-encodes a stripped copy. Make ONLY that child contiguous —
            // never the whole frame. A span source subspans it; a rope source materializes
            // the sub-rope through the injected byte backend (#730), since an ingress flatten
            // is peer-provoked and a bounded node's bound must cover it (ADR-0052 legitimate
            // flatten).
            const std::span<const std::byte> route = contig(head->child1_off, head->child1_total);
            // Flatten OOM ⇒ bind NOTHING. This is a REDUNDANT EARLY-OUT, not a guard: the
            // `wire::decode` on the next line is what actually answers an OOM'd flatten (an
            // empty span does not decode), and deleting this line changes no observable
            // behaviour — verified by ablation, twice. It is kept only so the REASON the
            // binding failed is the flatten and not the codec's leniency. Nothing may cite
            // it as a proven guard; the SEAM above is what the test pins.
            if (route.empty() && head->child1_total != 0) return;
            const auto dec = wire::decode(route);
            if (!dec) return;
            on_advertise(inbound_name, head->label, *dec);
            return;
        }
        case type_t::COMPACT: {
            if (head->child1_off == 0) return;
            // The payload is stored (deliver_local) or re-wrapped (emit_compact) as
            // contiguous bytes — a transport-egress / local-store boundary (ADR-0055 §2). The
            // caller's seam holds whatever ownership that costs on its tier for the duration
            // of this call.
            const std::span<const std::byte> payload = contig(head->child1_off, head->child1_total);
            // Flatten OOM ⇒ DROP the delivery (#730). Nothing downstream catches it: an
            // empty span is an engaged-empty `view::over_bytes` BY DESIGN, `graph_t::write`
            // stores it and reports success — so without this line a heap exhaustion here
            // REPLACES the subscriber's last-known value with nothing and calls it a
            // delivery. Missing one value under exhaustion is valid; corrupting the stored
            // one is not.
            if (payload.empty() && head->child1_total != 0) return;
            on_compact(inbound_name, head->label, payload);
            return;
        }
        default:
            return;  // drop anything else
    }
}

void fwd_router_t::on_control_rope(std::string_view inbound_name, view::rope_t frame) {
    // Only a MULTI-link control frame reaches here — a contiguous (single-link) one
    // decodes eagerly in on_frame_impl. A control frame is never a DEVICE payload, so a
    // non-all-host rope is not one; drop it (as the old flatten→failed-decode path did).
    if (!frame.all_host()) return;
    const wire::grammar::rope_cursor cur{frame};
    // The materialized child sub-rope, held across the handler call: the switch reads it as a
    // span, so its owner has to outlive the arm that asked for it.
    view_t hold;
    dispatch_control(inbound_name, cur,
                     [&](std::size_t off, std::size_t total) -> std::span<const std::byte> {
                         // A REFUSED materialize (an OOM — the frame is all-host-guarded
                         // above) yields an empty span, which every arm's own guard already
                         // reads as "drop". Named rather than inferred from `empty()` (#917).
                         std::expected<view_t, view::flatten_err_t> m =
                             frame.subrope(off, total).try_materialize(*flat_);
                         if (!m) return {};
                         hold = std::move(*m);
                         return hold.bytes();
                     });
}

void fwd_router_t::on_advertise(std::string_view inbound_name, std::uint16_t label,
                                const tlv_t& route) {
    // A route is an ADDRESS, and this is canonical / key context: it must be a packed `PATH`
    // (`opt.PL = 0`) whose body tiles exactly into LITERAL records — no ragged length, no
    // RFC-0018 §5.4 escape. Refusing here rather than at the bind is what keeps the label
    // UNBOUND for a malformed route: before RFC-0018 an undecodable body was caught by the
    // frame decode itself, because a `PATH` was a child run and garbage children failed the
    // grammar. A packed body is opaque to the grammar, so the address rule has to be checked
    // by the one tier that owns it — here and in `resolve_route_vertex`'s `path_key`.
    if (route.type != type_t::PATH || route.opt.pl || !wire::packed_path_valid_key(route.payload))
        return;

    // The mount-shape stamp (#765), read BEFORE the descent, never after. Read after, a
    // registration that landed between the descent and the store would be stamped as though
    // the binding had already accounted for it — the binding would claim to know a shape it
    // resolved before. Read first, that race stamps a shape the binding may pre-date, which
    // costs one self-healing re-advertise and never a misroute.
    const std::uint32_t shape = registry_.mount_generation();

    // Resolve the leading route segments through the SAME strip-K mount descent the FWD
    // forward step uses (@ref resolve_mount_by), so a label tracks exactly the route a FWD
    // would take. It resolved a single BARE segment until #516, which meant every route
    // addressed to an RFC-0014 `/net/<module>/<name>` mount missed, fell through to the
    // terminus arm, and was ABSORBED at the first intermediate node — the compacted flow
    // then delivered locally at a node that was only supposed to relay it.
    //
    // Fed to the descent as a lazy accessor over the decoded children — no array, so an
    // ADVERTISE naming a mount of any width binds exactly the route a FWD to it would take
    // (#523). It used to collect into a `kMountPeekMax`-sized array, which silently truncated
    // a deeper route to the first four segments before resolving it.
    // The route's PACKED records, walked by the shared canonical-key cursor (RFC-0018): an
    // ADVERTISE route is a `PATH` body, which is exactly what `key_view_t` navigates, so the
    // segment indices come from the SAME framing decode the vertex map keys on. That also
    // settles the escape: `key_view_t` is canonical/key context and reports a `len == 0`
    // record as ragged, so a route carrying a label binds NOTHING rather than binding a
    // truncation — the §5.4 rejection, arriving for free through the one locus that owns it.
    wire::key_view_t::record_cursor_t adv_walk{wire::key_view_t{route.payload}};
    const auto adv_at = [&adv_walk](std::size_t i) -> std::optional<std::string_view> {
        const std::optional<wire::key_view_t::record_t> rec = adv_walk.at(i);
        if (!rec) return std::nullopt;
        return detail::as_string_view(rec->payload);
    };
    // The decoded route outlives this call, so a segment is already in storage that outlives
    // the hop: retaining is the identity here.
    const auto adv_retain = [&adv_at](std::size_t i) -> std::string_view {
        const std::optional<std::string_view> s = adv_at(i);
        return s ? *s : std::string_view{};
    };
    const mount_hit_t hit = resolve_mount_by(registry_, adv_at, adv_retain);

    // A route through a bus link's own NAME is not routable (ADR-0073 §3 / RFC-0020): bind
    // NOTHING — neither a downstream swap (the old fall-through re-advertised over the bus,
    // i.e. broadcast) nor a terminus binding (which would absorb every COMPACT locally).
    // The peer's COMPACTs then draw the same HANDLE_NACK a stale label draws, and the flow
    // stays on the full-route FWD form — where the forward path answers the rejection.
    if (hit.rejected) return;

    if (hit.link != nullptr) {
        // Forwarding hop: strip the K segments this node consumed, allocate OUR own
        // out-label, record the swap, retain the stripped egress route (for NACK
        // re-advertise), and re-advertise downstream with the new label (MPLS-style swap).
        const std::string down_name(hit.link_name);
        // Strip the K consumed records off the PACKED body — a byte slice, where it used to
        // be a child-vector erase (RFC-0018). `end_of` is the same cursor the descent walked,
        // so the split cannot disagree with the resolution that produced `strip_k`.
        tlv_t stripped = route;
        stripped.payload =
            route.payload.subspan(std::min(adv_walk.end_of(hit.strip_k), route.payload.size()));
        const std::vector<std::byte> stripped_bytes = wire::encode(stripped);

        // Sample the downstream link's clear epoch BEFORE minting anything against it (#827).
        // This runs on the INBOUND link's rx thread, so a reconnect of `down_name` on its own
        // thread can land anywhere in the three steps below; the sample is what lets the bind
        // tell "the tables I minted into" from "the tables that are there now". It must precede
        // `ensure_egress`, which creates those tables: sampling after would name a post-clear
        // allocator while the label came from the pre-clear one.
        const std::uint32_t down_epoch = handles_.link_epoch(down_name);
        // ONE label per (down-link, stripped route), never one per re-advertise (#913). This arm
        // runs on EVERY upstream re-advertise — a reconnect loop, a flapping link — so an
        // unconditional mint burned a label out of the saturating 16-bit space AND left another
        // egress entry behind each cycle, neither reclaimable short of a whole-link `clear_link`.
        // `ensure_egress` is the primitive `deliver_remote` already uses: under the egress
        // table's own lock it reuses the label bound to an identical route, mints only for a
        // genuinely new one, and records in that same critical section — which also retires the
        // old alloc-then-record pair's split outcome, a label minted then burned for nothing on
        // a refused record. Egress still precedes ingress and both precede the wire (#603), and
        // a refused bind below now leaves an entry the NEXT cycle REUSES rather than duplicates.
        const std::uint16_t out_label = handles_.ensure_egress(down_name, stripped_bytes).first;
        // 0 ⇒ exhausted label space or a full egress table: bind and advertise nothing, and let
        // the upstream's COMPACTs draw the HANDLE_NACK a stale label already draws (reusing a
        // LIVE label would swap this flow onto another's route). An ESTABLISHED flow is never
        // refused — the reuse scan runs ahead of the bound, so only NEW flows degrade.
        if (out_label == 0) return;
        // Field by field, not a designated-initializer brace: -Werror=missing-field-initializers.
        handle_binding_t fwd;
        fwd.terminus = false;
        fwd.down_link = down_name;
        fwd.out_label = out_label;
        fwd.mount_gen = shape;
        // Epoch-checked (#827): a downstream reconnect anywhere between the sample above and
        // this bind refuses the swap, so no ingress binding is left aiming at an out-label
        // whose egress route died with the erased table. The refusal takes the same path a
        // full table takes — the upstream's next COMPACT misses and draws the ordinary
        // stale-label HANDLE_NACK, which prompts it to re-advertise onto the new tables.
        if (!handles_.bind_ingress_forward(inbound_name, label, std::move(fwd), down_epoch)) {
            // Hand the take back (#833). A refusal returns without advertising, so the label
            // this hop just took aliases a route no ingress binding aims at and no peer has
            // ever seen — it sat in the LIVE downstream table until that link's next
            // clear_link, one label plus its route bytes per refused route, and it also spent
            // one of the downstream table's bounded slots. `release_egress` erases only what
            // THIS call minted: a label some other advertise has since taken (#913's sharing)
            // is left exactly where it is, so an established flow cannot be unwound by a new
            // one's refusal. Nothing goes on the wire either way.
            handles_.release_egress(down_name, out_label, stripped_bytes);
            return;
        }
        // Gathered, not built (#885): this arm runs on the INBOUND link's receive thread and is
        // reached only because a peer sent an ADVERTISE, so the frame build it used to do here
        // was a peer-provoked throwing allocation. The stripped route is already contiguous —
        // it has to be, `ensure_egress` above stored a copy of exactly these bytes.
        emit_advertise(*hit.link, out_label, stripped_bytes);
        return;
    }

    // Terminus: the route resolves locally here — bind the label to the local route. A
    // refusal (full ingress table) leaves the label unbound, so the peer's COMPACT draws the
    // same HANDLE_NACK a stale label draws and the flow stays on the full-route form.
    handle_binding_t term;
    term.terminus = true;
    term.local_route = wire::encode(route);
    // Stamped even here: "no mount matched, so this is local" is itself a claim about the
    // mount shape, and a later registration can falsify it — that is the deeper-mount half of
    // #765, where a COMPACT keeps being absorbed locally after a FWD to the same address
    // started forwarding.
    term.mount_gen = shape;
    (void)handles_.bind_ingress(inbound_name, label, std::move(term));
}

namespace {

/**
 * @brief Inline capacity for a warm delivery's observed local route, in bytes.
 *
 * Not the protocol maximum: a `dst` may carry `graph::kMaxSegments` segments of
 * `graph::kMaxSegmentBytes` each (RFC-0023), which is ~17 KB and cannot sit in a receive
 * thread's frame on a bounded node. This is the same two-segment working size the mount
 * descent's scratch already uses, sized for the routes a delivery flow actually terminates
 * at; anything wider takes the owning fallback below.
 */
inline constexpr std::size_t kCompactRouteInline = graph::kMaxSegmentBytes * 2;

/**
 * @brief Hand an installed COMPACT-delivery observer the bound local route + payload.
 *
 * The warm arm reaches this having already resolved (@ref route_handle_t::resolved) — it
 * needs no route to WRITE, only to REPORT. Serving that report through
 * @ref route_handle_t::lookup_ingress re-paid the owning copy (a `std::string` plus a
 * `std::vector`, both allocating) on every observed frame, which is precisely the per-frame
 * cost `resolved` was introduced to remove (ADR-0062). The route is copied into this frame
 * instead, so the steady-state observed delivery allocates NOTHING — and cannot be turned
 * into an allocation-failure drop by an observer that merely watches.
 *
 * Outlined (`noinline`) deliberately: the buffer must cost stack only when an observer is
 * actually installed, not on every COMPACT.
 *
 * @param handles      The label store to read the binding out of.
 * @param fn           The installed observer; never null (the caller tests the slot).
 * @param ctx          The observer's opaque context.
 * @param inbound_name This node's NAME for the link the COMPACT arrived on.
 * @param label        The inbound label whose terminus binding was just delivered to.
 * @param payload      The delivered payload TLV bytes, borrowed for the call.
 */
[[gnu::noinline]] void observe_compact_delivery(const route_handle_t& handles,
                                                fwd_router_t::compact_delivery_fn_t fn, void* ctx,
                                                std::string_view inbound_name, std::uint16_t label,
                                                std::span<const std::byte> payload) {
    std::array<std::byte, kCompactRouteInline> route{};
    const std::size_t n = handles.copy_local_route(inbound_name, label, route);
    // 0 ⇒ the binding went away between the write and this observation. The delivery still
    // happened; there is simply no route left to name it by, so the observer is not called —
    // the same silence an uninstalled sink gives, never a drop and never an error.
    if (n == 0) return;
    if (n <= route.size()) {
        fn(ctx, std::span<const std::byte>(route.data(), n), payload);
        return;
    }
    // Wider than the inline buffer. Rare enough to be worth an allocation rather than a
    // frame sized for the protocol maximum, and the observation stays complete either way.
    if (const std::optional<handle_binding_t> b = handles.lookup_ingress(inbound_name, label))
        fn(ctx, b->local_route, payload);
}

}  // namespace

void fwd_router_t::on_compact(std::string_view inbound_name, std::uint16_t label,
                              std::span<const std::byte> payload_bytes) {
    // `payload_bytes` is the already-contiguous wire encoding of the COMPACT payload TLV
    // (the span path re-encodes the decoded child; the rope path materializes only the
    // payload sub-rope — ADR-0055 §2). It is never re-decoded here — just stored/forwarded.
    // ADR-0062: the STEADY-STATE lookup first — ~24 trivially copyable bytes, no allocation.
    // `lookup_ingress` copies a std::string + a std::vector out of the table, and it did so on
    // EVERY frame, before anything checked whether this flow was already resolved. The owning
    // form is now taken only where the route bytes are genuinely needed: the cold re-resolve.
    const resolved_binding_t rb = handles_.resolved(inbound_name, label);
    // #765: the binding's SPLIT is only as valid as the mount shape it was decided against.
    // One acquire load and one compare on the warm path — the leg this whole mechanism exists
    // to keep cheap — and a mismatch is not an error, it is the RFC-0004 §E.1 self-heal that
    // an unknown label already takes. Folded into the SAME branch, so a stale shape and a
    // stale label cost one test between them rather than two.
    if (!rb.found || rb.mount_gen != registry_.mount_generation()) {
        // Stale/unknown label: drop, observe, and NACK back to prompt a re-advertise
        // (self-heal). Never a crash — the route is simply re-learned (RFC-0004 §E.1).
        if (const auto sink = stale_.get(); sink.fn != nullptr)
            sink.fn(sink.ctx, inbound_name, label);
        // Ten bytes off the stack (#885). This is the arm a hostile peer reaches for free —
        // one unbound label per frame, no state to consult — so it is the one that must not
        // be able to exhaust anything. It now allocates NOTHING at all, on any tier.
        if (transport_t* const up = registry_.by_name(inbound_name)) emit_handle_nack(*up, label);
        return;
    }

    if (rb.terminus) {
        // WARM: dereference the cached vertex and write. No decode, no path walk, no
        // graph_.find — the whole point of RFC-0004 §E.1's label, finally applied to the
        // RESOLUTION and not merely to the wire. The generation guard is what makes the
        // cached handle safe: retire() bumps it (#511), so a retired-and-revived vertex is
        // detected here rather than silently written through (RFC-0009 §B.6 re-virginize).
        //
        // `inbound_name` is the ACL caller context (#974), exactly as `inbound_link` is for
        // the full-route FWD{WRITE} (op_resolve_walk.hpp's WRITE arm). A COMPACT is a
        // delivery-is-a-write (RFC-0004 §E.1 / §D) and RFC-0004 §F gates the target vertex's
        // `:acl` at the final hop, so the two forms of ONE write must present one subject.
        // Writing with the default empty caller spelled the local-trusted short-circuit in
        // `graph_t::acl_allows`, so a flow auto-promoted to COMPACT skipped every ACE the
        // full-route form is checked against. ADR-0062 already ruled the shape — "a binding
        // caches the address, never the authorization" — so the cached handle is reused and
        // the gate is re-evaluated per frame: a later `:acl` binds the very next COMPACT.
        if (rb.warm && rb.target && graph_.retire_generation(*rb.target) == rb.target_gen) {
            // Both bails below are allocation failures, counted so a node shedding COMPACT
            // frames under memory pressure says so (#1068). A DENIAL is NOT counted here:
            // `graph_.write` counts it at the gate that produces it, so this arm must not
            // add a second count for the same refusal. Nothing is counted on success — the
            // steady-state warm path is exactly the instructions it was before.
            const auto payload_view = view::over_bytes(payload_bytes);
            if (!payload_view) {  // alloc failure ⇒ drop (one audited locus)
                graph_.count_external_drop(graph::graph_t::external_drop_t::OUT_OF_MEMORY, 1);
                return;
            }
            view::rope_t value;
            // #981: no residual HERE. One link on a fresh rope is the inline no-op arm of
            // `rope_t::try_reserve` — it reaches no allocator, so there is no probe window
            // and nothing to migrate; the check stays because the return is [[nodiscard]]
            // and a future kInline of 0 must not silently drop the guard.
            if (!value.try_reserve(1)) {
                graph_.count_external_drop(graph::graph_t::external_drop_t::OUT_OF_MEMORY, 1);
                return;
            }
            value.append(*payload_view);
            if (graph_.write(*rb.target, std::move(value), inbound_name).has_value()) {
                if (const auto sink = delivery_.get(); sink.fn != nullptr)
                    observe_compact_delivery(handles_, sink.fn, sink.ctx, inbound_name, label,
                                             payload_bytes);
            }
            return;
        }
        // COLD or STALE: take the owning form, resolve the route, then memoize it.
        const std::optional<handle_binding_t> binding =
            handles_.lookup_ingress(inbound_name, label);
        // The label resolved a moment ago (rb.found) and its binding is gone now: a
        // concurrent unbind between the two reads. The delivery was admitted and has no
        // target left, which is what NO_TARGET means (#1068) — it is not the stale-label
        // arm above, which self-heals by NACK; there is nothing here to re-advertise to.
        if (!binding) {
            graph_.count_external_drop(graph::graph_t::external_drop_t::NO_TARGET, 1);
            return;
        }
        // Same caller context as the warm arm above (#974): the cold and warm halves of one
        // flow must not disagree about who is writing.
        if (deliver_local(binding->local_route, payload_bytes, inbound_name)) {
            if (const auto v = resolve_route_vertex(binding->local_route)) {
                resolved_binding_t fill = rb;
                fill.warm = true;
                fill.target = *v;
                fill.target_gen = graph_.retire_generation(*v);
                handles_.cache_resolution(inbound_name, label, fill);
            }
            if (const auto sink = delivery_.get(); sink.fn != nullptr)
                sink.fn(sink.ctx, binding->local_route, payload_bytes);
        }
        return;
    }

    // Forwarding hop: swap to our out-label and re-emit the COMPACT downstream — the
    // route still does NOT ride, only the (swapped) label.
    //
    // WARM: the cached registry SLOT is read directly. It is safe because teardown nulls the
    // slot's `link` IN PLACE and ADR-0063 made slot addresses permanently stable — so a
    // departed link reads nullptr, which is the same clean miss an unresolved lookup gives.
    // The tombstone IS the invalidation; no generation and no teardown sweep are needed.
    if (rb.warm && rb.down_slot != nullptr) {
        const auto* const slot = static_cast<const child_registry_t::child_t*>(rb.down_slot);
        if (transport_t* const down = slot->link()) {
            emit_compact(*down, rb.out_label, payload_bytes);
            return;
        }
        // Tombstoned: fall through and re-resolve, so a re-created link re-warms.
    }
    const std::optional<handle_binding_t> binding = handles_.lookup_ingress(inbound_name, label);
    if (!binding) return;
    if (const child_registry_t::child_t* const slot = registry_.entry_by_name(binding->down_link)) {
        if (transport_t* const down = slot->link()) {
            emit_compact(*down, binding->out_label, payload_bytes);
            resolved_binding_t fill = rb;
            fill.warm = true;
            fill.down_slot = slot;
            handles_.cache_resolution(inbound_name, label, fill);
        }
    }
}

void fwd_router_t::on_nack(std::string_view inbound_name, std::uint16_t label) {
    // A downstream peer lost the binding for `label` on this link — re-advertise the
    // route we hold for it so the flow self-heals without a setup handshake.
    // `egress_route` is the ONE allocation left on this peer-provoked arm: an owning copy of
    // the stored route out from under the egress table's lock. It is already NOTHROW (#603) —
    // exhaustion returns the same `nullopt` an unbound label returns, and the peer simply gets
    // no re-advertise. The frame build that used to follow it is gone (#885), so the arm's
    // whole allocation budget is now that one guarded copy; `transport_alloc_softfail_test`
    // derives the budget from it rather than hard-coding a number.
    const std::optional<std::vector<std::byte>> route = handles_.egress_route(inbound_name, label);
    if (!route) return;
    if (transport_t* const link = registry_.by_name(inbound_name))
        emit_advertise(*link, label, *route);
}

std::optional<graph::vertex_handle_t> fwd_router_t::resolve_route_vertex(
    std::span<const std::byte> route_path) const {
    // The SAME resolution deliver_local performs, factored out so the memoized handle can
    // never diverge from the one the cold path would have used. Two rules would be two
    // sources of truth for "which vertex does this label mean".
    const auto route = wire::decode(route_path);
    if (!route || route->type != type_t::PATH) return std::nullopt;
    // A route whose body is not a run of literal packed records is not an address (#681, and
    // RFC-0018's escape-in-key-context rule): binding a label to it would resolve a vertex the
    // sender never named. No vertex, no binding, no delivery.
    const auto key = wire::path_key(*route);
    if (!key) return std::nullopt;
    return graph_.find(*key);
}

bool fwd_router_t::deliver_local(std::span<const std::byte> route_path,
                                 std::span<const std::byte> payload, std::string_view caller) {
    // Through the SAME helper the memoized handle is resolved by — so the cold path and the
    // cached path cannot disagree about which vertex a label names. Two decode+find copies
    // would be two sources of truth, which is the shape #516 turned out to be.
    // The bool contract is deliberately unchanged (#1068): the CAUSE does not travel back to
    // the caller, it travels into the counters at the site that knows it. Widening the return
    // would hand every caller a reason it has nothing to do with — on_compact's only question
    // is "did it land", and the operator's question is answered by delivery_drops().
    const std::optional<graph::vertex_handle_t> v = resolve_route_vertex(route_path);
    if (!v) {
        // Not an address, or an address naming no live vertex — indistinguishable here and
        // the same outcome either way: an admitted delivery with nowhere to land.
        graph_.count_external_drop(graph::graph_t::external_drop_t::NO_TARGET, 1);
        return false;
    }
    // `payload` is a wire-encoded TLV (never empty); `nullopt` is exactly an alloc
    // failure → drop the delivery (one audited alloc/copy/over locus).
    const auto payload_view = view::over_bytes(payload);
    if (!payload_view) {
        graph_.count_external_drop(graph::graph_t::external_drop_t::OUT_OF_MEMORY, 1);
        return false;
    }
    // A denial inside `write` is counted at the graph's WRITE gate, never here — the false
    // this returns for a refusal is the caller's answer, not a second drop.
    // @p caller is the ACL subject context (#974) — the inbound link's NAME on the COMPACT
    // path, matching what the full-route FWD{WRITE} presents. It is a required parameter
    // precisely so a future delivery path cannot land here unattributed by omission.
    return graph_.write(*v, *payload_view, caller).has_value();
}

graph::wire_target_split_t fwd_router_t::split_subscriber_target(
    std::span<const std::byte> key) const {
    // Walk the target key's packed records into the segment spans the shared descent takes —
    // the SAME resolve_mount_by the forward path walks a frame's dst into, so a bound
    // route and a routed frame cannot disagree about where a mount ends (ADR-0061).
    //
    // The key's packed records are walked LAZILY, exactly as the forward path walks a frame's
    // `dst` — no array, so a target routing through a mount of any width binds (#523). The
    // two fixed `kMountPeekMax`-sized arrays this replaces truncated a deeper target to its
    // first four segments and then resolved the truncation.
    //
    // The walk is `key_view_t`'s INCREMENTAL cursor (#888): the framing lives in one place,
    // and an ascending ask resumes where the last one stopped instead of rescanning from
    // offset 0 per segment, as the two hand-rolled lambdas here used to. n is a mount width
    // and this is bind-time, so that is cleanliness, not a measured cost.
    wire::key_view_t::record_cursor_t walk{wire::key_view_t{key}};
    const auto key_at = [&walk](std::size_t i) -> std::optional<std::string_view> {
        const std::optional<wire::key_view_t::record_t> rec = walk.at(i);
        if (!rec) return std::nullopt;
        return detail::as_string_view(rec->payload);
    };
    // The key's bytes outlive this call, so retaining is the identity.
    const auto key_retain = [&key_at](std::size_t i) -> std::string_view {
        const std::optional<std::string_view> s = key_at(i);
        return s ? *s : std::string_view{};
    };

    const mount_hit_t hit = resolve_mount_by(registry_, key_at, key_retain);
    // NO mount at all is the one outcome that is not an error: the target names something
    // this node terminates, which each caller answers for itself (RFC-0021 §4.B.2 / §7 q3 at
    // the wire door, `INVALID_PATH` at `subscribe_toward`, which exists only to bind a
    // mount-path target).
    if (hit.link == nullptr && !hit.rejected) return {};
    // A bus link's own NAME as the next hop (`rejected`, ADR-0073 §3 / RFC-0020), a bus PEER
    // first hop (no directed registry entry to store — see `subscribe_toward`'s header doc,
    // #741), and a target naming the mount EXACTLY (nothing below it to deliver to) are all
    // the same answer: a mount was named, and no directed delivery can be bound through it.
    if (hit.link == nullptr || !hit.peer.empty() || walk.end_of(hit.strip_k) >= key.size())
        return graph::wire_target_split_t{.unroutable = true};
    // The residual packed records are reused verbatim: the key suffix IS the route payload.
    return graph::wire_target_split_t{.link = hit.link_name,
                                      .residual = key.subspan(walk.end_of(hit.strip_k))};
}

graph::result_t<void> fwd_router_t::subscribe_toward(const graph::path_t& producer,
                                                     const graph::path_t& target) {
    const std::optional<graph::vertex_handle_t> v = graph_.find(producer.key());
    if (!v) return std::unexpected(graph::status_t::NOT_FOUND);

    // The SAME descent the wire door borrows through the RFC-0021 seam, so the host-local
    // dual and its wire twin cannot disagree about what a mount-path target means.
    const graph::wire_target_split_t split = split_subscriber_target(target.key());
    // No mount, an exact-mount target, or a bus PEER first hop all fail the same way the
    // string parser fails an unroutable spelling.
    if (split.link.empty()) return std::unexpected(graph::status_t::INVALID_PATH);

    // The return route is the residual below the mount, as ONE owned PATH TLV — the
    // single copy every delivery then clones by refcount (ADR-0041 §2).
    const std::span<const std::byte> residual = split.residual;
    std::vector<std::byte> route_tlv;
    wire::emit_tlv(route_tlv, wire::type_t::PATH, wire::opt_t{}, residual);
    const auto route_view = view::over_bytes(route_tlv);
    if (!route_view) return std::unexpected(graph::status_t::BACKPRESSURE);

    // A minimal SUBSCRIBER composite — the same admission door as the wire append
    // (subscribe_wire parses it once; no compact opt-in, deliveries ride the
    // full-route FWD{WRITE} form).
    std::vector<std::byte> sub_tlv;
    wire::emit_tlv(sub_tlv, wire::type_t::SUBSCRIBER, wire::opt_t{.pl = true}, {});
    const auto sub_view = view::over_bytes(sub_tlv);
    if (!sub_view) return std::unexpected(graph::status_t::BACKPRESSURE);

    return graph_.subscribe_wire(*v, *sub_view, *route_view, std::string(split.link));
}

void fwd_router_t::deliver_remote(const graph::remote_delivery_t& sub, const view::rope_t& value) {
    transport_t* const link = registry_.by_name(sub.link);
    if (link == nullptr) return;  // link torn down between subscribe and this write
    const std::span<const std::byte> route = sub.return_route.bytes();  // the stored PATH TLV

    if (sub.delivery_compact) {
        // Auto-promote (Q5 / RFC-0004 §E.1): advertise the label once per flow, then stream
        // lean COMPACT. ensure_egress is idempotent per (link,route); clear_link on a
        // reconnect drops the binding so the next delivery re-advertises (self-heal).
        // A COMPACT wraps a CONTIGUOUS payload, so a multi-link value pays one flatten here
        // — single-link, the common case, is a zero-copy adopt. The scatter-gather win is
        // the default full-route path below (the hot fan-out leg).
        // The flatten on this writer-thread leg is NOTHROW (#477): a failed flatten DROPS
        // the delivery (the subscriber misses one value under heap exhaustion — valid
        // delivery behavior), never an abort. The two frame BUILDS that used to follow it
        // are gone (#885) — this leg now emits through the same gather locus the forwarding
        // hop and the producer doors use, so it allocates for the flatten and nothing
        // else. A dropped fresh ADVERTISE self-heals via the peer's HANDLE_NACK (§E.1). NOT yet
        // nothrow: ensure_egress below still records the egress binding through a throwing
        // std::pmr allocation (#603 defect 1) — an ADVERTISE-driven abort under
        // -fno-exceptions until the label tables migrate to the failable seam.
        // Resolve the label BEFORE flattening: an exhausted label space (#603) falls
        // through to the full-route form below, which gathers the rope's links and needs
        // no flatten at all — so the wasted materialize is skipped rather than discarded.
        const auto [label, fresh] = handles_.ensure_egress(sub.link, route);
        if (label != 0) {
            // Through the injected byte backend (#730): this egress flatten is the writer
            // thread's, but it is the same store the ingress ones draw from, so one
            // injection bounds the router's own four flattens — and, since #766 hands the
            // same pointer to `resolver_`, the terminus resolver's rope-tier ones
            // (`op_resolve_view.cpp`) too: all rope flattens on the forward AND terminus
            // paths draw from the injected seam. `flat_` is therefore reached from BOTH
            // this writer thread and the receive threads, which is why an injected backend
            // must be thread-safe (ADR-0060 §2).
            // A REFUSED materialize drops the delivery (#917): an OOM, or a DEVICE-link
            // value this COMPACT cannot carry either way. The old `empty && total != 0`
            // inference is gone — the error channel names the failure, and a legitimately
            // empty value now emits the empty COMPACT it always should have.
            const std::expected<view_t, view::flatten_err_t> flat = value.try_materialize(*flat_);
            if (!flat) return;
            if (fresh) emit_advertise(*link, label, route);
            emit_compact(*link, label, flat->bytes());
            return;
        }
        // label == 0: this link has issued all 65535 labels. Compaction is an optimization
        // over a delivery form that carries its own route, so the flow degrades to that
        // form instead of dropping — fall through.
    }
    // The reverse-list delivery (RFC-0024 §7.1 amendment 1, #1223 step 4): consume the
    // stored list's element 0 — this node's OWN reference to the connection vertex the
    // subscribe arrived on — by validating it against this node's vertex map (§5.1 bounds +
    // generation, then §6.2's ACL at the dereferenced vertex under the edge's stored
    // subject) and egressing through the vertex it names. Elements 1.. go on the wire as
    // the delivery's bound `dst`. ANY refusal — the link re-dialled (generation moved), the
    // child gone, the ACL revoked — falls through to the canonical route below, which is
    // stored alongside precisely so this binding is an optimisation plus a liveness check
    // and never the only way home.
    const std::span<const std::byte> rev = sub.reverse_route.bytes();
    if (rev.size() >= 4u + 2u * wire::kPathRefElementBytes) {
        const wire::grammar::span_cursor rcur{rev};
        const wire::path_ref_element_t e0 = read_path_ref_element(rcur, 4);
        if (transport_t* const out = bound_egress(e0, sub.caller, graph::acl_right_t::WRITE)) {
            const std::span<const std::byte> dst_body =
                rev.subspan(4u + wire::kPathRefElementBytes);
            constexpr std::array<std::byte, 5> op_tlv{
                std::byte{0x01}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
                std::byte{std::to_underlying(fwd_op_t::WRITE)}};
            constexpr std::array<std::byte, 4> empty_src{std::byte{0x06}, std::byte{0x00},
                                                         std::byte{0x00}, std::byte{0x00}};
            const std::size_t body_len =
                op_tlv.size() + 4u + dst_body.size() + empty_src.size() + value.total_length();
            stack_writer<20> head;  // FWD header (<=6) + 5-byte op + 4-byte PATH_REF header
            head.header(type_t::FWD, body_len);
            head.raw(op_tlv);
            head.header_bare(type_t::PATH_REF, dst_body.size());
            if (head.ok()) {
                // The iov table on the ADR-0065 failable seam (#981) — see the default arm
                // below for the argument; this arm is the same table, same element type.
                mem::block_array_t<std::span<const std::byte>> iov(graph_.control_source());
                if (!iov.reserve(3 + value.link_count())) return;  // OOM — drop
                const bool built = iov.push_back(head.span()) && iov.push_back(dst_body) &&
                                   iov.push_back(std::span<const std::byte>(empty_src));
                if (!built) return;
                for (const view_t& l : value.links())
                    if (!iov.push_back(l.bytes())) return;
                out->send(std::span<const std::span<const std::byte>>(iov.data(), iov.size()));
                return;
            }
        }
    }
    // Default: full-route `FWD{ op=WRITE, dst=<return route>, src=<empty PATH>,
    // payload=<VALUE> }` (delivery-is-a-write, RFC-0004 §D / #136), scatter-gathered over
    // the stored value's rope links (ADR-0053 ⑤): a fresh stack head + the ROPED stored
    // route + each value segment, NO flatten. The route bytes were copied ONCE at subscribe
    // (ADR-0041 §2); a delivery copies nothing — a multi-link value crosses as its own
    // segments. src starts empty — each forwarding hop grows it (the way back). The
    // refcounted route view (`sub.return_route`) stays alive for this call even if the slot
    // is concurrently unsubscribed.
    constexpr std::array<std::byte, 5> op_tlv{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
                                              std::byte{0x00},
                                              std::byte{std::to_underlying(fwd_op_t::WRITE)}};
    constexpr std::array<std::byte, 4> empty_src{std::byte{0x06}, std::byte{0x00}, std::byte{0x00},
                                                 std::byte{0x00}};
    const std::size_t body_len =
        op_tlv.size() + route.size() + empty_src.size() + value.total_length();
    stack_writer<16> head;  // FWD header (≤6) + the 5-byte op TLV
    head.header(type_t::FWD, body_len);
    head.raw(op_tlv);
    if (!head.ok()) return;

    // iov = head + route + empty_src + one span per value link (sized to the rope, no
    // synthetic cap — the same per-send iov table the rope terminus reply builds).
    //
    // MIGRATED to the ADR-0065 failable seam (#981), which is the destination `graph_t`'s
    // `ctl` parameter names for "`fwd_router` iov". `std::vector` + `detail::try_reserve`
    // was nothrow only where the growth THROWS: under `-fno-exceptions` that helper probes
    // the global heap, frees the probe block, and then runs the throwing `reserve` on the
    // inference that the block is still there — a writer-thread context switch in that
    // window makes the `reserve` abort() the node (#850). A `block_array_t` growth is ONE
    // refusable `try_alloc`, so there is no window to lose, and the table is drawn from the
    // node's injected source rather than the global heap. `std::span` is trivially copyable,
    // so the memcpy relocation is exact. Exhaustion still just drops this delivery.
    mem::block_array_t<std::span<const std::byte>> iov(graph_.control_source());
    if (!iov.reserve(3 + value.link_count())) return;  // OOM — drop
    // Reserved to the exact final count above, so none of these can grow again; they are
    // still checked because `push_back` is `[[nodiscard]]` and a silent short table would
    // put a truncated frame on the wire.
    const bool built = iov.push_back(head.span()) && iov.push_back(route) &&
                       iov.push_back(std::span<const std::byte>(empty_src));
    if (!built) return;
    for (const view_t& l : value.links())
        if (!iov.push_back(l.bytes())) return;
    link->send(std::span<const std::span<const std::byte>>(iov.data(), iov.size()));
}

}  // namespace tr::net
