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

void fwd_router_t::add_child(std::string name, transport_t& link) {
    // Populate the registry BEFORE wiring the receiver: an async transport (UDP/ws) may
    // already have a live recv thread, so `set_receiver` is the publish point — once the
    // callback is installed, on_frame can read the registry on that thread. Adding the
    // child first ensures the entry is visible before any inbound frame can resolve it
    // (the set_receiver mutex provides the release/acquire fence). Registry is otherwise
    // immutable after setup — no lock on the read hot path.
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
        // A bus frame arrives tagged with the sending peer's name, so the ctx is
        // just the router itself.
        // Departure seam (RFC-0009 §D extended): a bus peer that hangs up carries its
        // own name, so link_down needs no per-child state either.
        bus->set_peer_down_notifier(
            [](void* c, std::string_view peer) { static_cast<fwd_router_t*>(c)->link_down(peer); },
            this);
        if (bus->delivers_ropes()) {
            bus->set_peer_rope_receiver(
                [](void* c, std::string_view peer, view::rope_t frame) {
                    static_cast<fwd_router_t*>(c)->on_frame_rope(peer, std::move(frame));
                },
                this);
        } else {
            bus->set_peer_receiver(
                [](void* c, std::string_view peer, std::span<const std::byte> frame) {
                    static_cast<fwd_router_t*>(c)->on_frame(peer, frame);
                },
                this);
        }
        return;
    }
    // Point-to-point: the inbound NAME is fixed per child, carried by a stable
    // per-child ctx (child_rx_ holds the address for the transport's lifetime).
    child_rx_ctx_t& ctx = child_rx_.emplace_back(this, std::move(name));
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
                cc->self->on_frame_rope(cc->name, std::move(frame));
            },
            &ctx);
    } else {
        link.set_receiver(
            [](void* c, std::span<const std::byte> frame) {
                auto* const cc = static_cast<child_rx_ctx_t*>(c);
                cc->self->on_frame(cc->name, frame);
            },
            &ctx);
    }
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

void fwd_router_t::on_frame_rope(std::string_view inbound_name, view::rope_t frame) {
    // Single-link (every current producer): the link's bytes span feeds the SAME
    // routing as the borrowed path — the forward hop below never touches the
    // refcount (zero-heap, ADR-0038); only the terminus sees the owner, for the
    // ADR-0042 §3 referenced store. This is the pre-ADR-0053 view path, unchanged.
    if (frame.link_count() == 1) {
        const view_t& v = frame.links()[0];
        if (v.is_device()) return;  // CPU routing cannot read a DEVICE frame
        on_frame_impl(inbound_name, v.bytes(), &v);
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
        if (const auto seg = peek_fwd_first_dst_seg(cur)) {
            // Resolve the first dst segment NAME. A routable segment is bounded
            // (≤ kMaxSegmentBytes), so it is stitched into a small stack scratch —
            // contiguous when it lies in one link, a byte-at-a-time copy across a link
            // boundary; an over-long "segment" is not routable ⇒ fall to the terminus.
            const auto [seg_off, seg_len] = *seg;
            if (seg_len > 0 && seg_len <= graph::kMaxSegmentBytes) {
                std::array<std::byte, graph::kMaxSegmentBytes> scratch;
                std::size_t w = 0;
                cur.for_each_span(seg_off, seg_len, [&](std::span<const std::byte> s) {
                    for (std::byte b : s) scratch[w++] = b;
                });
                if (transport_t* const child =
                        registry_.by_segment(std::span<const std::byte>(scratch.data(), seg_len))) {
                    route_fwd_forward(inbound_name, cur, *child);
                    return;
                }
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
                                 const view_t* frame_view) {
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
        if (const auto seg = peek_fwd_first_dst_seg(cur)) {
            if (transport_t* const child =
                    registry_.by_segment(frame.subspan(seg->first, seg->second))) {
                route_fwd_forward(inbound_name, cur, *child);
                return;
            }
            // First dst segment names no child ⇒ this node is the terminus.
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

    // Control frames (route-handle flow setup) keep the owning decode — a contiguous
    // span decodes eagerly (byte-identical MCU terminus). Decompose the tree into the
    // (label, child) the refactored sinks take, so the span and rope (on_control_rope)
    // paths share one handler body (ADR-0055 §2).
    const auto dec = wire::decode(frame);
    if (!dec || !dec->opt.pl) return;  // drop malformed / non-structured
    if (dec->children.empty() || dec->children[0].type != type_t::VALUE ||
        dec->children[0].payload.size() < 2)
        return;  // every control frame leads with a u16 label VALUE
    const auto label = detail::load_le<std::uint16_t>(dec->children[0].payload);
    switch (dec->type) {
        case type_t::ADVERTISE:
            if (dec->children.size() < 2) return;
            on_advertise(inbound_name, label, dec->children[1]);
            return;
        case type_t::COMPACT:
            if (dec->children.size() < 2) return;
            on_compact(inbound_name, label, wire::encode(dec->children[1]));
            return;
        case type_t::HANDLE_NACK:
            on_nack(inbound_name, label);
            return;
        default:
            return;  // drop anything else
    }
}

template <class Cursor>
void fwd_router_t::route_fwd_forward(std::string_view inbound_name, const Cursor& cur_src,
                                     transport_t& child) {
    // All offsets, no decoded tree: the shrunk-dst / grown-src head rebuild lives in
    // fwd_frame_view.hpp (rebuild_fwd_forward — unit-tested directly); this hop only
    // resolves the child and scatter-gathers the result. Reads AND the egress go
    // through the grammar `Cursor` seam (ADR-0053 ④b): the same code serves a
    // contiguous `span_cursor` (each region is one sub-span) and a link-walking
    // `rope_cursor` (a region yields one sub-span per link it crosses).
    const auto rebuilt = rebuild_fwd_forward(cur_src, inbound_name);
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
        // Contiguous source: at most 6 regions, each a single sub-span — a stack array.
        std::array<std::span<const std::byte>, 6> iov;
        std::size_t n = 0;
        rebuilt->gather(cur_src, [&](std::span<const std::byte> s) { iov[n++] = s; });
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
    if (!reply) return;  // structurally non-request ⇒ drop
    if (transport_t* in = registry_.by_name(inbound_name)) {
        const std::vector<std::span<const std::byte>> iov = reply->to_iovec();
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
    // The rope tier stores its one ownership copy (own_tlv) — the ADR-0042 §3
    // referenced store needs a contiguous frame view, absent for a scatter-gather
    // rope, so no frame_view is threaded. Reply routes back over the inbound link,
    // its dst the request's accumulated src, exactly as the arena terminus.
    auto reply = resolver_.resolve(*view, inbound_name, nullptr);
    if (!reply) return;  // structurally non-request ⇒ drop
    if (transport_t* in = registry_.by_name(inbound_name)) {
        const std::vector<std::span<const std::byte>> iov = reply->to_iovec();
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

    // Resolve the first route segment against this node's transport children — the
    // SAME rule the FWD forward step uses, so the label tracks the delivery route.
    transport_t* const down =
        route.children.empty() ? nullptr : registry_.by_segment(route.children[0].payload);

    if (down != nullptr) {
        // Forwarding hop: strip the leading segment, allocate OUR own out-label,
        // record the swap, retain the stripped egress route (for NACK re-advertise),
        // and re-advertise downstream with the new label (MPLS-style swap).
        const std::span<const std::byte> seg = route.children[0].payload;
        const std::string down_name(detail::as_string_view(seg));
        tlv_t stripped = route;
        stripped.children.erase(stripped.children.begin());
        const std::vector<std::byte> stripped_bytes = wire::encode(stripped);

        const std::uint16_t out_label = handles_.alloc_label(down_name);
        handles_.bind_ingress(inbound_name, label,
                              handle_binding_t{.terminus = false,
                                               .down_link = down_name,
                                               .out_label = out_label,
                                               .local_route = {}});
        handles_.record_egress(down_name, out_label, stripped_bytes);
        const std::vector<std::byte> adv2 = encode_advertise(out_label, stripped_bytes);
        down->send(std::span<const std::byte>(adv2));
        return;
    }

    // Terminus: the route resolves locally here — bind the label to the local route.
    handles_.bind_ingress(
        inbound_name, label,
        handle_binding_t{
            .terminus = true, .down_link = {}, .out_label = 0, .local_route = wire::encode(route)});
}

void fwd_router_t::on_compact(std::string_view inbound_name, std::uint16_t label,
                              std::span<const std::byte> payload_bytes) {
    // `payload_bytes` is the already-contiguous wire encoding of the COMPACT payload TLV
    // (the span path re-encodes the decoded child; the rope path materializes only the
    // payload sub-rope — ADR-0055 §2). It is never re-decoded here — just stored/forwarded.
    const std::optional<handle_binding_t> binding = handles_.lookup_ingress(inbound_name, label);
    if (!binding) {
        // Stale/unknown label: drop, observe, and NACK back to prompt a re-advertise
        // (self-heal). Never a crash — the route is simply re-learned (RFC-0004 §E.1).
        if (stale_cb_) stale_cb_(inbound_name, label);
        if (transport_t* const up = registry_.by_name(inbound_name)) {
            const std::vector<std::byte> nack = encode_handle_nack(label);
            up->send(std::span<const std::byte>(nack));
        }
        return;
    }

    if (binding->terminus) {
        // Local delivery — expand the label to the bound route and apply the write
        // (a delivery IS a write, RFC-0004 §D), then notify the delivery sink.
        if (deliver_local(binding->local_route, payload_bytes) && delivery_cb_)
            delivery_cb_(binding->local_route, payload_bytes);
        return;
    }
    // Forwarding hop: swap to our out-label and re-emit the COMPACT downstream — the
    // route still does NOT ride, only the (swapped) label.
    if (transport_t* const down = registry_.by_name(binding->down_link)) {
        const std::vector<std::byte> out = encode_compact(binding->out_label, payload_bytes);
        down->send(std::span<const std::byte>(out));
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

bool fwd_router_t::deliver_local(std::span<const std::byte> route_path,
                                 std::span<const std::byte> payload) {
    const auto route = wire::decode(route_path);
    if (!route || route->type != type_t::PATH) return false;
    // The canonical PATH key (concatenated NAME encodings) — the graph vertex-map key.
    const std::optional<graph::vertex_handle_t> v = graph_.find(wire::path_key(*route));
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
        const view_t flat = value.materialize();
        const auto [label, fresh] = handles_.ensure_egress(sub.link, route);
        if (fresh) {
            const std::vector<std::byte> adv = encode_advertise(label, route);
            link->send(std::span<const std::byte>(adv));
        }
        const std::vector<std::byte> out = encode_compact(label, flat.bytes());
        link->send(std::span<const std::byte>(out));
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
    std::vector<std::span<const std::byte>> iov;
    iov.reserve(3 + value.link_count());
    iov.push_back(head.span());
    iov.push_back(route);
    iov.push_back(std::span<const std::byte>(empty_src));
    for (const view_t& l : value.links()) iov.push_back(l.bytes());
    link->send(std::span<const std::span<const std::byte>>(iov));
}

}  // namespace tr::net
