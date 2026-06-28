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
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/frame.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/op_resolve.hpp"
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
    /** @brief Bind to the local @p graph; terminus ops resolve against it. */
    explicit fwd_router_t(graph::graph_t& graph) : graph_(graph), resolver_(graph) {}

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

    graph::graph_t& graph_;
    graph::op_resolver_t resolver_;
    std::vector<child_t> children_;  // immutable after setup — no lock on the hot path
    std::function<void(const wire::tlv_t&)> reply_cb_;
    std::function<void(std::string_view, const wire::tlv_t&)> inbound_cb_;
};

}  // namespace tr::net
