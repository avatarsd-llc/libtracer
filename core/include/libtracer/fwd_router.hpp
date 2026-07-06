/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0004 / ADR-0035 slice 3 — multi-hop FWD forwarding + zero-copy `src`
 * accumulation across transports. A node holds a set of NAMED transport-child
 * vertices (ADR-0027): each child is a link, addressed in THIS node's space by a
 * single NAME segment. On an inbound FWD that arrived on child `inbound_name`:
 *
 *   - resolve the FIRST `dst` segment. If it names a local NON-transport vertex
 *     (terminus), apply the op via the slice-2 op_resolver_t, build FWD{REPLY},
 *     and send it back over the link the request arrived on;
 *   - if it names a transport-child vertex (forward), STRIP that segment from
 *     `dst`, PREPEND to `src` (zero-copy rope head-prepend) the NAME by which THIS
 *     node addresses the inbound link (the way back), and send the shortened FWD
 *     over the named child to the peer;
 *   - a FWD{op=REPLY} routes by the same step — its `dst` (the accumulated return
 *     route) is resolved segment-by-segment and forwarded hop-by-hop; a REPLY does
 *     NOT accumulate `src` (a reply expects no reply, RFC-0004 §B). When a REPLY's
 *     first `dst` segment is local (the originator's reply endpoint) it terminates
 *     and is delivered to the registered reply sink.
 *
 * Forwarders are STATELESS: there is no per-request table — the forward route is
 * the shrinking `dst` and the return route is the growing `src`, both carried in
 * the frame (RFC-0004 §B/§D). A hop may reboot mid-operation and the reply still
 * routes. Forward resolution lives at the L4 router + the tr::net transport seam
 * (ADR-0035). The FWD plane builds no tlv_t: forward hops offset-dispatch with
 * zero heap (ADR-0038), terminus requests decode into the pmr arena (ADR-0041).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/child_registry.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

/**
 * @brief A stateless hop-by-hop FWD forwarder (RFC-0004 §A/§B, ADR-0035 slice 3).
 *
 * Wires a local @ref graph::graph_t (terminus op resolution, via an internal
 * @ref graph::op_resolver_t) to a set of named transport children. Configure the
 * children (and optional reply sink) before frames flow; thereafter @ref on_frame
 * fires on the transports' receive threads — possibly several concurrently — and
 * holds no mutable routing state, so no per-request locking is required.
 */
class fwd_router_t {
   public:
    /**
     * @brief Bind to the local @p graph; terminus ops resolve against it.
     *
     * Also installs the graph's remote-delivery sink (#136): a write to a vertex with a
     * remote subscriber fans out a `FWD{WRITE}` (or auto-promoted `COMPACT`) back over the
     * subscriber's link. The sink captures `this`, so the router must outlive @p graph's
     * use — the same lifetime the held `graph_` reference already requires.
     *
     * @param graph The node's local graph.
     * @param mr    The node's container memory resource (ADR-0039 §1) — the terminus
     *              arena draws from it directly; the library holds no buffer of its
     *              own. A bounded node injects a pool resource over its static slab
     *              (one slab, whole stack — ADR-0039 §2) and the terminus then
     *              allocates nothing from the global heap; the default is the
     *              standard heap (a terminus may allocate, ADR-0039). Must outlive
     *              the router.
     */
    explicit fwd_router_t(graph::graph_t& graph,
                          std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : graph_(graph), resolver_(graph), mr_(mr), handles_(mr) {
        graph_.set_remote_delivery_sink(
            [this](const graph::remote_delivery_t& sub, const view::rope_t& value) {
                deliver_remote(sub, value);
            });
    }

    fwd_router_t(const fwd_router_t&) = delete;
    fwd_router_t& operator=(const fwd_router_t&) = delete;

    /**
     * @brief Register a named transport-child vertex (ADR-0027).
     *
     * @p name is the single NAME segment by which THIS node addresses @p link —
     * both the segment a `dst` names to route onward through @p link, and the
     * segment prepended to `src` when a frame arrives on @p link (the way back).
     * Installs a receiver on @p link that funnels each inbound frame to
     * `on_frame(name, ...)`. Call once per link, during setup (before frames flow).
     *
     * Installs the receiver matching the link's capability (ADR-0042 §1, ropes
     * per ADR-0053): an owning link (`link.delivers_ropes()`) gets a rope
     * receiver whose frame funnels through the SAME routing (the forward hop
     * stays span-based zero-heap; the refcounts ride only to the terminus);
     * every other link keeps the borrowed-span receiver unchanged.
     *
     * @param name This node's local NAME for the link (e.g. "up", "cli").
     * @param link The transport carrying the next/previous hop.
     */
    void add_child(std::string name, transport_t& link);

    /**
     * @brief Set the sink for a REPLY that terminates at this node's reply endpoint.
     *
     * Invoked (with the `FWD{REPLY}` frame as a @ref view::rope_t) when a REPLY's first
     * `dst` segment does not name a transport child — i.e. the accumulated return route
     * has been fully consumed and this node is the originator. Optional; absent ⇒ such a
     * reply is dropped.
     *
     * The frame is handed over rope-native (ADR-0055): the router performs NO decode and
     * NO flatten — a rope-delivered reply reaches the sink zero-copy. A sink that wants
     * contiguous bytes holds `const view_t m = reply.materialize()` and reads `m.bytes()`
     * — a single-link reply (the common case) is returned zero-copy, no alloc, no copy;
     * only a multi-link reply pays one flatten, on demand. A sink that wants the eager
     * tree decodes those bytes (`wire::decode(m.bytes())`). The materialize escape hatch
     * (ADR-0052) now lives at the consumer, not the router; keep `m` alive while reading
     * its span.
     *
     * @param cb Callback invoked on a transport receive thread; keep it cheap.
     */
    void on_reply(std::function<void(const view::rope_t&)> cb);

    /**
     * @brief Set a read-only observer of every inbound FWD (observability/tests).
     *
     * Invoked after decode with the inbound child name and the decoded FWD, before
     * routing. Carries no routing semantics; used to assert the per-hop `dst`-shrink
     * / `src`-grow invariant and as the seam where a per-hop `:acl` forward-right
     * check (RFC-0004 §F) will later hang.
     *
     * @param cb Callback invoked on a transport receive thread.
     */
    void on_inbound(std::function<void(std::string_view, const wire::tlv_t&)> cb);

    /**
     * @brief Set a read-only observer of every inbound frame's RAW bytes (any type).
     *
     * Fires before dispatch with the inbound link name and the complete frame span —
     * used by tests to measure the on-wire byte-delta between a lean COMPACT delivery
     * and the equivalent full-route FWD{WRITE} (the point of the route-handle).
     * @param cb Callback invoked on a transport receive thread.
     */
    void on_raw(std::function<void(std::string_view, std::span<const std::byte>)> cb);

    /**
     * @brief Set the sink for a label-compacted delivery that terminates at this node.
     *
     * Invoked when a COMPACT's label resolves to a LOCAL terminus binding (the
     * established route names a vertex here): the payload has already been written to
     * that vertex (delivery-is-a-write, RFC-0004 §D). Carries the bound local route
     * PATH bytes and the delivered payload TLV bytes (both borrowed for the call).
     * @param cb Callback invoked on a transport receive thread; keep it cheap.
     */
    void on_compact_delivery(
        std::function<void(std::span<const std::byte>, std::span<const std::byte>)> cb);

    /**
     * @brief Set the observer for a dropped stale/unknown-label COMPACT (RFC-0004 §E.1).
     *
     * Invoked when a COMPACT bears a label with no ingress binding on its link — the
     * frame is dropped and a HANDLE_NACK is sent back to prompt a re-advertise (never
     * a crash). Carries the inbound link name and the stale label.
     * @param cb Callback invoked on a transport receive thread.
     */
    void on_stale_label(std::function<void(std::string_view, std::uint16_t)> cb);

    /**
     * @brief Advertise a `label ↔ route` binding over link @p link_name (producer side).
     *
     * Allocates a fresh per-link label, sends an ADVERTISE carrying it and @p route,
     * and records the egress binding (so a NACK can re-advertise). Call when a
     * compact-flagged flow starts or on (re)connect — re-advertising is the self-heal.
     * @param link_name  This node's NAME for the downstream link to advertise over.
     * @param route_path A complete PATH TLV's bytes — the delivery route to alias.
     * @return The allocated label (to stamp on subsequent @ref send_compact), or 0 if
     *         @p link_name names no child.
     */
    std::uint16_t advertise(std::string_view link_name, std::span<const std::byte> route_path);

    /**
     * @brief Send a label-compacted delivery over link @p link_name (producer side).
     *
     * Emits `COMPACT{ label, payload }` — the route does NOT ride, only the label
     * bound by a prior @ref advertise. No-op if @p link_name names no child.
     * @param link_name This node's NAME for the downstream link.
     * @param label     A label returned by @ref advertise for that link.
     * @param payload   A complete payload TLV's bytes (the delivered VALUE).
     */
    void send_compact(std::string_view link_name, std::uint16_t label,
                      std::span<const std::byte> payload);

    /**
     * @brief Forget all route-handle label state for link @p link_name (self-heal hook).
     *
     * A transport calls this on (re)connect/disconnect; a subsequent re-advertise
     * rebinds cleanly and a delivery on a now-cleared label is NACK'd, not misrouted.
     * @param link_name This node's NAME for the link whose label state to drop.
     */
    void clear_link(std::string_view link_name);

    /** @brief The route-handle label store (test introspection — assert statelessness). */
    [[nodiscard]] const route_handle_t& handles() const noexcept { return handles_; }

    /**
     * @brief Route one inbound FWD frame that arrived on child @p inbound_name.
     *
     * Forwards (dst-shrink + src-grow) toward a transport child, resolves+replies
     * for a local terminus, or delivers a terminal REPLY to the reply sink. A
     * malformed or non-FWD frame is dropped. Never blocks on a downstream peer
     * beyond the transport's own bounded send.
     *
     * @param inbound_name This node's NAME for the link the frame arrived on.
     * @param frame        The complete inbound FWD frame bytes (borrowed; consumed
     *                     before this call returns — sends happen inline).
     */
    void on_frame(std::string_view inbound_name, std::span<const std::byte> frame);

    /** @brief The connection registry (test introspection — the shared demux table). */
    [[nodiscard]] const child_registry_t& registry() const noexcept { return registry_; }

   private:
    /**
     * @brief Route one OWNING inbound frame from a rope-delivering link
     *        (ADR-0042 §1, generalized per ADR-0053).
     *
     * A single-link rope (every current producer) runs the same routing as
     * @ref on_frame over the link's bytes span — the forward hop is untouched
     * (offset-dispatch, zero heap) — and threads the owning view to the
     * terminus, where a big trailer-less WRITE payload may be stored as a
     * subview of it (refcount pin, zero copy) under the vertex's
     * `store_ref_min_bytes` policy. A multi-link rope routes a FORWARD hop
     * directly over the rope cursor (ADR-0053 ④b): the dispatch offsets are read
     * through the link-walking @ref wire::grammar::rope_cursor and the egress
     * scatter-gathers the untouched links — no flatten. Only a terminus / reply /
     * control frame, which still needs a contiguous decode (the rope-aware sink is
     * the migration ⑤/⑥ follow-on), pays the one flatten fallback.
     */
    void on_frame_rope(std::string_view inbound_name, view::rope_t frame);
    /** @brief The shared routing body: @p frame_view is the owning frame when the
     *         link delivers ropes (nullptr on the borrowed-span path). */
    void on_frame_impl(std::string_view inbound_name, std::span<const std::byte> frame,
                       const view::view_t* frame_view);
    /**
     * @brief Terminus: arena-decode @p frame (ADR-0041) and resolve + reply.
     *
     * The arena draws directly from the injected `mr_` (ADR-0039 §1) and is
     * released before returning — the memory policy is entirely the host's. The
     * FWD{REPLY} is sent back over the link the request arrived on. @p frame_view
     * (non-null on the owning-delivery path) is threaded into the resolver for
     * the ADR-0042 §3 referenced WRITE store.
     */
    void resolve_terminus(std::string_view inbound_name, std::span<const std::byte> frame,
                          const view::view_t* frame_view);
    /**
     * @brief Terminus over a MULTI-LINK rope: resolve straight off the rope, NO flatten.
     *
     * The owning rope-tier twin of `resolve_terminus` (ADR-0053 ④b / 3c-iii):
     * a request FWD reassembled as a scatter-gather rope (fragmented WS / CAN) is
     * adopted as a lazy @ref wire::tlv_view_t and resolved through the view-tier
     * `op_resolver_t::resolve(tlv_view_t)` — the interim flatten this replaces is
     * deleted. The lazy view defers CRC, so integrity is verified here
     * (verify-all-then-apply, ADR-0053 §4) before the op mutates state, matching the
     * arena terminus's `decode_into(VERIFY)`. The ADR-0042 §3 referenced store needs
     * a contiguous frame view, so the rope tier stores its one ownership copy (no
     * `frame_view`). Reply routes back over @p inbound_name exactly as the arena path.
     */
    void resolve_terminus_rope(std::string_view inbound_name, view::rope_t frame);
    /**
     * @brief The forward hop, read entirely by OFFSET — no decoded tree (ADR-0038 inv. #1).
     *
     * Strips the leading `dst` segment, prepends the inbound-link NAME to `src` (unless a
     * REPLY), and scatter-gather-sends onward via @p child. @p child is the transport the
     * first `dst` segment already resolved to. Templated over the grammar `Cursor` (ADR-0053
     * ④b): a @ref wire::grammar::span_cursor reads a contiguous frame (byte-identical to the
     * pre-rope path, zero heap — a stack `iov` array), a @ref wire::grammar::rope_cursor reads
     * a scatter-gather frame (the egress gathers each region's per-link sub-spans into a
     * `std::pmr::vector` drawn from the injected `mr_` — still no payload copy).
     *
     * @tparam Cursor A grammar byte-source cursor (span or rope).
     * @param cur     The cursor positioned at the inbound FWD frame's first byte.
     */
    template <class Cursor>
    void route_fwd_forward(std::string_view inbound_name, const Cursor& cur, transport_t& child);
    /**
     * @brief Dispatch a multi-link control frame (ADVERTISE / COMPACT / HANDLE_NACK)
     *        rope-native (ADR-0055 §2).
     *
     * Reads the outer type + `u16` label straight off the scatter-gather @p frame via a
     * @ref wire::grammar::rope_cursor — NO whole-frame flatten. A `HANDLE_NACK` acts on the
     * label alone (zero materialize); ADVERTISE / COMPACT materialize **only** the child
     * sub-rope a handler genuinely needs contiguous (the route to strip+re-encode, the
     * payload to store/forward — an ADR-0052 legitimate egress/store boundary), never the
     * whole frame. A contiguous (single-link) control frame never reaches here — it decodes
     * eagerly in `on_frame_impl`. This is the sink that let the interim
     * `on_frame_rope` whole-frame flatten be deleted (ADR-0053 ⑥ / ADR-0055 §3).
     */
    void on_control_rope(std::string_view inbound_name, view::rope_t frame);
    /** @brief Learn (or re-advertise downstream) a `label ↔ route` binding (RFC-0004 §E.1). */
    void on_advertise(std::string_view inbound_name, std::uint16_t label, const wire::tlv_t& route);
    /** @brief Forward (swap label) or locally deliver a label-compacted COMPACT payload. */
    void on_compact(std::string_view inbound_name, std::uint16_t label,
                    std::span<const std::byte> payload_bytes);
    /** @brief Re-advertise an egress binding in response to a downstream HANDLE_NACK. */
    void on_nack(std::string_view inbound_name, std::uint16_t label);
    /** @brief Resolve a bound local route and apply the delivered write (delivery-is-a-write). */
    [[nodiscard]] bool deliver_local(std::span<const std::byte> route_path,
                                     std::span<const std::byte> payload);
    /**
     * @brief The graph remote-delivery sink (#136): emit one producer delivery to @p sub.
     *
     * Sends a full-route `FWD{WRITE, dst=return_route, payload}` by default, or — when
     * `sub.delivery_compact` — lazily advertises a label once for the flow then streams a
     * lean `COMPACT` (RFC-0004 §D/§E.1). Fires on the writer thread (outside the vertex
     * lock); all label state is in the mutex-guarded @ref route_handle_t.
     */
    void deliver_remote(const graph::remote_delivery_t& sub, const view::rope_t& value);

    graph::graph_t& graph_;
    graph::op_resolver_t resolver_;
    std::pmr::memory_resource* mr_;  // terminus-arena spill resource (ADR-0039 §1)
    child_registry_t registry_;      // the one NAME→link demux table (Brick 3a, ADR-0037)
    route_handle_t handles_;         // per-link label tables (compact flows only)
    std::function<void(const view::rope_t&)> reply_cb_;
    std::function<void(std::string_view, const wire::tlv_t&)> inbound_cb_;
    std::function<void(std::string_view, std::span<const std::byte>)> raw_cb_;
    std::function<void(std::span<const std::byte>, std::span<const std::byte>)> delivery_cb_;
    std::function<void(std::string_view, std::uint16_t)> stale_cb_;
};

}  // namespace tr::net
