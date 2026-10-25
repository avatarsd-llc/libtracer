/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/fwd_router.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

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
     *         `by_name` round-trips, so it is the key anything filed per link must store. */
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
    /**
     * @brief The registry slot @ref link_name is that slot's OWN `name` — null otherwise.
     *
     * Non-null exactly on the point-to-point arm, which is the one arm where the two spellings
     * are the same object. The bus-peer arm deliberately leaves it null: there `link_name` is
     * the PEER segment and the slot's name is the mount above it, so a caller keying a
     * per-slot cache on `link_name` would write a peer's answer into the mount's word. Nothing
     * here is worth that footgun — the one consumer (#1437's mount-token hint) has no bus arm
     * to serve anyway, because a bus peer is `unroutable` at the subscriber door.
     */
    const child_registry_t::child_t* entry = nullptr;
    /**
     * @brief The registry slot the descent matched, on BOTH egress arms — the mount whose
     *        connection vertex the NAME-spelled hop is authorized at. Null when no mount
     *        matched (or the hit is `rejected`).
     */
    const child_registry_t::child_t* mount = nullptr;
    /** @brief The status a `rejected` hit answers: `INVALID_PATH` for a bus NAME with a
     *         residual, `NOT_FOUND` for a hop its mount's `:acl` refuses. */
    graph::status_t refusal = graph::status_t::INVALID_PATH;
    /**
     * @brief The point-to-point or bus mount the `dst` names EXACTLY, with nothing below it —
     *        null otherwise. Not a hop for a request (that `dst` addresses the connection
     *        vertex itself), but the last hop of a REPLY: see @ref take_reply_egress.
     */
    const child_registry_t::child_t* exact = nullptr;

    /**
     * @brief Make the mount a REPLY's `dst` names exactly its egress, `dst` consumed to empty
     *        (#2042); a no-op for a REPLY whose `dst` names no mount exactly.
     *
     * A REPLY's `dst` is the request's accumulated `src` — a return route, never the address
     * of a vertex here — so a run naming a point-to-point mount exactly means the originator
     * is behind that link: §B's empty seed `src`, grown by the first hop alone (RFC-0004
     * Amendment 2 §Scope boundary). Read as an address, it terminated here and the RESULT was
     * dropped uncounted. A bus mount's own NAME stays no next hop (RFC-0020).
     *
     * Holds only until #1946 lands RFC-0030 §8.5: a forwarding hop then never grows an empty
     * `src`, the terminus builds no RESULT for it, and this case no longer arises.
     */
    void take_reply_egress() noexcept {
        if (exact == nullptr) return;  // the descent did not stop exactly on a mount
        const child_registry_t::egress_t eg = exact->egress();
        if (eg.multi_peer) return;
        link = eg.link;
        link_name = exact->name;
        entry = exact;
        mount = exact;
    }

    /** @brief Turn this hit into a `NOT_FOUND` refusal unless @p allowed — a refused hop is a
     *         rejected hit, answered by the one rejection arm, never a second one. */
    void refuse_unless(bool allowed) noexcept {
        if (allowed) return;
        link = nullptr;
        rejected = true;
        refusal = graph::status_t::NOT_FOUND;
    }
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
    // exactly still forwards, with an empty residual. The hit still names the mount (`exact`),
    // because for a REPLY that run is the last hop, not an address (#2042).
    const std::optional<std::string_view> next = at(k);
    if (!next) return mount_hit_t{.peer = {}, .strip_k = k, .link_name = {}, .exact = c};
    // ONE snapshot of link + shape (#882). Read as two fields, a reconnect rebind that FLIPS
    // this name's shape could pair a stale point-to-point shape with the fresh BUS link and
    // send a directed request over the bus's broadcasting `send()` — the very fall-through
    // the rejected branch below exists to stop.
    const child_registry_t::egress_t eg = c->egress();
    if (eg.multi_peer) {
        if (!next->empty()) {
            if (transport_t* const p = child_registry_t::resolve_peer(*c, *next)) {
                const std::string_view peer = retain(k);
                return mount_hit_t{.link = p,
                                   .peer = peer,
                                   .strip_k = k + 1,
                                   .link_name = peer,
                                   .entry = nullptr,
                                   .mount = c};
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
    return mount_hit_t{
        .link = eg.link, .peer = {}, .strip_k = k, .link_name = c->name, .entry = c, .mount = c};
}

/**
 * @brief @p hit's mount as an INTERNED link token — the carry for a mount-routed subscribe
 *        (#1437), or a default token when @p hit resolved no registry slot of its own.
 *
 * A mount-routed target REBINDS the key the subscriber index is written under, from the link
 * the subscribe arrived over to the mount's own name. The arrival's carried token therefore
 * names the wrong link and is rejected at the index door, and the admission falls through to
 * the graph's NAME door — which is a linear scan of the live link slots, cheaper than the hash
 * it replaced under ~32 links and about 2x dearer at 65. Unlike the departure doors that scan
 * is argued for, this one runs per SUBSCRIBE.
 *
 * Resolved through the registry slot the descent ALREADY matched, so it costs no second
 * lookup: @ref child_registry_t::child_t::owner_hint is four bytes of padding the slot was
 * carrying anyway, and @ref graph::graph_t::intern_link_hinted turns it into the whole find.
 * The reaches that were available instead — `ctx_by_name` over the receive-context chain,
 * `entry_by_name` over the registry — are themselves linear NAME scans, over nodes far larger
 * than a 64-byte index slot, so they would have swapped the graph's scan for a worse one
 * rather than removed it.
 *
 * No invalidation, and none is owed: the hint is validated by NAME on every use, so a rebound
 * tenancy, a released slot and a never-written zero each cost one comparison and then
 * re-derive. That is what lets this word live on a registry slot the graph knows nothing about
 * and the router never has to keep in step with.
 */
[[nodiscard]] graph::link_id_t mount_token(graph::graph_t& graph, const mount_hit_t& hit) {
    if (hit.entry == nullptr) return {};
    std::uint32_t hint = hit.entry->owner_hint.load(std::memory_order_relaxed);
    const graph::link_id_t token = graph.intern_link_hinted(hit.link_name, hint);
    hit.entry->owner_hint.store(hint, std::memory_order_relaxed);
    return token;
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
    // The walk just visited these segments; record where the consumed run ENDS, which is the
    // one strip the rebuild applies (it walks nothing itself). A hit always read segment
    // `strip_k - 1` to match it (`strip_k >= 1`: no registry slot has zero segments), so the
    // walk answers; were it ever not to, past-the-end is the strip the rebuild refuses, a drop.
    if (hit.strip_k != 0) pre.strip_at = walk.end_of(hit.strip_k - 1).value_or(pre.dst_end + 1);
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
 * @pre @p pre is the peek's `PATH` answer — the only `dst` form with a descent.
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
    // Record where the consumed run ENDS — the strip the rebuild applies. Known only once the
    // descent has chosen `strip_k`, which is why it is filled here rather than in the peek. A
    // hit matched segments `0..strip_k-1` out of these `n`, so the index is in range.
    if (hit.strip_k != 0) pre.strip_at = seg_end[hit.strip_k - 1];
    return hit;
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
 * (the resolver's own rule). The frame is validated once, in place, by `tlv_node_t::over`
 * (#1648), and nothing is built. Its op and refused `dst` are NOT looked for: they are the
 * routing peek's (@p pre — child 0 and child 1 by the peek's own reading), so the only child
 * this function finds is the `src`, the first `PATH` after them (#1794).
 *
 * The reply bytes are not mirrored from the resolver's grammar — they ARE the resolver's
 * grammar (#887): @ref tr::graph::assemble_error_reply is the one definition of
 * `FWD{ VALUE op=REPLY, PATH dst=req.src, PATH src=req.dst, VALUE kind=ERROR,
 * STATUS{ ERROR{ VALUE u16 LE code } } }` (RFC-0004 §D with the RFC-0002 §C registered-code
 * identity, `tr::path::invalid` = 0x0021). This function's job is reduced to the two things
 * only it knows: which frame earns a rejection, and where the two route TLVs come from.
 *
 * The read side allocates nothing for a frame nested no deeper than the walk's inline
 * slots; a deeper frame spills its walk stack into @p rx, the receiving link's own source
 * (receiver pays), so a source that refuses drops the frame as malformed. The only other
 * draw is the reply head, from @p egress, which reports refusal by value.
 *
 * @param registry     The child registry the answer is routed back through.
 * @param inbound_name This node's name for the link the refused frame arrived on.
 * @param frame        The refused frame's bytes.
 * @param pre          The routing peek's offsets for @p frame.
 * @param rx           The receiving link's source (`rx_for(inbound_ctx)`): the walk's spill
 *                     for a frame nested deeper than its inline slots.
 * @param egress       The byte backend the reply head draws from (#795, ADR-0074) — the
 *                     router's own egress seam, so a bounded node bounds this reply too.
 */
void reject_bus_name_hop(const child_registry_t& registry, std::string_view inbound_name,
                         std::span<const std::byte> frame, const fwd_pre_t& pre,
                         mem::block_source_t& rx, mem::mem_backend_t& egress,
                         graph::status_t status) {
    const auto dec = wire::tlv_node_t::over(frame, rx);
    if (!dec) return;  // malformed ⇒ drop by value
    // A one-byte op, or nowhere trustworthy to reply to ⇒ drop by value.
    if (pre.op_body_len != 1) return;
    // The `dst` is child 1 in either routable form (RFC-0024 §4): a canonical `PATH`, or the
    // BOUND `PATH_REF` a reverse-list delivery is addressed by (§7.1 amendment 1) — the
    // refusal of a bound delivery echoes the refused `PATH_REF` exactly as the canonical
    // reject echoes the refused route, and the producer's step-5 reclaim correlates either
    // (`evict_route_edges` classifies the echo's type byte). The `src` is the first `PATH`
    // after it, and `PATH` only (`05-protocol-tlvs.md` hop rules). The scan stops at `src`, so
    // a subscribe request's TRAILING reverse list (which sits after `src`, typed
    // `PATH_REF_REVERSE`) can never be mistaken for either route.
    std::optional<wire::tlv_node_t> src;
    std::size_t child = 0;
    for (const wire::tlv_node_t c : dec->children()) {
        if (child++ > 1 && c.type() == type_t::PATH) {
            src = c;
            break;
        }
    }
    if (!src) return;
    // Never answer a REPLY with a reply (the resolver rejects a REPLY by value too): an
    // unroutable reply hop erroring BACK would ping-pong between two confused nodes.
    //
    // MASKED, like every other op-byte read (RFC-0024 §9.3 — "a forwarder MUST mask rather
    // than switch on the raw byte"). Unmasked, a REPLY carrying any flag bit is not
    // recognised as a REPLY at all and this guard waves it through, so the node answers a
    // reply with an addressed error reply — the exact frame the line above forbids.
    if (static_cast<fwd_op_t>(u8(frame[pre.op_body_off]) & graph::kFwdOpcodeMask) ==
        fwd_op_t::REPLY)
        return;

    // Reply routes swapped, as the resolver assembles them: reply dst = request src (the
    // accumulated return route), reply src = request dst (the refused spelling — what the
    // peer asked for, echoed so it can correlate). Both are the nodes' trailer-EXCLUDED
    // `wire()` spans, the exact shape the resolver hands over from its arena (ADR-0041 §4):
    // the reply assembler copies them once and clears the trailer bits of the copied opt
    // byte, so a peer that stamped or CRC'd its route TLV never sees those bytes echoed back
    // inside the reply's address (#887). No re-encode, no owning copy. The refused `dst`'s
    // trailer-excluded span runs from its header (right after the op) to its body's end.
    const std::size_t dst_pos = pre.op_pos + pre.op_total;
    graph::reply_route_t route{.dst_wire = src->wire(),
                               .src_wire = frame.subspan(dst_pos, pre.dst_end - dst_pos)};
    // The wire-time echo (#1109) rides rejections too: an origin probing RTT against a route
    // this node refuses still gets its stamp back with the addressed error, so the same
    // frame answers both questions. TF=1 is not echoed — anchorless at the root, the spec's
    // own MUST-reject case (the terminus resolver applies the identical filter).
    if (dec->opt().ts && !dec->opt().tf) route.echo_ts = dec->trailer()->ts;
    // The status is the CALLER's, because the refusals a hop makes on its own are different
    // answers to different questions: a bus NAME with a residual is a malformed address
    // (`tr::path::invalid`), while a PAIR this node cannot validate is an address that does not
    // resolve HERE (`tr::path::not_found`, RFC-0029 §6.3). Everything else about the refusal — the
    // swapped routes, the echoed spelling the sender correlates on, the wire-time stamp echo, the
    // egress seam and the bus-peer fallback — is identical, which is why this is one function with
    // a status rather than two that drift.
    const view::rope_t reply = graph::assemble_error_reply(route, status, egress);
    // One link — plus the echo trailer link on a stamped request — by construction: an error
    // reply carries no shared payload. A zero-link rope is the egress
    // refusal; drop rather than emit a headerless frame. The echoed shape is two links, so
    // this COLD path materializes rather than growing a scatter-gather send it never needed.
    if (reply.link_count() == 0) return;
    // `by_name` includes the bus-peer fallback, so a frame that arrived FROM a peer (whose
    // inbound_name is the peer's own name) answers back over that peer's directed endpoint —
    // the same lookup `reply_link` falls back to for a bus peer's terminus reply. This cold
    // refusal keeps the by-name form for every inbound link; the terminus's point-to-point
    // replies leave through the ctx's link instead (#1709).
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

/**
 * @brief Below this many reply bytes the refusal reply is gathered into a stack buffer and
 *        sent contiguous; above it, the collected spans go out through the link's
 *        scatter-gather `send`. A strategy selector, not a limit — every reply is emitted
 *        either way (`core/STYLE.md` §Introspection: "tuning knobs are not limits").
 */
constexpr std::size_t kRefusalReplyInlineBytes = 256;

/**
 * @brief The most spans a refusal reply gathers: the head, the two route bodies (each one span
 *        on the contiguous tier, one per straddled link on the rope tier), the middle route
 *        header, the status tail and the echo trailer.
 */
constexpr std::size_t kRefusalReplySpans = 12;

/**
 * @brief The stack storage a terminus lends the resolver for an acknowledgement reply (#1658):
 *        the head segment of an acked WRITE or subscribe is placed here instead of drawn from
 *        `egress`, so acking a remote write allocates nothing.
 *
 * Sized like @ref kRefusalReplyInlineBytes: one segment header plus a head over two ordinary
 * routes, with room left for a PAIR seed and an echo. A strategy selector, not a limit: a head that
 * does not fit draws from `egress` as every reply did before.
 */
constexpr std::size_t kReplyStoreBytes = 256;

/**
 * @brief Answer a request this terminus could not serve for want of MEMORY with an addressed
 *        `FWD{REPLY, kind=ERROR, STATUS{ERROR{tr::flow::backpressure}}}` — built on the
 *        stack, from the request's own bytes, through no allocator at all (#1612).
 *
 * The three resource arms of the terminus — the rx source refusing the decode arena, the
 * egress backend refusing the reply head, the rx source refusing the reply's iov table — used
 * to count a drop and return, so a requester whose frame met an exhausted receiver saw only
 * its own timeout: a memory condition indistinguishable from a lost frame, an unreachable
 * node or a wedged link. Reference 04 §"Exhaustion is a value" now states the rule this
 * emitter exists for: *the reply must not need the memory that just refused*.
 * `tr::graph::assemble_error_reply` — the ordinary addressed-error path — draws its head from
 * the egress backend, which on two of the three arms is exactly what refused, and on the
 * third shares the node's memory ceiling with it. So this reply borrows the request instead.
 *
 * The request's routes are located by OFFSET through the one grammar (`read_fwd_header`) with
 * no decode and no arena: `op` is child 0, `dst` the first `PATH` / `PATH_REF` after it, `src`
 * the next `PATH` after `dst` — the routes `reject_bus_name_hop` answers with, found over the
 * raw bytes (this arm is reached from the terminus, past the routing peek's scope), so a
 * `FIELD` selector between the two routes (a `:subscribers[]` SUBSCRIBE carries one) is
 * stepped over. The reply is the resolver's own grammar — routes swapped, each route header
 * re-emitted with its trailer bits cleared and its body sliced exactly as `struct_opt` does,
 * `kind=ERROR`, then `error_status_tail` — so a requester cannot tell which emitter answered.
 * A request whose `src` cannot be located has nowhere to reply to and stays a counted drop,
 * which is the correct answer for it; so does one whose `src` is EMPTY, because that is the
 * origin declining a reply (RFC-0004 Amendment 2), not a route.
 *
 * The wire-time echo (#1109) rides too when the request's TF=0 stamp sits in one span: the
 * origin measuring RTT against an exhausted node still gets its stamp back. TF=1 is not echoed
 * (anchorless at the root, the spec's own MUST-reject case), and a stamp straddling a rope link
 * is dropped rather than stitched — the echo is a capability, the reply is the obligation.
 *
 * Two egress shapes, one span list: a reply that fits @ref kRefusalReplyInlineBytes is
 * gathered into a stack buffer and sent contiguous — the transport's scatter-gather gather (which
 * draws from the link's egress store) is never reached, so nothing on this path can allocate.
 * A longer reply goes out through the link's `send(iov)`; a transport with native
 * scatter-gather still allocates nothing, and one without answers from its own egress seam.
 *
 * `[[gnu::cold]]` and out of line: this is a refusal arm, and the terminus's warm path must
 * not carry its stack frame.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope) over the request frame.
 * @param  cur    The cursor positioned at the request's first byte.
 * @param  link   The link the request arrived on — the reply goes back the way it came.
 * @param  status The refusal; `BACKPRESSURE` at every current caller.
 */
template <class Cursor>
[[gnu::noinline, gnu::cold]] void emit_refusal_reply(const Cursor& cur, transport_t& link,
                                                     graph::status_t status) {
    const auto outer = read_fwd_header(cur, 0);
    if (!outer || outer->type != type_t::FWD || !outer->opt.pl) return;
    const std::size_t end = outer->body_off + outer->body_len;
    // Child 0 — the op, one byte, masked (RFC-0024 §9.3). A REPLY is never answered with a
    // reply: an exhausted node erroring BACK at a reply would ping-pong between two nodes.
    const auto op = read_fwd_header(cur, outer->body_off);
    if (!op || op->type != type_t::VALUE || op->body_len != 1) return;
    if (static_cast<fwd_op_t>(cur.byte_at(op->body_off) & graph::kFwdOpcodeMask) == fwd_op_t::REPLY)
        return;
    // dst = the first routable child after the op; src = the next PATH after dst. Anything
    // between them (a FIELD selector) is stepped over; anything after src is never read.
    std::optional<fwd_hdr_t> dst;
    std::optional<fwd_hdr_t> src;
    std::size_t dst_pos = 0;
    std::size_t src_pos = 0;
    for (std::size_t pos = outer->body_off + op->total; pos < end && !src;) {
        const auto h = read_fwd_header(cur, pos);
        if (!h || pos + h->total > end) return;  // ragged child ⇒ malformed ⇒ nowhere to reply
        if (!dst) {
            if (h->type == type_t::PATH || h->type == type_t::PATH_REF) {
                dst = h;
                dst_pos = pos;
            }
        } else if (h->type == type_t::PATH) {
            src = h;
            src_pos = pos;
        }
        pos += h->total;
    }
    // An EMPTY `src` is not a route: it is the origin declining a reply (RFC-0004 Amendment 2,
    // #1491) — the resolver answers such a WRITE with nothing, and so does this arm. Answering
    // it would mint exactly the addressed error on a zero-length route the amendment forbids.
    if (!dst || !src || src->body_len == 0) return;

    // The echo stamp: the request's TF=0 outer trailer sits right after its body. Echoed only
    // when it is one span, decided BEFORE the head is written so the TS bit tells the truth.
    std::span<const std::byte> echo;
    if (outer->opt.ts && !outer->opt.tf) {
        std::size_t pieces = 0;
        cur.for_each_span(end, wire::trailer_ts_bytes(false), [&](std::span<const std::byte> s) {
            ++pieces;
            echo = s;
        });
        if (pieces != 1) echo = {};
    }

    // A route header re-emitted trailer-less: the same bytes, `opt` cleared of TS/TF/CR/CW as
    // `struct_opt` clears them, because the body copied below excludes the trailer the bits
    // would otherwise announce. The length field is untouched — it never counted the trailer.
    const auto route_header = [&cur](const fwd_hdr_t& h, std::size_t pos,
                                     std::array<std::byte, 6>& out) {
        for (std::size_t i = 0; i < h.header_len; ++i) out[i] = std::byte{cur.byte_at(pos + i)};
        out[1] = static_cast<std::byte>(h.opt.without_trailer().encode());
        return std::span<const std::byte>(out.data(), h.header_len);
    };

    // Reply = FWD{ op=REPLY, dst=req.src, src=req.dst, kind=ERROR, STATUS tail }: the routes
    // swap, so the reply's FIRST route is the request's src.
    constexpr std::size_t kU8Value = 5;
    const std::size_t body_len = kU8Value + src->header_len + src->body_len + dst->header_len +
                                 dst->body_len + kU8Value + graph::kErrorStatusTailBytes;
    std::array<std::byte, 6> rdst_hdr{};
    std::array<std::byte, 6> rsrc_hdr{};
    const std::span<const std::byte> rdst = route_header(*src, src_pos, rdst_hdr);
    const std::span<const std::byte> rsrc = route_header(*dst, dst_pos, rsrc_hdr);
    const std::array<std::byte, kU8Value> reply_op{
        static_cast<std::byte>(std::to_underlying(type_t::VALUE)), std::byte{0}, std::byte{1},
        std::byte{0}, static_cast<std::byte>(std::to_underlying(fwd_op_t::REPLY))};
    stack_writer<18> head;  // FWD header (<=6) + the 5-byte op + the reply-dst header (<=6)
    head.header(type_t::FWD, body_len, opt_t{.ts = !echo.empty(), .tf = false});
    head.raw(reply_op);
    head.raw(rdst);
    if (!head.ok()) return;  // cannot happen at N=18; drop rather than emit a truncated head
    // kind=ERROR then the status tail — one constant-sized block.
    std::array<std::byte, kU8Value + graph::kErrorStatusTailBytes> tail{
        static_cast<std::byte>(std::to_underlying(type_t::VALUE)), std::byte{0}, std::byte{1},
        std::byte{0}, static_cast<std::byte>(std::to_underlying(graph::reply_kind_t::ERROR))};
    const std::array<std::byte, graph::kErrorStatusTailBytes> status_tail =
        graph::error_status_tail(status);
    std::memcpy(tail.data() + kU8Value, status_tail.data(), status_tail.size());

    // Gather the spans: nothing below copies a route byte; the bodies are the request's own.
    std::array<std::span<const std::byte>, kRefusalReplySpans> iov{};
    std::size_t n = 0;
    bool overflow = false;
    const auto put = [&](std::span<const std::byte> s) {
        if (n == iov.size()) {
            overflow = true;
            return;
        }
        iov[n++] = s;
    };
    put(head.span());
    cur.for_each_span(src->body_off, src->body_len, put);
    put(rsrc);
    cur.for_each_span(dst->body_off, dst->body_len, put);
    put(tail);
    if (!echo.empty()) put(echo);
    if (overflow) return;  // a route straddling more links than the table holds — drop

    std::size_t total = 0;
    for (std::size_t i = 0; i < n; ++i) total += iov[i].size();
    if (total <= kRefusalReplyInlineBytes) {
        std::array<std::byte, kRefusalReplyInlineBytes> buf;
        std::size_t off = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!iov[i].empty()) std::memcpy(buf.data() + off, iov[i].data(), iov[i].size());
            off += iov[i].size();
        }
        link.send(std::span<const std::byte>(buf.data(), total));
        return;
    }
    link.send(std::span<const std::span<const std::byte>>(iov.data(), n));
}

/**
 * @brief Answer a retired outer frame with one bare `ERROR{tr::schema::type_mismatch}` back on
 *        the link it arrived on (RFC-0032 §6.1).
 *
 * reference/01 §"Handling unknown type codes" answers an unknown outer type with
 * `ERROR{tr::schema::type_mismatch}` when a return path exists. The retired `ADVERTISE` /
 * `COMPACT` / `HANDLE_NACK` carry no `src`, so the arrival link is that path, and the answer
 * takes RFC-0002 §C's bare form: a protocol-stack report with no request to answer. A receiver
 * never answers a bare `ERROR` (RFC-0032 amends RFC-0002 §C), so the answer is never answered.
 *
 * Ten constant bytes, built on the stack: nothing on this path can allocate.
 *
 * @param link The link the frame arrived on.
 */
[[gnu::noinline, gnu::cold]] void emit_retired_type_answer(transport_t& link) {
    // `STATUS{ERROR{...}}` minus its 4-byte STATUS header is the bare `ERROR`.
    const std::array<std::byte, graph::kErrorStatusTailBytes> tail =
        graph::error_status_tail(graph::status_t::TYPE_MISMATCH);
    link.send(std::span<const std::byte>(tail).subspan(4));
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
 * OFFSET (no decode, no allocation — the warm read-reply path bails at the first non-matching
 * child) and hands back the `src` window, which is the refused route byte-for-byte as this
 * node stored and emitted it.
 *
 * It starts where the routing peek stopped (#1794): the FWD header, the op and the reply's
 * `dst` are @p pre's, read once by @ref peek_fwd_dst_any, and the caller has already found
 * the op to be a REPLY there. What is read here is only what lies past the `dst`. A canonical
 * `dst` is required, as the reply's consumed way home always is; a reply whose `dst` the
 * routing peek did not accept as a canonical `PATH` is not one this node's refusal produced.
 *
 * Strict on the shape, deliberately: a reply with a FIELD child, a non-ERROR kind, or any
 * status but `tr::path::invalid` (0x0021) — or, for a PAIR-spelled route, the
 * `tr::path::not_found` a PAIR hop refuses with (RFC-0029 §6) — is NOT a route refusal and
 * must not evict: a TIMEOUT or BACKPRESSURE names a live route having a bad day.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 * @param  pre    The routing peek's offsets for the frame, which is a REPLY.
 * @retval std::nullopt Not an addressed `tr::path::invalid` refusal.
 */
template <class Cursor>
[[nodiscard]] std::optional<refused_src_t> peek_refused_route(const Cursor& cur,
                                                              const fwd_pre_t& pre) {
    // Children 1 and 2 — the one-byte REPLY op and the PATH `dst`, the reply's consumed way
    // home — are the routing peek's; neither is read again. A valid peek here is a `PATH`
    // `dst`: the router refuses a `PATH_REF` one before its REPLY test (RFC-0029 §5.3).
    if (!pre.valid || pre.op_body_len != 1) return std::nullopt;
    // Every later read is one shape: a header of an expected type that starts inside the FWD
    // body. Stated once, so the children below differ only in what they then check.
    const std::size_t end = pre.body_end;
    const auto child = [&](std::size_t at, wire::type_t want) -> std::optional<fwd_hdr_t> {
        if (at >= end) return std::nullopt;
        const auto h = read_fwd_header(cur, at);
        if (!h || h->type != want) return std::nullopt;
        return h;
    };
    // Child 3 — the refused route, echoed whole: a PATH of NAMEs, or (RFC-0024 §7.1
    // amendment 1, spelled per RFC-0029 §4.2) the PAIR-headed `PATH` a reverse-list delivery
    // was refused as. Non-empty by the same rule as the eviction it feeds (an empty route
    // names nothing and matches nothing).
    std::size_t pos = pre.after_dst;
    const auto rsrc = child(pos, wire::type_t::PATH);
    if (!rsrc || rsrc->body_len == 0) return std::nullopt;
    // An escape-headed body is an element-spelled route: a PAIR hop refuses it `NOT_FOUND`
    // (RFC-0029 §6.2), which for THAT spelling is the dead-binding answer `tr::path::invalid`
    // is for a NAME route. A canonical route's `NOT_FOUND` stays a live route's bad day.
    const bool pair_echo = cur.byte_at(rsrc->body_off) == wire::kPackedEscapeLen;
    const refused_src_t out{.off = pos, .len = rsrc->total};
    pos += rsrc->total;
    // Child 4 — VALUE kind == ERROR.
    const auto kind = child(pos, wire::type_t::VALUE);
    if (!kind || kind->body_len != 1 ||
        cur.byte_at(kind->body_off) !=
            static_cast<std::uint8_t>(std::to_underlying(graph::reply_kind_t::ERROR)))
        return std::nullopt;
    pos += kind->total;
    // Child 5 — STATUS{ ERROR{ VALUE u16 LE } }, and the code is tr::path::invalid.
    const auto st = child(pos, wire::type_t::STATUS);
    if (!st || !st->opt.pl) return std::nullopt;
    const auto err = child(st->body_off, wire::type_t::ERROR);
    if (!err || !err->opt.pl) return std::nullopt;
    const auto code = child(err->body_off, wire::type_t::VALUE);
    if (!code || code->body_len != 2) return std::nullopt;
    const auto code_v = cur.load_le(code->body_off, 2);
    if (code_v != std::to_underlying(wire::err_t::PATH_INVALID) &&
        !(pair_echo && code_v == std::to_underlying(wire::err_t::PATH_NOT_FOUND)))
        return std::nullopt;
    return out;
}

}  // namespace

bool fwd_router_t::add_child(std::string_view name, transport_t& link, mem::block_source_t* rx,
                             link_kind_t kind) {
    // ADR-0063 §3: serialize control-plane writers. The registry's scan-then-append and the
    // receiver chain's append are both non-atomic, and two creates arriving on two
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
    // most `graph::kMaxSegments` of them (RFC-0023). A name wider than that can never be the
    // prefix of any address that exists, so registering it is the same silent misroute #523
    // was filed about, one step further out. It is refused instead, by value. A name with no
    // segments, or with an empty or over-long one, keys no vertex, so the registry refuses it
    // below as a mount with no connection vertex.
    if (child_registry_t::segment_count(name) > graph::kMaxSegments) return false;
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
    //
    // The link's catalog identity (#1650) is interned FIRST, so its one control-plane append
    // happens before anything is registered. Interned once here, every write the child carries
    // hands the graph a pointer and nothing else; stored on both arms below, before publication.
    //
    // The router's own state for this registration draws from the link plane's source
    // (#1779), and every allocation is taken BEFORE anything changes or undone at once: the
    // kind record and a bus mount's token cache up front, the ctx right after the registry
    // slot it views. A refusal is this function's ordinary `false`, with nothing registered.
    // A kind record already interned is kept; it is what the next such registration finds.
    const std::optional<const link_kind_t*> interned = intern_kind(kind);
    if (!interned) return false;
    const link_kind_t* const interned_kind = *interned;
    bus_link_t* const bus = bus_of(link);
    // A bus mount's per-peer token cache, drawn up front on every bus registration. A name keeps
    // ONE cache for the router's life (see `peer_tokens_own`), so on a re-add whose ctx already
    // has one the fresh block simply goes back below — a control-plane alloc/free pair, in
    // exchange for one undo path instead of two.
    bus_token_cache_t* tokens = nullptr;
    if (bus != nullptr) {
        void* const raw =
            link_src_->try_alloc(sizeof(bus_token_cache_t), alignof(bus_token_cache_t));
        if (raw == nullptr) return false;
        tokens = ::new (raw) bus_token_cache_t();
    }
    const auto drop_tokens = [&] {
        if (tokens == nullptr) return;
        tokens->~bus_token_cache_t();
        link_src_->release(tokens, sizeof(bus_token_cache_t), alignof(bus_token_cache_t));
    };
    // The door (RFC-0029 §13.2 S6): the child's mount run IS its connection vertex's
    // canonical key, so its graph slot is one keyed find of bytes the registry already
    // encoded, made once per REGISTRATION (#884). Every spelling's gate reads it lock-free,
    // and a PAIR finds its egress by it. A mount whose vertex does not exist is REFUSED here,
    // with nothing registered (#1940): a link cannot exist without its connection vertex.
    // A BUS mount records its door too — the NAME `<mount>/<peer>` and a PAIR-named anchor
    // are gated there — and is refused as an egress by its shape (§10), not by a slot.
    const auto conn_slot_of = [this](std::span<const std::byte> run) {
        const std::optional<graph::vertex_handle_t> v = graph_.find(run);
        const std::optional<graph::vertex_slot_t> slot = v ? graph_.vertex_slot(*v) : std::nullopt;
        return slot ? std::optional<std::uint32_t>(slot->index) : std::nullopt;
    };
    if (!registry_.add(name, link, conn_slot_of)) {
        drop_tokens();
        return false;
    }
    child_rx_ctx_t* const ctx_p = acquire_ctx(name);
    if (ctx_p == nullptr) {
        // Only a name with no ctx at all gets here, so its slot was not live before this
        // call: tombstoning it again is the whole undo.
        (void)registry_.erase(name);
        drop_tokens();
        return false;
    }
    // The registration's own facts, its decode source and its catalog claim, go on the
    // child's ctx (#1941): only a router child has them, so the link object carries nothing
    // for them. The ctx is hidden until `publish_ctx` below, which is their publication edge.
    ctx_p->rx.store(rx, std::memory_order_relaxed);
    ctx_p->kind.store(interned_kind, std::memory_order_relaxed);
    // Capability-matched receiver (ADR-0042 §1 / ADR-0044): a BUS link delivers
    // frames tagged with the SENDING peer's name, which becomes the hop's inbound
    // NAME — so the `src` grown on a forward (and the link a terminus reply goes
    // back over) names the bus PEER, and the registry's peer fallback turns that
    // name into a directed send. An owning-delivery link funnels through the same
    // routing with the frame view alongside; a span link keeps the borrowed-span
    // path. No adapter wraps a span into a lying view.
    // Asked through `bus_of` (#375 deliverable 3): a target that closed the bus module out
    // wires every child point-to-point, and the whole peer-named arm below — the two
    // peer-lifecycle notifiers, the peer-named receivers, the slot shape bit
    // `resolve_peer_name` keys off — is discarded at compile time rather than branched over.
    if (bus != nullptr) {
        // A reassembling bus that delivers ropes (ADR-0053 §5) hands its group up
        // as-is — zero-copy; a span-only bus keeps the borrowed peer-named path.
        // A bus frame arrives tagged with the sending peer's name — but a PEER has no
        // registry entry, so the peer name alone cannot say which mount it hangs under.
        // The receiver therefore carries the same stable per-child ctx the point-to-point
        // path uses, holding the child's QUALIFIED name (#510): a forward hop then grows
        // `src` by the full `net/<module>/<name>/<peer>` path rather than a bare peer
        // segment, which is what keeps two buses' same-named peers distinct on the way back.
        // Departure seam (RFC-0009 §D extended): a bus peer that hangs up carries its
        // own name, and its edges are keyed by that name, so link_down still takes the peer.
        // A bound route never EGRESSES over the bus mount itself: its `send()` broadcasts
        // (ADR-0073 §3, RFC-0020 §3), so `door_egress` refuses a multi-peer door by shape,
        // and an announce-census peer has no vertex an element could name (ADR-0044 as
        // amended, #1223). Such a route fails validation here and the origin falls back to
        // the canonical spelling.
        child_rx_ctx_t& bctx = *ctx_p;
        // The BUS tier's per-peer LINK-TOKEN cache (#1417). Allocated on this name's first
        // bus registration and REUSED — never replaced — on every later one, so a lock-free
        // reader on the receive thread can hold its address across a re-add. `acquire_ctx`
        // already cleared its contents; what is stored here is the facet flag that tells
        // `link_id_of` which tier this ctx is.
        if (bctx.peer_tokens_own == nullptr) std::swap(bctx.peer_tokens_own, tokens);
        drop_tokens();  // the fresh cache, unless the ctx just took it
        bctx.peer_tokens.store(bctx.peer_tokens_own, std::memory_order_relaxed);
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
            [](void* c, peer_handle_t handle, std::string_view peer) {
                auto* const cc = static_cast<child_rx_ctx_t*>(c);
                const frame_scope_t scope;
                cc->self->bus_peer_up(*cc, handle, peer);
            },
            &bctx);
        bus->set_peer_down_notifier(
            [](void* c, peer_handle_t handle, std::string_view peer) {
                auto* const cc = static_cast<child_rx_ctx_t*>(c);
                const frame_scope_t scope;
                cc->self->bus_peer_down(*cc, handle, peer);
            },
            &bctx);
        if (bus->delivers_ropes()) {
            bus->set_peer_rope_receiver(
                [](void* c, peer_handle_t peer, view::rope_t frame) {
                    auto* const cc = static_cast<child_rx_ctx_t*>(c);
                    const frame_scope_t scope;
                    cc->self->on_frame_rope_bus(*cc, peer, std::move(frame));
                },
                &bctx);
        } else {
            bus->set_peer_receiver(
                [](void* c, peer_handle_t peer, std::span<const std::byte> frame) {
                    auto* const cc = static_cast<child_rx_ctx_t*>(c);
                    const frame_scope_t scope;
                    cc->self->on_frame_bus(*cc, peer, frame);
                },
                &bctx);
        }
        return true;
    }
    // Point-to-point: the inbound NAME is fixed per child, carried by a stable
    // per-child ctx (the chain holds the address for the router's lifetime).
    child_rx_ctx_t& ctx = *ctx_p;
    // No per-peer token cache on this tenancy — CLEARED rather than left, because a re-add
    // rebinds this ctx (#884) and a name re-added as a point-to-point child must not keep
    // answering `link_id_of` through the bus table it used to have (#1417). The table itself
    // is kept alive on the ctx — a later re-add as a bus mount reuses it rather than
    // allocating a second one a reader might still be standing on. (The bus FACET itself is
    // the link's, and the slot's shape bit says whether to ask for it: #1294, #1941.)
    ctx.peer_tokens.store(nullptr, std::memory_order_relaxed);
    // LAST: every field a lock-free reader may look at is written above, and this is the
    // release edge that makes the node visible to one (see `child_rx_ctx_t::next`).
    publish_ctx(ctx);
    // Departure seam (RFC-0009 §D extended): the same stable ctx carries the child's
    // NAME to link_down when the transport reports its one connection dead.
    link.set_down_notifier(
        [](void* c) {
            auto* const cc = static_cast<child_rx_ctx_t*>(c);
            cc->self->link_gone(cc->name, {.conn = cc->entry->conn_slot});
            // The eviction released this child's index slot, so the cached token is spent
            // (#1417). Dropped here rather than inside `link_down`, which is also a public
            // by-NAME door with no ctx in hand; a reconnect over the same registration mints
            // a fresh token on its first subscribe.
            cc->link_token.store(0, std::memory_order_relaxed);
        },
        &ctx);
    if (link.delivers_ropes()) {
        link.set_rope_receiver(
            [](void* c, view::rope_t frame) {
                auto* const cc = static_cast<child_rx_ctx_t*>(c);
                const frame_scope_t scope;
                cc->self->on_frame_rope_impl(cc->name, std::move(frame), cc, false);
            },
            &ctx);
    } else {
        link.set_receiver(
            [](void* c, std::span<const std::byte> frame) {
                auto* const cc = static_cast<child_rx_ctx_t*>(c);
                const frame_scope_t scope;
                cc->self->on_frame_impl(cc->name, frame, nullptr, cc, false);
            },
            &ctx);
    }
    return true;
}

bool fwd_router_t::remove_child(std::string_view name) {
    // Stop resolving FIRST: once the entry is tombstoned no forward can reach the link,
    // so the caller is free to destroy the transport as soon as this returns. Then the
    // ordinary departure eviction reclaims the graph edges — the same
    // work link_down does, reused rather than duplicated.
    //
    // The eviction runs with `ctl_m_` RELEASED, as it does from every other door (a
    // transport's down-notifier calls `link_down` holding nothing). It gives back each
    // evicted edge's link hold, and that call takes `transport_vertex_t`'s control lock,
    // which the declared order puts ABOVE this one (#1816). The cost: a direct embedder that
    // races `add_child(name)` against this call can see the departing eviction reach the
    // re-added child's edges. `transport_vertex_t` serializes both under its `ops_m_`.
    graph::link_pair_t gone;  // the departing slot's pair, off its receive ctx (#1941)
    {
        const std::lock_guard ctl(ctl_m_);  // pairs with add_child (ADR-0063 §3)
        if (!registry_.erase(name)) return false;
        // TOMBSTONE the receiver ctx (#884) — it stays on the published chain, because a lock-free
        // reader may be standing on it right now and the transport still holds its address, but it
        // stops answering `ctx_by_name`. Leaving it RESOLVING was the defect:
        // `add_child` of the same name appended a second ctx, and the name-keyed lookups answer
        // with the FIRST match, so every `connection_ref`/`hop_mint`/`adopt_binding` after a
        // re-add resolved the DEAD context — bound routes for the re-created child were minted
        // against the retired tenancy and never validated. A `release` store, paired with the
        // acquire each walk performs, so a reader either skips this node or has already passed it.
        if (child_rx_ctx_t* const ctx = ctl_ctx_by_name(name)) {
            ctx->retired.store(true, std::memory_order_release);
            gone.conn = ctx->entry->conn_slot;
            // The interned link token goes with the tenancy too (#1417): `link_down` below
            // releases the index slot it names, so a copy left here would outlive its stamp.
            ctx->link_token.store(0, std::memory_order_relaxed);
            if (ctx->peer_tokens_own != nullptr) ctx->peer_tokens_own->clear();
        }
    }
    link_gone(name, gone);
    return true;
}

// --- origination (#1645): the reply half — the request half is fwd_originate.cpp --------

namespace {

/**
 * @brief The `[off, len)` of a FWD frame's `dst` PATH body — on a REPLY, the return route that
 *        is left once every hop has stripped its own part. `nullopt` for anything else.
 */
template <class Cursor>
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> peek_reply_route(
    const Cursor& cur) {
    const auto fwd_h = read_fwd_header(cur, 0);
    if (!fwd_h || fwd_h->type != wire::type_t::FWD || !fwd_h->opt.pl) return std::nullopt;
    const auto op_h = read_fwd_header(cur, fwd_h->body_off);
    if (!op_h || op_h->type != wire::type_t::VALUE) return std::nullopt;
    const std::size_t dst_pos = fwd_h->body_off + op_h->total;
    if (dst_pos >= fwd_h->body_off + fwd_h->body_len) return std::nullopt;
    const auto dst_h = read_fwd_header(cur, dst_pos);
    if (!dst_h || dst_h->type != wire::type_t::PATH || dst_h->opt.pl) return std::nullopt;
    return std::pair{dst_h->body_off, dst_h->body_len};
}

}  // namespace

template <class Cursor>
sink_slot_t<fwd_router_t::reply_fn_t>::snapshot_t fwd_router_t::reply_target(const Cursor& cur) {
    // No armed record ⇒ the shipped path exactly: one relaxed load, no lock, no peek.
    if (origins_.load(std::memory_order_acquire) != nullptr) {
        if (const auto route = peek_reply_route(cur)) {
            const auto [off, len] = *route;
            const auto same_route = [&](std::span<const std::byte> r) {
                if (r.size() != len) return false;
                for (std::size_t i = 0; i < len; ++i)
                    if (static_cast<std::uint8_t>(cur.byte_at(off + i)) !=
                        std::to_integer<std::uint8_t>(r[i]))
                        return false;
                return true;
            };
            const std::lock_guard lock(origin_m_);
            // Newest-first list, so the LAST match is the oldest outstanding request.
            origin_t* match_prev = nullptr;
            origin_t* match = nullptr;
            for (origin_t *prev = nullptr, *o = origins_.load(std::memory_order_relaxed);
                 o != nullptr; prev = o, o = o->next_) {
                if (same_route(o->route_)) {
                    match_prev = prev;
                    match = o;
                }
            }
            if (match != nullptr) {
                if (match_prev != nullptr)
                    match_prev->next_ = match->next_;
                else
                    origins_.store(match->next_, std::memory_order_release);
                const sink_slot_t<reply_fn_t>::snapshot_t s{match->fn_, match->ctx_};
                match->next_ = nullptr;
                match->owner_.store(nullptr, std::memory_order_release);
                return s;
            }
        }
    }
    return reply_.get();
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

/**
 * @brief The link-departure hook body: graph eviction (deliveries to the dead session stop
 *        and its per-edge state is reclaimed), then the deferred AWAITs filed under it. See
 *        the header doc for the seam and threading contract.
 */
void fwd_router_t::link_down(std::string_view link_name) {
    link_gone(link_name, inbound_pair(nullptr, link_name, {}));
}

bool fwd_router_t::link_down(std::string_view link_name, peer_handle_t peer) {
    // The published walk, as a frame path reads it: a removed mount has no peers to evict.
    const child_rx_ctx_t* const ctx = ctx_by_name(link_name);
    bus_link_t* const bus =
        ctx != nullptr && peer.valid() ? bus_of(*ctx->entry->bound().link) : nullptr;
    std::array<char, kPeerNameChars> scratch{};
    const std::string_view name =
        bus != nullptr ? bus->peer_name(peer, scratch) : std::string_view{};
    if (name.empty()) return false;
    const frame_scope_t scope;
    bus_peer_down(*ctx, peer, name);
    return true;
}

void fwd_router_t::link_gone(std::string_view link_name, graph::link_pair_t link) {
    const frame_scope_t scope;
    graph_.evict_link_edges(link_name, link);
    // A deferred AWAIT's reply has nowhere to go once its link is down: release its waiter
    // (ADR-0084). The receiver holds no deadline, so this is how an unanswered one ends.
    const std::lock_guard lock(awaits_.m);
    cancel_awaits_locked(link_name, false);
}

namespace {
/**
 * @brief Rebuild a @ref peer_handle_t from the `bits()` a receiver ctx stores it as.
 *
 * The handle is kept as ONE `u64` on the ctx so a frame path loads it with a single relaxed
 * atomic rather than two that could straddle a control-plane rebind and pair one tenancy's
 * index with another's generation — the field-tearing shape #882 fixed one seam out.
 */
[[nodiscard]] constexpr peer_handle_t peer_handle_from_bits(std::uint64_t bits) noexcept {
    return peer_handle_t{wire::pair_t{.index = static_cast<std::uint32_t>(bits),
                                      .generation = static_cast<std::uint32_t>(bits >> 32)}};
}

}  // namespace

peer_handle_t fwd_router_t::session_tenant(const child_rx_ctx_t& ctx, std::uint32_t slot) noexcept {
    session_tenancy_t* const t = ctx.tenancy.load(std::memory_order_acquire);
    return t != nullptr && slot < t->size
               ? peer_handle_from_bits(t->word(slot).load(std::memory_order_acquire))
               : peer_handle_t{};
}

peer_handle_t fwd_router_t::anchor_tenant(const child_rx_ctx_t* ctx, std::string_view name,
                                          graph::vertex_handle_t anchor,
                                          std::uint32_t generation) const noexcept {
    // The word FIRST, the generation after: see `bus_peer_up` for the publication order.
    const std::optional<std::uint32_t> slot = session_slot_index(name);
    const peer_handle_t tenant =
        ctx != nullptr && slot ? session_tenant(*ctx, *slot) : peer_handle_t{};
    const std::optional<graph::vertex_slot_t> now = graph_.vertex_slot(anchor);
    return now && now->generation == generation ? tenant : peer_handle_t{};
}

bool fwd_router_t::set_session_tenant_locked(const child_rx_ctx_t& ctx, std::uint32_t slot,
                                             std::uint64_t bits) {
    session_tenancy_t* t = ctx.tenancy.load(std::memory_order_relaxed);
    if (t == nullptr || slot >= t->size) {
        // Doubled from 8 until it covers the slot; the old words are copied, the old table
        // parked for any reader still standing on it (see `session_tenancy_t`).
        std::uint32_t size = t != nullptr ? t->size : 8;
        while (size <= slot) {
            if (size > UINT32_MAX / 2) return false;
            size *= 2;
        }
        const std::size_t bytes = sizeof(session_tenancy_t) + size * sizeof(std::uint64_t);
        void* const raw = link_src_->try_alloc(bytes, alignof(session_tenancy_t));
        if (raw == nullptr) return false;
        auto* const grown = ::new (raw) session_tenancy_t{.size = size, .parked = t};
        auto* const words = reinterpret_cast<std::byte*>(grown + 1);
        for (std::uint32_t i = 0; i < size; ++i) {
            const std::uint64_t held =
                t != nullptr && i < t->size ? t->word(i).load(std::memory_order_relaxed) : 0;
            ::new (words + i * sizeof(std::uint64_t)) std::atomic<std::uint64_t>(held);
        }
        ctx.tenancy.store(grown, std::memory_order_release);
        t = grown;
    }
    t->word(slot).store(bits, std::memory_order_release);
    return true;
}

session_anchor_id_t fwd_router_t::session_anchor_id(std::string_view mount,
                                                    std::string_view peer) noexcept {
    session_anchor_id_t id;
    // Keyed by the SLOT, not the session: one slot keeps one anchor however many sessions
    // pass through it (ADR-0044 amendment), and the anchor's own generation tells them apart.
    peer = session_slot_of(peer);
    // Over one segment record ⇒ EMPTY: no anchor key could hold it (see the declaration).
    if (mount.size() + peer.size() + 2 > id.bytes.size()) return id;
    char* out = id.bytes.data();
    *out++ = ':';
    out = std::copy(mount.begin(), mount.end(), out);
    *out++ = '/';
    out = std::copy(peer.begin(), peer.end(), out);
    id.len = static_cast<std::size_t>(out - id.bytes.data());
    return id;
}

void fwd_router_t::bus_peer_up(const child_rx_ctx_t& ctx, peer_handle_t handle,
                               std::string_view peer) {
    // A tombstoned ctx names no live child (#884), so a late arrival from a transport whose
    // mount was already removed anchors nothing — the same skip every name-keyed consumer
    // makes. Read `retired` the way the frame paths do.
    if (ctx.retired.load(std::memory_order_acquire)) return;
    const session_anchor_id_t id = session_anchor_id(ctx.name, peer);
    if (id.len == 0) return;  // too long for one key record: anchors nothing
    // An arrival is the moment the SLOT changes hands (#1609), and the new session has sent
    // nothing yet — so a tenancy the slot's word still holds is a predecessor's whose
    // departure never reached this router. Its departure runs now under exactly the handle
    // and name it held (#2234): no generation arithmetic, which a slot that consumed a
    // generation without an arrival would get wrong. That departure retires the anchor,
    // evicts the edges and awaits filed under the predecessor's name, clears the
    // word and drops the slot's cached token. An empty word (the departure already ran, or
    // the slot was never anchored) has nothing to reclaim.
    if (const peer_handle_t prev = session_tenant(ctx, handle.index); prev.valid()) {
        std::array<char, kPeerNameChars> scratch{};
        bus_peer_down(ctx, prev, resolve_peer_name(*ctx.entry, prev, scratch));
    }
    // The word is published BEFORE the anchor revives at its new generation, so a reader that
    // loads it and then finds the anchor at the generation it was minted against has read the
    // tenancy that generation belongs to. A refused table anchors nothing; the anchor is never
    // live here, so PATH_IN_USE cannot answer, and a refused block is discarded by value too.
    const std::lock_guard lock(tenancy_m_);
    if (set_session_tenant_locked(ctx, handle.index, handle.bits()))
        (void)graph_.register_session_anchor(id);
}

void fwd_router_t::bus_peer_down(const child_rx_ctx_t& ctx, peer_handle_t handle,
                                 std::string_view peer) {
    // A departure leaves the slot's anchor, word and cached token alone when the word holds
    // ANOTHER session (#2234): a late departure that lands after a successor took the slot,
    // including one still pending authentication, finds the successor's handle there. An
    // empty word (never anchored, or already departed) still drops the token.
    bool holds_slot = true;
    {
        const std::lock_guard lock(tenancy_m_);
        const peer_handle_t tenant = session_tenant(ctx, handle.index);
        holds_slot = !tenant.valid() || tenant == handle;
        // Retire BEFORE the eviction, and before the word clears. Retirement is what bumps
        // the saturating generation (RFC-0024 §4.4 rule 3), and the slot this session sat in
        // can be handed to the next dialer the moment the poll thread returns from here — so
        // the stamp has to have moved by then, or the successor revives at a generation an
        // element minted against the DEAD session still matches.
        if (tenant.valid() && tenant == handle) {
            if (const std::optional<graph::vertex_handle_t> anchor =
                    graph_.find_session_anchor(session_anchor_id(ctx.name, peer)))
                (void)graph_.retire(*anchor);
            (void)set_session_tenant_locked(ctx, handle.index, 0);
        }
    }
    link_gone(peer, {.conn = ctx.entry->conn_slot, .peer = handle});
    // The eviction inside `link_down` RELEASED this peer's index slot, so the token cached
    // for it names a stamp that can never validate again (#1417). Dropping it is what lets
    // the next session in this slot mint a fresh one on its first subscribe instead of
    // asking with a dead stamp for the rest of its life. Ordered AFTER the eviction so no
    // window can refill the entry from a frame still in flight for the departing session.
    bus_token_cache_t* const cache =
        holds_slot ? ctx.peer_tokens.load(std::memory_order_acquire) : nullptr;
    if (cache != nullptr) cache->drop(handle);
}

// --- bound paths (RFC-0024) -------------------------------------------------

fwd_router_t::child_rx_ctx_t* fwd_router_t::ctl_ctx_by_name(std::string_view name) {
    // The chain read WITHOUT the tombstone test: this is the control plane, it holds `ctl_m_`
    // (the only writer of the links), and it must see a TOMBSTONE — which is exactly what the
    // published walks refuse to return.
    for (child_rx_ctx_t* c = rx_head_.load(std::memory_order_relaxed); c != nullptr;
         c = c->next.load(std::memory_order_relaxed))
        if (c->name == name) return c;
    return nullptr;
}

fwd_router_t::child_rx_ctx_t* fwd_router_t::acquire_ctx(std::string_view name) {
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
        // The interned LINK TOKEN goes with the tenancy (#1417), both tiers. A token is an
        // index-slot address, and the departing tenancy's `link_down` released the slot it
        // named — so carrying one across a re-add would hand the new tenancy a stamp that
        // can never validate. Rewound to "not minted yet"; the next subscribe mints afresh.
        hit->link_token.store(0, std::memory_order_relaxed);
        if (hit->peer_tokens_own != nullptr) hit->peer_tokens_own->clear();
        // `entry` is NOT rewritten, because it cannot have changed: a registry slot is keyed
        // by name and reused for that name forever (`erase` tombstones in place, `add`
        // rebinds the tombstone), and `add_child` has already re-added by the time we run.
        // The assert is where that coupling is pinned rather than assumed.
        assert(hit->entry == registry_.entry_by_name(name));
        // `name` and `mount_tlv` are immutable after the first publish, for the reason
        // `child_registry_t::add` gives about its own `mount_tlv`: a reader on a receive
        // thread may be holding either right now, and the bytes a rewrite would store are
        // identical anyway — the mount run is a pure function of the name.
        return hit;
    }
    // A fresh node: one block from the link plane's source (#1779). It VIEWS its registry
    // slot's name and mount run rather than copying them — the slot is keyed by this name for
    // the registry's life and its text never changes, which is the property `entry` relies on.
    const child_registry_t::child_t* const entry = registry_.entry_by_name(name);
    void* const raw = link_src_->try_alloc(sizeof(child_rx_ctx_t), alignof(child_rx_ctx_t));
    if (raw == nullptr || entry == nullptr) {
        if (raw != nullptr)
            link_src_->release(raw, sizeof(child_rx_ctx_t), alignof(child_rx_ctx_t));
        return nullptr;
    }
    child_rx_ctx_t& fresh = *::new (raw) child_rx_ctx_t(this, entry->name, entry->mount_tlv);
    fresh.entry = entry;
    // Linked NOW, still retired: a reader that reaches it skips it, and `publish_ctx`'s
    // release-clear is its publication edge — the same one a revived tombstone takes.
    fresh.retired.store(true, std::memory_order_relaxed);
    if (rx_tail_ == nullptr) {
        rx_head_.store(&fresh, std::memory_order_release);
    } else {
        rx_tail_->next.store(&fresh, std::memory_order_release);
    }
    rx_tail_ = &fresh;
    return &fresh;
}

std::optional<const link_kind_t*> fwd_router_t::intern_kind(link_kind_t kind) {
    if (kind.kind.empty()) return nullptr;
    for (const kind_rec_t* r = kinds_; r != nullptr; r = r->next) {
        if (r->view.is(kind.kind, kind.role)) return &r->view;
    }
    // A miss appends, at most once per distinct pair — the same control-plane allocation
    // `acquire_ctx` makes for a fresh name, and bounded by the catalog, not by traffic. One
    // block: the record, then its `kind` bytes (#1779).
    void* const raw =
        link_src_->try_alloc(sizeof(kind_rec_t) + kind.kind.size(), alignof(kind_rec_t));
    if (raw == nullptr) return std::nullopt;
    auto* const text = static_cast<char*>(raw) + sizeof(kind_rec_t);
    std::memcpy(text, kind.kind.data(), kind.kind.size());
    kinds_ = ::new (raw) kind_rec_t{
        .view = link_kind_t{.kind = std::string_view(text, kind.kind.size()), .role = kind.role},
        .next = kinds_};
    return &kinds_->view;
}

void fwd_router_t::publish_ctx(child_rx_ctx_t& ctx) noexcept {
    // Every node is already on the chain (`acquire_ctx` links a fresh one retired), so
    // clearing the flag IS the publish, and a reader that reaches the node sees a fully built
    // one. Fresh and revived nodes take the same edge.
    ctx.retired.store(false, std::memory_order_release);
}

const fwd_router_t::child_rx_ctx_t* fwd_router_t::ctx_by_name(std::string_view link_name) const {
    // The tombstone test comes FIRST and acquires: it is the edge that orders this node's
    // `rx` against the control thread's rewrite, so reading them before it would
    // be reading a tenancy that may already be half-replaced (#884).
    for (const child_rx_ctx_t* c = rx_head_.load(std::memory_order_acquire); c != nullptr;
         c = c->next.load(std::memory_order_acquire))
        if (!c->retired.load(std::memory_order_acquire) && c->name == link_name) return c;
    return nullptr;
}

std::optional<wire::path_ref_element_t> fwd_router_t::connection_ref(
    std::string_view link_name) const {
    // The origin's element for its first hop is the forwarder's for its arrival link: the
    // same door, read the same way. The walk skips tombstones (#884), so a removed child has
    // no reference to give and a re-added one gives its CURRENT tenancy's.
    return hop_mint(link_name, nullptr);
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
    // route through a link this node no longer has (#884). A bus mount's door is no egress
    // (§10), so it mints nothing and the hop contributes its NAME run instead (§6.2).
    if (ctx == nullptr || ctx->retired.load(std::memory_order_acquire) ||
        ctx->entry->egress().multi_peer)
        return std::nullopt;
    const std::optional<graph::vertex_slot_t> slot = graph_.vertex_slot_at(ctx->entry->conn_slot);
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
    // Minted only for the tenancy the slot's word holds (#2234): a frame still in flight
    // from a departed session must not name its successor's anchor. The word is stored only
    // after the retire that moved the generation, so a word naming this session means the
    // generation just read is its.
    const peer_handle_t tenant =
        anchor_tenant(inbound_ctx, inbound_name, *anchor, slot->generation);
    std::array<char, kPeerNameChars> scratch{};
    if (!tenant.valid() || resolve_peer_name(*inbound_ctx->entry, tenant, scratch) != inbound_name)
        return std::nullopt;
    return wire::path_ref_element_t{.index = slot->index, .generation = slot->generation};
}

transport_t* fwd_router_t::bound_egress(wire::path_ref_element_t e, std::string_view caller,
                                        std::optional<graph::acl_right_t> right) const {
    // §5.1 in order: bounds and generation (inside `deref_vertex_slot`, which also refuses a
    // saturated element), then the door the vertex is and its ONE gate. The authorization
    // comes last and is never skipped — a generation match authorizes nothing (§6.2).
    if (!graph_.deref_vertex_slot(e.index, e.generation)) return nullptr;
    const child_registry_t::child_t* const door = registry_.by_conn_slot(e.index);
    if (door == nullptr || !door_allows(door, caller, right)) return nullptr;
    // The link and its shape from ONE word (#882). A bus mount's door is never an egress:
    // its `send()` broadcasts (§10, ADR-0073 §3).
    const child_registry_t::egress_t eg = door->egress();
    return eg.multi_peer ? nullptr : eg.link;  // null ⇒ tombstoned ⇒ drop
}

bool fwd_router_t::adopt_binding(graph::path_t& path, std::string_view link_name,
                                 const wire::tlv_node_t& reply) {
    // The reply's `src` IS the forward route from the first link onward (RFC-0029 §6.2): one
    // element per host, each prepended by the host that relayed or issued the reply. Children
    // in order: op, dst, src, kind, so the third child is `src` and the fourth says whether
    // the operation succeeded — a refusal is not a route worth caching.
    std::optional<wire::tlv_node_t> src;
    bool result = false;
    std::size_t at = 0;
    for (const wire::tlv_node_t c : reply.children()) {
        if (at == 2) src = c;
        if (at == 3)
            result = c.payload().size() == 1 &&
                     c.payload()[0] ==
                         static_cast<std::byte>(std::to_underlying(graph::reply_kind_t::RESULT));
        ++at;
    }
    if (!src || !result || src->type() != type_t::PATH || src->payload().empty()) return false;
    // Element 0 is this node's own, and nobody else could have written it: the hop out of the
    // origin is the one hop no peer ever sees.
    const std::optional<wire::path_ref_element_t> own = connection_ref(link_name);
    if (!own) return false;
    // Scratch from the link plane's source (#1779): a refusal leaves the path canonical, the
    // same non-event a reply without a usable `src` is.
    const std::span<const std::byte> rest = src->payload();
    mem::block_array_t<std::byte> chain(*link_src_);
    if (!chain.reserve(wire::kPathPairRecordBytes + rest.size())) return false;
    for (std::size_t i = 0; i < wire::kPathPairRecordBytes; ++i) (void)chain.push_slot();
    wire::path_pair_store(std::span<std::byte>(chain.data(), wire::kPathPairRecordBytes)
                              .first<wire::kPathPairRecordBytes>(),
                          *own);
    for (const std::byte b : rest) (void)chain.push_back(b);  // reserved above: cannot grow
    return path.bind(std::span<const std::byte>(chain.data(), chain.size()));
}

bool fwd_router_t::bound_send(const graph::path_t& path, graph::acl_right_t right, egress_fn_t send,
                              void* ctx) const {
    const graph::path_binding_t& b = path.binding();
    const std::optional<wire::path_pair_t> e0 =
        b.bound ? wire::path_pair_at(b.chain, 0) : std::nullopt;
    if (!e0) return false;
    // Opened before the link is looked up and held until `send` returns, so a link removed
    // meanwhile outlives the call (see `frame_scope_t`).
    const frame_scope_t scope;
    // The origin's caller is local, so the subject context is empty — the trusted-caller
    // convention every other local API here uses. The check still runs: it is the same one
    // line a forwarder runs, and having ONE of them is what keeps the two from drifting.
    transport_t* const link = bound_egress(*e0, {}, right);
    if (link == nullptr) return false;
    // The residual — every element after the origin's own — as the `dst` `PATH` (RFC-0029
    // §4.2), written straight into a block of exactly its size from the link plane's source
    // (#1779); a refusal is the same `false` a stale element is. `bind` capped the chain at a
    // 16-bit body, so the short header always fits.
    const auto residual = std::span<const std::byte>(b.chain).subspan(wire::kPathPairRecordBytes);
    mem::block_array_t<std::byte> dst(*link_src_);
    const std::size_t bytes = wire::header_bytes(wire::opt_t{}) + residual.size();
    if (!dst.reserve(bytes)) return false;
    for (std::size_t i = 0; i < bytes; ++i) (void)dst.push_slot();  // reserved: cannot grow
    const std::span<std::byte> w(dst.data(), bytes);
    wire::store_header(w, wire::type_t::PATH, wire::opt_t{}, residual.size());
    std::memcpy(w.data() + wire::header_bytes(wire::opt_t{}), residual.data(), residual.size());
    send(ctx, *link, w);
    return true;
}

std::optional<graph::acl_right_t> fwd_router_t::fwd_op_right(std::uint8_t op_byte) noexcept {
    // Masked (RFC-0024 §9.3): bits 7-6 are flags, never part of the opcode. AWAIT reads, so it
    // asks for READ. A REPLY carries no right of its own — it answers an operation already
    // authorized on the way in — and an opcode this build cannot name has no right to evaluate:
    // guessing one is how a write-like future opcode would cross a READ-only gate.
    switch (static_cast<fwd_op_t>(op_byte & graph::kFwdOpcodeMask)) {
        case fwd_op_t::READ:
        case fwd_op_t::AWAIT:
            return graph::acl_right_t::READ;
        case fwd_op_t::WRITE:
            return graph::acl_right_t::WRITE;
        default:
            return std::nullopt;
    }
}

template <class Cursor, class Reject>
fwd_router_t::head_dst_t fwd_router_t::route_pair_forward(std::string_view inbound_name,
                                                          const child_rx_ctx_t* inbound_ctx,
                                                          bool from_peer, const Cursor& cur,
                                                          const fwd_pre_t& pre,
                                                          std::uint8_t op_byte, Reject&& reject,
                                                          wire::path_pair_t& out_target) {
    // The head element, read off the window the peek already opened. Only the HEAD can be this
    // hop's (RFC-0029 §4.2: element k is read by node k and by no other), so nothing past it is
    // looked at. The peek answered `PATH_ESCAPE`, so the head IS an escape record (`00` first)
    // whose three framing bytes and declared payload all lie inside the `dst` (`read_packed_seg`
    // bounds both): a head that frames as `00 16 08` therefore holds all 11 bytes, and needs no
    // second length check here. A record that is not `00 16 08` is not a PAIR and passes
    // untouched: the terminus's own refusal of an escape-carrying `dst` answers every other
    // shape (`tr::path::invalid`).
    const std::size_t head = pre.dst_body_off;
    if (!wire::path_pair_record_valid(static_cast<std::uint8_t>(cur.byte_at(head + 1)),
                                      cur.byte_at(head + 2)))
        return head_dst_t::PASS;
    // The two u32 fields through the cursor's own little-endian load: on the rope tier the
    // element may straddle a link, and `load_le` needs no stitch slot and no flatten.
    const std::size_t at = head + wire::kPackedEscapeOverhead;
    const wire::path_pair_t pair{.index = static_cast<std::uint32_t>(cur.load_le(at, 4)),
                                 .generation = static_cast<std::uint32_t>(cur.load_le(at + 4, 4))};
    const std::size_t head_end = head + wire::kPathPairRecordBytes;
    const bool last = head_end == pre.dst_end;

    // From here the address IS pair-spelled and §6 governs every exit. No refusal repairs: no
    // re-resolution, no nearest match, no fall-through to the canonical walk — the pair
    // REPLACED the name bytes, so there is nothing left to walk (§6 step 2).
    //
    // An EMPTY op VALUE reads as the masked opcode 63 (`route_fwd_ingress`), which this build
    // does not define: it is not dropped here but takes the one disposition the NAME spelling
    // gives it (#870) — resolved, and answered, by the terminus — and, naming no right, it
    // crosses no hop on a graph that enforces an ACL.
    const auto op = static_cast<fwd_op_t>(op_byte & graph::kFwdOpcodeMask);
    // §6.1: a reply's `dst` is the request's canonical return route, so a PAIR never heads one.
    // Dropped rather than answered — a reply is never answered with a reply.
    if (op == fwd_op_t::REPLY) {
        count_drop(malformed_rx_);
        return head_dst_t::HANDLED;
    }

    // §6 step 2: bounds, generation (a saturated one refused), registered — all three inside
    // `deref_vertex_slot`. Any of them failing is `NOT_FOUND`, the one answer RFC-0024 §5.3 and
    // RFC-0027 §7.2 now share; the origin's recovery is the canonical string it still holds.
    const std::optional<graph::vertex_handle_t> v =
        graph_.deref_vertex_slot(pair.index, pair.generation);
    if (!v) {
        reject(graph::status_t::NOT_FOUND, pre);
        return head_dst_t::HANDLED;
    }

    // §6 step 3 — WHAT the vertex is decides what happens next, and nothing else does. Both
    // forwarding outcomes end in one EGRESS: the link the hop leaves on, or the session the
    // delivery's last element names. Every way either can refuse is `NOT_FOUND`.
    //
    // A WRITE that names an accepted session's ANCHOR is a chain delivery's hop into that
    // session (RFC-0029 §7.1), wherever the element sits: last when the origin's own `src` was
    // empty, followed by that `src`'s residual NAMEs otherwise (the reverse list a hop creates
    // starts with it, #2245). The disclosure fix is the deref above — a dead session's element
    // carries the retired generation and refuses. An anchor lives OUTSIDE the path tree and has
    // no `:acl` ancestry of its own; the delivery crosses the mount exactly as the NAME spelling
    // `<mount>/<peer>` does, so it is gated by the ONE `door_allows` at the same door — failing
    // closed on a door whose vertex has retired. A denial stays the shipped plain drop (the
    // anti-enumeration rule on the delivery leg); a session gone between the deref and here is
    // `NOT_FOUND`, below.
    const std::optional<graph::graph_t::session_anchor_route_t> ar =
        op == fwd_op_t::WRITE ? graph_.session_anchor_route(*v) : std::nullopt;
    transport_t* egress = nullptr;
    if (ar) {
        const child_rx_ctx_t* const mount = ctx_by_name(ar->mount);
        const child_registry_t::child_t* const door = mount != nullptr ? mount->entry : nullptr;
        if (!door_allows(door, inbound_name, graph::acl_right_t::WRITE)) return head_dst_t::HANDLED;
        // The session is the one the slot's word names, read BEFORE the anchor's generation is
        // checked again (#2234): a successor's word is published only after the retire that
        // moved the generation, so a word read here that is not the minted tenancy's fails
        // the re-check, and so does a session that departed since. The delivery never reaches
        // a tenancy the element was not minted for.
        const peer_handle_t tenant = anchor_tenant(mount, ar->peer, *v, pair.generation);
        egress = tenant.valid() ? child_registry_t::resolve_peer(*door, tenant) : nullptr;
    } else if (!last) {
        // A door is a HOP: `bound_egress` runs the ONE gate the NAME spelling runs
        // (`door_allows`, §6.4) and refuses a shared (bus) mount's door as an egress,
        // `NOT_FOUND` (§10). A tail below a vertex that is no door names nothing below itself,
        // `INVALID_PATH` (§6 step 3) — asked only once the hop has refused, so a forwarded
        // hop scans the registry once.
        egress = bound_egress(pair, inbound_name, fwd_op_right(op_byte));
        if (egress == nullptr && registry_.by_conn_slot(pair.index) == nullptr) {
            reject(graph::status_t::INVALID_PATH, pre);
            return head_dst_t::HANDLED;
        }
    } else {
        // The TERMINUS: an ordinary vertex, or a point-to-point connection vertex named last,
        // which addresses its own `:`-facets (RFC-0004 §A's dual nature), gets the op applied
        // by the same `apply_op` the NAME spelling reaches — the op's gate is the resolver's
        // `graph_t` call at that vertex, reused, not restated.
        out_target = pair;
        return head_dst_t::TERMINUS;
    }
    if (egress == nullptr) {
        reject(graph::status_t::NOT_FOUND, pre);
        return head_dst_t::HANDLED;
    }
    // Consume exactly the one element and forward the tail through the ONE rebuild locus, `src`
    // grown canonically by the inbound mount run (RFC-0029 §6.1): the rebuild consumes
    // `[dst_body_off, strip_at)`, the head record and nothing else. When the session was the
    // last element, the peer receives an EMPTY `PATH` `dst` — the canonical delivery an origin
    // always got — and otherwise the origin's own `src` residual.
    fwd_pre_t hop = pre;
    hop.strip_at = head_end;
    route_fwd_forward(inbound_name, inbound_ctx, from_peer, cur, *egress, hop);
    return head_dst_t::HANDLED;
}

void fwd_router_t::on_frame(std::string_view inbound_name, std::span<const std::byte> frame) {
    const frame_scope_t scope;
    on_frame_impl(inbound_name, frame, nullptr);
}

std::string_view fwd_router_t::resolve_peer_name(const child_registry_t::child_t& door,
                                                 peer_handle_t peer, std::span<char> scratch) {
    // The bus facet is the link's (#1941). Only a bus mount's ctx is wired to the peer
    // receivers that call this, and only a bus mount's edges store a peer, so a door with no
    // bus facet is a flat link — whose pair names the link itself — or a build with the bus
    // module closed out, which wires no peer receiver. Either way the name is the door's own.
    bus_link_t* const bus = bus_of(*door.bound().link);
    return bus != nullptr ? bus->peer_name(peer, scratch) : std::string_view(door.name);
}

peer_handle_t fwd_router_t::terminus_peer(const child_rx_ctx_t* ctx, peer_handle_t peer) noexcept {
    // Tagged by the bus seam already, or handed in with no receive context (the public
    // `on_frame` entries): either way there is no link to ask.
    if (peer.valid() || ctx == nullptr) return peer;
    // The FLAT arm. The link has one routing identity for every peer it carries — that is
    // what `peer_named=false` means — so the frame arrived untagged and the only place the
    // writer's identity still exists is inside the link, which is delivering it right now.
    // Asked HERE, at the point of consumption, and on the transport's own receive thread:
    // this call is meaningful only inside the delivery callback we are standing in. The
    // router keeps no copy of the link (#1941): it is read off the registry slot's binding,
    // tombstoned or not (`child_registry_t::child_t::bound`), because the frame is inside the
    // link's receive callback and the link is alive while it runs.
    return ctx->entry->bound().link->inbound_peer();
}

std::string_view fwd_router_t::peer_subject_of(const child_rx_ctx_t* ctx, peer_handle_t peer,
                                               std::span<char> scratch) {
    if (!peer.valid() || ctx == nullptr) return {};
    // Asked of the link the frame is inside, as `terminus_peer` asks it (#1941).
    return ctx->entry->bound().link->peer_subject(peer, scratch);
}

graph::carried_link_t fwd_router_t::link_id_of(void* ctx, const graph::inbound_ref_t& inbound) {
    // Unlike `peer_subject_thunk`, this supplier DOES need the router: the lazy mint calls
    // `graph_t::intern_link`, and the graph is the router's, not the receive context's. A
    // resolve with no receive context (a direct embedder) names its link only by NAME: it
    // carries no token, and the cold scan finds the pair (#1941).
    auto* const self = static_cast<fwd_router_t*>(ctx);
    const auto* const rx = static_cast<const child_rx_ctx_t*>(inbound.origin);
    if (rx == nullptr) return {.link = self->inbound_pair(nullptr, inbound.link, {})};
    const std::uint32_t conn = rx->entry->conn_slot;  // the pair's slot (#1941)
    // The BUS tier. `peer_tokens` is non-null exactly when this ctx was registered as a bus
    // mount, so the facet test IS the tier test — no kind check, no branch on the handle.
    if (bus_token_cache_t* const cache = rx->peer_tokens.load(std::memory_order_acquire)) {
        const graph::link_pair_t at{.conn = conn, .peer = inbound.peer};
        if (const graph::link_id_t hit = cache->peek(inbound.peer); hit.valid()) return {hit, at};
        // The census-bus fallback, and it is load-bearing (see the header): a bus that never
        // announces arrivals has no eagerly-filled entry to hit, so the mint happens HERE —
        // once per peer, on the subscribe path, never on the frame path.
        const graph::link_id_t fresh = self->graph_.intern_link(inbound.link);
        if (fresh.valid()) cache->put(inbound.peer, fresh);
        return {fresh, at};
    }
    // The FLAT tier: one link, one name, one word — and a pair with no peer in it.
    if (const std::uint64_t bits = rx->link_token.load(std::memory_order_relaxed); bits != 0)
        return {graph::link_id_t::from_bits(bits), {.conn = conn}};
    // An empty name mints nothing (`link_index_t`'s #1056 empty-key rule), so neither tier
    // tests for one. A refused mint answers the invalid id, whose bits are this word's own
    // "no token" (0), so it is stored as is: the next subscribe asks again.
    const graph::link_id_t fresh = self->graph_.intern_link(inbound.link);
    rx->link_token.store(fresh.bits(), std::memory_order_relaxed);
    return {fresh, {.conn = conn}};
}

std::string_view fwd_router_t::link_name_of(graph::link_pair_t link,
                                            std::span<char> scratch) const {
    // Off the delivery path (#1941): the session seam's RELEASE and the link-hold count ask,
    // once per edge. A flat link's name is its slot's; a peer's is its session name on the bus.
    // Bracketed (#2087): an application thread reaches here (a retire, an API unsubscribe,
    // `subscribe_toward`) with no frame open, and the bus facet is read off the link itself,
    // which a concurrent removal may park and `collect()` free. The answer outlives the
    // bracket safely: a slot's name is the registry's, a peer's is formatted into `scratch`.
    const frame_scope_t scope;
    const child_registry_t::child_t* const door = registry_.by_conn_slot(link.conn);
    return door != nullptr ? resolve_peer_name(*door, link.peer, scratch) : std::string_view{};
}

std::string_view fwd_router_t::peer_subject_thunk(void* ctx, const graph::inbound_ref_t& inbound,
                                                  std::span<char> scratch) {
    // `ctx` is the router and is unused: everything the derivation needs travels on the call
    // — the handle, and the receive context the terminus stashed in the opaque `origin`. The
    // {fn, ctx} shape is kept because that is the seam's shape (ADR-0047), not because this
    // supplier has state.
    (void)ctx;
    return peer_subject_of(static_cast<const child_rx_ctx_t*>(inbound.origin), inbound.peer,
                           scratch);
}

bool fwd_router_t::sample_stats(std::string_view seam_class, std::string_view seam_name,
                                graph::stats_block_t* out) const noexcept {
    if (seam_class == "router") {
        if (seam_name != "drops") return false;
        if (out == nullptr) return true;  // the recognition probe: sample nothing
        const router_stats_t s = drop_stats();
        out->add("flatten_dropped", s.flatten_dropped);
        out->add("forward_iov_dropped", s.forward_iov_dropped);
        out->add("arena_dropped", s.arena_dropped);
        out->add("assemble_dropped", s.assemble_dropped);
        out->add("reply_iov_dropped", s.reply_iov_dropped);
        out->add("delivery_iov_dropped", s.delivery_iov_dropped);
        out->add("malformed_rx", s.malformed_rx);
        out->add("retired_rx", s.retired_rx);
        return true;
    }
    if (seam_class != "link") return false;
    // A LIVE registered child, by the same NAME its `/net/<module>/<name>` connection vertex
    // carries. A tombstone (`erase`d link) is deliberately NOT a seam: a removed link's
    // sub-key must answer SCHEMA_NOT_FOUND, not a frozen block that looks live.
    const child_registry_t::child_t* const entry = registry_.entry_by_name(seam_name);
    if (entry == nullptr || !entry->live()) return false;
    if (out == nullptr) return true;
    const transport_t* const link = entry->link();
    const transport_drop_stats_t d = link->drop_stats();
    out->add("dropped_rx", d.dropped_rx);
    out->add("malformed_rx", d.malformed_rx);
    out->add("dropped_tx", d.dropped_tx);
    link->kind_stats(*out);  // the kind's own nouns, if any (RFC-0033 §5.11)
    // A seam names only the nouns it HAS (RFC-0010 Am. 1 §D.3).
    return true;
}

bool fwd_router_t::stats_sampler_thunk(void* ctx, std::string_view seam_class,
                                       std::string_view seam_name, graph::stats_block_t* out) {
    return static_cast<const fwd_router_t*>(ctx)->sample_stats(seam_class, seam_name, out);
}

void fwd_router_t::on_frame_bus(const child_rx_ctx_t& ctx, peer_handle_t peer,
                                std::span<const std::byte> frame) {
    // The handle is the seam's identity; the routing plane's is the NAME, so it is resolved
    // here, once, on the transport's own receive thread — the scratch outlives the whole
    // routing call below, which is what the resolved view may point into.
    std::array<char, kPeerNameChars> scratch{};
    const std::string_view name = resolve_peer_name(*ctx.entry, peer, scratch);
    if (name.empty()) return;  // no bus facet, or a handle this kind does not name
    on_frame_impl(name, frame, nullptr, &ctx, true, peer);
}

void fwd_router_t::on_frame_rope(std::string_view inbound_name, view::rope_t frame) {
    const frame_scope_t scope;
    on_frame_rope_impl(inbound_name, std::move(frame), nullptr, false);
}

void fwd_router_t::on_frame_rope_bus(const child_rx_ctx_t& ctx, peer_handle_t peer,
                                     view::rope_t frame) {
    std::array<char, kPeerNameChars> scratch{};
    const std::string_view name = resolve_peer_name(*ctx.entry, peer, scratch);
    if (name.empty()) return;  // see on_frame_bus
    on_frame_rope_impl(name, std::move(frame), &ctx, true, peer);
}

bool fwd_router_t::door_allows(const child_registry_t::child_t* door, std::string_view caller,
                               std::optional<graph::acl_right_t> right) const {
    // One relaxed load on a graph that enforces no ACL, and nothing else.
    if (door == nullptr || !graph_.acl_enforced()) return true;
    // The door's connection vertex, by the slot `add_child` recorded: a bounds check, a load
    // and the registration test, lock-free (`registered_vertex_at`, #1939). Enforcing, a door
    // whose vertex has retired has nothing to grant the right and refuses (fail closed), as
    // does an op this build names no right for.
    const std::optional<graph::vertex_handle_t> v =
        right ? graph_.registered_vertex_at(door->conn_slot) : std::nullopt;
    // THE `allows` of the router — every spelling's hop is decided here (RFC-0029 §6.4). It
    // refuses a vertex that retired while the gate ran, so a door is never crossed
    // mid-retire (#1940).
    return v && graph_.allows(*v, caller, *right);
}

// `always_inline`, and the reason is measured: each of its two callers (`on_frame_impl`,
// `on_frame_rope_impl`) is its only call site for that cursor, and GCC's choice to inline it
// there is a unit-wide budget decision, not a local one. When #2124 shrank every frame bracket
// in this TU by the guard check and `__cxa_thread_atexit` registration it no longer needs,
// the freed budget went elsewhere and the span instance fell out of `on_frame_impl` as an
// 8 KB function of its own, one more call on every received frame. The attribute keeps the
// receive path's shape off that budget.
template <class Cursor, class Observe, class Reject, class Terminus, class Reply>
[[gnu::always_inline]] inline bool fwd_router_t::route_fwd_ingress(
    std::string_view inbound_name, const Cursor& cur, const child_rx_ctx_t* inbound_ctx,
    bool from_peer, Observe&& observe, Reject&& reject, Terminus&& terminus, Reply&& reply) {
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
    // A frame from a REMOVED child is dropped here, before any arm can forward or apply it
    // (#2302). Two doors reach a removed child: its transport may still deliver through the
    // receive ctx `add_child` installed, which `remove_child` tombstoned (#884), and an SDK host
    // may still call the by-name door with its name. Left alone, the first went out with the
    // dead child's mount run in `src`, and the second fell into the fallback for a name never
    // registered, growing `src` by the whole name packed as ONE segment. The reply side already
    // answers a tombstoned ctx "not at all" (`reply_link`); this is the request side catching
    // up. A frame with a ctx pays one acquire load; only the ctx-less door asks the registry.
    if (inbound_ctx != nullptr ? inbound_ctx->retired.load(std::memory_order_acquire)
                               : registry_.tombstoned(inbound_name))
        return true;
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
    const fwd_dst_kind_t kind = peek_fwd_dst_any(cur, pre);
    // The op the peek located, read once for every arm: an EMPTY op VALUE reads as the masked
    // opcode 63, which no build defines (#870). It is located for every `dst` form, including
    // the ones the peek refused (it fills the op fields first), which is what lets the REPLY
    // test below skip a second parse of the FWD and op headers (#1794).
    const std::uint8_t op_byte = pre.op_body_len != 0
                                     ? static_cast<std::uint8_t>(cur.byte_at(pre.op_body_off))
                                     : graph::kFwdOpcodeMask;
    const auto op = static_cast<fwd_op_t>(op_byte & graph::kFwdOpcodeMask);
    // RFC-0029 §5.3: `PATH_REF` (`0x14`) is retired as an address form. Its element survives
    // as the PAIR inside `PATH` (below); the bare fixed-stride array is refused as a `dst`,
    // addressed, exactly as a malformed address is — never routed, never applied.
    if (kind == fwd_dst_kind_t::RETIRED) {
        reject(graph::status_t::INVALID_PATH, pre);
        return true;
    }
    // An escape-headed `dst` — the head element is not a NAME. RFC-0029 §6's PAIR arm, BESIDE
    // the mount descent and ahead of it: folding a digest chain over an escape record would
    // read somebody's slot index as UTF-8. Gated on the peek's verdict, so a NAME-headed `dst`
    // — every string route — pays one not-taken compare and reaches `resolve_mount_at` as
    // before. Any other escape head (RFC-0027's retired `len = 4` label among them) passes,
    // and the terminus refuses it as a malformed address (RFC-0029 §5.1, §5.3).
    if (kind == fwd_dst_kind_t::PATH_ESCAPE) {
        wire::path_pair_t head_target{};
        const head_dst_t d = route_pair_forward(inbound_name, inbound_ctx, from_peer, cur, pre,
                                                op_byte, reject, head_target);
        // TERMINUS: the head element dereferenced to a LOCAL vertex and was the last one, so
        // this node is the terminus and the frame stops here. It does NOT pass through the
        // REPLY test at the bottom of this driver, and must not: the arm already refused a
        // REPLY (a reply's `dst` is canonical, RFC-0029 §6.1), so what reaches here is a
        // request resolved against the element rather than against a name nothing spelled.
        // HANDLED stops here too; only PASS goes on to the canonical path.
        if (d == head_dst_t::TERMINUS) terminus(&head_target);
        if (d != head_dst_t::PASS) return true;
    }
    {
        // The reader outlives the forward hop on purpose: a resolved bus PEER name is a view
        // into its retained stitch slot and is referenced right through the egress `gather`.
        seg_reader_t<Cursor> rd{cur};
        // Resolve the mount prefix (ADR-0061 strip-K, single-pass since #523). Segments are
        // walked lazily and read in place when the source keeps them contiguous, stitched
        // into the reader's slot when they straddle a rope link; an over-long segment is not
        // routable ⇒ fall to the terminus.
        mount_hit_t hit = kind == fwd_dst_kind_t::PATH ? resolve_mount_at(registry_, cur, rd, pre)
                                                       : mount_hit_t{};
        // A REPLY is routed, never authorized: it answers an operation every gate passed on
        // the way in (RFC-0004 §B), and one whose `dst` names a mount exactly leaves over it
        // (#2042). Any other hop is authorized at the door the descent reached, through the ONE
        // gate every spelling runs (`door_allows`), so a hop's verdict never depends on how it
        // was spelled. A refused hop leaves as a rejected hit.
        if (op == fwd_op_t::REPLY)
            hit.take_reply_egress();
        else
            hit.refuse_unless(door_allows(hit.mount, inbound_name, fwd_op_right(op_byte)));
        if (hit.link != nullptr) {
            route_fwd_forward(inbound_name, inbound_ctx, from_peer, cur, *hit.link, pre);
            return true;
        }
        // A bus NAME + residual (never broadcast, never terminus), or a hop its mount refuses.
        if (hit.rejected) {
            reject(hit.refusal, pre);
            return true;
        }
    }
    // The `dst` names no mount here and the PAIR arm did not take it ⇒ this node is its
    // terminus. The `PATH` arm above is the mount descent's gate, not a frame classifier: it
    // says "this frame has an address this node can descend", and an escape-headed `dst` the
    // arms passed has no NAME to descend on — the terminus refuses it as a malformed address.
    // A REPLY whose accumulated return route is consumed has reached its originator.
    if (op == fwd_op_t::REPLY) {
        // The accumulated return route is fully consumed — this node is the originator.
        reply(pre);
        return true;
    }
    terminus(nullptr);
    return true;
}

void fwd_router_t::on_frame_rope_impl(std::string_view inbound_name, view::rope_t frame,
                                      const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                      peer_handle_t peer) {
    // Single-link (every current producer): the link's bytes span feeds the SAME
    // routing as the borrowed path — the forward hop below never touches the
    // refcount (zero-heap, ADR-0038); only the terminus sees the owner, for the
    // ADR-0042 §3 referenced store. This is the pre-ADR-0053 view path, unchanged.
    if (frame.link_count() == 1) {
        const view_t& v = frame.links()[0];
        if (v.is_device()) return;  // CPU routing cannot read a DEVICE frame
        on_frame_impl(inbound_name, v.bytes(), &v, inbound_ctx, from_peer, peer);
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
        // A frame that is not a FWD is a type this router does not serve; a retired one is
        // counted and answered (`refuse_unserved_type`), never dropped in silence (#1951).
        if (!route_fwd_ingress(
                inbound_name, cur, inbound_ctx, from_peer,
                /* observe */ [] {},
                /* reject */
                [&](graph::status_t status, const fwd_pre_t& pre) {
                    // The rejection reply needs a contiguous decode; this is a COLD error
                    // path, so the one flatten is the ADR-0052 legitimate kind (exactly the
                    // control-plane precedent). Through the injected byte backend (#730), so
                    // a bounded node's memory bound covers this flatten too.
                    const std::expected<view_t, view::flatten_err_t> flat =
                        frame.subrope(0, frame.total_length()).try_materialize(*flat_);
                    // Flatten REFUSED ⇒ drop the frame. This arm is all-host-guarded above,
                    // so the refusal is the OOM — named by the error channel now rather than
                    // inferred from an empty view a zero-byte success could fake (#917). Still
                    // an early-out: `reject_bus_name_hop` opens
                    // with a `tlv_node_t::over`, an empty span does not validate, and it returns
                    // without replying — so the FRAME's fate does not depend on this line.
                    // What does depend on it is the COUNT (#1503 step 3): the OOM is named
                    // here rather than left to the codec's leniency, so an operator sees a
                    // flatten-backend exhaustion instead of silence. The SEAM on the line
                    // above is the testable part, and `fwd_flatten_backend_test` pins it.
                    if (!flat) {
                        count_drop(flatten_dropped_);
                        return;
                    }
                    reject_bus_name_hop(registry_, inbound_name, flat->bytes(), pre,
                                        rx_for(inbound_ctx), *egress_, status);
                },
                /* terminus */
                [&](const wire::path_ref_element_t* dst_target) {
                    // A request FWD is resolved straight off the rope (ADR-0053 3c-iii — NO
                    // flatten, verify-at-access §4).
                    resolve_terminus_rope(inbound_name, std::move(frame), dst_target, inbound_ctx,
                                          peer);
                },
                /* reply */
                [&](const fwd_pre_t& pre) {
                    // The step-5 reclaim (#1223) peeks the rope IN PLACE; only an actual
                    // RFC-0020 refusal pays the one cold flatten (the reject arm's ADR-0052
                    // precedent, same injected backend), because the route compare needs the
                    // echoed src contiguous.
                    if (const std::optional<refused_src_t> ref = peek_refused_route(cur, pre)) {
                        const std::expected<view_t, view::flatten_err_t> flat =
                            frame.subrope(0, frame.total_length()).try_materialize(*flat_);
                        if (flat)
                            (void)graph_.evict_route_edges(
                                inbound_name, inbound_pair(inbound_ctx, inbound_name, peer),
                                flat->bytes().subspan(ref->off, ref->len));
                    }
                    // A REPLY that reaches its originator here is handed to the sink
                    // rope-native (ADR-0055): NO flatten — the sink materializes on demand.
                    // Absent sink ⇒ dropped (as the flatten path would, into a no-op decode).
                    if (const auto sink = reply_target(cur); sink.fn != nullptr)
                        sink.fn(sink.ctx, frame);
                }))
            refuse_unserved_type(std::byte{cur.byte_at(0)}, inbound_name, inbound_ctx);
    }
    // A device or short rope is not CPU-readable here and is not routed.
}

void fwd_router_t::on_frame_impl(std::string_view inbound_name, std::span<const std::byte> frame,
                                 const view_t* frame_view, const child_rx_ctx_t* inbound_ctx,
                                 bool from_peer, peer_handle_t peer) {
    if (const auto sink = raw_.get(); sink.fn != nullptr) sink.fn(sink.ctx, inbound_name, frame);
    if (frame.size() < 4) return;

    // The FWD plane never builds a tlv_t (ADR-0038 inv. #1 / ADR-0041 §5): the
    // forward-vs-terminus split and the op discriminant are read by OFFSET; a
    // forward hop scatter-gathers with zero heap; a terminus request decodes into
    // the pmr arena; the inbound observer and a refused hop read the frame in place
    // (`wire::tlv_node_t`, #1648).
    const wire::grammar::span_cursor cur{frame};
    const bool routed = route_fwd_ingress(
        inbound_name, cur, inbound_ctx, from_peer,
        /* observe */
        [&] {
            if (const auto sink = inbound_.get(); sink.fn != nullptr) {
                // Read in place (#1648): validated once by `over`, nothing built. A
                // frame deeper than the walk's inline slots spills into the receiving
                // link's own source, never the process heap (receiver pays).
                if (const auto dec = wire::tlv_node_t::over(frame, rx_for(inbound_ctx));
                    dec && dec->opt().pl)
                    sink.fn(sink.ctx, inbound_name, *dec);
            }
        },
        /* reject */
        // Always inlined (#1859): out of line, this by-reference closure forces `frame` onto
        // the stack and the cursor's 16-byte copy of it stalls store-to-load forwarding on
        // every frame (~9 ns per forwarded frame). `reject_bus_name_hop` itself stays a call.
        [&](graph::status_t status, const fwd_pre_t& pre) __attribute__((always_inline)) {
            reject_bus_name_hop(registry_, inbound_name, frame, pre, rx_for(inbound_ctx), *egress_,
                                status);
        },
        /* terminus */
        [&](const wire::path_ref_element_t* dst_target) {
            resolve_terminus(inbound_name, frame, frame_view, inbound_ctx, dst_target, peer);
        },
        /* reply */
        [&](const fwd_pre_t& pre) {
            // The step-5 reclaim (#1223) runs BEFORE the sink and without one: an
            // addressed RFC-0020 refusal evicts the edge that stored the refused route,
            // whether or not anything else is listening for replies. Not a refusal ⇒
            // the peek bails allocation-free.
            if (const std::optional<refused_src_t> ref = peek_refused_route(cur, pre))
                (void)graph_.evict_route_edges(inbound_name,
                                               inbound_pair(inbound_ctx, inbound_name, peer),
                                               frame.subspan(ref->off, ref->len));
            // Hand the FWD{REPLY} to the sink rope-native (ADR-0055): NO decode. A
            // view-delivered frame ropes zero-copy off its owning view; a borrowed span is
            // copied once into an owned segment (the copy the old decode-then-consumer-
            // encode round-trip already paid).
            if (const auto sink = reply_target(cur); sink.fn != nullptr) {
                if (frame_view != nullptr) {
                    sink.fn(sink.ctx, view::rope_t(*frame_view));
                } else if (view_t owned = view::over_bytes(frame).value_or(view_t{});
                           !owned.empty()) {
                    sink.fn(sink.ctx, view::rope_t(std::move(owned)));
                }
            }
        });
    // A frame that is not a FWD is a type this router does not serve; a retired one is counted
    // and answered, never dropped in silence (#1951).
    if (!routed) refuse_unserved_type(frame[0], inbound_name, inbound_ctx);
}

void fwd_router_t::refuse_unserved_type(std::byte type, std::string_view inbound_name,
                                        const child_rx_ctx_t* inbound_ctx) noexcept {
    // The retired range (RFC-0032 §6.1): `0x11` ADVERTISE, `0x12` COMPACT, `0x13` HANDLE_NACK.
    // Counted first, so a refused answer still counts.
    if (type < std::byte{0x11} || type > std::byte{0x13}) return;
    count_drop(retired_rx_);
    if (transport_t* const in = reply_link(inbound_name, inbound_ctx))
        emit_retired_type_answer(*in);
}

template <class Cursor>
void fwd_router_t::route_fwd_forward(std::string_view inbound_name,
                                     const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                     const Cursor& cur_src, transport_t& child,
                                     const fwd_pre_t& pre) {
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
    // ctx is created once per child in add_child and lives on the router's chain, so its
    // address is stable for the router's lifetime, and its bytes are its registry slot's
    // immutable text (#1779). A frame delivered through the public on_frame (no
    // ctx: tests, SDK hosts, a link wired outside add_child) still resolves by name.
    //
    // ONE rebuild call, whichever of those supplied the mount (#1794). The three spellings
    // used to be three calls of the `[[gnu::flatten]]` rebuild, each flattened in full. A
    // link with no mount run at all — a name the registry does not hold — grows `src` by
    // the bare inbound name instead, as the one dynamic segment; so does a bus PEER, on top
    // of its bus child's run. (`from_peer` frames always carry a ctx.)
    const child_registry_t::child_t* const inbound =
        inbound_ctx != nullptr ? nullptr : registry_.entry_by_name(inbound_name);
    const std::span<const std::byte> mount =
        inbound_ctx != nullptr ? std::span<const std::byte>(inbound_ctx->mount_tlv)
        : inbound != nullptr   ? std::span<const std::byte>(inbound->mount_tlv)
                               : std::span<const std::byte>{};
    const std::string_view peer_seg =
        from_peer || mount.empty() ? inbound_name : std::string_view{};
    // This hop's PAIR on a relayed REPLY (RFC-0029 §6.2): this node's own reference to the
    // connection vertex for the link the reply came back over. Handed to the rebuild as a
    // CLOSURE, not a value, and the laziness is the measurement: deciding here means asking
    // whether the frame is a REPLY, and the rebuild is about to read that same op byte anyway.
    // Asking twice put a second `byte_at` on every forwarded frame — on a multi-link rope a
    // cursor walk, not a load — which is exactly the shape `bench_forward_rope` measures. A
    // node with no PAIR to give answers `nullopt` and the reply grows by the NAME run instead.
    const auto mint_fn = [&]() -> std::optional<wire::path_ref_element_t> {
        return hop_mint(inbound_name, inbound_ctx);
    };
    // The REVERSE-direction twin (§7.1), equally lazy: called at most once, and only on a
    // relayed subscribe request.
    const auto reverse_fn = [&]() -> std::optional<wire::path_ref_element_t> {
        return reverse_hop_ref(inbound_name, inbound_ctx, from_peer);
    };
    const auto rebuilt = rebuild_fwd_forward(cur_src, pre, mount, peer_seg, mint_fn, reverse_fn);
    // Both arms are the one `malformed_rx` bucket (#1503 Q3): a frame this node cannot
    // parse into a forwardable hop is one operator symptom, and nothing is SIZED against it.
    if (!rebuilt) {  // not a forwardable FWD ⇒ drop (callers pre-peeked)
        count_drop(malformed_rx_);
        return;
    }
    if (!rebuilt->ok()) {  // malformed oversized op ⇒ drop, no overrun
        count_drop(malformed_rx_);
        return;
    }

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
        // budget. Every frame but one fits it: a relayed SUBSCRIBE request's reverse child
        // (RFC-0029 §7.1) can emit up to 15 regions, and that cold frame is gathered again
        // into the growable table below rather than sizing the hot arm's array for it.
        std::array<std::span<const std::byte>, kFwdMaxIov> iov;
        std::size_t n = 0;
        rebuilt->gather(cur_src, [&](std::span<const std::byte> s) {
            if (n < iov.size()) iov[n] = s;
            ++n;
        });
        // A region that overshot the source window (#986) is malformed: a clamped feed emits
        // FEWER bytes than the rebuilt head declares — a truncated frame on the wire.
        if (cur_src.poisoned()) {
            count_drop(malformed_rx_);
            return;
        }
        if (n <= iov.size()) {
            child.send(std::span<const std::span<const std::byte>>(iov.data(), n));
            return;
        }
    }
    // Rope source (or the span arm's overflow): a region may cross several links, so the
    // sub-span count is only known at run time — gather into a NOTHROW block array drawn from
    // the failable seam (#596). A `std::pmr::vector` here was a peer-reachable abort(): the
    // element count is chosen by the sender (link count x region count), the growth is
    // `operator new`, and on -fno-exceptions the throw is a reboot. This is the FORWARD path —
    // it sits behind no ACL, exactly like the RX decode (#588). Both TERMINUS REPLY tables took
    // the same migration in #1570 — see `gather_reply_iov` and its two call sites — so no
    // egress iov in this file is on the throwing `reserve` any more.
    //
    // Exhaustion drops the frame. That is the correct answer for a forward hop: FWD is not
    // delivery-guaranteed, the sender retries, and emitting a partial iov would put a
    // TRUNCATED frame on the wire — worse than none.
    //
    // Out of line (`noinline`): the span instantiation reaches it only on the cold overflow, and
    // inlined there it was a second copy of the whole gather in the hot hop (+2.6 KB).
    const auto send_spilled = [&]() __attribute__((noinline)) {
        mem::block_array_t<std::span<const std::byte>> iov{rx_for(inbound_ctx)};
        bool ok = true;
        rebuilt->gather(cur_src, [&](std::span<const std::byte> s) {
            if (ok && !iov.push_back(s)) ok = false;
        });
        // The #986 window-overshoot drop and the resource drop, split into two counters (#1503
        // step 3): the iov exhaustion is a RESOURCE drop an operator sizes against, while a
        // poisoned cursor is a frame whose regions left the source window — malformed.
        if (!ok || cur_src.poisoned()) {
            count_drop(ok ? malformed_rx_ : forward_iov_dropped_);
            return;
        }
        child.send(std::span<const std::span<const std::byte>>(iov.data(), iov.size()));
    };
    send_spilled();
}

namespace {

/**
 * @brief Gather a terminus reply's links into @p iov — the reply egress's scatter-gather
 *        table, on the ADR-0065 failable seam (#1570).
 *
 * The LAST migratable `-fno-exceptions` probe window on the reply path. Both termini used to
 * build this table as a `std::vector` through `rope_t::try_to_iovec`, whose growth goes
 * through `tr::detail::try_reserve`: on a profile whose allocation does not throw, that
 * helper probes the GLOBAL heap, frees the probe block, and then runs the throwing `reserve`
 * on the inference that the block is still free — a task switch in that window `abort()`s the
 * node (#850). Once per inbound request frame, behind no ACL, on a table whose element type
 * (`std::span`) is trivially copyable. #981 ruled exactly this shape onto the failable seam
 * and took the router's own EGRESS tables there; this is the reply path catching up.
 *
 * Sized to the rope, with no synthetic cap: a composed-root reply has as many links as the
 * read produced, so a fixed-size array would either drop every composed read's reply or need
 * a heap arm behind it anyway. One refusable `try_alloc` per gather, no window.
 * @retval false The table could not be grown — the caller drops the reply and the client
 *               retries. Never an abort, at any reply size.
 */
[[nodiscard]] bool gather_reply_iov(const view::rope_t& reply,
                                    mem::block_array_t<std::span<const std::byte>>& iov) {
    if (!iov.reserve(reply.link_count())) return false;
    for (const view_t& l : reply.links())
        if (!iov.push_back(l.bytes())) return false;
    return true;
}

}  // namespace

/** @brief The terminus reply's egress: the inbound ctx's own link, or a counted name lookup. */
transport_t* fwd_router_t::reply_link(std::string_view inbound_name,
                                      const child_rx_ctx_t* inbound_ctx) noexcept {
    // The point-to-point arm: the frame came up this child's own receiver, whose ctx already
    // holds the link — the reply goes back the way the request came, with no registry scan.
    // A BUS ctx is excluded on purpose: its frames arrive under a PEER's name, and the reply
    // must reach that peer's directed endpoint, never the bus's broadcasting `send`
    // (ADR-0073 §3) — the peer resolution below is the one ADR-0044 rules. A tombstoned ctx
    // (#884) falls through too, so a child removed mid-frame is answered exactly as before:
    // by a name the registry no longer holds, i.e. not at all.
    if (inbound_ctx != nullptr && !inbound_ctx->retired.load(std::memory_order_acquire)) {
        const child_registry_t::egress_t eg = inbound_ctx->entry->bound();
        if (!eg.multi_peer) return eg.link;  // a registered slot always binds a link
    }
    reply_name_lookups_.fetch_add(1, std::memory_order_relaxed);
    return registry_.by_name(inbound_name);
}

void fwd_router_t::resolve_terminus(std::string_view inbound_name, std::span<const std::byte> frame,
                                    const view_t* frame_view, const child_rx_ctx_t* inbound_ctx,
                                    const wire::path_ref_element_t* dst_target,
                                    peer_handle_t peer) {
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
    if (!arena) {
        // Two causes behind one `expected` (#1503 Q3): `TLV_NESTING_TOO_DEEP` is
        // documented as "exceeds this RECEIVER's decode resources" — the rx source refused
        // a draw, which is a RESOURCE drop an operator sizes the terminus arena against.
        // Everything else is the frame's own fault and joins the malformed bucket.
        if (arena.error() != wire::err_t::TLV_NESTING_TOO_DEEP) {
            count_drop(malformed_rx_);
            return;
        }
        count_drop(arena_dropped_);
        // Counted AND answered (#1612): the requester learns its frame met an exhausted
        // receiver instead of inferring a lost frame from its own timeout. The reply is built
        // from the request's bytes on the stack — it cannot draw from the source that just
        // refused, and it does not draw from anything else either (`emit_refusal_reply`).
        if (transport_t* const in = reply_link(inbound_name, inbound_ctx))
            emit_refusal_reply(wire::grammar::span_cursor{frame}, *in,
                               graph::status_t::BACKPRESSURE);
        return;
    }
    // The terminus deref rides as the fourth argument and nothing else changes: a PAIR-spelled
    // `dst` is resolved against the element the PAIR arm already produced, and a `nullptr` —
    // every NAME-spelled frame — is the shipped call byte for byte.
    // WHERE it arrived and WHO sent it, as two claims rather than one string (ADR-0082).
    // The handle is the bus seam's when there is one and the FLAT link's in-flight peer
    // otherwise, so a per-writer subject is reachable at either setting of `peer_named`; the
    // ctx rides along as the opaque token the router's own subject supplier reads back.
    const graph::inbound_ref_t inbound{inbound_name, terminus_peer(inbound_ctx, peer), inbound_ctx,
                                       terminus_kind(inbound_ctx)};
    // ADR-0084: a remote AWAIT is deferred, never waited for here — this is the receive
    // context of `inbound_name`, and holding it for the await's deadline stalled every frame
    // behind it. A deferred resolve answers later, from the writer's thread.
    bool deferred = false;
    // The acknowledgement's storage (#1658), declared before `reply` so the rope that may hold
    // views into it is released first. Not lent to a request this node originated: that reply
    // goes to the record below, which may keep it past this frame.
    alignas(view::segment_t) std::array<std::byte, kReplyStoreBytes> store;
    auto reply =
        resolver_.resolve(*arena, inbound, frame_view, dst_target, &deferred,
                          inbound_name.empty() ? std::span<std::byte>{} : std::span(store));
    if (!reply) {
        if (reply.error() != graph::status_t::BACKPRESSURE) {  // non-request / malformed ⇒ drop
            count_drop(malformed_rx_);
            return;
        }
        // Assembly OOM, by value (#2256): the resolver's own `or_backpressure` already tried
        // an addressed answer and the egress backend refused THAT head too; the stack emitter
        // needs neither (#1612).
        count_drop(assemble_dropped_);
        if (transport_t* const in = reply_link(inbound_name, inbound_ctx))
            emit_refusal_reply(wire::grammar::span_cursor{frame}, *in,
                               graph::status_t::BACKPRESSURE);
        return;
    }
    // Nothing to send: an unacknowledged WRITE, or a deferred AWAIT (ADR-0084) that the
    // writer's thread answers later.
    if (reply->link_count() == 0) return;
    if (transport_t* const in = reply_link(inbound_name, inbound_ctx)) {
        // Nothrow scatter-gather egress on the failable seam (#1570, see `gather_reply_iov`),
        // drawn from the SAME source this frame's own decode drew from two calls up: the
        // per-owner receive source (ADR-0067 §3), not the graph's control budget. The table
        // is a transient cost of THIS peer's request, charged on the receive thread, so it
        // belongs where the arena is charged — one peer's replies cannot then exhaust the
        // control budget the whole node's mutations share.
        mem::block_array_t<std::span<const std::byte>> iov{rx_for(inbound_ctx)};
        if (!gather_reply_iov(*reply, iov)) {
            // The rx source refused the table — the same source the reply head would need,
            // so the answer is the stack-built one (#1612). `reply` is released with the
            // arena; the requester gets BACKPRESSURE and retries.
            count_drop(reply_iov_dropped_);
            emit_refusal_reply(wire::grammar::span_cursor{frame}, *in,
                               graph::status_t::BACKPRESSURE);
            return;
        }
        in->send(std::span<const std::span<const std::byte>>(iov.data(), iov.size()));
    } else if (inbound_name.empty()) {
        // This node originated the request (`originate`) and terminates it: there is no link
        // to send the reply over, so it goes where a reply arriving home would — the record.
        if (const auto sink = reply_target(wire::grammar::rope_cursor{*reply}); sink.fn != nullptr)
            sink.fn(sink.ctx, *reply);
    }
}

void fwd_router_t::resolve_terminus_rope(std::string_view inbound_name, view::rope_t frame,
                                         const wire::path_ref_element_t* dst_target,
                                         const child_rx_ctx_t* inbound_ctx, peer_handle_t peer) {
    // ADR-0053 3c-iii: the multi-link request terminus, resolved straight off the
    // rope — the interim flatten is gone. Adopt the frame as a lazy view: over()
    // anchors the root header + total-size bounds, and verify() adds the root
    // trailer-CRC linear scan. Ingress checks END there (CONTEXT.md §Validation
    // timing) — no whole-tree walk: a malformed or CRC-failing interior TLV
    // surfaces its error where the resolver CONSUMES that level (per-TLV
    // verify-at-access, ADR-0053 §4).
    const auto view = wire::tlv_view_t::over(std::move(frame));
    if (!view) {  // malformed root ⇒ drop (as a decode error)
        count_drop(malformed_rx_);
        return;
    }
    if (!view->verify().has_value()) {  // root CRC failure ⇒ drop
        count_drop(malformed_rx_);
        return;
    }
    // No frame_view is threaded, and the rope tier does not need one: unlike the arena's
    // pin_wire — which requires a contiguous frame view and is gated on it —
    // view_node::pin_wire IGNORES the argument and subropes the payload directly
    // (`op_resolve_view.cpp:113-135`), so the ADR-0042 §3 referenced store works here with
    // ZERO copy, retaining only the payload's links rather than the whole frame. (This
    // comment previously read as though the pin were unavailable on the rope path; it is
    // the reverse, and that misreading nearly justified deleting the tier.) Reply routes
    // back over the inbound link, its dst the request's accumulated src, as at the arena
    // terminus.
    const graph::inbound_ref_t inbound{inbound_name, terminus_peer(inbound_ctx, peer), inbound_ctx,
                                       terminus_kind(inbound_ctx)};
    bool deferred = false;  // ADR-0084, as at the arena terminus
    // The acknowledgement's storage (#1658), as at the arena terminus: this reply is only ever
    // sent from this frame, so the store is always lent.
    alignas(view::segment_t) std::array<std::byte, kReplyStoreBytes> store;
    auto reply = resolver_.resolve(*view, inbound, nullptr, dst_target, &deferred, store);
    if (!reply) {
        if (reply.error() != graph::status_t::BACKPRESSURE) {  // non-request / malformed ⇒ drop
            count_drop(malformed_rx_);
            return;
        }
        // Assembly OOM (#2256), answered from the request's own links (#1612) as at the arena
        // terminus: the rope cursor hands the emitter the same offsets the contiguous one does.
        count_drop(assemble_dropped_);
        if (transport_t* const in = reply_link(inbound_name, inbound_ctx))
            emit_refusal_reply(wire::grammar::rope_cursor{view->wire()}, *in,
                               graph::status_t::BACKPRESSURE);
        return;
    }
    // Nothing to send: an unacknowledged WRITE, or a deferred AWAIT (ADR-0084) that the
    // writer's thread answers later.
    if (reply->link_count() == 0) return;
    if (transport_t* const in = reply_link(inbound_name, inbound_ctx)) {
        // Nothrow scatter-gather egress (see `resolve_terminus`): the same failable-seam
        // table, from the same per-owner receive source, dropping rather than aborting on a
        // growth failure. This tier is where the unbounded case actually lives — a rope-tier
        // terminus resolves composed reads straight off the rope — so the absence of a fixed
        // bound here is load-bearing, not defensive.
        mem::block_array_t<std::span<const std::byte>> iov{rx_for(inbound_ctx)};
        if (!gather_reply_iov(*reply, iov)) {
            count_drop(reply_iov_dropped_);
            emit_refusal_reply(wire::grammar::rope_cursor{view->wire()}, *in,
                               graph::status_t::BACKPRESSURE);
            return;
        }
        in->send(std::span<const std::span<const std::byte>>(iov.data(), iov.size()));
    }
}

// --- remote subscribers: the target split, the host-side subscribe, delivery (#136) ---

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
                                      .residual = key.subspan(walk.end_of(hit.strip_k)),
                                      .token = mount_token(graph_, hit),
                                      .pair = {.conn = hit.mount->conn_slot}};
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
    // single copy every delivery then clones by refcount (ADR-0041 §2). Both owned copies
    // below draw from the injected `flat` seam (#1582), the same store the wire door's
    // un-injected default lands on; a refusal is BACKPRESSURE by value, never a heap fallback.
    //
    // Each TLV is written straight into its owned segment (#1779): no intermediate buffer,
    // one copy of the residual. The header widens past 0xFFFF exactly as `emit_tlv` would.
    const std::span<const std::byte> residual = split.residual;
    const wire::opt_t route_opt{.ll = residual.size() > 0xFFFFu};
    const std::size_t route_head = wire::header_bytes(route_opt);
    view::segment_ptr_t route_seg = view::segment_alloc(*flat_, route_head + residual.size());
    if (!route_seg) return std::unexpected(graph::status_t::BACKPRESSURE);
    wire::store_header(route_seg->bytes, wire::type_t::PATH, route_opt, residual.size());
    if (!residual.empty())
        std::memcpy(route_seg->bytes.data() + route_head, residual.data(), residual.size());
    const view::view_t route_view = view::view_t::over(std::move(route_seg));

    // A minimal SUBSCRIBER composite — the same admission door as the wire append
    // (subscribe_wire parses it once; deliveries ride the FWD{WRITE} form). An empty body:
    // the header alone.
    const wire::opt_t sub_opt{.pl = true};
    view::segment_ptr_t sub_seg = view::segment_alloc(*flat_, wire::header_bytes(sub_opt));
    if (!sub_seg) return std::unexpected(graph::status_t::BACKPRESSURE);
    wire::store_header(sub_seg->bytes, wire::type_t::SUBSCRIBER, sub_opt, 0);
    const view::view_t sub_view = view::view_t::over(std::move(sub_seg));

    // The mount's own interned token rides along (#1437). This door builds no SUBSCRIBER PATH
    // child, so the graph's mount arm never runs and never gets the chance to swap the token
    // itself: the link handed over IS the mount, and the token beside it is the mount's, so
    // the index insert is a subscript rather than the name door's scan of the live slots.
    // Un-carried, this was the ONE caller paying that scan per subscribe in steady state.
    return graph_.subscribe_wire(*v, sub_view, route_view, split.link, {}, {}, split.token,
                                 split.pair);
}

void fwd_router_t::deliver_remote(const graph::remote_delivery_t& sub, const graph::value_t& val) {
    const frame_scope_t scope;
    // The edge names its link by PAIR (#1941): one `u32` compare per registry slot, and for a
    // bus peer the slot index the handle carries — no name compare on either.
    const child_registry_t::child_t* const door = registry_.by_conn_slot(sub.link.conn);
    transport_t* const link =
        door != nullptr ? child_registry_t::resolve_peer(*door, sub.link.peer) : nullptr;
    if (link == nullptr) return;  // link torn down between subscribe and this write
    const std::span<const std::byte> route = sub.return_route.bytes();  // the stored PATH TLV

    // ONE frame build for both delivery forms (#1795): `FWD{ op=WRITE, dst, src=<empty PATH>,
    // payload=<VALUE> }` (delivery-is-a-write, RFC-0004 §D / #136). Every stream sample takes
    // it: the label-compacted form is gone (#1951), so no hop keeps per-flow state. Only the
    // `dst` and the link it leaves on differ, so they are chosen first and the head is written
    // once.
    //
    // Default: the canonical full route — the stored PATH TLV verbatim, copied ONCE at
    // subscribe (ADR-0041 §2), on the link the edge names. A delivery copies nothing; `src`
    // starts empty and each forwarding hop grows it (the way back). The refcounted route view
    // (`sub.return_route`) stays alive for this call even if the slot is concurrently
    // unsubscribed.
    transport_t* out = link;
    std::span<const std::byte> dst = route;
    stack_writer<6> ref_head;  // the PAIR-chain form's PATH header; empty for the full route
    // The chain delivery (RFC-0029 §7.1, learned with no flag since #2245): consume the
    // stored chain's element 0 — this node's OWN pair for the connection vertex the
    // subscribe arrived on — by validating it against this node's vertex map (§5.1 bounds +
    // generation, then §6.2's ACL at the dereferenced vertex under the edge's stored
    // subject) and egressing through the vertex it names. The rest goes on the wire verbatim
    // as the delivery's `dst` body: a `PATH` body of each hop's PAIR (else its NAME run) and
    // the origin's own `src`, stored pre-spelled at subscribe, so this leg re-spells nothing
    // and writes only the shrunk `PATH` header. ANY refusal — the link re-dialled (generation
    // moved), the child gone, the ACL revoked — keeps the canonical route above, which is stored
    // alongside precisely so this binding is an optimisation plus a liveness check and never the
    // only way home.
    const std::span<const std::byte> rev = sub.reverse_route.bytes();
    const std::optional<wire::path_pair_t> e0 = rev.size() > 4u + wire::kPathPairRecordBytes
                                                    ? wire::path_pair_at(rev.subspan(4), 0)
                                                    : std::nullopt;
    if (e0) {
        if (transport_t* const bound = bound_egress(*e0, sub.caller, graph::acl_right_t::WRITE)) {
            out = bound;
            dst = rev.subspan(4u + wire::kPathPairRecordBytes);
            // Six bytes hold either header width: a hop leaves off a list past 0xFFFF
            // (`fwd_frame_view.hpp`), and a peer's own oversized child widens to `LL`.
            ref_head.header_route(type_t::PATH, dst.size());
        }
    }
    constexpr std::array<std::byte, 5> op_tlv{std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
                                              std::byte{0x00},
                                              std::byte{std::to_underlying(fwd_op_t::WRITE)}};
    constexpr std::array<std::byte, 4> empty_src{std::byte{0x06}, std::byte{0x00}, std::byte{0x00},
                                                 std::byte{0x00}};
    const std::size_t body_len =
        op_tlv.size() + ref_head.span().size() + dst.size() + empty_src.size() + val.total_length();
    stack_writer<20> head;  // FWD header (≤6) + the 5-byte op TLV + the PATH header (≤4)
    head.header(type_t::FWD, body_len);
    head.raw(op_tlv);
    head.raw(ref_head.span());
    if (!head.ok()) {
        // Unreachable for a head this size (the FWD header is at most 6 bytes), and counted
        // rather than silent if a future head ever outgrows its buffer.
        count_drop(delivery_iov_dropped_);
        return;
    }

    // The retained send (RFC-0028 §6.9): head + dst + empty src as three spans on the stack,
    // and the value by reference. The per-delivery iov table this leg used to draw from
    // `graph_.control_source()` (#981) is gone with it: the link lowers `head ++ value.links()`
    // through its own inline table (`transport_t::send(head, value, lane)`), and a link that QUEUES
    // the frame keeps the value (one refcount) and copies only the head — so neither this leg
    // nor a queued link copies the payload, and neither allocates for a value of up to five
    // links. A multi-link value still crosses as its own segments (ADR-0053 ⑤).
    const std::array<std::span<const std::byte>, 3> head_iov{head.span(), dst,
                                                             std::span<const std::byte>(empty_src)};
    // The edge's lane rides along (ADR-0087 §2): a link that queues per peer waits it in
    // that vertex's own ring, so one vertex's backlog never sheds another's.
    out->send(std::span<const std::span<const std::byte>>(head_iov), val, sub.lane);
}

// --- deferred remote AWAIT (ADR-0084) ------------------------------------------------

/**
 * @brief One deferred remote AWAIT: the armed waiter plus everything its reply needs, in ONE
 *        block of the receiving link's rx source.
 *
 * The receiver pays (ADR-0084 §Decision 2): the block is `sizeof` this header plus the link
 * name and the reply's route bytes, drawn from the same source that link's decode arena draws
 * from, so a peer that parks AWAITs spends its own link's budget and no one else's. The route
 * bytes are copied once here because the request frame they came from is released when the
 * receive context returns.
 */
struct fwd_router_t::pending_await_t {
    graph::await_waiter_t waiter;             /**< @brief Armed on the vertex; its ctx is this. */
    fwd_router_t* router = nullptr;           /**< @brief The owner the fire thunk calls back. */
    mem::block_source_t* source = nullptr;    /**< @brief The rx source this block came from. */
    std::size_t block_bytes = 0;              /**< @brief The block's size, for its release. */
    graph::vertex_handle_t vertex;            /**< @brief The awaited vertex. */
    pending_await_t* prev = nullptr;          /**< @brief Plane list link. */
    pending_await_t* next = nullptr;          /**< @brief Plane list link. */
    std::optional<wire::timestamp_t> echo_ts; /**< @brief The request's TF=0 stamp. */
    std::size_t name_len = 0;                 /**< @brief Link name bytes. */
    std::size_t dst_len = 0;                  /**< @brief Reply `dst` bytes. */
    std::size_t src_len = 0;                  /**< @brief Reply `src` bytes. */
    bool sending = false; /**< @brief A reply is being built outside the lock. */
    bool orphan = false;  /**< @brief Cancelled while firing: send nothing. */

    /** @brief A fresh, unarmed waiter block for @p v. */
    explicit pending_await_t(graph::vertex_handle_t v) noexcept : vertex(v) {}

    /** @brief The trailing byte region: name, dst, src, in that order. */
    [[nodiscard]] std::byte* tail() noexcept { return reinterpret_cast<std::byte*>(this + 1); }
    /** @brief The link name the reply goes back over. */
    [[nodiscard]] std::string_view name() noexcept {
        return {reinterpret_cast<const char*>(tail()), name_len};
    }
    /** @brief Reply `dst`. */
    [[nodiscard]] std::span<const std::byte> dst() noexcept { return {tail() + name_len, dst_len}; }
    /** @brief Reply `src`. */
    [[nodiscard]] std::span<const std::byte> src() noexcept {
        return {tail() + name_len + dst_len, src_len};
    }
};

fwd_router_t::~fwd_router_t() {
    // First, the graph seams this router installed (#2239), each only if it is still this
    // router's: from here on a write that reaches a remote edge, a wire subscribe and a `:stats`
    // read find no sink rather than calling into a router that is going away.
    graph_.clear_hook(&graph::graph_hooks_t::remote_delivery, this);
    graph_.clear_hook(&graph::graph_hooks_t::wire_target, this);
    graph_.clear_hook(&graph::graph_hooks_t::stats_sampler, this);
    graph_.clear_hook(&graph::graph_hooks_t::link_name, this);
    std::unique_lock lock(awaits_.m);
    awaits_.stop = true;
    cancel_awaits_locked({}, true);
    // A waiter a writer already took, or one whose reply is being sent, still calls back
    // into this router: wait for each of them to retire itself.
    awaits_.cv.wait(lock, [&] { return awaits_.head == nullptr; });
    lock.unlock();
    // A record outliving its router must not keep a dangling owner: disarm every one still
    // linked, so its destructor finds nothing to cancel (#1645).
    const std::lock_guard origin_lock(origin_m_);
    for (origin_t* o = origins_.load(std::memory_order_relaxed); o != nullptr;) {
        origin_t* const next = o->next_;
        o->next_ = nullptr;
        o->owner_.store(nullptr, std::memory_order_release);
        o = next;
    }
    origins_.store(nullptr, std::memory_order_relaxed);
    // The router's registration state goes back to the link plane's source (#1779): every
    // receiver ctx with its bus token cache, then the interned kind records. Nothing reads
    // them any more — the transports outlive no router they are registered with.
    for (child_rx_ctx_t* c = rx_head_.load(std::memory_order_relaxed); c != nullptr;) {
        child_rx_ctx_t* const next = c->next.load(std::memory_order_relaxed);
        if (bus_token_cache_t* const t = c->peer_tokens_own) {
            t->~bus_token_cache_t();
            link_src_->release(t, sizeof(bus_token_cache_t), alignof(bus_token_cache_t));
        }
        for (session_tenancy_t* t = c->tenancy.load(std::memory_order_relaxed); t != nullptr;) {
            session_tenancy_t* const parked = t->parked;
            link_src_->release(t, sizeof(session_tenancy_t) + t->size * sizeof(std::uint64_t),
                               alignof(session_tenancy_t));
            t = parked;
        }
        c->~child_rx_ctx_t();
        link_src_->release(c, sizeof(child_rx_ctx_t), alignof(child_rx_ctx_t));
        c = next;
    }
    for (kind_rec_t* r = kinds_; r != nullptr;) {
        kind_rec_t* const next = r->next;
        const std::size_t bytes = sizeof(kind_rec_t) + r->view.kind.size();
        r->~kind_rec_t();
        link_src_->release(r, bytes, alignof(kind_rec_t));
        r = next;
    }
}

std::size_t fwd_router_t::pending_awaits() const {
    const std::lock_guard lock(awaits_.m);
    return awaits_.count;
}

graph::result_t<void> fwd_router_t::defer_await_thunk(void* ctx,
                                                      const graph::deferred_await_t& req) {
    return static_cast<fwd_router_t*>(ctx)->defer_await(req);
}

void fwd_router_t::await_fired_thunk(void* ctx, graph::await_waiter_t& /*w*/) noexcept {
    const frame_scope_t scope;
    auto* const p = static_cast<pending_await_t*>(ctx);  // the waiter's ctx is its own block
    fwd_router_t& r = *p->router;
    {
        const std::lock_guard lock(r.awaits_.m);
        p->sending = true;
    }
    r.answer_await(*p);
    const std::lock_guard lock(r.awaits_.m);
    r.retire_await_locked(p);
}

graph::result_t<void> fwd_router_t::defer_await(const graph::deferred_await_t& req) {
    const child_rx_ctx_t* const ctx =
        req.inbound != nullptr ? static_cast<const child_rx_ctx_t*>(req.inbound->origin) : nullptr;
    const std::string_view name = req.inbound != nullptr ? req.inbound->link : std::string_view{};
    // No link to answer over ⇒ nothing to defer: the caller answers BACKPRESSURE at once.
    if (name.empty()) return std::unexpected(graph::status_t::BACKPRESSURE);

    mem::block_source_t& source = rx_for(ctx);
    const std::size_t bytes =
        sizeof(pending_await_t) + name.size() + req.dst.size() + req.src.size();
    void* const mem = source.try_alloc(bytes, alignof(pending_await_t));
    // The receiving link's own budget is spent: refuse THIS await, by value. Never a fallback
    // to the blocking wait.
    if (mem == nullptr) return std::unexpected(graph::status_t::BACKPRESSURE);
    auto* const p = new (mem) pending_await_t(req.vertex);
    p->waiter.fire = &fwd_router_t::await_fired_thunk;
    p->waiter.ctx = p;
    p->router = this;
    p->source = &source;
    p->block_bytes = bytes;
    // No deadline is stored: the requester owns the timeout (RFC-0004 Amendment 3, ADR-0084),
    // so `req.timeout` is ignored here. A waiter whose vertex never changes ends on link_down /
    // remove_child or at teardown, and it costs only this block of its own link's source.
    p->echo_ts = req.echo_ts;
    p->name_len = name.size();
    p->dst_len = req.dst.size();
    p->src_len = req.src.size();
    std::byte* out = p->tail();
    std::memcpy(out, name.data(), name.size());
    out += name.size();
    for (const std::span<const std::byte> part : {req.dst, req.src}) {
        if (!part.empty()) std::memcpy(out, part.data(), part.size());
        out += part.size();
    }

    std::unique_lock lock(awaits_.m);
    const auto refuse = [&](graph::status_t st) -> graph::result_t<void> {
        p->~pending_await_t();
        source.release(mem, bytes, alignof(pending_await_t));
        return std::unexpected(st);
    };
    if (awaits_.stop) return refuse(graph::status_t::BACKPRESSURE);
    // Armed UNDER the plane lock (plane → stripe is the lock order). A writer that fires it
    // the instant it is armed blocks on this lock in its fire thunk until it is listed.
    const graph::result_t<void> armed = graph_.arm_await(req.vertex, p->waiter, req.subject);
    if (!armed) return refuse(armed.error());
    p->next = awaits_.head;
    if (awaits_.head != nullptr) awaits_.head->prev = p;
    awaits_.head = p;
    ++awaits_.count;
    return {};
}

void fwd_router_t::retire_await_locked(pending_await_t* p) noexcept {
    if (p->prev != nullptr) p->prev->next = p->next;
    if (p->next != nullptr) p->next->prev = p->prev;
    if (awaits_.head == p) awaits_.head = p->next;
    --awaits_.count;
    mem::block_source_t* const source = p->source;
    const std::size_t bytes = p->block_bytes;
    p->~pending_await_t();
    source->release(p, bytes, alignof(pending_await_t));
    awaits_.cv.notify_all();  // teardown waits for the list to drain
}

void fwd_router_t::cancel_awaits_locked(std::string_view link_name, bool all) noexcept {
    for (pending_await_t* p = awaits_.head; p != nullptr;) {
        pending_await_t* const next = p->next;
        if (all || p->name() == link_name) {
            // Not yet fired ⇒ ours to release now. Fired or sending ⇒ its thread retires it;
            // it only learns to send nothing.
            if (!p->sending && graph::graph_t::disarm_await(p->waiter)) {
                retire_await_locked(p);
            } else {
                p->orphan = true;
            }
        }
        p = next;
    }
}

void fwd_router_t::answer_await(pending_await_t& p) {
    // `orphan` is written under the plane lock; read it there too. The reply may still race
    // a teardown that lands after this read — it then goes to a link `by_name` no longer
    // resolves, which is the same window `deliver_remote` has.
    {
        const std::lock_guard lock(awaits_.m);
        if (p.orphan) return;
    }
    transport_t* const link = registry_.by_name(p.name());
    if (link == nullptr) return;
    const graph::reply_route_t route{
        .dst_wire = p.dst(), .src_wire = p.src(), .echo_ts = p.echo_ts};
    view::rope_t reply;
    const graph::result_t<graph::value_ref_t> v = graph_.await_value(p.vertex);
    if (v) {
        const graph::value_t& val = **v;
        reply = graph::assemble_reply(route, graph::reply_kind_t::RESULT, {}, val.links(),
                                      val.total_length(), *egress_);
        if (reply.link_count() == 0)
            reply = graph::assemble_error_reply(route, graph::status_t::BACKPRESSURE, *egress_);
    } else {
        reply = graph::assemble_error_reply(route, v.error(), *egress_);
    }
    if (reply.link_count() == 0) {
        count_drop(assemble_dropped_);
        return;
    }
    // The waiter's own source, not its link's: the fire may land after the link it arrived
    // over was removed, when nothing may be read through that link any more (#1941).
    mem::block_array_t<std::span<const std::byte>> iov{*p.source};
    if (!gather_reply_iov(reply, iov)) {
        count_drop(reply_iov_dropped_);
        return;
    }
    link->send(std::span<const std::span<const std::byte>>(iov.data(), iov.size()));
}

}  // namespace tr::net
