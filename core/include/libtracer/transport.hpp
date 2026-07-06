/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The transport seam: one wire technology behind a uniform interface. The FWD
 * router sends framed bytes (a complete TLV, typically a FWD frame) via send(),
 * and receives inbound frames through a registered receiver callback (which may
 * fire on an internal transport thread). This C++ seam is callback + recv-thread
 * (docs/reference/10 §"Transport ↔ L4: tr::Transport"), an implementation choice
 * (ADR-0013) that matches how a real socket transport's receive loop feeds the
 * FWD router. A transport never sees TLV semantics — only framed bytes.
 */
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

// A 16-byte node/peer identity — the ROUTER `origin_peer_id` (docs/reference/05
// §0x0D ROUTER).
using peer_id_t = std::array<std::byte, 16>;

class transport_t;

/**
 * @brief The optional multi-peer (bus) capability of a transport link (ADR-0044).
 *
 * A point-to-point link (ws/tcp/udp/quic) carries exactly one peer, so its child
 * NAME fully addresses the far side. A BUS link (CAN) reaches many peers over one
 * wire; this interface is how such a link exposes them to the routing plane with
 * ZERO stored graph state (ADR-0044 §1 — no vertex is ever created for a peer):
 *
 *  - @ref enumerate_peers synthesizes, on the fly, the names of the peers
 *    currently audible on the bus (from the transport kind's own live
 *    announce/heartbeat traffic) — the `:children[]` listing of the link's
 *    connection vertex;
 *  - @ref peer_link resolves one such NAME to a directed sending endpoint, the
 *    seam `child_registry_t` falls back to when a FWD's next `dst` segment
 *    names no static child — so a peer name IS a routable hop segment;
 *  - @ref set_peer_receiver replaces the flat inbound sink with a peer-named
 *    one: each inbound frame arrives tagged with the SENDING peer's name, which
 *    the router uses as the hop's inbound NAME — so the return route grown into
 *    `src` names the bus peer to route the reply back to, symmetrically, with no
 *    per-request state.
 *
 * Peer names are transport-defined but MUST be deterministic and collision-safe
 * within the bus (the CAN binding derives them from the structured ID's `node`
 * field). All three calls may race the transport's receive thread; impls
 * synchronize internally.
 */
class bus_link_t {
   public:
    /** @brief Visitor invoked once per currently-audible peer name. */
    using peer_visitor_t = std::function<void(std::string_view)>;
    /** @brief The peer-named inbound sink: (sending peer's name, frame bytes). */
    using peer_receiver_t = std::function<void(std::string_view, std::span<const std::byte>)>;
    /** @brief The OWNING peer-named sink (ADR-0053 §5): (sending peer's name, the
     *         reassembled frame as the rope it already is — refcounted links the
     *         receiver may keep, subrope, or forward past the callback). */
    using peer_rope_receiver_t = std::function<void(std::string_view, view::rope_t)>;

    /**
     * @brief Visit the peers currently audible on the bus (a live-traffic snapshot).
     * @note Synthesized on the fly — no call allocates peer state or graph structure.
     */
    virtual void enumerate_peers(const peer_visitor_t& visit) const = 0;

    /**
     * @brief Resolve a peer NAME to a directed sending endpoint on this bus.
     *
     * The returned transport sends to THAT peer only (the bus binding's directed
     * framing); it is owned by this link and stays valid for the link's lifetime.
     * @retval nullptr @p peer names no currently-known bus peer.
     */
    [[nodiscard]] virtual transport_t* peer_link(std::string_view peer) = 0;

    /**
     * @brief Register the peer-named inbound sink (used INSTEAD of `set_receiver`).
     *
     * Must be set before frames flow; delivery may occur on an internal transport
     * thread. When set, it takes precedence over a flat @ref transport_t receiver.
     */
    virtual void set_peer_receiver(peer_receiver_t receiver) = 0;

    /**
     * @brief Register the OWNING peer-named sink (ADR-0053 §5) — used INSTEAD of
     *        @ref set_peer_receiver when the bus @ref delivers_ropes.
     *
     * A reassembling bus (CAN groups, fragmented WS) hands the frame up as the
     * rope its reassembly already built — chained refcounted slice views, never a
     * flatten memcpy; transport padding is trimmed by shortening the tail link.
     * The base implementation is a documented no-op, the same honesty rule as
     * `transport_t::set_rope_receiver`: a span-only bus must not fake ownership.
     */
    virtual void set_peer_rope_receiver(peer_rope_receiver_t receiver) { (void)receiver; }

    /** @brief True iff this bus honors @ref set_peer_rope_receiver (ADR-0053 §5). */
    [[nodiscard]] virtual bool delivers_ropes() const { return false; }

   protected:
    ~bus_link_t() = default;  // never deleted through this facet
};

/**
 * @brief A point-to-point (or bus-facet-exposing) transport link: the byte seam
 *        between the routing plane and one wire (ws/tcp/udp/quic/CAN).
 *
 * The router sends complete TLV frames via @ref send and receives them through an
 * installed sink (@ref set_receiver for borrowed spans, or @ref set_rope_receiver
 * for owning refcounted rope frames when @ref delivers_ropes is true). A multi-peer
 * bus link additionally exposes a @ref bus_link_t facet via @ref bus.
 */
class transport_t {
   public:
    /** @brief The borrowed-span inbound sink: a frame valid only for the callback. */
    using receiver_t = std::function<void(std::span<const std::byte>)>;
    /** @brief The OWNING inbound sink (ADR-0042, generalized to ropes per
     *         ADR-0053): each frame is a `rope_t` of refcounted links the receiver
     *         may keep, subrope, or forward — a contiguous frame is the trivial
     *         single-link case ("delivers views" and "delivers ropes" are ONE
     *         capability, not two tiers — CONTEXT.md §ingress rope delivery). */
    using rope_receiver_t = std::function<void(view::rope_t)>;

    virtual ~transport_t() = default;

    /** @brief Emit one frame (a complete TLV's bytes) onto the wire. */
    virtual void send(std::span<const std::byte> frame) = 0;

    /**
     * @brief Scatter-gather send: emit the gathered spans as ONE frame, no flatten copy.
     *
     * Hand a rope's `to_iovec()` straight to the wire. The default gathers into a
     * temporary and calls @ref send(std::span<const std::byte>); transports with native
     * scatter-gather (sendmsg/writev/RDMA SGE) override this to avoid the copy.
     * @param iov The spans to emit, in order, as a single frame.
     */
    virtual void send(std::span<const std::span<const std::byte>> iov) {
        std::size_t total = 0;
        for (const auto& s : iov) total += s.size();
        std::vector<std::byte> tmp;
        tmp.reserve(total);
        for (const auto& s : iov) tmp.insert(tmp.end(), s.begin(), s.end());
        send(std::span<const std::byte>(tmp));
    }

    /**
     * @brief Register the borrowed-span sink for inbound frames (the bridge's ingest).
     *
     * Must be set before frames flow; delivery may occur on an internal transport
     * thread. The delivered span is valid only for the callback — a receiver that needs
     * to keep the frame uses @ref set_rope_receiver instead.
     * @param receiver The inbound frame sink.
     */
    virtual void set_receiver(receiver_t receiver) = 0;

    /**
     * @brief Register the optional OWNING inbound sink (the ADR-0042 receiver seam).
     *
     * A transport that can hand up owning frames (its @ref delivers_ropes returns
     * true) delivers each inbound frame to @p receiver as a `view::rope_t` whose
     * links are refcounted views over segments drawn from a host-injected
     * `mem_backend_t` — the receiver may pin, subrope, or forward the frame beyond
     * the callback (unlike the borrowed span of @ref set_receiver, which dies when
     * the callback returns). A contiguous frame arrives as a single-link rope; a
     * scattered one (a CAN reassembly group, fragmented WS message) crosses this
     * seam AS THE ROPE IT ALREADY IS — reassembly is chaining views, never a
     * memcpy (ADR-0053 §5). Must be set before frames flow; delivery may occur on
     * an internal transport thread.
     *
     * The base implementation is a documented no-op: a span-only transport ignores
     * an installed rope receiver, honestly — there is NO adapter that wraps a
     * borrowed span into a rope whose refcounts would lie about lifetime
     * (ADR-0042 §1). A transport that honors this seam MUST also override
     * @ref delivers_ropes to return true, so `fwd_router_t::add_child` installs
     * the receiver matching the link's capability.
     */
    virtual void set_rope_receiver(rope_receiver_t receiver) { (void)receiver; }

    /**
     * @brief The owning-delivery capability (ADR-0042 §1): true iff this transport
     *        honors @ref set_rope_receiver by delivering refcounted rope frames.
     */
    [[nodiscard]] virtual bool delivers_ropes() const { return false; }

    /**
     * @brief The multi-peer (bus) capability (ADR-0044): non-null iff this link
     *        reaches many peers and exposes them via @ref bus_link_t.
     *
     * A point-to-point transport keeps the default nullptr; a bus transport (the
     * CAN binding) returns its own @ref bus_link_t facet, which the router and the
     * connection vertex consult for peer resolution and peer enumeration.
     */
    [[nodiscard]] virtual bus_link_t* bus() { return nullptr; }
};

}  // namespace tr::net
