/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/fwd_router.hpp"

#include <array>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/mem_heap.hpp"
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
template <class Cursor>
struct seg_reader_t {
    const Cursor& cur;
    std::array<std::byte, graph::kMaxSegmentBytes * tr::net::kMountPeekMax> scratch;
    std::size_t used = 0;

    /** @brief Segment `[off, len)` as a view, or empty when it is not routable. */
    std::string_view read(std::size_t off, std::size_t len) {
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
        if (!straddles && first_len == len)
            return std::string_view(reinterpret_cast<const char*>(first), len);
        if (used + len > scratch.size()) return {};
        std::size_t w = used;
        cur.for_each_span(off, len, [&](std::span<const std::byte> s) {
            for (const std::byte b : s) scratch[w++] = b;
        });
        std::string_view out(reinterpret_cast<const char*>(scratch.data() + used), len);
        used += len;
        return out;
    }
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
};

/**
 * @brief Resolve leading path segments against the registry (ADR-0061's strip-K descent).
 *
 * Matches LONGEST-FIRST — the full `net/<module>/<name>` mount before any shorter key — so a
 * more specific mount always wins, and a node still carrying flat single-segment children
 * (the pre-RFC-0014 shape, and what the benches register) resolves through the same code.
 * When the matched child is multi-peer and another segment follows, that segment is resolved
 * in THAT endpoint's own peer table (never across buses) and the hop eats one more segment.
 *
 * Segment-based rather than cursor-based so the two planes share ONE descent: the forward
 * path feeds it segments peeked out of the frame, and the control plane (@ref
 * fwd_router_t::on_advertise) feeds it the NAME children of a decoded route. Those two used
 * to resolve by different rules — the control plane still resolved a single BARE segment
 * (#516) — which silently absorbed every advertise addressed to a `/net/<module>/<name>`
 * mount at the first intermediate node.
 */
[[nodiscard]] mount_hit_t resolve_mount_segs(const child_registry_t& registry,
                                             std::span<const std::string_view> seg) {
    const std::size_t n = seg.size();
    if (n == 0) return {};
    const std::size_t widest = std::min<std::size_t>(n, tr::net::kMountPeekMax - 1);
    for (std::size_t k = widest; k >= 1; --k) {
        bool usable = true;
        for (std::size_t i = 0; i < k; ++i) {
            if (seg[i].empty()) usable = false;
        }
        if (!usable) continue;
        const child_registry_t::child_t* const c =
            registry.by_segments(std::span<const std::string_view>(seg.data(), k));
        if (c == nullptr) continue;
        // A dst that names the mount EXACTLY addresses the connection vertex itself — its
        // own `:children[]`, `:settings`, liveness value — so it terminates HERE. Only a dst
        // with something BELOW the mount is a forward (ADR-0038 §3a: a local dst descends to
        // a local vertex and terminates). A bus PEER is the exception: it has no vertex, so
        // naming a peer exactly still forwards, with an empty residual.
        if (n == k) return {};
        if (c->multi_peer && !seg[k].empty()) {
            if (transport_t* const p = child_registry_t::resolve_peer(*c, seg[k])) {
                return mount_hit_t{p, seg[k], k + 1, seg[k]};
            }
        }
        return mount_hit_t{c->link.load(std::memory_order_acquire), {}, k, c->name};
    }

    return {};
}

/**
 * @brief The forward path's entry to @ref resolve_mount_segs — peeks `dst`, then descends.
 */
template <class Cursor>
[[nodiscard]] mount_hit_t resolve_mount(const child_registry_t& registry, const Cursor& cur,
                                        seg_reader_t<Cursor>& rd) {
    std::array<std::pair<std::size_t, std::size_t>, tr::net::kMountPeekMax> off{};
    const std::size_t n = peek_fwd_dst_segs(cur, off);
    if (n == 0) return {};
    std::array<std::string_view, tr::net::kMountPeekMax> seg;
    for (std::size_t i = 0; i < n; ++i) seg[i] = rd.read(off[i].first, off[i].second);
    return resolve_mount_segs(registry, std::span<const std::string_view>(seg.data(), n));
}

}  // namespace

void fwd_router_t::add_child(std::string name, transport_t& link) {
    // Populate the registry BEFORE wiring the receiver: an async transport (UDP/ws) may
    // already have a live recv thread, so `set_receiver` is the publish point — once the
    // callback is installed, on_frame can read the registry on that thread. Adding the
    // child first ensures the entry is visible before any inbound frame can resolve it
    // (the set_receiver mutex provides the release/acquire fence). No lock is taken on the
    // read hot path. NOTE: "the registry is immutable after setup" stopped being true when
    // RFC-0014 made connection create/remove a RUNTIME operation — an add of a new name
    // reallocates the table under a concurrent forward read (#521). Closed by the S5
    // mutation contract, where the TSan gate and a reclamation scheme land together.
    registry_.add(name, link);
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
        child_rx_ctx_t& bctx = child_rx_.emplace_back(this, name, registry_.mount_run_for(name));
        bus->set_peer_down_notifier(
            [](void* c, std::string_view peer) {
                static_cast<child_rx_ctx_t*>(c)->self->link_down(peer);
            },
            &bctx);
        if (bus->delivers_ropes()) {
            bus->set_peer_rope_receiver(
                [](void* c, std::string_view peer, view::rope_t frame) {
                    auto* const cc = static_cast<child_rx_ctx_t*>(c);
                    cc->self->on_frame_rope_bus(*cc, peer, std::move(frame));
                },
                &bctx);
        } else {
            bus->set_peer_receiver(
                [](void* c, std::string_view peer, std::span<const std::byte> frame) {
                    auto* const cc = static_cast<child_rx_ctx_t*>(c);
                    cc->self->on_frame_bus(*cc, peer, frame);
                },
                &bctx);
        }
        return;
    }
    // Point-to-point: the inbound NAME is fixed per child, carried by a stable
    // per-child ctx (child_rx_ holds the address for the transport's lifetime).
    child_rx_ctx_t& ctx = child_rx_.emplace_back(this, name, registry_.mount_run_for(name));
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
}

bool fwd_router_t::remove_child(std::string_view name) {
    // Stop resolving FIRST: once the entry is tombstoned no forward can reach the link,
    // so the caller is free to destroy the transport as soon as this returns. Then the
    // ordinary departure eviction reclaims the graph edges and label state — the same
    // work link_down does, reused rather than duplicated.
    if (!registry_.erase(name)) return false;
    link_down(name);
    // NOTE: the point-to-point receiver ctx in child_rx_ is intentionally left in place.
    // The transport held its address, and the deque never invalidates, so the slot is
    // inert once the link is gone; reclaiming it needs the S5 mutation contract.
    return true;
}

void fwd_router_t::on_reply(std::function<void(const view::rope_t&)> cb) {
    reply_cb_ = std::move(cb);
}

void fwd_router_t::on_inbound(std::function<void(std::string_view, const tlv_t&)> cb) {
    inbound_cb_ = std::move(cb);
}

void fwd_router_t::on_raw(std::function<void(std::string_view, std::span<const std::byte>)> cb) {
    raw_cb_ = std::move(cb);
}

void fwd_router_t::on_compact_delivery(
    std::function<void(std::span<const std::byte>, std::span<const std::byte>)> cb) {
    delivery_cb_ = std::move(cb);
}

void fwd_router_t::on_stale_label(std::function<void(std::string_view, std::uint16_t)> cb) {
    stale_cb_ = std::move(cb);
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

std::uint16_t fwd_router_t::advertise(std::string_view link_name,
                                      std::span<const std::byte> route_path) {
    transport_t* const link = registry_.by_name(link_name);
    if (link == nullptr) return 0;
    const std::uint16_t label = handles_.alloc_label(link_name);
    handles_.record_egress(link_name, label,
                           std::vector<std::byte>(route_path.begin(), route_path.end()));
    const std::vector<std::byte> adv = encode_advertise(label, route_path);
    link->send(std::span<const std::byte>(adv));
    return label;
}

void fwd_router_t::send_compact(std::string_view link_name, std::uint16_t label,
                                std::span<const std::byte> payload) {
    if (transport_t* const link = registry_.by_name(link_name)) {
        const std::vector<std::byte> out = encode_compact(label, payload);
        link->send(std::span<const std::byte>(out));
    }
}

void fwd_router_t::on_frame(std::string_view inbound_name, std::span<const std::byte> frame) {
    on_frame_impl(inbound_name, frame, nullptr);
}

void fwd_router_t::on_frame_bus(const child_rx_ctx_t& ctx, std::string_view peer,
                                std::span<const std::byte> frame) {
    on_frame_impl(peer, frame, nullptr, &ctx, true);
}

void fwd_router_t::on_frame_rope(std::string_view inbound_name, view::rope_t frame) {
    on_frame_rope_impl(inbound_name, std::move(frame), nullptr, false);
}

void fwd_router_t::on_frame_rope_bus(const child_rx_ctx_t& ctx, std::string_view peer,
                                     view::rope_t frame) {
    on_frame_rope_impl(peer, std::move(frame), &ctx, true);
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
        // Only a structured FWD enters the routing arm; ADVERTISE / COMPACT / HANDLE_NACK
        // fall through to the control path below, exactly as they did when this was gated on
        // peek_fwd_first_dst_seg.
        std::array<std::pair<std::size_t, std::size_t>, tr::net::kMountPeekMax> probe{};
        if (peek_fwd_dst_segs(cur, probe) > 0) {
            // Resolve the mount prefix (ADR-0061 strip-K). Segments are read in place when
            // the rope keeps them contiguous and stitched into the reader's scratch when they
            // straddle a link; an over-long segment is not routable ⇒ fall to the terminus.
            seg_reader_t<wire::grammar::rope_cursor> rd{cur, {}, 0};
            const mount_hit_t hit = resolve_mount(registry_, cur, rd);
            if (hit.link != nullptr) {
                route_fwd_forward(inbound_name, inbound_ctx, from_peer, hit.strip_k, cur,
                                  *hit.link);
                return;
            }
            // No child (or over-long segment) ⇒ this node is the terminus for the frame.
            // A request FWD is resolved straight off the rope (ADR-0053 3c-iii — NO
            // flatten, verify-at-access §4).
            if (peek_fwd_op(cur) != fwd_op_t::REPLY) {
                resolve_terminus_rope(inbound_name, std::move(frame));
                return;
            }
            // A REPLY that reaches its originator here is handed to the sink rope-native
            // (ADR-0055): NO flatten — the sink materializes on demand. Absent sink ⇒
            // dropped (as the flatten path would, into a no-op decode).
            if (reply_cb_) reply_cb_(frame);
            return;
        }
    }
    // Control frame (or a device/short rope): served rope-native (ADR-0055 §2/§3). The
    // route-handle sinks read the label off the rope and materialize only the sub-rope
    // they need contiguous — the interim whole-frame flatten is gone (ADR-0053 ⑥).
    on_control_rope(inbound_name, std::move(frame));
}

void fwd_router_t::on_frame_impl(std::string_view inbound_name, std::span<const std::byte> frame,
                                 const view_t* frame_view, const child_rx_ctx_t* inbound_ctx,
                                 bool from_peer) {
    if (raw_cb_) raw_cb_(inbound_name, frame);
    if (frame.size() < 4) return;

    // The FWD plane never builds a tlv_t (ADR-0038 inv. #1 / ADR-0041 §5): the
    // forward-vs-terminus split and the op discriminant are read by OFFSET; a
    // forward hop scatter-gathers with zero heap; a terminus request decodes into
    // the pmr arena. Only the originator REPLY sink and the control frames below
    // keep the owning wire::decode (test/SDK-facing and flow-setup paths, allowed
    // to allocate per ADR-0039).
    if (static_cast<type_t>(u8(frame[0])) == type_t::FWD) {
        if (inbound_cb_) {  // read-only observer (tests/ACL seam) — wants the tree
            if (const auto dec = wire::decode(frame); dec && dec->opt.pl)
                inbound_cb_(inbound_name, *dec);
        }
        const wire::grammar::span_cursor cur{frame};
        {
            seg_reader_t<wire::grammar::span_cursor> rd{cur, {}, 0};
            const mount_hit_t hit = resolve_mount(registry_, cur, rd);
            if (hit.link != nullptr) {
                route_fwd_forward(inbound_name, inbound_ctx, from_peer, hit.strip_k, cur,
                                  *hit.link);
                return;
            }
            // The dst names no mount ⇒ this node is the terminus.
        }
        if (peek_fwd_op(cur) == fwd_op_t::REPLY) {
            // The accumulated return route is fully consumed — this node is the
            // originator. Hand the FWD{REPLY} to the sink rope-native (ADR-0055): NO
            // decode. A view-delivered frame ropes zero-copy off its owning view; a
            // borrowed span is copied once into an owned segment (the copy the old
            // decode-then-consumer-encode round-trip already paid).
            if (reply_cb_) {
                if (frame_view != nullptr) {
                    reply_cb_(view::rope_t(*frame_view));
                } else if (view_t owned = view::over_bytes(frame).value_or(view_t{});
                           !owned.empty()) {
                    reply_cb_(view::rope_t(std::move(owned)));
                }
            }
            return;
        }
        resolve_terminus(inbound_name, frame, frame_view);
        return;
    }

    // Control frames are read BY OFFSET, exactly as `on_control_rope` already does — the
    // span arm was the last reader in the ingress plane still building an owning `tlv_t`.
    //
    // That owning decode was justified by ADR-0041 §5 / ADR-0055 §3 as a flow-setup cost,
    // "allowed to allocate per ADR-0039". ADR-0062 invalidated that premise: a warm COMPACT
    // is now the steady-state per-sample data frame, not setup. It cost 3 allocations for
    // the tree spine plus 5 more re-encoding a payload that is ALREADY contiguous in
    // `frame` — together ~55-63% of a warm terminus frame.
    //
    // `crc_check_t::VERIFY` is passed explicitly and is load-bearing. `peek_control`
    // defaults to DEFER for the forward-hop callers (byte-for-byte unchanged), but the
    // owning `wire::decode` this replaces verified every node's CRC, so deferring here
    // would silently start ACCEPTING a COMPACT whose root trailer says its payload is
    // corrupt — verify-before-apply (CONTEXT.md §Frame integrity, ADR-0041 §1). The cost is
    // zero allocations and, on our own traffic, zero cycles: `encode_compact` emits no CR
    // bit. A peer may legally set one, which is exactly why the check must be explicit.
    const wire::grammar::span_cursor ccur{frame};
    const auto head = peek_control(ccur, wire::grammar::crc_check_t::VERIFY);
    if (!head) return;  // malformed / not a control frame / CRC failure ⇒ drop
    switch (head->type) {
        case type_t::ADVERTISE: {
            if (head->child1_off == 0) return;
            // The route is the one child that genuinely needs a tree: on_advertise walks its
            // NAME segments and re-encodes a stripped copy. Decode ONLY that sub-span, as
            // the rope arm does — never the whole frame.
            const auto route = wire::decode(frame.subspan(head->child1_off, head->child1_total));
            if (!route) return;
            on_advertise(inbound_name, head->label, *route);
            return;
        }
        case type_t::COMPACT:
            if (head->child1_off == 0) return;
            // The payload TLV is already contiguous here — hand over the span. The old path
            // decoded it into a tree and then `wire::encode`d it straight back.
            on_compact(inbound_name, head->label,
                       frame.subspan(head->child1_off, head->child1_total));
            return;
        case type_t::HANDLE_NACK:
            on_nack(inbound_name, head->label);  // acts on the label alone — no child needed
            return;
        default:
            return;  // drop anything else
    }
}

template <class Cursor>
void fwd_router_t::route_fwd_forward(std::string_view inbound_name,
                                     const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                     std::size_t strip_k, const Cursor& cur_src,
                                     transport_t& child) {
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
    const child_registry_t::child_t* const inbound =
        inbound_ctx != nullptr ? nullptr : registry_.entry_by_name(inbound_name);
    const auto rebuilt =
        !mount.empty() ? rebuild_fwd_forward(cur_src, mount,
                                             from_peer ? inbound_name : std::string_view{}, strip_k)
        : inbound != nullptr && !inbound->mount_tlv.empty()
            ? rebuild_fwd_forward(cur_src, std::span<const std::byte>(inbound->mount_tlv),
                                  std::string_view{}, strip_k)
            : rebuild_fwd_forward(cur_src, std::span<const std::byte>{}, inbound_name, strip_k);
    if (!rebuilt) return;        // not a forwardable FWD ⇒ drop (callers pre-peeked)
    if (!rebuilt->ok()) return;  // malformed oversized op ⇒ drop, no overrun

    // Scatter-gather egress: the small stack heads interleaved with the untouched inbound
    // regions (remaining dst, selector, original src bytes, payload) — no payload copy. The
    // gather is written ONCE over the cursor seam (fwd_rebuild_t::gather); each region is
    // emitted via `for_each_span`, which yields exactly one sub-span for a contiguous source
    // and one per straddled link for a rope. So the container is the only thing that varies:
    // a stack `std::array` for the span path (ZERO heap, ADR-0038 inv. #2) vs a
    // `std::pmr::vector` over the injected @ref mr_ for the rope path (a link count is only
    // known at run time).
    if constexpr (std::is_same_v<Cursor, wire::grammar::span_cursor>) {
        // Contiguous source: each region is a single sub-span — a stack array sized by
        // kFwdMaxIov, which is derived from the layout (see its docs), not a chosen budget.
        // The write is bounds-guarded regardless: this array was previously a bare 6 with an
        // unchecked `iov[n++]`, so any growth in the region count was a silent overrun.
        std::array<std::span<const std::byte>, kFwdMaxIov> iov;
        std::size_t n = 0;
        rebuilt->gather(cur_src, [&](std::span<const std::byte> s) {
            if (n < iov.size()) iov[n++] = s;
        });
        child.send(std::span<const std::span<const std::byte>>(iov.data(), n));
    } else {
        // Rope source: a region may cross several links — gather into a pmr vector drawn
        // from the terminus arena's resource (the forward hop still copies no payload).
        std::pmr::vector<std::span<const std::byte>> iov{mr_};
        rebuilt->gather(cur_src, [&](std::span<const std::byte> s) { iov.push_back(s); });
        child.send(std::span<const std::span<const std::byte>>(iov.data(), iov.size()));
    }
}

void fwd_router_t::resolve_terminus(std::string_view inbound_name, std::span<const std::byte> frame,
                                    const view_t* frame_view) {
    // Local request terminus (ADR-0041 §5): arena-decode straight from the
    // node's injected resource (ADR-0039 §1) — the library keeps no buffer of
    // its own; a bounded host injects a pool resource over its slab and the
    // terminus allocates nothing from the global heap. The arena is released
    // before this call returns. Apply the op and route the FWD{REPLY} back over
    // the link the request arrived on (its dst is the request's accumulated
    // src). The inbound link makes a `:subscribers[]` WRITE bind a REMOTE
    // subscriber whose deliveries route back over it (#136); the latch
    // (transient-local) fires inside resolve.
    const auto arena = wire::decode_into(frame, *mr_);
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
    // (`op_resolve_view.cpp:99-101`), so the ADR-0042 §3 referenced store works here with
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

void fwd_router_t::on_control_rope(std::string_view inbound_name, view::rope_t frame) {
    // Only a MULTI-link control frame reaches here — a contiguous (single-link) one
    // decodes eagerly in on_frame_impl. A control frame is never a DEVICE payload, so a
    // non-all-host rope is not one; drop it (as the old flatten→failed-decode path did).
    if (!frame.all_host()) return;
    const wire::grammar::rope_cursor cur{frame};
    const auto head = peek_control(cur);
    if (!head) return;
    switch (head->type) {
        case type_t::HANDLE_NACK:
            // Label only — no materialize at all.
            on_nack(inbound_name, head->label);
            return;
        case type_t::ADVERTISE: {
            if (head->child1_off == 0) return;
            // Materialize ONLY the route sub-rope: on_advertise strips its leading segment
            // and re-encodes, which needs a contiguous tree (ADR-0052 legitimate flatten).
            const view_t route_flat =
                frame.subrope(head->child1_off, head->child1_total).materialize();
            const auto route = wire::decode(route_flat.bytes());
            if (!route) return;
            on_advertise(inbound_name, head->label, *route);
            return;
        }
        case type_t::COMPACT: {
            if (head->child1_off == 0) return;
            // Materialize ONLY the payload sub-rope: it is stored (deliver_local) or
            // re-wrapped (encode_compact) as contiguous bytes — a transport-egress / local-
            // store boundary (ADR-0055 §2). Hold the owning view while reading its span.
            const view_t payload_flat =
                frame.subrope(head->child1_off, head->child1_total).materialize();
            on_compact(inbound_name, head->label, payload_flat.bytes());
            return;
        }
        default:
            return;
    }
}

void fwd_router_t::on_advertise(std::string_view inbound_name, std::uint16_t label,
                                const tlv_t& route) {
    if (route.type != type_t::PATH) return;

    // Resolve the leading route segments through the SAME strip-K mount descent the FWD
    // forward step uses (resolve_mount_segs), so a label tracks exactly the route a FWD
    // would take. It resolved a single BARE segment until #516, which meant every route
    // addressed to an RFC-0014 `/net/<module>/<name>` mount missed, fell through to the
    // terminus arm, and was ABSORBED at the first intermediate node — the compacted flow
    // then delivered locally at a node that was only supposed to relay it.
    std::array<std::string_view, tr::net::kMountPeekMax> seg;
    std::size_t n = 0;
    for (const tlv_t& child : route.children) {
        // LEADING NAMEs only, and the collected count is exactly the erase count below —
        // stopping at the first non-NAME keeps the segment indices positional, so `strip_k`
        // never names a child that is not the segment it resolved.
        if (n == seg.size() || child.type != type_t::NAME) break;
        seg[n++] = detail::as_string_view(child.payload);
    }
    const mount_hit_t hit =
        resolve_mount_segs(registry_, std::span<const std::string_view>(seg.data(), n));

    if (hit.link != nullptr) {
        // Forwarding hop: strip the K segments this node consumed, allocate OUR own
        // out-label, record the swap, retain the stripped egress route (for NACK
        // re-advertise), and re-advertise downstream with the new label (MPLS-style swap).
        const std::string down_name(hit.link_name);
        tlv_t stripped = route;
        stripped.children.erase(
            stripped.children.begin(),
            stripped.children.begin() + static_cast<std::ptrdiff_t>(hit.strip_k));
        const std::vector<std::byte> stripped_bytes = wire::encode(stripped);

        const std::uint16_t out_label = handles_.alloc_label(down_name);
        // Built field-by-field rather than with a designated-initializer brace: the binding
        // now carries ADR-0062's memoized resolution too, and an aggregate init that names
        // only some members trips -Werror=missing-field-initializers on the ESP toolchain.
        handle_binding_t fwd;
        fwd.terminus = false;
        fwd.down_link = down_name;
        fwd.out_label = out_label;
        handles_.bind_ingress(inbound_name, label, std::move(fwd));
        handles_.record_egress(down_name, out_label, stripped_bytes);
        const std::vector<std::byte> adv2 = encode_advertise(out_label, stripped_bytes);
        hit.link->send(std::span<const std::byte>(adv2));
        return;
    }

    // Terminus: the route resolves locally here — bind the label to the local route.
    handle_binding_t term;
    term.terminus = true;
    term.local_route = wire::encode(route);
    handles_.bind_ingress(inbound_name, label, std::move(term));
}

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
    if (!rb.found) {
        // Stale/unknown label: drop, observe, and NACK back to prompt a re-advertise
        // (self-heal). Never a crash — the route is simply re-learned (RFC-0004 §E.1).
        if (stale_cb_) stale_cb_(inbound_name, label);
        if (transport_t* const up = registry_.by_name(inbound_name)) {
            const std::vector<std::byte> nack = encode_handle_nack(label);
            up->send(std::span<const std::byte>(nack));
        }
        return;
    }

    if (rb.terminus) {
        // WARM: dereference the cached vertex and write. No decode, no path walk, no
        // graph_.find — the whole point of RFC-0004 §E.1's label, finally applied to the
        // RESOLUTION and not merely to the wire. The generation guard is what makes the
        // cached handle safe: retire() bumps it (#511), so a retired-and-revived vertex is
        // detected here rather than silently written through (RFC-0009 §B.6 re-virginize).
        if (rb.warm && rb.target && graph_.retire_generation(*rb.target) == rb.target_gen) {
            const auto payload_view = view::over_bytes(payload_bytes);
            if (!payload_view) return;  // alloc failure ⇒ drop (one audited locus)
            view::rope_t value;
            if (!value.try_reserve(1)) return;
            value.append(*payload_view);
            if (graph_.write(*rb.target, std::move(value)).has_value() && delivery_cb_) {
                const std::optional<handle_binding_t> b =
                    handles_.lookup_ingress(inbound_name, label);
                if (b) delivery_cb_(b->local_route, payload_bytes);
            }
            return;
        }
        // COLD or STALE: take the owning form, resolve the route, then memoize it.
        const std::optional<handle_binding_t> binding =
            handles_.lookup_ingress(inbound_name, label);
        if (!binding) return;
        if (deliver_local(binding->local_route, payload_bytes)) {
            if (const auto v = resolve_route_vertex(binding->local_route)) {
                resolved_binding_t fill = rb;
                fill.warm = true;
                fill.target = *v;
                fill.target_gen = graph_.retire_generation(*v);
                handles_.cache_resolution(inbound_name, label, fill);
            }
            if (delivery_cb_) delivery_cb_(binding->local_route, payload_bytes);
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
        if (transport_t* const down = slot->link.load(std::memory_order_acquire)) {
            // The NOTHROW exact-reserve encoder, not `encode_compact`: this is a steady-state
            // data frame, so its two growth-doubling vectors were four avoidable allocations
            // per frame. On reserve failure it DROPS — the audited soft-fail locus, matching
            // `deliver_remote` and the #477 never-abort discipline (`encode_compact` grows
            // `std::vector`s, which abort under -fno-exceptions).
            std::vector<std::byte> out;
            if (!try_encode_compact(out, rb.out_label, payload_bytes)) return;
            down->send(std::span<const std::byte>(out));
            return;
        }
        // Tombstoned: fall through and re-resolve, so a re-created link re-warms.
    }
    const std::optional<handle_binding_t> binding = handles_.lookup_ingress(inbound_name, label);
    if (!binding) return;
    if (const child_registry_t::child_t* const slot = registry_.entry_by_name(binding->down_link)) {
        if (transport_t* const down = slot->link.load(std::memory_order_acquire)) {
            std::vector<std::byte> out;
            if (!try_encode_compact(out, binding->out_label, payload_bytes)) return;
            down->send(std::span<const std::byte>(out));
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
    const std::optional<std::vector<std::byte>> route = handles_.egress_route(inbound_name, label);
    if (!route) return;
    if (transport_t* const link = registry_.by_name(inbound_name)) {
        const std::vector<std::byte> adv = encode_advertise(label, *route);
        link->send(std::span<const std::byte>(adv));
    }
}

std::optional<graph::vertex_handle_t> fwd_router_t::resolve_route_vertex(
    std::span<const std::byte> route_path) const {
    // The SAME resolution deliver_local performs, factored out so the memoized handle can
    // never diverge from the one the cold path would have used. Two rules would be two
    // sources of truth for "which vertex does this label mean".
    const auto route = wire::decode(route_path);
    if (!route || route->type != type_t::PATH) return std::nullopt;
    return graph_.find(wire::path_key(*route));
}

bool fwd_router_t::deliver_local(std::span<const std::byte> route_path,
                                 std::span<const std::byte> payload) {
    // Through the SAME helper the memoized handle is resolved by — so the cold path and the
    // cached path cannot disagree about which vertex a label names. Two decode+find copies
    // would be two sources of truth, which is the shape #516 turned out to be.
    const std::optional<graph::vertex_handle_t> v = resolve_route_vertex(route_path);
    if (!v) return false;
    // `payload` is a wire-encoded TLV (never empty); `nullopt` is exactly an alloc
    // failure → drop the delivery (one audited alloc/copy/over locus).
    const auto payload_view = view::over_bytes(payload);
    if (!payload_view) return false;
    return graph_.write(*v, *payload_view).has_value();
}

void fwd_router_t::deliver_remote(const graph::remote_delivery_t& sub, const view::rope_t& value) {
    transport_t* const link = registry_.by_name(sub.link);
    if (link == nullptr) return;  // link torn down between subscribe and this write
    const std::span<const std::byte> route = sub.return_route.bytes();  // the stored PATH TLV

    if (sub.delivery_compact) {
        // Auto-promote (Q5 / RFC-0004 §E.1): advertise the label once per flow, then stream
        // lean COMPACT. ensure_egress is idempotent per (link,route); clear_link on a
        // reconnect drops the binding so the next delivery re-advertises (self-heal).
        // A COMPACT wraps a CONTIGUOUS payload (encode_compact), so a multi-link value pays
        // one flatten here — single-link, the common case, is a zero-copy adopt. The
        // scatter-gather win is the default full-route path below (the hot fan-out leg).
        // Every per-delivery allocation on this writer-thread leg is NOTHROW (#477): a
        // failed flatten or frame build DROPS the delivery (the subscriber misses one
        // value under heap exhaustion — valid delivery behavior), never an abort. A
        // dropped fresh ADVERTISE self-heals via the peer's HANDLE_NACK (§E.1).
        const view_t flat = value.materialize();
        if (flat.empty() && value.total_length() != 0) return;  // flatten OOM — drop
        const auto [label, fresh] = handles_.ensure_egress(sub.link, route);
        std::vector<std::byte> frame;
        if (fresh && try_encode_advertise(frame, label, route))
            link->send(std::span<const std::byte>(frame));
        if (!try_encode_compact(frame, label, flat.bytes())) return;  // OOM — drop
        link->send(std::span<const std::byte>(frame));
        return;
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
    constexpr std::array<std::byte, 4> empty_src{std::byte{0x06}, std::byte{0x40}, std::byte{0x00},
                                                 std::byte{0x00}};
    const std::size_t body_len =
        op_tlv.size() + route.size() + empty_src.size() + value.total_length();
    stack_writer<16> head;  // FWD header (≤6) + the 5-byte op TLV
    head.header(type_t::FWD, body_len);
    head.raw(op_tlv);
    if (!head.ok()) return;

    // iov = head + route + empty_src + one span per value link (sized to the rope, no
    // synthetic cap — the same per-send iov vector the rope terminus reply builds).
    // Nothrow-reserved (#477): an OOM drops this delivery instead of a bad_alloc
    // abort() on the writer thread — the try_to_iovec discipline (d352998).
    std::vector<std::span<const std::byte>> iov;
    if (!tr::detail::try_reserve(iov, 3 + value.link_count())) return;  // OOM — drop
    iov.push_back(head.span());
    iov.push_back(route);
    iov.push_back(std::span<const std::byte>(empty_src));
    for (const view_t& l : value.links()) iov.push_back(l.bytes());
    link->send(std::span<const std::span<const std::byte>>(iov));
}

}  // namespace tr::net
