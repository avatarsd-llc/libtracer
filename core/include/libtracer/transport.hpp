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
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/receiver_slot.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

namespace tr::net {

// A 16-byte node/peer identity — the ROUTER `origin_peer_id` (docs/reference/05
// §0x0D ROUTER).
using peer_id_t = std::array<std::byte, 16>;

class transport_t;

/**
 * @brief An opaque per-peer LINK HANDLE — the identity the peer-receiver seam carries
 *        (#1294), minted once when a peer becomes audible and valid until it departs.
 *
 * The seam used to re-supply a peer NAME string on every inbound frame, which forced every
 * consumer that wanted a per-peer identity to re-derive one from that string per frame — a
 * hash and a map find on the subscribe path (#1266), and nothing at all to hang a per-peer
 * auth subject off (#375 Part 2). This handle is that identity, handed down instead.
 *
 * It is `(index, generation)`, the same node-local-index-plus-validate-on-use-stamp primitive
 * the in-tree edge binding (#830), the RFC-0024 vref and the ESP link's session ref already
 * mint — a 8-byte trivially-copyable POD, cheap to copy per frame and cheap to key a table by.
 * The two fields are OPAQUE to a consumer: only the minting link knows what an index means,
 * and a consumer may only compare handles, hash them, and hand them back.
 *
 * **It is not a session reference.** A session ref (`httpd_ws_link_t::session_ref_t`,
 * #1146/#1262) is ONE SUPPLIER of a handle, not the handle itself: an announce-census CAN
 * peer has no session at all and still needs a stable link key, so the handle is the general
 * concept and the session ref produces one.
 *
 * **It does not carry the subject.** A per-peer auth subject is RESOLVED from the handle at
 * ACL-check time rather than carried in it, which is what keeps the per-frame POD minimal
 * (#1294 ruling 2).
 *
 * **It is never absent.** Every handle this seam hands down is `valid()`: a link with no
 * meaningful per-peer identity mints `kSolePeerHandle` once at link-up and hands that down
 * for every frame, so no consumer needs a "handle absent" branch (#1294 ruling 3).
 */
struct peer_handle_t {
    /** @brief The minting link's own peer INDEX — meaningless to anyone else. */
    std::uint32_t index = 0;
    /** @brief The validate-on-use stamp; `0` is reserved to mean "no peer". */
    std::uint32_t generation = 0;

    /** @brief True iff this handle names a peer (a zero generation never does). */
    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }

    /** @brief The handle's whole identity as one integer — the key an interning
     *         consumer (#1266) hashes, so it never has to know the field split. */
    [[nodiscard]] constexpr std::uint64_t bits() const noexcept {
        return (static_cast<std::uint64_t>(generation) << 32) | index;
    }

    /** @brief Handles compare by identity — same index AND same generation. */
    [[nodiscard]] friend constexpr bool operator==(peer_handle_t, peer_handle_t) noexcept = default;
};

static_assert(sizeof(peer_handle_t) == 8, "the per-frame peer handle stays an 8-byte POD");
static_assert(std::is_trivially_copyable_v<peer_handle_t>);

/**
 * @brief The handle a link with no meaningful per-peer identity mints at link-up (#1294
 *        ruling 3) — one constant peer, valid for the link's whole life.
 *
 * A point-to-point kind exposing the bus facet for one peer, and a test double that carries
 * exactly one far side, hand this down rather than an invalid handle: the seam is UNIVERSAL,
 * so the "no peer identity here" case costs one constant at link-up instead of a branch in
 * every consumer.
 */
inline constexpr peer_handle_t kSolePeerHandle{0, 1};

/**
 * @brief Scratch big enough for any peer NAME a bus link resolves (@ref
 *        bus_link_t::peer_name) — `p<slot>` / `n<node>` are both far inside it.
 */
inline constexpr std::size_t kPeerNameChars = 32;

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
    /** @brief Visitor invoked once per currently-audible peer name. */
    using peer_visitor_t = std::function<void(std::string_view)>;
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
     * it landed in, and @ref transport_can names one `n<node>` for its bus node id. So the
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
     * edge stores the peer NAME rather than this pointer.
     *
     * Which kinds are exposed follows from the naming regime alone:
     *  - IDENTITY-derived names are immune — @ref transport_can names a peer `n<node-id>`
     *    for its own bus node id, so the name, the table key and the endpoint are one
     *    identity that no other peer can inherit.
     *  - POSITIONAL names are exposed — @ref slot_server_t names a peer `p<slot>` for the
     *    slot index it landed in, and slots are recycled in place.
     *
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

    virtual ~transport_t() = default;

    /** @brief Emit one frame (a complete TLV's bytes) onto the wire. */
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
     * @param iov The spans to emit, in order, as a single frame.
     */
    virtual void send(std::span<const std::span<const std::byte>> iov) {
        // NOTHROW soft-fail (#477/#848): this used a throwing `reserve` + `insert`, which
        // under `-fno-exceptions` ABORTS the node on an exhausted heap rather than shedding
        // the frame. That is reachable on the FORWARD hot path today — `route_fwd_forward`
        // scatter-gathers into `send(iov)`, and a transport that does not override this
        // (`transport_can`, and any embedder's) lands here. An egress that cannot allocate
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
     * The default is the process heap, so a link nothing was wired into behaves exactly as
     * it did before this seam existed.
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
     */
    void set_egress_source(mem::block_source_t& src) noexcept { egress_src_ = &src; }

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
    mem::block_source_t* egress_src_ = &mem::heap_source();

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
