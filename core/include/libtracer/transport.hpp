/**
 * @file
 * @brief The transport seam: one wire technology behind a uniform send / receive interface.
 *
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
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

#include "libtracer/config.hpp"
#include "libtracer/function_ref.hpp"
#include "libtracer/iov_table.hpp"
#include "libtracer/link_kind.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/peer_handle.hpp"
#include "libtracer/receiver_slot.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/value.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

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
 *    one: each inbound frame arrives tagged with the sending peer's @ref
 *    peer_handle_t, from which @ref peer_name resolves the hop's inbound NAME —
 *    so the return route grown into `src` names the bus peer to route the reply
 *    back to, symmetrically, with no per-request state.
 *
 * Peer names are transport-defined but MUST be deterministic and collision-safe
 * within the bus (the CAN binding derives them from the structured ID's `node`
 * field). All three calls may race the transport's receive thread; impls
 * synchronize internally.
 *
 * **The per-frame identity is the HANDLE, not the name (#1294).** The name is the
 * ADDRESSING surface — @ref enumerate_peers, @ref peer_link and @ref close_peer
 * still speak it, because a name is what a routable `dst` segment carries. The
 * inbound seam speaks handles, because a name is a string a consumer would have to
 * re-derive an identity from on every frame. @ref peer_name is the one bridge
 * between them.
 */
class bus_link_t {
   public:
    /** @brief Visitor invoked once per currently-audible peer name — a synchronous,
     *         non-owning callable reference (ADR-0083 Q10): passing a lambda costs nothing. */
    using peer_visitor_t = function_ref_t<void(std::string_view)>;
    /** @brief The peer-named inbound sink fn: (ctx, sending peer's HANDLE, frame bytes). */
    using peer_receiver_fn_t = receiver_slot_t<peer_handle_t>::span_fn_t;
    /** @brief The OWNING peer-named sink fn (ADR-0053 §5): (ctx, sending peer's
     *         HANDLE, the reassembled frame as the rope it already is — refcounted
     *         links the receiver may keep, subrope, or forward past the callback). */
    using peer_rope_receiver_fn_t = receiver_slot_t<peer_handle_t>::rope_fn_t;

    /**
     * @brief Visit the peers currently audible on the bus (a live-traffic snapshot).
     * @note Synthesized on the fly — no call allocates peer state or graph structure.
     */
    virtual void enumerate_peers(const peer_visitor_t& visit) const = 0;

    /**
     * @brief Resolve a peer HANDLE back to the peer NAME it addresses — the ONE bridge
     *        between the handle the inbound seam carries and the name the routing plane
     *        grows into `src` (#1294).
     *
     * Every kind answers this as a PURE FUNCTION of the handle's index, because every
     * kind's peer name already is one: `slot_server_t` names a peer `p<slot>` for the slot
     * it landed in, and @ref can_transport_t names one `n<node>` for its bus node id. So the
     * call takes no lock, allocates nothing, and is safe to make from the delivery callback
     * on the transport's own receive thread — which is where the router makes it, once per
     * inbound frame, exactly where the name used to arrive for free.
     *
     * Being positional, the answer is about the SLOT and not about the session that
     * occupies it — the same distinction @ref peer_link documents. A caller that wants the
     * SESSION's identity holds the handle, whose generation is what tells the two apart.
     *
     * @param peer    The handle a delivery was tagged with.
     * @param scratch Caller-owned characters the impl MAY format into; at least
     *                `kPeerNameChars`. The returned view points either into @p scratch
     *                or into storage the link owns for its lifetime, so it is valid for as
     *                long as BOTH survive.
     * @retval {} @p peer is not @ref peer_handle_t::valid, or names no peer of this kind.
     */
    [[nodiscard]] virtual std::string_view peer_name(peer_handle_t peer,
                                                     std::span<char> scratch) const = 0;

    /**
     * @brief Resolve a peer NAME to a directed sending endpoint on this bus.
     *
     * The returned transport sends to THAT peer only (the bus binding's directed
     * framing); it is owned by this link and stays valid for the link's lifetime.
     *
     * RESOLVE PER USE — never cache the pointer across a possible departure (#1153).
     * Pointer VALIDITY and peer IDENTITY are two different guarantees, and only the
     * first holds for every kind. Where a kind names peers POSITIONALLY, the endpoint
     * is scoped to the SLOT, not to the session that occupied it: after the named peer
     * departs, a pointer resolved for it addresses whatever session inherits the slot,
     * and the endpoint's own liveness check is satisfied by that stranger. The pointer
     * never dangles; it silently changes who it means. A caller that re-resolves before
     * each send is unexposed, which is why no production caller is affected today —
     * `child_registry_t` resolves and sends in one expression, and a remote subscriber
     * edge stores the peer's handle rather than this pointer (@ref peer_link_of).
     *
     * Which kinds are exposed follows from the naming regime alone:
     *  - IDENTITY-derived names are immune — @ref can_transport_t names a peer `n<node-id>`
     *    for its own bus node id, so the name, the table key and the endpoint are one
     *    identity that no other peer can inherit.
     *  - POSITIONAL names are exposed — @ref slot_server_t names a peer `p<slot>` for the
     *    slot index it landed in, and slots are recycled in place.
     *
     * @retval nullptr @p peer names no currently-known bus peer.
     */
    [[nodiscard]] virtual transport_t* peer_link(std::string_view peer) = 0;

    /**
     * @brief Resolve a peer HANDLE to a directed sending endpoint on this bus — the
     *        @ref peer_link a remote subscriber's delivery asks, with the name taken out
     *        (#1941).
     *
     * Answers exactly what @ref peer_link answers for `peer_name(peer)`, and is scoped the
     * same way: to the SLOT the handle's index names, whatever session occupies it now, the
     * same contract the name always gave a stored edge. The same resolve-per-use rule holds.
     *
     * The default IS that composition, so a kind that overrides nothing behaves exactly as
     * before. A kind whose table is indexed by the handle overrides it to skip the name.
     *
     * @retval nullptr @p peer names no currently-known bus peer.
     */
    [[nodiscard]] virtual transport_t* peer_link_of(peer_handle_t peer) {
        std::array<char, kPeerNameChars> scratch{};
        const std::string_view name = peer_name(peer, scratch);
        return name.empty() ? nullptr : peer_link(name);
    }

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

    /**
     * @brief The MODE AUTHORITY: true iff this link's peer-named tier exists (#889).
     *
     * A kind that is a bus by construction (the CAN binding) keeps the default `true`.
     * A kind whose multi-peer surface is a WIRING-TIME choice — the tcp/ws listeners,
     * constructed `peer_named` or FLAT — reports that choice here, and its
     * `transport_t::bus()` returns null for the same reason: without the facet the link
     * keeps point-to-point hop naming, inbound frames carry the registered child NAME,
     * and `send()` fans out to every open peer.
     *
     * Each of the six peer-named wiring calls declared below — @ref set_peer_receiver and
     * @ref set_peer_rope_receiver (both spellings each) and @ref set_peer_down_notifier and
     * @ref set_peer_up_notifier — passes this gate, so a link that reports false ends up with
     * an empty `peer_rx_` and neither peer-lifecycle notifier. (A DERIVED class can still reach the
     * protected `peer_rx_` directly; the gate governs this interface's own doors.)
     * It is a query, not a knob: `bus_link_t` is a PUBLIC base, so a flat link's
     * `set_peer_receiver` is reachable by an explicit upcast past the null `bus()`, and
     * before this gate that call silently flipped the link into peer-named delivery the
     * `bus() == nullptr` contract said did not exist.
     *
     * A kind whose mode is CONSTRUCTED — the tcp/ws listeners, i.e. `slot_server_t` —
     * additionally routes its per-frame tier select and its departure seam through the same
     * flag, so for those two "which mode is this link in" has one answer. A kind that is a
     * bus outright keeps its own delivery precedence (the CAN binding still falls back to
     * the flat sink for a single-peer consumer that wired no bus facet), which this gate
     * does not disturb: `peer_named()` is true there.
     * @note Cold path only (wiring frequency, ADR-0047 §4) — an implementation's own
     *       per-frame tier select reads its stored mode directly, never this virtual.
     */
    [[nodiscard]] virtual bool peer_named() const noexcept { return true; }

    /** @brief The peer-departure notifier fn: (ctx, the departed peer's HANDLE, its NAME).
     *         The handle is the one minted at arrival and is RETIRED by this call — after
     *         it the link may hand the same index back at a higher generation. */
    using peer_down_fn_t = void (*)(void* ctx, peer_handle_t handle, std::string_view peer);

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
     * fires it. The peer's HANDLE rides alongside the name (#1294) so a consumer that keyed
     * per-peer state by handle at arrival can drop it here without a name lookup.
     * @note REFUSED on a link that is not @ref peer_named — a flat link's departure is the
     *       whole link's (`transport_t::set_down_notifier`), so this wiring would be dead.
     * @param fn  The notifier; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible notification.
     */
    void set_peer_down_notifier(peer_down_fn_t fn, void* ctx) noexcept {
        if (!peer_named()) return;
        // Publish ctx-before-fn with a release store on fn: a transport thread
        // that observes a non-null fn (acquire, in notify_peer_down) is
        // guaranteed to see the paired ctx. Atomic because an internal transport
        // thread may fire the notifier concurrently with this wiring — a fast
        // remote hangup can beat fwd_router_t::add_child's install (the callback
        // thread is already live once the connection opens in the ctor).
        peer_down_ctx_.store(ctx, std::memory_order_relaxed);
        peer_down_fn_.store(fn, std::memory_order_release);
    }

    /** @brief The peer-ARRIVAL notifier fn: (ctx, the arriving peer's HANDLE, its NAME).
     *         This is where the handle is MINTED, so it is also where a consumer binds
     *         whatever hangs off it (an intern slot, #1266; an auth subject, #375 Part 2). */
    using peer_up_fn_t = void (*)(void* ctx, peer_handle_t handle, std::string_view peer);

    /**
     * @brief Register the peer-ARRIVAL notifier — the seam that says "this node's own accept
     *        policy just admitted a session", and the boundary ADR-0044 §Decision 1 was
     *        scoped to by its 2026-08-13 amendment (#1223).
     *
     * The mirror of @ref set_peer_down_notifier, fired from the thread that observed the
     * session become usable — for `slot_server_t` that is `accept()` for a raw stream peer
     * and the `101 Switching Protocols` publish for a WS peer, i.e. exactly the transition
     * whose inverse fires the departure notifier.
     *
     * **Only an accepting listener fires it, and that is the whole point.** An
     * announce-census bus (CAN, ADR-0030) learns of a peer from ANOTHER node's traffic, has
     * no closure event by design (RFC-0009 §D.5), and keeps §Decision 1 in full force; it
     * therefore never fires this seam and never grows a session vertex. So "does
     * this kind fire peer-up" IS the announced-peer / accepted-session line, expressed as a
     * capability rather than as a kind check at the consumer.
     *
     * `fwd_router_t::add_child` installs a notifier that registers (or REVIVES) the
     * session's identity anchor in the graph's vertex map, so the session gains an index and
     * a saturating generation. Must be set before frames flow, like the receivers.
     *
     * **An arrival is also the RE-TENANT edge for the peer's NAME (#1609).** A transport that
     * recycles names (a slot-positional `p<slot>`) fires it once per new session, at the
     * claim, and BEFORE delivering any frame of that session. The router's notifier first
     * reclaims everything still filed under the name — subscriber edges, label state,
     * pending awaits, the anchor's generation — as the departure notifier would have, so a
     * predecessor whose departure was never reported leaves nothing for the successor to
     * inherit and nothing for a later name-keyed eviction to confuse with the successor's.
     * @note REFUSED on a link that is not @ref peer_named, for the reason
     *       @ref set_peer_down_notifier is: a flat link has one routing identity for every
     *       peer it carries, so there is no per-session identity to anchor.
     * @param fn  The notifier; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible notification.
     */
    void set_peer_up_notifier(peer_up_fn_t fn, void* ctx) noexcept {
        if (!peer_named()) return;
        // Same ctx-before-fn publication as set_peer_down_notifier, and for the same
        // reason: an accept can land on the poll thread while this wiring is still running.
        peer_up_ctx_.store(ctx, std::memory_order_relaxed);
        peer_up_fn_.store(fn, std::memory_order_release);
    }

    /**
     * @brief Register the peer-named inbound sink (used INSTEAD of `set_receiver`).
     *
     * Must be set before frames flow; delivery may occur on an internal transport
     * thread. When set, it takes precedence over a flat @ref transport_t receiver.
     * @note REFUSED on a link that is not @ref peer_named (#889): a flat link has no
     *       peer-named tier to install into, and admitting the sink here is exactly the
     *       silent mode flip the null `bus()` contract denied.
     * @param fn  The sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible delivery.
     */
    void set_peer_receiver(peer_receiver_fn_t fn, void* ctx) noexcept {
        if (!peer_named()) return;
        peer_rx_.set(fn, ctx);
    }

    /**
     * @brief Register the peer-named inbound sink from a caller-owned callable.
     *
     * Zero-erasure sugar over the `{fn, ctx}` form: @p sink is bound by address
     * (lvalues only — a temporary would dangle) and MUST outlive every delivery.
     * Routed through the `{fn, ctx}` overload, so the mode gate is stated once.
     */
    template <typename F>
        requires std::invocable<F&, peer_handle_t, std::span<const std::byte>>
    void set_peer_receiver(F& sink) noexcept {
        set_peer_receiver([](void* c, peer_handle_t peer,
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
     * @note REFUSED on a link that is not @ref peer_named (#889), for the same reason
     *       @ref set_peer_receiver is.
     * @param fn  The sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible delivery.
     */
    void set_peer_rope_receiver(peer_rope_receiver_fn_t fn, void* ctx) noexcept {
        if (!peer_named()) return;
        peer_rx_.set_rope(fn, ctx);
    }

    /**
     * @brief Register the OWNING peer-named sink from a caller-owned callable.
     *
     * Zero-erasure sugar over the `{fn, ctx}` form: @p sink is bound by address
     * (lvalues only — a temporary would dangle) and MUST outlive every delivery.
     * Routed through the `{fn, ctx}` overload, so the mode gate is stated once.
     */
    template <typename F>
        requires std::invocable<F&, peer_handle_t, view::rope_t>
    void set_peer_rope_receiver(F& sink) noexcept {
        set_peer_rope_receiver([](void* c, peer_handle_t peer,
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
    void notify_peer_down(peer_handle_t handle, std::string_view peer) const {
        const peer_down_fn_t fn = peer_down_fn_.load(std::memory_order_acquire);
        if (fn != nullptr) fn(peer_down_ctx_.load(std::memory_order_relaxed), handle, peer);
    }

    /**
     * @brief Fire the peer-ARRIVAL notifier for @p peer (no-op when none installed).
     *
     * Same contract as the departure notifier below and the same discipline: called from the
     * thread that observed the arrival, after the slot's own bookkeeping is complete and
     * with none of the adapter's internal locks held, because the notifier re-enters the
     * routing plane and takes graph locks. Firing it while a slot lock is held would nest
     * the transport's mutex inside `graph_t::map_mutex_`, the reverse of the order
     * `fwd_router_t` documents.
     */
    void notify_peer_up(peer_handle_t handle, std::string_view peer) const {
        const peer_up_fn_t fn = peer_up_fn_.load(std::memory_order_acquire);
        if (fn != nullptr) fn(peer_up_ctx_.load(std::memory_order_relaxed), handle, peer);
    }

    /** @brief The peer-named delivery-tier slot (the ONE tier-select mechanism);
     *         the bus adapter's receive path dispatches through it. */
    receiver_slot_t<peer_handle_t> peer_rx_;

   private:
    /** @brief Installed peer-departure sink. Atomic: a transport thread may fire
     *         it (notify_peer_down) while add_child is still installing it. */
    std::atomic<peer_down_fn_t> peer_down_fn_{nullptr};
    std::atomic<void*> peer_down_ctx_{nullptr}; /**< @brief Its caller-owned context. */
    /** @brief Installed peer-arrival sink. Atomic for the same reason its departure
     *         twin is: the poll thread can accept while add_child is still wiring. */
    std::atomic<peer_up_fn_t> peer_up_fn_{nullptr};
    std::atomic<void*> peer_up_ctx_{nullptr}; /**< @brief Its caller-owned context. */
};

/**
 * @brief The memory a link draws from, as the one nested member every per-kind link config
 *        carries (#1593, RFC-0028 §8.2).
 *
 * Shared, and deliberately tiny: the transport-vertex lean rule keeps kind-private knobs in
 * the kind's own config struct, and this holds only the two memory planes every link kind
 * has. A deployer that bounds a node points both at its slab; since RFC-0028 slice 10 a
 * `mem_backend_t` IS a `block_source_t`, so one synchronized pool can serve both.
 *
 * Each must outlive the link, and each must be thread-safe on a target where the link's
 * receive thread and a sender run concurrently.
 */
struct link_memory_t {
    /**
     * @brief The RX memory seam (ADR-0042 §2): each inbound frame is read into a segment drawn
     *        from it. Exhaustion is backpressure — the frame is dropped and counted, never an
     *        OOM. Default: the process net sub-pool (#1777). A kind whose zero-copy delivery is
     *        opt-in (the ESP-IDF `httpd_ws_link_t`) defaults it to null, meaning "borrowed
     *        delivery".
     */
    mem::mem_backend_t* rx = &mem::net_backend();
    /**
     * @brief The link's EGRESS store (ADR-0079, #873): the per-frame scratch a kind draws
     *        while one outbound frame is in flight — `ws_client_transport_t`'s masked-frame copy
     *        and the base class's gather temporary. A kind with no such draw ignores it.
     *        Default: the process net sub-pool (#1777).
     */
    mem::block_source_t* io = &mem::net_source();

    /**
     * @brief The link's connection-STATE store (#1780): what a kind holds for as long as a
     *        connection or the link lives — the link's own endpoint object, a server's slot
     *        table and per-peer sessions, handshake and receive-accumulation buffers, a QUIC
     *        stream-context table. Peer-provoked, so the receiving link pays: a refusal at
     *        runtime sheds that one peer or frame, counted; only a refusal while the link
     *        is being built leaves it inert (`ok()` false). Default: the process net
     *        sub-pool. Kept apart from @ref io so a bound on in-flight egress never caps
     *        how many peers can connect, and the reverse.
     */
    mem::block_source_t* state = &mem::net_source();

    /** @brief @ref io, or the process net sub-pool when a config left it null. */
    [[nodiscard]] mem::block_source_t& io_or_default() const noexcept {
        return io != nullptr ? *io : mem::net_source();
    }
    /** @brief @ref state, or the process net sub-pool when a config left it null. */
    [[nodiscard]] mem::block_source_t& state_or_default() const noexcept {
        return state != nullptr ? *state : mem::net_source();
    }
};

/**
 * @brief One transport's shed-frame counters, as a generic `transport_t*` holder
 *        can read them (#932).
 *
 * Every transport already counted *some* of this behind its own concrete accessors
 * (`dropped_rx()`, `malformed_rx()`, `dropped_tx()`), which a consumer that holds
 * only the interface cannot call — so swapping tcp for ws or CAN silently lost all
 * drop observability. This is the ONE shape they all answer with; a transport that
 * does not count a given class leaves it zero. Named `drop_stats`, not `stats`: a
 * platform link may already publish a RICHER, kind-specific stats block of its own
 * (`httpd_ws_link_t::stats()`), and this seam is only the shed-frame subset every
 * kind can answer.
 *
 * Monotonic since construction, sampled without synchronization: the three fields
 * are read one relaxed load at a time, so a snapshot is eventually-consistent, not
 * an atomic instant across counters.
 */
struct transport_drop_stats_t {
    /** @brief Inbound frames shed rather than delivered — backend exhausted, or the
     *         frame is undeliverable through the injected resources (backpressure). */
    std::uint64_t dropped_rx = 0;
    /** @brief Inbound frames refused as protocol-malformed; for a stream transport this
     *         is also the teardown reason (a desynced stream cannot be re-framed). */
    std::uint64_t malformed_rx = 0;
    /** @brief Outbound frames the caller believed sent that never reached the wire —
     *         oversize for the peer's cap, no peer/dead socket, or a refused gather. */
    std::uint64_t dropped_tx = 0;
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

    /**
     * @brief Emit one frame (a complete TLV's bytes) onto the wire.
     *
     * @par The transport plane is BEST-EFFORT by contract, and a drop MUST be counted.
     * This is the one place in libtracer where the refuse-by-value law
     * (`docs/reference/22-backpressure-and-sizing.md` §1, the third reading rule)
     * does not apply, and the reason is not an oversight: **there is no wire carrier for
     * backpressure in v1** (the per-edge credit window is parked as the v2 escalation,
     * RFC-0025 §4.6.1 clause 7). A link that could refuse by value would have nobody to
     * refuse *to* — the caller is a forwarding hop with a frame it cannot un-receive, and the
     * peer cannot be told to slow down. So the contract is:
     *
     * - **`void` is deliberate, and stays.** A returned status here would be a promise the
     *   plane cannot keep: on every real link the frame is queued and written later, on
     *   another task, so "sent" at this call site can only ever mean "accepted", which is
     *   what a `void` already says. Callers MUST NOT read a successful return as delivery.
     * - **A link that drops a frame MUST count it** in @ref drop_stats — `dropped_tx` for a
     *   frame the caller believed sent, `dropped_rx` / `malformed_rx` on the ingress side.
     *   That counter is the ONLY feedback this plane offers, which is exactly why it is
     *   mandatory rather than a courtesy: an uncounted drop is invisible to a deployment,
     *   and `core/STYLE.md` §Introspection forbids the silent loss it hides.
     * - **The counter is how a monitor closes the loop.** The tally is wire-readable as
     *   `read <any-vertex>:stats.link.<child>` (RFC-0010 Amendment 2), so a supervisor polls
     *   it and reacts, in place of the per-frame refusal v1 has no room for.
     *
     * @note Re-opening this is scoped to **M5** (a real socket transport), where a link that
     *       owns its own socket buffer has something to push back WITH. Until then, "counted
     *       shed" IS the transport contract.
     */
    virtual void send(std::span<const std::byte> frame) = 0;

    /**
     * @brief This link's shed-frame counters — the interface-level observability seam.
     *
     * The DEFAULT is all-zero, which is the honest answer for a link that counts
     * nothing (an in-process or test stub): "no drops observed here", never a
     * fabricated number. A concrete transport overrides it with its own counters
     * (#932); the per-transport accessors stay for callers that hold the concrete type.
     */
    [[nodiscard]] virtual transport_drop_stats_t drop_stats() const noexcept { return {}; }

    /**
     * @brief Scatter-gather send: emit the gathered spans as ONE frame, no flatten copy.
     *
     * Hand a rope's `to_iovec()` straight to the wire. The default gathers into a
     * temporary and calls @ref send(std::span<const std::byte>); transports with native
     * scatter-gather (sendmsg/writev/RDMA SGE) override this to avoid the copy.
     *
     * **The same best-effort contract as @ref send(std::span<const std::byte>)**: `void` is
     * deliberate, and a gather this overload cannot complete is DROPPED and COUNTED in
     * @ref drop_stats (`dropped_tx`) — never truncated, and never silently lost. The default
     * body below is where that rule is enforced for every transport that does not override
     * it.
     *
     * @param iov The spans to emit, in order, as a single frame.
     */
    virtual void send(std::span<const std::span<const std::byte>> iov) {
        // NOTHROW soft-fail (#477/#848): this used a throwing `reserve` + `insert`, which
        // under `-fno-exceptions` ABORTS the node on an exhausted heap rather than shedding
        // the frame. That is reachable on the FORWARD hot path today — `route_fwd_forward`
        // scatter-gathers into `send(iov)`, and a transport that does not override this
        // (`can_transport_t`, and any embedder's) lands here. An egress that cannot allocate
        // must DROP, exactly as every other writer-side allocation on this plane does.
        //
        // The store is a `tr::mem::block_array_t`, NOT a `std::vector` + `try_reserve`:
        // `std::vector::reserve` reports exhaustion by throwing, and under
        // `-fno-exceptions` that is a bare `abort()` inside `reserve` that no wrapper can
        // intercept — so on the profile this body exists for, `try_reserve` can only guess
        // ahead with a nothrow probe and hope nothing takes the block in between (#923).
        // Drawing from the failable seam (ADR-0065) leaves ONE refusable allocation on both
        // profiles.
        std::size_t total = 0;
        for (const auto& s : iov) total += s.size();
        mem::block_array_t<std::byte> tmp(egress_source());
        // Honour the `probe_fail_hook` OOM-injection seam explicitly, exactly as
        // `iov_table_t::acquire` does: the hook lives inside `probe_bytes`, which the
        // failable seam does not route through, so a drop leg reached only via
        // `block_source_t::try_alloc` would otherwise be untestable without genuinely
        // exhausting the host heap.
        if (!detail::probe_hook_ok(total)) return;
        if (!tmp.reserve(total)) return;  // heap exhausted ⇒ drop, never abort
        // Reserved exactly, so the copies below cannot grow the block and cannot fail.
        std::byte* out = tmp.data();
        std::size_t off = 0;
        for (const auto& s : iov) {
            if (!s.empty()) std::memcpy(out + off, s.data(), s.size());
            off += s.size();
        }
        send(std::span<const std::byte>(out, total));
    }

    /**
     * @brief Retained send (RFC-0028 §6.9, §8.2): emit @p head followed by @p value's bytes as
     *        ONE frame, where a link that writes LATER keeps the value by reference instead of
     *        gathering a copy of it.
     *
     * The egress half of the lean value path. A remote delivery is a small head (the FWD
     * header, the op, the return route) in front of a published value; the head is the
     * caller's and short-lived, the value is a refcounted block. A link that writes in-call
     * (`writev` under its write lock) needs neither kept, and the DEFAULT below is exactly
     * that: it lowers to @ref send(std::span<const std::span<const std::byte>>) over
     * `head ++ value.links()`, through an iov table on the stack. A link that QUEUES the frame
     * and writes it from another thread overrides this: it copies the head (header-sized) into
     * its queue slot and RETAINS the value (`value_ref_t::keep` — one refcount for a published
     * block), so the payload is written from the block it was published in and never
     * gathered. That is the difference between a queued fan-out to K peers costing K payload
     * copies and costing K refcounts.
     *
     * Same best-effort contract as @ref send(std::span<const std::byte>): `void`, and a frame
     * the link cannot carry (the iov table's overflow refused, the value could not be kept,
     * the queue is full) is dropped, never truncated — and counted in @ref drop_stats by every
     * link that counts, which is every link that overrides this.
     *
     * @warning An overriding link owns the one hazard the queued form adds (RFC-0028 §9 item
     *          6): the frame's bytes now leave in several writes from a queue shared with every
     *          other frame on the socket, so a partial write MUST end the frame's stream (close
     *          the peer), never let another frame's bytes follow it.
     *
     * @param head  The frame's leading bytes, in order. Borrowed for the call only.
     * @param value The payload that completes the frame. Borrowed for the call; a link that
     *              writes later keeps it through `value_ref_t::keep`.
     */
    virtual void send(std::span<const std::span<const std::byte>> head,
                      const graph::value_t& value) {
        // The #1620 (c) table: a delivery's head is a handful of spans and a value is one or
        // two links, so the stack array holds the common frame and the overflow block (from
        // this link's egress store) is the exception, not a per-send allocation.
        std::array<std::span<const std::byte>, 8> inline_iov;
        iov_table_t<std::span<const std::byte>> table(inline_iov, egress_source());
        const std::size_t n = head.size() + value.link_count();
        std::span<const std::byte>* const iov = table.acquire(n);
        // Overflow refused: the frame is dropped exactly as the base gather above drops a
        // refused temporary — a link with a counter overrides one of the two entries.
        if (iov == nullptr) return;
        std::size_t i = 0;
        for (const std::span<const std::byte>& h : head) iov[i++] = h;
        for (const view::view_t& l : value.links()) iov[i++] = l.bytes();
        send(std::span<const std::span<const std::byte>>(iov, n));
    }

    /**
     * @brief The EGRESS store this link's per-send gather allocations draw from
     *        (ADR-0079's net-plane failable store, #873 family 1).
     *
     * Every allocation an outbound frame provokes on this link — the base
     * @ref send(std::span<const std::span<const std::byte>>) gather temporary above, and the
     * `tr::net::iov_table_t` overflow block of the socket transports that build a gather
     * table — is drawn from HERE rather than from the process-wide `%mem::heap_source()`. The
     * entry count and the byte count are both the SENDING peer's choice (a rope's link count
     * x its region count), so this is the seam that makes "bounded node" a property the
     * deployer injects (ADR-0079 §Decision 4) instead of one the library fixes: size the
     * store and the egress path is bounded by it, with exhaustion answered the way it
     * already is — the frame is DROPPED and counted, never truncated and never `abort()`.
     *
     * The default is the process net sub-pool (#1777): unbounded, as a link nothing was wired
     * into always was.
     */
    [[nodiscard]] mem::block_source_t& egress_source() const noexcept { return *egress_src_; }

    /**
     * @brief Wire this link's egress store — the transport-factory injection point.
     *
     * Same contract as the receiver slots: call it during bring-up, BEFORE frames flow. The
     * built-in factories apply it to every socket they construct (the `egress_src` argument
     * of `register_builtin_transports`), which is how a deployer choosing ADR-0079's MID
     * composition hands the whole net plane its own store, or its NARROW fan gives each
     * link's own thread a contention-free one. @p src must outlive this transport.
     *
     * @warning A link's egress store is touched by EVERY thread that sends on that link, so
     *          @p src must declare a concurrency contract covering them (`block_source_t`
     *          §"each source declares its own"). `heap_source_t` does; a
     *          `pool_source_t<sync_none_t>` or a `bump_source_t` does NOT, and belongs to a
     *          link only one thread ever sends on — which is the ADR-0079 NARROW shape, and
     *          the reason it is per-link rather than one node-wide store. None of the six
     *          egress sites holds a transport lock across the allocation, so a locking
     *          @p src introduces no lock-ordering obligation here (#1049).
     *
     * @warning This setter reaches the allocations a send makes THROUGH this base — it does
     *          not re-seat a concrete link's CONSTRUCTION-BOUND buffers. A
     *          `mem::block_array_t` member takes its source in its own constructor and keeps
     *          it for life, so a link that owns one (e.g. `ws_client_transport_t::tx_buf_`)
     *          takes the store as a CONSTRUCTOR argument and applies it to both halves
     *          there (#873). Wiring such a link only through this setter would leave that
     *          buffer on whatever source it was built with.
     */
    void set_egress_source(mem::block_source_t& src) noexcept { egress_src_ = &src; }

    /**
     * @brief The block source the routing plane decodes this link's inbound frames through
     *        (ADR-0067 §3) — null ⇒ the router's own default.
     *
     * The link's state, not the router's (#1941): a link has its own receive thread, so a
     * source recorded here is touched by exactly one thread, which is the per-thread shape
     * ADR-0067 §3 obtains by ownership rather than by a lock. A bounded node gives each link
     * its own slab, and the bound is then per-peer: one noisy link cannot starve another's
     * decode. `fwd_router_t::add_child` records the source it is given here.
     *
     * Atomic and relaxed on both sides: a re-registration of this link may store while its
     * receive thread reads, and the value names an object that outlives the link either way.
     */
    [[nodiscard]] mem::block_source_t* rx_source() const noexcept {
        return rx_src_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Record the source @ref rx_source answers; null restores the router's default.
     * @param src Must outlive every frame this link delivers, and be safe for its receive
     *            thread to draw from (ADR-0067 §3).
     */
    void set_rx_source(mem::block_source_t* src) noexcept {
        rx_src_.store(src, std::memory_order_relaxed);
    }

    /**
     * @brief This link's transport-catalog `(kind, role)` (#1650) — null when it was
     *        registered without one.
     *
     * The admission context's LINK claim: every write the link carries hands this pointer to
     * the target's admission filter and handler (`graph::write_ctx_t::link`). It lives on the
     * link it describes (#1941), so a frame reads the claim of the link that is delivering it.
     * `fwd_router_t::add_child` records the router's interned record for the kind it is
     * given, which outlives every link the router holds.
     */
    [[nodiscard]] const link_kind_t* kind() const noexcept {
        return kind_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Record the record @ref kind answers.
     * @param kind Must outlive every frame this link delivers; null records "none".
     */
    void set_kind(const link_kind_t* kind) noexcept {
        kind_.store(kind, std::memory_order_relaxed);
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
     * @brief Begin delivering inbound frames — the second half of a two-phase bring-up.
     *
     * Every sink above says "must be set before frames flow", and for a transport whose
     * receive thread starts inside its own constructor that contract is UNSATISFIABLE from
     * the outside: the thread is already draining the socket while the owner is still
     * installing its sinks, so a frame the peer pushes the instant the connection comes up
     * is decoded into an empty slot and dropped — silently, with no counter moving (#1025).
     * A DIAL connection is where that bites, because the peer's push is triggered by our own
     * connect. This is the window: construct (dial + handshake), install the sinks, then
     * call this.
     *
     * IDEMPOTENT, and the DEFAULT IS A NO-OP — a transport that is already receiving from
     * its constructor has nothing left to do — so an owner may call it unconditionally on
     * any link. `%transport_vertex_t::make_connection` does exactly that, once the link
     * is registered and `%fwd_router_t::add_child` has installed its receiver.
     */
    virtual void start_receiving() {}

    /**
     * @brief The owning-delivery capability (ADR-0042 §1): true iff this transport
     *        honors @ref set_rope_receiver by delivering refcounted rope frames.
     */
    [[nodiscard]] virtual bool delivers_ropes() const { return false; }

    /**
     * @brief Liveness: true while this link can still carry frames (#1059).
     *
     * The uniform PULL-side liveness query, the poll twin of @ref set_down_notifier's push
     * — an owner can ask any link the same question. It is deliberately NOT the concrete
     * types' `ok()`: `ok()` is the CAME-UP predicate (did construction — the dial, the
     * handshake, the bind — succeed), answered once, right after construction (the
     * `make_checked` gate), and it never reverts; THIS is the runtime state, cleared by
     * the transport's own teardown path when its one connection dies. After a teardown
     * the two diverge: `ok()` stays true (the link DID come up), `link_up()` answers
     * false.
     *
     * The default is TRUE: a connectionless (UDP) or bus (CAN) kind has no closure
     * concept — its link is as up as it ever is — and a multi-peer server outlives any
     * one peer. Connection-oriented transports override it. Implementations read a
     * relaxed atomic (or state that is already atomic): this is a hint, never a
     * synchronisation point, and deliberately carries no is-always-lock-free assertion
     * (one target is an rv32 core without the A extension).
     */
    [[nodiscard]] virtual bool link_up() const noexcept { return true; }

    /**
     * @brief The peer HANDLE of the frame this link is delivering RIGHT NOW — the WHO seam
     *        (#375 Part 2), answerable at either setting of `peer_named` (ADR-0082).
     *
     * The bus seam already tags each frame with its peer, so a peer-named link's consumers
     * never need this. A FLAT link has no such seam by design — one routing identity for
     * every peer it carries — and ADR-0082 is explicit that the two claims are independent:
     * a subject must be reachable at `peer_named=false` or the decouple is not real. This is
     * the door that makes it reachable WITHOUT the addressing facet: the link states which
     * peer the in-flight frame came from, and nothing about where that peer sits in the graph.
     *
     * @warning Valid ONLY for the duration of a receive callback, read on the thread that is
     *          running it. Outside one the answer is unspecified (implementations return the
     *          last delivery's handle or a default one); it is never a query about link state.
     * @return The in-flight frame's peer, or a default-constructed (not
     *         @ref peer_handle_t::valid) handle when this kind has no per-peer identity —
     *         the DEFAULT, so a dialer, a datagram kind and every custom transport keep
     *         today's behaviour: the subject falls back to the inbound link's own name.
     */
    [[nodiscard]] virtual peer_handle_t inbound_peer() const noexcept { return {}; }

    /**
     * @brief The SUBJECT token of the peer @p peer names — *who wrote this*, never *where*
     *        (ADR-0082 §Decision 1).
     *
     * Called at the resolve TERMINUS, once per locally-terminating operation, to derive the
     * ACL caller context (ADR-0018's pluggable subject token) and the `graph::write_ctx_t`
     * a HANDLER sees. It is deliberately NOT `bus_link_t::peer_name`: that is an ADDRESSING
     * answer gated on the bus facet, and this must answer on a FLAT link too. A kind whose
     * two answers coincide — the stream servers, whose subject is the same `p<slot>` session
     * token their peer name is — implements both from one place.
     *
     * @warning The token attests to NOTHING about the far end on its own: it is minted by
     *          this node's own transport at accept, exactly as ADR-0082 §Guidance warns. An
     *          authenticated subject is the identity-layer's job (ADR-0045); this is the
     *          per-writer discriminator the ACL evaluates until one exists.
     * @param peer    The handle to resolve — typically @ref inbound_peer's answer.
     * @param scratch Caller storage the token may be formatted into, at least
     *                `kPeerNameChars` bytes. The returned view points either into
     *                @p scratch or at storage that outlives the call.
     * @retval {} @p peer is not @ref peer_handle_t::valid, or this kind mints no per-peer
     *            subject — the DEFAULT, on which the terminus falls back to the inbound
     *            link's own name, i.e. exactly the pre-#375 caller context.
     */
    [[nodiscard]] virtual std::string_view peer_subject(peer_handle_t peer,
                                                        std::span<char> scratch) const {
        (void)peer;
        (void)scratch;
        return {};
    }

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
        // Publish ctx-before-fn (release on fn); notify_down's acquire load pairs
        // fn with its ctx. Atomic because a transport's callback thread — spawned
        // in the ctor, so already live — can fire notify_down on a fast remote
        // hangup before fwd_router_t::add_child finishes this install.
        down_ctx_.store(ctx, std::memory_order_relaxed);
        down_fn_.store(fn, std::memory_order_release);
    }

   protected:
    /**
     * @brief Destroyed only as the class it is, never through this base.
     *
     * Protected and non-virtual (#2022). The one owner of a link through this base is
     * `%tr::mem::poly_ptr_t` (a factory's `transport_ptr_t`), and it records the concrete
     * destructor, so nothing here needs a virtual one. A virtual one would emit a deleting
     * destructor naming `operator delete` in every object that emits a link's vtable, on a
     * build that must name no heap. Own a link as its concrete type, or through
     * `%tr::mem::make_poly`; `std::unique_ptr<transport_t>` no longer compiles.
     */
    ~transport_t() = default;

    /** @brief Fire the link-down notifier (no-op when none installed) — see
     *         @ref set_down_notifier for the calling discipline. */
    void notify_down() const {
        const down_fn_t fn = down_fn_.load(std::memory_order_acquire);
        if (fn != nullptr) fn(down_ctx_.load(std::memory_order_relaxed));
    }

    /** @brief The delivery-tier slot (the ONE tier-select mechanism, ADR-0042 /
     *         ADR-0053): adapters dispatch inbound frames through it —
     *         `rx_.deliver(view)` for owning frames, `rx_.deliver_borrowed(span)`
     *         for borrowed ones — and key receive-buffer strategy off
     *         `rx_.has_rope()`. */
    receiver_slot_t<> rx_;

   private:
    /** @brief Installed link-down sink. Atomic: a transport's callback thread may
     *         fire it (notify_down) while add_child is still installing it. */
    std::atomic<down_fn_t> down_fn_{nullptr};
    std::atomic<void*> down_ctx_{nullptr}; /**< @brief Its caller-owned context. */

    /** @brief The injected egress store — see @ref egress_source. Not atomic: it is wired
     *         once during bring-up, before any thread can send on this link, exactly as the
     *         receiver slots are. */
    mem::block_source_t* egress_src_ = &mem::net_source();
    /** @brief The decode source — see @ref rx_source. */
    std::atomic<mem::block_source_t*> rx_src_{nullptr};
    /** @brief The catalog claim — see @ref kind. */
    std::atomic<const link_kind_t*> kind_{nullptr};

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

/**
 * @brief @p link's BUS facet, or nullptr on a target that closed the bus module out —
 *        the ONE door the routing plane asks through (#375 deliverable 3).
 *
 * At the default binding this is `link.bus()` and nothing else: the `if constexpr` selects the
 * live branch, so every consumer's machine code is byte-identical to the direct call (verified
 * by object-file `cmp` — 68 of 68 objects unchanged). Bound
 * @ref tr::graph::default_config_t::kBusLinks `false`, the call is not merely predicted away —
 * it is not compiled, so the peer-resolution, peer-enumeration and peer-lifecycle code that
 * hangs off a non-null result is unreachable and the linker keeps none of it.
 *
 * Consumers ask through here rather than reading @ref tr::net::kBusLinks so that the
 * point-to-point answer is spelled once. `transport_t::bus()` itself is untouched — still a
 * virtual, still `return nullptr` by default (ADR-0047 §4: peer wiring is a wiring-frequency
 * query, and this adds no template parameter and no dispatch mechanism to any control
 * structure).
 *
 * @warning A DIRECT `link.bus()` call bypasses the gate and is correct only where the caller
 *          IS the bus (a transport's own facet, a test pinning a link's shape). Anything on
 *          the routing plane asks here.
 * @param link The link to interrogate.
 * @retval nullptr @p link is point-to-point, or this target carries no bus module at all.
 */
[[nodiscard]] inline bus_link_t* bus_of(transport_t& link) {
    if constexpr (kBusLinks)
        return link.bus();
    else
        return nullptr;
}

}  // namespace tr::net
