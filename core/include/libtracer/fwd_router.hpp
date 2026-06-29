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
 * routes. This places "forward resolution at the L4 router + the tr::net transport
 * seam" (ADR-0035), mirroring bridge_t's graph<->transport wiring.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
     */
    explicit fwd_router_t(graph::graph_t& graph) : graph_(graph), resolver_(graph) {
        graph_.set_remote_delivery_sink(
            [this](const graph::remote_delivery_t& sub, const view::view_t& value) {
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
     * @param name This node's local NAME for the link (e.g. "up", "cli").
     * @param link The transport carrying the next/previous hop.
     */
    void add_child(std::string name, transport_t& link);

    /**
     * @brief Set the sink for a REPLY that terminates at this node's reply endpoint.
     *
     * Invoked (with the decoded FWD{REPLY}) when a REPLY's first `dst` segment does
     * not name a transport child — i.e. the accumulated return route has been fully
     * consumed and this node is the originator. Optional; absent ⇒ such a reply is
     * dropped.
     *
     * @param cb Callback invoked on a transport receive thread; keep it cheap.
     */
    void on_reply(std::function<void(const wire::tlv_t&)> cb);

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

   private:
    struct child_t {
        std::string name;
        transport_t* link;
    };

    /** @brief The child whose NAME equals the raw segment bytes @p seg (nullptr if none). */
    [[nodiscard]] transport_t* child_by_segment(std::span<const std::byte> seg) const;
    /** @brief The link a frame arrived on, by this node's NAME for it (nullptr if unknown). */
    [[nodiscard]] transport_t* link_by_name(std::string_view name) const;

    /** @brief The slice-3 stateless FWD forward/terminus/reply step (decoded @p dec). */
    void route_fwd(std::string_view inbound_name, std::span<const std::byte> frame,
                   const wire::tlv_t& dec);
    /** @brief Learn (or re-advertise downstream) a `label ↔ route` binding (RFC-0004 §E.1). */
    void on_advertise(std::string_view inbound_name, const wire::tlv_t& adv);
    /** @brief Forward (swap label) or locally deliver a label-compacted COMPACT. */
    void on_compact(std::string_view inbound_name, const wire::tlv_t& comp);
    /** @brief Re-advertise an egress binding in response to a downstream HANDLE_NACK. */
    void on_nack(std::string_view inbound_name, const wire::tlv_t& nack);
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
    void deliver_remote(const graph::remote_delivery_t& sub, const view::view_t& value);

    graph::graph_t& graph_;
    graph::op_resolver_t resolver_;
    std::vector<child_t> children_;  // immutable after setup — no lock on the hot path
    route_handle_t handles_;         // per-link label tables (compact flows only)
    std::function<void(const wire::tlv_t&)> reply_cb_;
    std::function<void(std::string_view, const wire::tlv_t&)> inbound_cb_;
    std::function<void(std::string_view, std::span<const std::byte>)> raw_cb_;
    std::function<void(std::span<const std::byte>, std::span<const std::byte>)> delivery_cb_;
    std::function<void(std::string_view, std::uint16_t)> stale_cb_;
};

}  // namespace tr::net
