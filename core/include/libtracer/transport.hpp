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
#include <concepts>
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/receiver_slot.hpp"
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
    /** @brief The peer-named inbound sink fn: (ctx, sending peer's name, frame bytes). */
    using peer_receiver_fn_t = receiver_slot_t<std::string_view>::span_fn_t;
    /** @brief The OWNING peer-named sink fn (ADR-0053 §5): (ctx, sending peer's
     *         name, the reassembled frame as the rope it already is — refcounted
     *         links the receiver may keep, subrope, or forward past the callback). */
    using peer_rope_receiver_fn_t = receiver_slot_t<std::string_view>::rope_fn_t;

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
     * @brief Close one peer's connection by NAME, freeing its slot for reuse.
     *
     * Tears down exactly the peer @p peer names, exactly as a remote hangup would:
     * the recycle is asynchronous (the link's own receive loop observes the close
     * and reclaims the slot), so @ref enumerate_peers stops listing it shortly
     * after this returns true. A point-to-point kind (the default) has no
     * per-peer teardown and returns false; a bus link that supports directed
     * teardown overrides this.
     * @retval true  @p peer named an open connection and its teardown was initiated.
     * @retval false @p peer names no open peer, or this kind cannot close one peer.
     */
    [[nodiscard]] virtual bool close_peer(std::string_view peer) {
        (void)peer;
        return false;
    }

    /** @brief The peer-departure notifier fn: (ctx, the departed peer's NAME). */
    using peer_down_fn_t = void (*)(void* ctx, std::string_view peer);

    /**
     * @brief Register the peer-departure notifier — the bus half of the link-teardown
     *        eviction seam (RFC-0009 §D extended to peer departure).
     *
     * The bus adapter invokes it (possibly on an internal transport thread) each time a
     * peer's session dies — remote hangup, protocol CLOSE, or a teardown initiated by
     * @ref close_peer — carrying the NAME the peer was audible under (the same NAME inbound
     * frames were tagged with, i.e. the routing plane's inbound link name for that peer).
     * `fwd_router_t::add_child` installs a notifier that evicts the departed peer's
     * subscriber edges and label state (`fwd_router_t::link_down`). Must be set before
     * frames flow, like the receivers; a kind with no departure concept simply never
     * fires it.
     * @param fn  The notifier; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible notification.
     */
    void set_peer_down_notifier(peer_down_fn_t fn, void* ctx) noexcept {
        peer_down_ctx_ = ctx;
        peer_down_fn_ = fn;
    }

    /**
     * @brief Register the peer-named inbound sink (used INSTEAD of `set_receiver`).
     *
     * Must be set before frames flow; delivery may occur on an internal transport
     * thread. When set, it takes precedence over a flat @ref transport_t receiver.
     * @param fn  The sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible delivery.
     */
    void set_peer_receiver(peer_receiver_fn_t fn, void* ctx) noexcept { peer_rx_.set(fn, ctx); }

    /**
     * @brief Register the peer-named inbound sink from a caller-owned callable.
     *
     * Zero-erasure sugar over the `{fn, ctx}` form: @p sink is bound by address
     * (lvalues only — a temporary would dangle) and MUST outlive every delivery.
     */
    template <typename F>
        requires std::invocable<F&, std::string_view, std::span<const std::byte>>
    void set_peer_receiver(F& sink) noexcept {
        peer_rx_.set([](void* c, std::string_view peer,
                        std::span<const std::byte> f) { (*static_cast<F*>(c))(peer, f); },
                     &sink);
    }

    /**
     * @brief Register the OWNING peer-named sink (ADR-0053 §5) — used INSTEAD of
     *        @ref set_peer_receiver when the bus @ref delivers_ropes.
     *
     * A reassembling bus (CAN groups, fragmented WS) hands the frame up as the
     * rope its reassembly already built — chained refcounted slice views, never a
     * flatten memcpy; transport padding is trimmed by shortening the tail link.
     * A span-only bus never dispatches to this sink (the honesty rule of
     * `transport_t::set_rope_receiver`): install per @ref delivers_ropes.
     * @param fn  The sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible delivery.
     */
    void set_peer_rope_receiver(peer_rope_receiver_fn_t fn, void* ctx) noexcept {
        peer_rx_.set_rope(fn, ctx);
    }

    /**
     * @brief Register the OWNING peer-named sink from a caller-owned callable.
     *
     * Zero-erasure sugar over the `{fn, ctx}` form: @p sink is bound by address
     * (lvalues only — a temporary would dangle) and MUST outlive every delivery.
     */
    template <typename F>
        requires std::invocable<F&, std::string_view, view::rope_t>
    void set_peer_rope_receiver(F& sink) noexcept {
        peer_rx_.set_rope([](void* c, std::string_view peer,
                             view::rope_t f) { (*static_cast<F*>(c))(peer, std::move(f)); },
                          &sink);
    }

    /** @brief True iff this bus delivers OWNING ropes to the peer-named rope sink
     *         (ADR-0053 §5). */
    [[nodiscard]] virtual bool delivers_ropes() const { return false; }

   protected:
    ~bus_link_t() = default;  // never deleted through this facet

    /**
     * @brief Fire the peer-departure notifier for @p peer (no-op when none installed).
     *
     * The bus adapter calls this from the thread that OBSERVED the departure, after its
     * own slot bookkeeping is done and with none of its internal locks held — the
     * notifier re-enters the routing plane (router → graph), which takes graph locks.
     */
    void notify_peer_down(std::string_view peer) const {
        if (peer_down_fn_ != nullptr) peer_down_fn_(peer_down_ctx_, peer);
    }

    /** @brief The peer-named delivery-tier slot (the ONE tier-select mechanism);
     *         the bus adapter's receive path dispatches through it. */
    receiver_slot_t<std::string_view> peer_rx_;

   private:
    peer_down_fn_t peer_down_fn_ = nullptr; /**< @brief Installed peer-departure sink. */
    void* peer_down_ctx_ = nullptr;         /**< @brief Its caller-owned context. */
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
    /** @brief The borrowed-span inbound sink fn: (ctx, frame) — the frame is
     *         valid only for the callback. */
    using receiver_fn_t = receiver_slot_t<>::span_fn_t;
    /** @brief The OWNING inbound sink fn (ADR-0042, generalized to ropes per
     *         ADR-0053): (ctx, frame) — each frame is a `rope_t` of refcounted
     *         links the receiver may keep, subrope, or forward — a contiguous
     *         frame is the trivial single-link case ("delivers views" and
     *         "delivers ropes" are ONE capability, not two tiers — CONTEXT.md
     *         §ingress rope delivery). */
    using rope_receiver_fn_t = receiver_slot_t<>::rope_fn_t;

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
     * @param fn  The inbound frame sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible delivery.
     */
    void set_receiver(receiver_fn_t fn, void* ctx) noexcept { rx_.set(fn, ctx); }

    /**
     * @brief Register the borrowed-span sink from a caller-owned callable.
     *
     * Zero-erasure sugar over the `{fn, ctx}` form: @p sink is bound by address
     * (lvalues only — a temporary would dangle) and MUST outlive every delivery.
     */
    template <typename F>
        requires std::invocable<F&, std::span<const std::byte>>
    void set_receiver(F& sink) noexcept {
        rx_.set([](void* c, std::span<const std::byte> f) { (*static_cast<F*>(c))(f); }, &sink);
    }

    /**
     * @brief Register the optional OWNING inbound sink (the ADR-0042 receiver seam).
     *
     * A transport that can hand up owning frames (its @ref delivers_ropes returns
     * true) delivers each inbound frame to the sink as a `view::rope_t` whose
     * links are refcounted views over segments drawn from a host-injected
     * `mem_backend_t` — the receiver may pin, subrope, or forward the frame beyond
     * the callback (unlike the borrowed span of @ref set_receiver, which dies when
     * the callback returns). A contiguous frame arrives as a single-link rope; a
     * scattered one (a CAN reassembly group, fragmented WS message) crosses this
     * seam AS THE ROPE IT ALREADY IS — reassembly is chaining views, never a
     * memcpy (ADR-0053 §5). Must be set before frames flow; delivery may occur on
     * an internal transport thread.
     *
     * A span-only transport never dispatches to this sink, honestly — there is NO
     * adapter that wraps a borrowed span into a rope whose refcounts would lie
     * about lifetime (ADR-0042 §1). A transport that honors this seam MUST
     * override @ref delivers_ropes to return true, so `fwd_router_t::add_child`
     * installs the receiver matching the link's capability.
     * @param fn  The owning frame sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible delivery.
     */
    void set_rope_receiver(rope_receiver_fn_t fn, void* ctx) noexcept { rx_.set_rope(fn, ctx); }

    /**
     * @brief Register the OWNING sink from a caller-owned callable.
     *
     * Zero-erasure sugar over the `{fn, ctx}` form: @p sink is bound by address
     * (lvalues only — a temporary would dangle) and MUST outlive every delivery.
     */
    template <typename F>
        requires std::invocable<F&, view::rope_t>
    void set_rope_receiver(F& sink) noexcept {
        rx_.set_rope([](void* c, view::rope_t f) { (*static_cast<F*>(c))(std::move(f)); }, &sink);
    }

    /**
     * @brief The owning-delivery capability (ADR-0042 §1): true iff this transport
     *        honors @ref set_rope_receiver by delivering refcounted rope frames.
     */
    [[nodiscard]] virtual bool delivers_ropes() const { return false; }

    /** @brief The link-down notifier fn: (ctx) — the link carries its own identity via ctx. */
    using down_fn_t = void (*)(void* ctx);

    /**
     * @brief Register the link-down notifier — the point-to-point half of the
     *        link-teardown eviction seam (RFC-0009 §D extended to peer departure).
     *
     * The transport invokes it (possibly on an internal transport thread) when its ONE
     * connection dies — remote hangup, protocol CLOSE, or a fatal receive error.
     * `fwd_router_t::add_child` installs a notifier that evicts the child's subscriber
     * edges and label state under the child's registered NAME (`fwd_router_t::
     * link_down`). Must be set before frames flow, like the receivers; a connectionless
     * kind (UDP) has no closure concept and never fires it. Fire with no internal
     * transport locks held — the notifier re-enters the routing plane, which takes
     * graph locks.
     * @param fn  The notifier; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible notification.
     */
    void set_down_notifier(down_fn_t fn, void* ctx) noexcept {
        down_ctx_ = ctx;
        down_fn_ = fn;
    }

   protected:
    /** @brief Fire the link-down notifier (no-op when none installed) — see
     *         @ref set_down_notifier for the calling discipline. */
    void notify_down() const {
        if (down_fn_ != nullptr) down_fn_(down_ctx_);
    }

    /** @brief The delivery-tier slot (the ONE tier-select mechanism, ADR-0042 /
     *         ADR-0053): adapters dispatch inbound frames through it —
     *         `rx_.deliver(view)` for owning frames, `rx_.deliver_borrowed(span)`
     *         for borrowed ones — and key receive-buffer strategy off
     *         `rx_.has_rope()`. */
    receiver_slot_t<> rx_;

   private:
    down_fn_t down_fn_ = nullptr; /**< @brief Installed link-down sink. */
    void* down_ctx_ = nullptr;    /**< @brief Its caller-owned context. */

   public:
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
