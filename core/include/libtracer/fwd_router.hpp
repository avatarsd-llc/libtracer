/**
 * @file
 * @brief Multi-hop `FWD` forwarding and zero-copy `src` accumulation across transports
 *        (RFC-0004).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0004 / ADR-0035 slice 3 — multi-hop FWD forwarding + zero-copy `src`
 * accumulation across transports. A node holds a set of NAMED transport-child
 * vertices (ADR-0027): each child is a link, addressed in THIS node's space by its
 * **mount run** `net/<module>/<name>[/<peer>]` — a run of NAME segments of ANY width,
 * not one (RFC-0014 S2a; the strip-K descent of ADR-0061, single-pass since #523).
 * On an inbound FWD that arrived on child `inbound_name`:
 *
 *   - resolve the LEADING `dst` segments against the child registry, longest mount
 *     first. If nothing matches a mount, the address is local: apply the op via the
 *     slice-2 op_resolver_t, build FWD{REPLY}, and send it back over the link the
 *     request arrived on;
 *   - if they name a transport-child vertex (forward), STRIP that whole matched run
 *     from `dst`, PREPEND to `src` (zero-copy rope head-prepend) the mount run by
 *     which THIS node addresses the inbound link (the way back), and send the
 *     shortened FWD over the named child to the peer;
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

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string_view>

#include "libtracer/child_registry.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/path_element.hpp"
#include "libtracer/path_pair.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/sink_slot.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

/** @brief Offsets a `dst` peek recorded for the head rebuild (defined in fwd_frame_view.hpp).
 *         Forward-declared: the router only passes it through by pointer, so this header does
 *         not pull in the forward-plane cluster. */
struct fwd_pre_t;

/**
 * @brief The router's counted cold-path drops — one snapshot of every frame this router
 *        lost rather than forwarded (#1503 step 3).
 *
 * Vocabulary and polarity per `core/STYLE.md` §Introspection; the snapshot-coherence clause
 * stated there applies verbatim (each field is one relaxed load, so a multi-field snapshot
 * may tear — the intended use is the difference between two snapshots).
 *
 * Every field counts a **`dropped`**, not a `refused`: these sites lose a frame and tell
 * nobody, which is exactly why the doctrine requires them counted. They are ROUTER-LOCAL and
 * deliberately NOT folded into the graph's delivery-drop counters — these frames die before
 * the graph is involved, and one refusal must not be tallied on both sides of the net/graph
 * seam (#1503 Q2).
 *
 * The resource causes are split per-cause because a sizing operator grows a *different* seam
 * for each; the malformed/opcode drops are fused into @ref malformed_rx because they are one
 * operator symptom (#1503 Q3).
 */
struct router_stats_t {
    /** @brief Frames dropped because the reject-reply flatten could not be served by the
     *         injected byte backend (`fwd_router.cpp`, the `reject` arm). */
    std::size_t flatten_dropped = 0;
    /** @brief Forward hops dropped because the scatter-gather iov table could not be built —
     *         the rope arm's source refusing growth, or the contiguous arm overrunning
     *         `kFwdMaxIov`. Both arms state one policy: drop, never truncate. */
    std::size_t forward_iov_dropped = 0;
    /** @brief Terminus requests dropped because the injected rx source could not serve the
     *         decode arena — `TLV_NESTING_TOO_DEEP`, which RFC-0006 spells as "exceeds this
     *         receiver's decode resources". The number a deployment grows `rx` against. The
     *         request is still ANSWERED — an addressed `STATUS{BACKPRESSURE}` built on the
     *         stack from its own bytes (#1612) — so this counts work lost, not silence. */
    std::size_t arena_dropped = 0;
    /** @brief Terminus REPLIES dropped because reply assembly was refused memory — the
     *         resolver's BACKPRESSURE by value (#2256). The request is still answered from the
     *         stack (#1612). An unacknowledged WRITE (empty `src`) has no reply and is never
     *         counted here. */
    std::size_t assemble_dropped = 0;
    /** @brief Terminus REPLIES dropped because the egress span table could not be drawn
     *         from the per-owner receive source (#1570). The request is lost; the client
     *         retries — at any reply size, including a composed root's. */
    std::size_t reply_iov_dropped = 0;
    /** @brief Remote DELIVERIES dropped because the delivery's head could not be built
     *         (`deliver_remote`, full-route arm). Since RFC-0028 slice 9 the delivery draws
     *         no iov table (the head rides stack spans and the value goes to the link by
     *         reference), so this stays 0 unless a head outgrows its fixed stack buffer. A
     *         link that refuses the frame counts it in its own `dropped_tx`. */
    std::size_t delivery_iov_dropped = 0;
    /** @brief Frames dropped as MALFORMED — a failed decode, a failed root CRC, an oversized
     *         or non-forwardable op, an unknown/unauthorized opcode. ONE bucket by ruling:
     *         an operator reads every one of these as the same symptom ("a peer is speaking
     *         something this node cannot parse"), and nothing is sized against them. */
    std::size_t malformed_rx = 0;
    /** @brief Outer frames of a RETIRED core type code — `0x11`, `0x12`, `0x13`, the
     *         `ADVERTISE` / `COMPACT` / `HANDLE_NACK` an older peer still sends (RFC-0032
     *         §6.1). Each one is also answered with a bare `ERROR{tr::schema::type_mismatch}`
     *         on its arrival link, and counted here whether or not that answer could be
     *         sent. A new node never sends these codes, so a non-zero count names an old peer
     *         on some link. */
    std::size_t retired_rx = 0;
};

/**
 * @brief The router's allocation planes as ONE aggregate (RFC-0028 §8.2, slice 10) — what
 *        `fwd_router_t`'s constructor takes in place of six positional, defaulted seams.
 *
 * Every member defaults to the process net sub-pool (`mem::net_source()` /
 * `mem::net_backend()`, #1777), so
 * `router_planes_t{}` is the unbounded host router and a bounded node names only the planes
 * it points at its own slab:
 *
 * @code
 * tr::net::fwd_router_t router{graph, {.link_src = &links, .rx = &rx_pool,
 *                                      .flat = &flat_pool, .egress = &flat_pool}};
 * @endcode
 *
 * Every plane must outlive the router, and every injected one must be thread-safe (several
 * transport receive threads reach them concurrently). Since slice 10 a `mem_backend_t` IS a
 * `block_source_t`, so one synchronized pool can serve a source plane and a backend plane.
 */
struct router_planes_t {
    /**
     * @brief The nothrow source the router's LONG-LIVED link state draws from (#603 defect 1 /
     * #873 family 3, ADR-0065 / ADR-0079 §Decision 4): the NAME->link demux table, each child's
     * receive context, the bus token caches and the bound-path scratch — the library holds no
     * buffer of its own. A bounded node injects a @ref mem::pool_source_t over its static slab
     * (one slab, whole stack — ADR-0039 §2); the default is the process net sub-pool
     * (`mem::net_source()`; the static arena on an MCU build). Must outlive the router, and must be
     * thread-safe: several transport receive threads reach it.
     *
     * This plane used to be a `%std::pmr::memory_resource*`. It could not stay one: a pmr resource
     * cannot report exhaustion by value, so on the shipping `-fno-exceptions` profile a peer-driven
     * allocation against an aborting `heap_resource_t` rebooted the node. A call site that passed
     * its OWN resource — a node whose graph and router historically shared one pmr arena —
     * migrates by pointing @ref mem::pool_source_t's SPAN CONSTRUCTOR at the same storage
     * that resource was partitioning (#1493). It must NOT reach for the adapter that shape invites,
     * a `block_source_t` wrapping the pmr resource: that wrapper's `try_alloc` cannot answer
     * `nullptr`, so it reinstates the very abort this parameter change removed. @ref
     * mem::block_source_t's warning carries the full reasoning, including why a budget-tracking
     * variant is declined too. Split from @p rx deliberately (ADR-0079's per-plane default): @p rx
     * is per-frame decode scratch and may legitimately be a `bump_source_t`, while link state is
     * LONG-LIVED and would monotonically fill one.
     */
    mem::block_source_t* link_src = &mem::net_source();

    /**
     * @brief The nothrow source the TERMINUS ARENA draws from (#588). Split from @p link_src
     * because the arena is built from a peer's frame, on the RX path, behind no ACL: a
     * `std::pmr::memory_resource` cannot report exhaustion by value, so on `-fno-exceptions` its
     * only failure mode is `abort()`. Drawing the arena from a @ref mem::block_source_t instead
     * makes an over-large frame a `TLV_NESTING_TOO_DEEP` refusal, counted as `arena_dropped` and
     * answered with an addressed `STATUS{BACKPRESSURE}` that draws from no source at all (#1612).
     * A bounded node points
     * this at the same slab as @p link_src. Must outlive the router.
     */
    mem::block_source_t* rx = &mem::net_source();

    /**
     * @brief The byte backend EVERY rope flatten on the router's forward AND terminus paths draws
     * its owned `segment` from — the router's own (#730): the cold bus-name rejection flatten and
     * the refused-route reclaim flatten; PLUS the terminus resolver's rope-tier flattens
     * one call below `resolve_terminus_rope` (#766) — `view_node::ensure_cache` (the per-node
     * contiguous span every `wire()`/`body()` read of a multi-link TLV materializes) and
     * `view_node::own_wire` (the ADR-0053 ⑤ ownership flatten) — which the router reaches by
     * passing this pointer straight to its @ref graph::op_resolver_t. Until #766 the terminus half
     * drew from the global heap, so a fragmented request from a peer escaped the bound; the honest
     * sentence now is that all rope flattens on the forward and terminus paths draw from the
     * injected seam. Since #801 that sentence covers the SPAN-tier terminus too:
     * `arena_node::own_wire`, the ADR-0041 §2 ownership copy a span-delivered request takes, is
     * this backend's as well — so the MCU terminus (a synchronous CAN/UART child delivers spans,
     * not ropes) no longer escapes the bound on its ordinary WRITE. Still NOT every allocation the
     * router path makes: the reply head segment and the arena are their own injections (@p egress
     * and @p rx) — the head drew from `view::heap_alloc`'s global heap until #795 gave it @p
     * egress below, which is the reading #730 was filed about. Split from @p rx because these are
     * BYTE buffers with cache hooks and an owning refcount (a @ref mem::mem_backend_t), not the
     * arena's raw blocks — the same split `graph_t` makes between its `ctl` and its
     * `value_backend` (ADR-0060). Until #730 all of them took @ref mem::heap_backend by default,
     * so a bounded node's memory bound did NOT cover them; now a node that points this at its own
     * slab bounds them all, and every flatten failure is answered by value — the forward-path
     * frame is dropped (never stored empty), and a refused terminus flatten answers an addressed
     * `kind=ERROR STATUS{BACKPRESSURE}` reply (or, when the refusal hit the reply's own route
     * bytes, a drop).
     *
     * An injected @p flat MUST be thread-safe, with the same force `graph_t` requires of its
     * `value_backend` (ADR-0060 §2) — and for the same two reasons, both of which hold here. Three
     * of the four sites run on a transport child's RECEIVE thread and several children receive
     * concurrently; the fourth runs on the WRITER thread inside the remote-delivery fan-out. And
     * the `segment` this backend hands out self-routes its reclaim on whichever thread drops the
     * last reference, which is not in general the thread that allocated it. The default
     * `heap_backend()` already is thread-safe. A bare @ref mem::pool_t is NOT — its free list is a
     * plain `std::size_t` head and count with no lock and no atomic, so two receive threads can be
     * handed the same slot — and must be composed with the target's arch-selected synchronisation
     * before injection: `mem::synchronized_pool_t<>` guards it with the build's one reader
     * guard — the host `mutex_guard_t`, or `tr::esp::critical_pool_t`'s interrupt-masked
     * `critical_guard_t` on an MCU. A bounded node points this at the same slab as @p link_src /
     * @p rx only through such a composition. Must outlive the router.
     */
    mem::mem_backend_t* flat = &mem::net_backend();

    /**
     * @brief The byte backend the terminus REPLY's egress-construction segments draw from (#795,
     * ADR-0074): the reply head (peer-driven size — the swapped route bytes plus the inline tail)
     * and, on a mint, the trailing 12-byte `PATH_REF`. It is the last *reply-egress* byte source
     * that escaped a bounded node's slab (both folded READs' POINT headers — the composed root's
     * and the `":children"` listing's — are payload framing and draw from the graph's own
     * `value_backend` seam instead: #831, closed) — the head was hard-wired to
     * `view::heap_alloc`'s global heap, one allocation on every reply, peer-drivable and pre-auth
     * reachable (the denied path builds a head too). A DEDICATED injection, deliberately NOT
     * folded into @p flat: @p flat is documented and sized against FLATTEN (payload) bytes, and a
     * reply head is egress construction sized against ROUTE bytes, so widening @p flat's contract
     * would silently re-scope a slab deployments already set for flattens (a node could begin
     * refusing replies it used to send). Passed straight to the @ref graph::op_resolver_t.
     * The default is the net sub-pool; only a bounded node that points it at its slab gets the
     * bound. A refusal degrades
     * through the same empty-rope → `or_backpressure` → addressed `STATUS{BACKPRESSURE}` path OOM
     * already takes — answered by value, never an abort. MUST be thread-safe on the same terms as
     * @p flat. Must outlive the router.
     */
    mem::mem_backend_t* egress = &mem::net_backend();

    /**
     * @brief The backend for the two allocations a REMOTE SUBSCRIBE keeps for the LIFE OF THE
     * SUBSCRIPTION — the source `SUBSCRIBER` TLV and "the ONE route copy of the subscription's
     * life" (ADR-0041 §2). A DEDICATED injection for the reason @p egress is one: @p flat is
     * documented and sized against per-operation FLATTEN bytes, and these are neither per-
     * operation nor flattens. Their live set is the SUBSCRIPTION POPULATION, so a host that size-
     * classes @p flat finds the classes filled by a set that never returns and has nothing left
     * for the churn they were cut for — the classes stop working exactly when a client is
     * attached, which is the only time they are needed.
     *
     * DEFAULTS TO NULL, meaning "@p flat" — where both have always been taken. An un-injected
     * router is byte-for-byte unchanged, so this costs nothing to ignore. A host that injects it
     * can send these to a plain heap arm: one alloc and one free per subscription is not churn and
     * wants no class.
     *
     * Sized against the subscription population, NOT a concurrency factor. Exhaustion is answered
     * by value — a `BACKPRESSURE` on the subscribe, which simply does not bind. MUST be thread-
     * safe on the same terms as @p flat. Must outlive the router.
     */
    mem::mem_backend_t* retained = nullptr;
};

/**
 * @brief A session-anchor id (`:<mount>/<peer>`) in a fixed buffer — what
 *        @ref fwd_router_t::session_anchor_id returns (#1779).
 *
 * Bounded by the one packed segment record the anchor key is stored as, so it never needs
 * an allocation. Reads as a `std::string_view`; an id that would not fit reads EMPTY.
 */
struct session_anchor_id_t {
    std::array<char, wire::kPackedSegMaxBytes> bytes{}; /**< @brief The id's characters. */
    std::size_t len = 0;                                /**< @brief How many are used. */
    /** @brief The id; empty ⇔ it did not fit one segment record. */
    [[nodiscard]] std::string_view view() const noexcept { return {bytes.data(), len}; }
    /** @brief Implicit, so the id passes wherever a `std::string_view` is taken. */
    // NOLINTNEXTLINE(google-explicit-constructor)
    operator std::string_view() const noexcept { return view(); }
};

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
     * remote subscriber fans out a `FWD{WRITE}` back over the subscriber's link. The sink captures
     * `this`, so the router must outlive @p graph's use — the same lifetime the held `graph_`
     * reference already requires.
     *
     * @param graph The node's local graph.
     * @param planes The router's allocation planes (@ref router_planes_t); every member
     *               defaults to the process net sub-pool. Each must outlive the router.
     */
    explicit fwd_router_t(graph::graph_t& graph, const router_planes_t& planes = {})
        : graph_(graph),
          resolver_(graph, planes.flat, planes.egress,
                    planes.retained),  // the terminus tier draws flatten from
                                       // the SAME seam (#766), reply egress from
                                       // `egress` (#795), and subscription-scoped
                                       // retention from `retained` (#1610)
          link_src_(planes.link_src),
          rx_(planes.rx),
          flat_(planes.flat),
          egress_(planes.egress),
          // The NAME->link demux table's chunks are long-lived control state whose high-water
          // mark is the count of DISTINCT link names ever registered, so they take the LINK
          // store rather than the per-frame `rx` one — which a `bump_source_t` may legitimately
          // be, and which would fill monotonically under them (#873 phase 1).
          registry_(planes.link_src != nullptr ? *planes.link_src : mem::net_source()) {
        // The captureless {fn, ctx} pair the ADR-0047 doctrine prescribes (#1049) — the same
        // shape as `on_reverse_ref` below. The graph publishes it through a `sink_slot_t`,
        // so `this` must outlive every write that can still reach the producer fan-out;
        // constructing a router against a graph that is ALREADY serving frames is
        // unsupported, which is what `configure_` in the verb's name says.
        // The responder's reverse-mint seam (RFC-0024 §7.1 amendment 1): the resolver's
        // remote-subscribe arm asks for THIS node's reference to the connection vertex a
        // mint-flagged subscribe arrived on — the transport plane's mapping, so it is
        // injected here as the captureless {fn, ctx} pair the ADR-0047 doctrine prescribes.
        resolver_.on_reverse_ref(
            [](void* ctx,
               std::string_view inbound_link) -> std::optional<wire::path_ref_element_t> {
                return static_cast<fwd_router_t*>(ctx)->connection_ref(inbound_link);
            },
            this);
        // The terminus SUBJECT seam (#375 Part 2 / #1266): the resolver carries the frame's
        // opaque `peer_handle_t` but only the transport plane can say what peer it names, so
        // the derivation is injected through the same captureless {fn, ctx} pair. Installed
        // unconditionally and costing nothing until a link actually mints a per-peer subject
        // — with none, the thunk answers empty and the resolver keeps the inbound link name.
        resolver_.on_peer_subject(&fwd_router_t::peer_subject_thunk, this);
        // The terminus LINK-TOKEN seam (#1266 / #1417), installed beside the subject one and
        // for the mirror-image reason: only the transport plane knows which link a frame's
        // handle belongs to, and only it has somewhere to CACHE the graph's answer. Asked
        // lazily, at a remote subscribe and nowhere else, so an ordinary read/write/await
        // frame never touches it.
        resolver_.on_link_id(&fwd_router_t::link_id_of, this);
        resolver_.on_await_defer(&fwd_router_t::defer_await_thunk, this);
        // The wire `:subscribers[]` door's target descent (RFC-0021, #491): a SUBSCRIBER's
        // PATH child is an address in THIS node's frame, so deciding whether it leaves the
        // node is a mount question — the transport plane's, which L4 cannot name. Installed
        // here, through the same captureless {fn, ctx} pair, so a node that HAS a transport
        // plane resolves mount-path targets and one that does not cannot.
        // The graph's four TRANSPORT-PLANE seams, each installed in its own slot (#2239), so
        // the application's seams (subject, subscription observer) are never touched and an
        // application that installs one later never unplugs these. Each is the captureless
        // {fn, ctx} pair the ADR-0047 doctrine prescribes (#1049), published through a
        // `sink_slot_t`; `~fwd_router_t` clears all four if they are still this router's, so
        // a write after the router is gone finds no sink. `this` must still outlive every
        // dispatch already in flight; constructing a router against a graph that is ALREADY
        // serving frames is unsupported.
        //  - remote_delivery (#136): a write to a vertex with a remote subscriber fans out
        //    over the subscriber's link.
        //  - wire_target (RFC-0021, #491): a SUBSCRIBER's PATH child is an address in THIS
        //    node's frame, so deciding whether it leaves the node is a mount question — the
        //    transport plane's, which L4 cannot name.
        //  - stats_sampler (RFC-0010 Amendment 2, #1503): the router / per-link
        //    counters live on this side of the L4<->net-plane seam, so the router registers a
        //    sampler UP rather than L4 reaching DOWN.
        graph_.set_hook(
            &graph::graph_hooks_t::remote_delivery,
            {[](void* ctx, const graph::remote_delivery_t& sub, const graph::value_t& value) {
                 static_cast<fwd_router_t*>(ctx)->deliver_remote(sub, value);
             },
             this});
        graph_.set_hook(
            &graph::graph_hooks_t::wire_target,
            {[](void* ctx, std::span<const std::byte> key) -> graph::wire_target_split_t {
                 return static_cast<fwd_router_t*>(ctx)->split_subscriber_target(key);
             },
             this});
        graph_.set_hook(&graph::graph_hooks_t::stats_sampler,
                        {&fwd_router_t::stats_sampler_thunk, this});
        //  - link_name (#1941): an edge stores its link as a pair, and the session seam
        //    (#1841) still names a session. A build without that seam stores nothing for it.
        graph_.set_hook(&graph::graph_hooks_t::link_name,
                        {[](void* ctx, graph::link_pair_t link, std::span<char> scratch) {
                             return static_cast<const fwd_router_t*>(ctx)->link_name_of(link,
                                                                                        scratch);
                         },
                         this});
    }

    /**
     * @brief Answer nothing more: clear the graph seams this router installed (#2239), cancel
     *        every deferred AWAIT (ADR-0084), wait for any reply already being sent. Then
     *        disarm every @ref origin_t still armed on this router (#1645).
     */
    ~fwd_router_t();

    fwd_router_t(const fwd_router_t&) = delete;
    fwd_router_t& operator=(const fwd_router_t&) = delete;

    /**
     * @brief How many remote AWAITs this router holds right now (ADR-0084): armed waiters
     *        plus any whose reply is being sent.
     *
     * Each one is charged to the rx source of the link it arrived on, so this is also the
     * count of waiter blocks those sources have lent out.
     */
    [[nodiscard]] std::size_t pending_awaits() const;

    /**
     * @brief Terminus replies whose egress link was found by NAME rather than taken from the
     *        receive context the request arrived on (#1709).
     *
     * A request that arrives through a point-to-point child's receiver is answered through
     * that child's link directly, and never moves this counter. It moves only where a name is
     * all the router has: a BUS frame, whose inbound name is the sending peer's and resolves
     * through ADR-0044's peer fallback, and a frame pushed through the public by-name
     * @ref on_frame door, which carries no receive context. A count that grows on a
     * deployment with no bus children and no by-name ingress is a defect, never a correctness
     * one. Monotonic and relaxed (`core/STYLE.md` §Introspection).
     */
    [[nodiscard]] std::size_t reply_name_lookups() const noexcept {
        return reply_name_lookups_.load(std::memory_order_relaxed);
    }

    /**
     * @brief One snapshot of this router's counted cold-path drops (#1503 step 3).
     *
     * See @ref router_stats_t for what each field means and why the drops are counted HERE
     * rather than in the graph's delivery-drop counters. Snapshot coherence is the
     * `core/STYLE.md` §Introspection clause: six relaxed loads, so use the difference
     * between two snapshots, never the instant.
     */
    [[nodiscard]] router_stats_t drop_stats() const noexcept {
        return {.flatten_dropped = flatten_dropped_.load(std::memory_order_relaxed),
                .forward_iov_dropped = forward_iov_dropped_.load(std::memory_order_relaxed),
                .arena_dropped = arena_dropped_.load(std::memory_order_relaxed),
                .assemble_dropped = assemble_dropped_.load(std::memory_order_relaxed),
                .reply_iov_dropped = reply_iov_dropped_.load(std::memory_order_relaxed),
                .delivery_iov_dropped = delivery_iov_dropped_.load(std::memory_order_relaxed),
                .malformed_rx = malformed_rx_.load(std::memory_order_relaxed),
                .retired_rx = retired_rx_.load(std::memory_order_relaxed)};
    }

    /**
     * @name The four injected seams, exposed
     *
     * A host that injected a seam already holds it; a host that took a DEFAULT (the
     * process-wide heap source / backend) could not reach it at all, so it could not read
     * the `stats()` #1503 step 2 put on `mem::block_source_t`. These mirror
     * `graph_t::control_source` — a reference, never null, never owned, and
     * observation only — nothing in the library reads its own seams back through them.
     *
     * Per-seam, never aggregated (ADR-0079): there is deliberately no combined census here.
     * @{
     */
    /** @brief The source the router's long-lived link state draws from (@ref
     * router_planes_t::link_src). */
    [[nodiscard]] mem::block_source_t& link_source() const noexcept { return *link_src_; }
    /** @brief The DEFAULT terminus-arena / rx-scratch source. A child that carries its own
     *         (ADR-0067 §3) is not reported here — that one is the child's. */
    [[nodiscard]] mem::block_source_t& rx_source() const noexcept { return *rx_; }
    /** @brief The byte backend every rope flatten this router performs draws from (#730). */
    [[nodiscard]] mem::mem_backend_t& flatten_backend() const noexcept { return *flat_; }
    /** @brief The byte backend the terminus REPLY head and the mint egress bytes draw from
     *         (#795). */
    [[nodiscard]] mem::mem_backend_t& egress_backend() const noexcept { return *egress_; }
    /** @} */

    /**
     * @brief Register a named transport-child vertex (ADR-0027) at its EXISTING connection
     *        vertex.
     *
     * @p name is the mount RUN by which THIS node addresses @p link — both the leading
     * `dst` segments that route onward through @p link, and the run prepended to `src`
     * when a frame arrives on @p link (the way back).
     *
     * **A link cannot exist without its connection vertex (#1940).** The vertex keyed by
     * @p name must already be registered in the router's graph; it is the door every address
     * spelling reaches the link through and is gated at (RFC-0029 §6, §6.4). A mount with no
     * vertex is REFUSED, by value, with nothing registered — so "no connection vertex" always
     * means "no route". `transport_vertex_t::make_connection` registers the vertex first;
     * @ref attach_link registers both for a link wired by hand.
     * Installs a receiver on @p link that funnels each inbound frame to
     * `on_frame(name, ...)`. Call once per link, during setup (before frames flow).
     *
     * Installs the receiver matching the link's capability (ADR-0042 §1, ropes
     * per ADR-0053): an owning link (`link.delivers_ropes()`) gets a rope
     * receiver whose frame funnels through the SAME routing (the forward hop
     * stays span-based zero-heap; the refcounts ride only to the terminus);
     * every other link keeps the borrowed-span receiver unchanged.
     *
     * **@p name may have ANY number of segments** — `"up"`, `"ws-server/up"`, the RFC-0014
     * mount `"net/<module>/<name>"`, or something far deeper. The 1..3 bound is GONE (#523):
     * the mount descent no longer tries key widths from a compile-time constant downward, it
     * makes ONE pass over the registry and matches each slot against the prefix of that slot's
     * own width. Mount width is therefore bounded by the path-depth budget every address
     * spends from, and by nothing else — there is no constant to raise.
     *
     * The one thing still refused, and now refused ALWAYS rather than in debug builds only, is
     * a name **no address could ever name**: empty, containing an empty or over-long segment,
     * or wider than `graph::kMaxSegments` (a `dst` cannot carry that many segments, so nothing
     * could ever prefix-match it). Such a name used to append a slot that `size()` and
     * `live_size()` reported as a healthy child while every forward to it missed and fell through
     * to the terminus with no error anywhere — the #516 failure shape, one layer up.
     *
     * **A NAME owns exactly one receiver ctx, for the router's life (#884).** Registering a
     * name that already has one — a re-add after @ref remove_child, or a duplicate add of a
     * live name — REBINDS that ctx rather than appending a second, the same one-slot-per-name
     * rule @ref child_registry_t::add follows and for the same two reasons. A second ctx would
     * shadow the first on every name-keyed lookup (which returns the FIRST match, so the DEAD
     * one), and churn on a stable name set would grow the published chain without bound.
     *
     * @param name This node's local mount name for the link (e.g. "up", "ws-server/up").
     * @param link The transport carrying the next/previous hop.
     * @param rx   Optional per-child failable-block source; null uses the router's. Give
     *             each child its OWN when injecting a bounded one — see ADR-0067 3
     *             for why sharing one across receive threads is the wrong shape. Recorded
     *             on the child's receive context, not on the link (#1941).
     * @param kind The link's transport-catalog `(kind, role)` (#1650), which every write this
     *             child carries presents to the target's admission filter and handler as
     *             `graph::write_ctx_t::link`. Copied and interned here, ONCE per
     *             registration, and the interned record kept on the child's receive context,
     *             so a frame carries a pointer and pays no lookup;
     *             an empty `kind.kind` (the default) registers a link with no catalog
     *             identity, whose writes present a null `link`. `transport_vertex_t` passes
     *             its connection's declared pair.
     * @return false ⇔ @p name is unaddressable, has no connection vertex, or the registry
     *         could not grow — in every case NOTHING was registered.
     *
     * `[[nodiscard]]` since #892, and the reason it was NOT is worth recording because it was
     * wrong in a way that shipped a bug. The attribute was declined on the ground that "every
     * existing call site registers a name it composed itself, so it stays correct ignoring
     * it" — true of the *unaddressable-name* half, and false of the other half the return
     * value also carries. `child_registry_t::add` returns false when the table could not grow,
     * which no composed name can rule out; `make_connection` discarded exactly that and minted
     * a connection published UP but resolvable by no `dst` (#930). With the attribute, that
     * bug is a compile error rather than a silent ghost.
     *
     * A caller that genuinely cannot act on the failure writes `(void)` and says so. That is
     * the point of the attribute: it does not forbid ignoring the result, it makes ignoring it
     * a deliberate, greppable act instead of the default.
     *
     * The router's own registration state — the receiver ctx, a bus mount's token cache and
     * an interned link kind — is drawn from the link plane's source (`router_planes_t::
     * link_src`, the net sub-pool by default; #1779). A refusal there is the same `false`,
     * with nothing registered.
     */
    [[nodiscard]] bool add_child(std::string_view name, transport_t& link,
                                 mem::block_source_t* rx = nullptr, link_kind_t kind = {});

    /**
     * @brief Register @p link together with its connection vertex — the one helper for a
     *        link wired by hand (tests, SDK hosts, examples; #1940).
     *
     * Registers a plain `STORED_VALUE` vertex keyed by @p name, unless one is already
     * registered there (a vertex the caller made first, to carry an `:acl`, is used as it is),
     * then @ref add_child with the remaining arguments. A refused @ref add_child retires the
     * vertex this call registered, so a `false` leaves the graph as it found it.
     *
     * The production wiring is `transport_vertex_t::make_connection`, which registers its own
     * vertex (with the bus `:children` seam) before @ref add_child; this helper is for the
     * code that used to call @ref add_child with no vertex at all.
     *
     * Defined here, inline, so it is emitted only where it is called: the MCU archive, whose
     * wiring is `make_connection`, carries none of it (the no-heap link check, #1783).
     *
     * @return @ref add_child's verdict.
     */
    [[nodiscard]] bool attach_link(std::string_view name, transport_t& link,
                                   mem::block_source_t* rx = nullptr, link_kind_t kind = {}) {
        // The vertex's key is the mount's packed run, the same bytes `add_child` finds it by,
        // spelled into control-plane scratch from the link plane's source (#1779) and released
        // before returning. An empty run keys nothing, so `add_child` refuses it below.
        const std::size_t n = child_registry_t::encode_mount_name(name, nullptr);
        auto* const key = static_cast<std::byte*>(link_src_->try_alloc(n + 1, 1));
        if (key == nullptr) return false;
        (void)child_registry_t::encode_mount_name(name, key);
        // `PATH_IN_USE` is the caller's own vertex, which is used as it is and never retired here.
        const graph::result_t<graph::vertex_handle_t> made = graph_.register_vertex_key(
            std::span<const std::byte>(key, n), graph::role_t::STORED_VALUE);
        link_src_->release(key, n + 1, 1);
        if (add_child(name, link, rx, kind)) return true;
        if (made) (void)graph_.retire(*made);
        return false;
    }

    /**
     * @brief Un-register child @p name — it stops resolving, and its routing state goes.
     *
     * The teardown counterpart of @ref add_child (#494). Tombstones the registry entry
     * (@ref child_registry_t::erase) and then runs the full @ref link_down eviction, so
     * the subscriber edges bound to the name leave with it.
     *
     * **This is removal, not departure.** @ref link_down alone is the right hook for a
     * link that merely dropped: under RFC-0014 a DIAL connection's *vertex* outlives its
     * *socket* and self-heals, so evicting its registry entry on a down notification
     * would permanently unroute a link that is only reconnecting. Call this one when the
     * connection itself is gone — and call it BEFORE destroying the `transport_t`, so no
     * NEW forward resolves it.
     *
     * **The link must outlive the call by a grace point.** A forward that resolved the child
     * just before this call may still be inside the link's `send` when it returns, so the
     * caller must not destroy the link yet. Every way into a child link is bracketed
     * (`frame_scope_t`), so the grace point is the graph's: hand the destruction to
     * `graph_t::park_release` after this call, and the first `graph_t::collect()` that no
     * frame open now is still inside runs it. @ref transport_t::shut_down stops the link
     * at once without destroying it. `tr::net::transport_vertex_t` does exactly this for the
     * links it owns; a link the app wired here itself is the app's to keep.
     *
     * The child's receiver ctx is TOMBSTONED, not unlinked (#884): it stays on the published
     * chain — a lock-free reader may be walking it right now — but stops answering every
     * name-keyed and slot-keyed lookup, and a later @ref add_child of the same name revives it
     * in place. That is what keeps `connection_ref`/`hop_mint` off the dead context after a
     * re-add, and what keeps create/remove churn on a stable name set from growing the chain.
     * @param name The child's registered NAME.
     * @return true if @p name named a live child, false if it named none.
     */
    bool remove_child(std::string_view name);

    /**
     * @brief Bind a local producer's subscription toward a MOUNT-PATH target — the
     *        locally-initiated dual of the wire `:subscribers[]` append (#739).
     *
     * @p target is one ordinary path that routes through a transport mount (e.g.
     * `/net/ws-client/b/display/val`, arbitrarily nested `/net/A/net/B/x`). It is
     * resolved through the SAME strip-K cached descent the forward path uses
     * (ADR-0061), so the caller never hand-splits `(link, return route)` — the split
     * that bakes in a single-hop assumption and the `net/<module>/<name>` string
     * shape. The residual below the matched mount becomes the delivery route the
     * first hop forwards, exactly as an inbound `FWD` would carry it.
     *
     * **Bind-time resolution, link-lifetime durability.** The `(link, route)` split is
     * computed ONCE, here. If the link later tears down, the binding is evicted with it
     * (@ref link_down) and re-binding is the application's job — this helper does not
     * track topology churn (re-establishment is #716's question, not this API's).
     *
     * A target whose first hop lands on a bus PEER (`/net/ws-server/mesh/p0/...`) is
     * rejected with `INVALID_PATH`: the per-delivery sink resolves the stored link by
     * its registry NAME, and a peer has no directed registry entry to store — binding
     * it would produce a subscription that silently broadcasts. Directed bus-peer
     * delivery arrives with the #741 work.
     *
     * @param producer The local producer vertex's path.
     * @param target   The mount-path target, spelled from THIS node's root.
     * @return `NOT_FOUND` if @p producer names no vertex; `INVALID_PATH` if @p target
     *         does not route through a mount (or lands on a bus peer);
     *         `BACKPRESSURE` on allocation failure; otherwise the
     *         @ref graph::graph_t::subscribe_wire admission result.
     */
    [[nodiscard]] graph::result_t<void> subscribe_toward(const graph::path_t& producer,
                                                         const graph::path_t& target);

    /**
     * @brief Split a subscriber target key at the mount it routes through (RFC-0021 §4.B) —
     *        the descent @ref subscribe_toward and the wire `:subscribers[]` door share.
     *
     * The one place "does this target leave this node, and over which link" is decided, so
     * the host-local dual and its wire twin (#491) cannot drift. Runs the ADR-0061 strip-K
     * descent over the child registry, exactly as the forward path resolves a frame's `dst`.
     *
     * @param key The target's canonical key — concatenated `NAME` records. Its bytes must
     *            outlive the returned split, whose `residual` borrows from them.
     * @return `{}` (empty link, not unroutable) ⇔ NO mount matched — the target names
     *         something this node terminates; `unroutable` ⇔ a mount was named but no
     *         directed delivery can be bound through it; otherwise the mount's registry NAME
     *         and the residual below it.
     */
    [[nodiscard]] graph::wire_target_split_t split_subscriber_target(
        std::span<const std::byte> key) const;

    // -- bound paths (RFC-0024) -------------------------------------------------------
    // The origin's half of the bound form. The forwarder's half needs no API: it is a
    // branch on the inbound frame's own `dst` type, taken inside the routing hop.

    /**
     * @brief This node's own vertex ref for the connection vertex of child @p link_name —
     *        element 0 of a route that leaves through it (RFC-0024 §4.1).
     *
     * The origin mints its OWN first element, because no one else can: an element is
     * node-scoped, and the hop out of this node is the one hop no peer ever sees. A mint
     * reply therefore comes back one element short of the route, and this is the element
     * that completes it (which @ref adopt_binding does for the caller).
     *
     * @retval std::nullopt @p link_name names no registered child, the child has no
     *         connection vertex in this node's graph, or that vertex's generation has
     *         SATURATED (permanently unbindable, §4.4 rule 3). Every one of those means the
     *         route has no bound spelling from here, and the canonical path is the answer.
     */
    [[nodiscard]] std::optional<wire::path_ref_element_t> connection_ref(
        std::string_view link_name) const;

    /**
     * @brief Install the bound form on @p path from the mint answer on a reply (RFC-0024 §7.4).
     *
     * The origin's side of the exchange, and the reason `path_t::bind` had no production
     * caller until now. @p reply is a validated `FWD{REPLY}` node (read in place, #1829); its
     * LAST child is the accumulated
     * `PATH_REF` — the terminus's element, with one prepended by every hop that forwarded the
     * reply. This prepends THIS node's own element for @p link_name (§4.1: element 0 is the
     * origin's reference to its first-hop connection vertex) and records the whole stack.
     *
     * Nothing is installed unless the complete route is spellable: a reply with no mint, a
     * link with no bindable connection vertex, or a stack past the normative element cap all
     * leave @p path exactly as it was — bound to nothing, and therefore canonical. A binding
     * is an optimisation with a fallback that always works, so a partial one is never worth
     * having.
     *
     * @return true iff @p path came out bound.
     */
    bool adopt_binding(graph::path_t& path, std::string_view link_name,
                       const wire::tlv_node_t& reply);

    /**
     * @brief What a bracketed send hands the app: the egress link and the `dst` to put on the
     *        wire, both valid only until the callback returns.
     *
     * The link is not the app's to keep. The call runs the callback inside the router's frame
     * bracket, so a link removed meanwhile is not freed until the callback has returned (see
     * `graph_t::collect`); a pointer kept past the callback has no such guarantee.
     */
    using egress_fn_t = void (*)(void* ctx, transport_t& link, std::span<const std::byte> dst);

    /**
     * @brief Send the next operation over @p path's binding — the origin's own hop (§4.1).
     *
     * The origin consumes element 0 exactly as every forwarder consumes its own: it is this
     * node's reference to its first-hop connection vertex, so it selects the link and does
     * NOT go on the wire. What goes out is the residual, a `PATH` of `H−1` PAIR elements
     * (`4 + 11×(H−1)` bytes, RFC-0029 §5.2 — the retired `PATH_REF` array is no longer an
     * address), and the host that receives it consumes ITS element in turn — the monotone
     * shrink that makes a bound path loop-free for the same reason a canonical `dst` is.
     *
     * @p send composes the frame around `dst` and sends it on `link`, on this thread, inside
     * the router's frame bracket (@ref egress_fn_t).
     *
     * @param right The right the operation carries, evaluated at the dereferenced connection
     *              vertex like any other hop's (§6.2).
     * @return false, with @p send not called, when @p path is unbound, or element 0 no longer
     *         validates — a link removed, a connection vertex retired, an ACL revoked — or the
     *         link plane's source refused the `dst` bytes. The caller's recovery is the one
     *         that always works: `clear_binding()` and send the canonical path it still holds,
     *         which may then re-mint (§5.3).
     */
    [[nodiscard]] bool bound_send(const graph::path_t& path, graph::acl_right_t right,
                                  egress_fn_t send, void* ctx) const;

    // -- observability / terminus sinks (fn-ptr + context, ADR-0047/ADR-0068 §3) -------
    // Not std::function: these fire on the per-frame RX path, where the erasure
    // machinery (code size, heap capability, exception paths) is the largest avoidable
    // embedded liability — the same call graph_t::subscriber_fn_t already makes at L4.
    //
    // Each pair lives in a `sink_slot_t` (#914), so a setter may run while frames flow:
    // the pair is published as a unit and every frame-path read snapshots it, which is
    // what makes "passing nullptr clears a sink" safe at runtime rather than a race.
    // What a clear does NOT do is stop a dispatch already in flight — a receive thread
    // may be inside the sink, or hold a snapshot taken a moment before — so the ctx must
    // still outlive every possible dispatch, as it must for `receiver_slot_t`.

    /** @brief Reply-terminus sink (@ref on_reply): @p ctx, then the FWD{REPLY} frame. */
    using reply_fn_t = void (*)(void* ctx, const view::rope_t& reply);
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
     * only a multi-link reply pays one flatten, on demand. A sink that reads the frame walks
     * those bytes in place (`wire::tlv_node_t::over(m.bytes())`). The materialize escape hatch
     * (ADR-0052) now lives at the consumer, not the router; keep `m` alive while reading
     * its span.
     *
     * @param fn  Callback invoked on a transport receive thread; keep it cheap.
     * @param ctx Opaque pointer handed back as @p fn's first argument.
     */
    void on_reply(reply_fn_t fn, void* ctx = nullptr) noexcept;

    /**
     * @brief The link-departure hook (RFC-0009 §D extended to peer departure): evict
     *        everything the routing plane holds against a link that just died.
     *
     * Two halves, in order: `graph_t::evict_link_edges(link_name)` deactivates and
     * reclaims every subscriber edge whose stored link is @p link_name (write fan-outs
     * stop addressing the dead session, and its ~90 B/edge of route/link/caller state is
     * released — the C6's measured ~27 KB/browser-session leak), then every deferred AWAIT
     * filed under it is released (ADR-0084).
     *
     * `add_child` installs this automatically behind every child's departure notifier
     * (`transport_t::set_down_notifier` with the child's registered NAME; the bus facet's
     * `bus_link_t::set_peer_down_notifier` with the departed PEER's name — the same name
     * inbound frames were tagged with, hence the name subscriber edges stored). A host
     * that learns of a departure out-of-band (its own session manager) may call it
     * directly; calling it for a live or unknown link is safe (eviction of nothing is a
     * no-op). Edges name their link by PAIR (#1941), so this by-name door evicts the edges of
     * the registered child @p link_name names; a bus PEER's edges leave through the bus's
     * own peer-down seam, which holds the peer's handle, or through the by-handle overload.
     *
     * Runs on the calling thread (typically a transport receive/close thread) and takes
     * graph locks — callers must hold no transport-internal locks (the
     * `set_down_notifier` calling discipline).
     * @param link_name The routing plane's inbound NAME for the departed link/peer.
     */
    void link_down(std::string_view link_name);

    /**
     * @brief The by-HANDLE door for one bus peer's departure (#2237): what the bus's own
     *        peer-down seam runs, for an application that learned of it some other way.
     *
     * An announce-census bus (the CAN binding) never sees a peer leave, so it never fires
     * `bus_link_t::set_peer_down_notifier`, and @ref link_down by name reaches only a
     * registered child's edges. Here the application names the bus mount @p link_name and
     * the peer's @p peer handle, and the router runs that peer's departure: it evicts every
     * edge and pending AWAIT filed under the peer, retires the peer's session anchor if the
     * peer still holds its slot, and drops its cached link token. A CAN peer's handle is
     * `{node id, transport_can::kAnnouncedPeerGeneration}`, the one its frames arrive under.
     *
     * Calling it for a peer that already left, or never subscribed, evicts nothing and is
     * safe. The calling discipline is @ref link_down's: callers hold no transport-internal
     * locks.
     * @retval true  @p link_name is a live bus mount and @p peer is a handle that bus mints
     *               (its `peer_name` names it), whether or not that peer is still heard.
     * @retval false no live child is named @p link_name, it has no bus facet, or @p peer is
     *               not a handle that bus mints. Nothing was evicted.
     */
    bool link_down(std::string_view link_name, peer_handle_t peer);

    /**
     * @brief This node's NAME for the link @p link names (#1941): its registry slot's name,
     *        or, for a bus peer under it, the bus's positional peer name.
     *
     * The edge stores its link as a pair, so a consumer that still keys by name asks here,
     * off the delivery path: the session seam's `RELEASE` (`graph::graph_hooks_t::link_name`)
     * and the link-hold count (`transport_vertex_t`). A peer's name may be formatted into
     * @p scratch (at least `kPeerNameChars`); the answer is borrowed for the call.
     * @retval {} @p link names no live slot.
     */
    [[nodiscard]] std::string_view link_name_of(graph::link_pair_t link,
                                                std::span<char> scratch) const;

    /**
     * @brief The node-scoped identity string of the session @p peer holds on bus mount
     *        @p mount — the key `graph_t::register_session_anchor` allocates against (#1223).
     *
     * Keyed by the peer's SLOT (`%session_slot_of`: `p<slot>~<gen>` anchors as `p<slot>`), so
     * one slot keeps ONE anchor however many sessions pass through it — the count is the slot
     * count (ADR-0044 amendment) — and the anchor's own retire generation tells its tenancies
     * apart. Which tenancy holds it is the mount's tenancy word for that slot
     * (`session_tenancy_t`): its session's peer handle, whose exact name the bus spells (#2234).
     *
     * Deliberately NOT a spellable address. It is `:<mount>/<slot>`, and both `:` and `/` are
     * among the seven characters `path::valid_segment` rejects, so the anchor's rendered key
     * is bytes no registered path in this graph can produce — the property that keeps an
     * anchor out of every listing, every descent and every sweep-set collision. Qualified by
     * the mount because two listeners each name their first session `p0`, and those are two
     * sessions.
     *
     * Exposed because it is what a test (and a later step's mint) needs in order to name the
     * anchor the router created; it is a pure function of its arguments and holds no state.
     *
     * Returned BY VALUE in a fixed buffer, with no allocation (#1779): the anchor key is ONE
     * packed segment record, so an id longer than `%wire::kPackedSegMaxBytes` could never be
     * stored anyway. Such an id comes back EMPTY, and the router anchors nothing for it.
     */
    [[nodiscard]] static session_anchor_id_t session_anchor_id(std::string_view mount,
                                                               std::string_view peer) noexcept;

    /**
     * @brief Sample one NET-PLANE `:stats` seam (RFC-0010 Amendment 2, #1503 residual).
     *
     * The body behind the sampler this router installs on the graph in its constructor;
     * public so an in-process supervisor can ask for the same block the wire door serves,
     * without re-deriving the seam vocabulary from the raw accessors.
     *
     * The classes, and the nouns each publishes:
     *
     * - `router.drops` — @ref drop_stats, all seven per-cause counters.
     * - `link.<child>` — one registered child, by its `child_registry_t` NAME (the same key
     *   as its `/net/<module>/<name>` connection vertex): `dropped_rx`, `malformed_rx`,
     *   `dropped_tx` from @ref transport_t::drop_stats. A name that is not registered, or
     *   whose slot is a
     *   tombstone (a removed link), is NOT a seam — the caller gets `false`.
     *
     * @param seam_class The seam CLASS.
     * @param seam_name  The seam NAME within it.
     * @param out        Where to write the block, or `nullptr` to PROBE recognition only —
     *                   which is how the graph settles NAME validity above its READ gate
     *                   without sampling anything for a caller it will refuse.
     * @return `true` iff the spelling names a seam this router serves.
     */
    [[nodiscard]] bool sample_stats(std::string_view seam_class, std::string_view seam_name,
                                    graph::stats_block_t* out) const noexcept;

    /**
     * @brief Route one inbound FWD frame that arrived on child @p inbound_name.
     *
     * Forwards (dst-shrink + src-grow) toward a transport child, resolves+replies
     * for a local terminus, or delivers a terminal REPLY to the reply sink. A
     * malformed or non-FWD frame is dropped. Never blocks on a downstream peer
     * beyond the transport's own bounded send.
     *
     * Runs on the calling (transport RX) thread and takes graph and router locks — the
     * caller must hold no transport-internal locks across this call.
     *
     * @param inbound_name This node's NAME for the link the frame arrived on.
     * @param frame        The complete inbound FWD frame bytes (borrowed; consumed
     *                     before this call returns — sends happen inline).
     */
    void on_frame(std::string_view inbound_name, std::span<const std::byte> frame);

    // -- origination (#1645) ------------------------------------------------------------

    /**
     * @brief The caller-owned record of ONE request this node originated with @ref originate.
     *
     * The requester owns correlation and deadline (RFC-0004 §B: forwarders are stateless), so
     * the state an outstanding request needs lives HERE, in storage the caller provides, and
     * not in the router. The router threads outstanding records through an intrusive list it
     * only borrows; it allocates nothing per request and keeps nothing once a record leaves.
     *
     * A record is armed by @ref originate and disarmed exactly once, by whichever comes
     * first: the matching `FWD{REPLY}` (the sink runs, then the record is free again), or
     * @ref cancel — which is how the caller's own deadline ends a request. There is no timer
     * and no clock anywhere in the router; "how long to wait" is the caller's decision.
     * Destroying an armed record cancels it.
     *
     * Not copyable and not movable: the router holds its address while it is armed.
     */
    class origin_t {
       public:
        /**
         * @brief A record whose reply goes to @p fn with @p ctx.
         * @param fn  Called once with @p ctx and the matching `FWD{REPLY}` frame, rope-native
         *            and with the same contract as the @ref on_reply sink. Runs on the thread
         *            that delivered the reply — a transport receive thread, or the caller's
         *            own thread inside @ref originate when the reply is produced
         *            synchronously (a local terminus, a synchronous link). The record is
         *            already disarmed when it runs, so it may re-originate on the same record.
         * @param ctx Opaque pointer handed back as @p fn's first argument. It must outlive
         *            every dispatch: when @ref cancel returns `false` a reply was, or is being,
         *            delivered, and the caller synchronizes with its own @p fn.
         */
        origin_t(reply_fn_t fn, void* ctx) noexcept : fn_(fn), ctx_(ctx) {}
        /** @brief Cancels the record if it is still armed. */
        ~origin_t();
        origin_t(const origin_t&) = delete;
        origin_t& operator=(const origin_t&) = delete;

        /** @brief True from a successful @ref originate until its reply or @ref cancel. */
        [[nodiscard]] bool armed() const noexcept {
            return owner_.load(std::memory_order_acquire) != nullptr;
        }

       private:
        friend class fwd_router_t;
        /** @brief The router this record is armed on, or null. Written under its lock. */
        std::atomic<fwd_router_t*> owner_{nullptr};
        origin_t* next_ = nullptr; /**< @brief Next-newer-to-older link, under the lock. */
        reply_fn_t fn_;            /**< @brief Reply sink. */
        void* ctx_;                /**< @brief Reply sink context. */
        /** @brief The return route's packed `PATH` body: `token_` or a caller's key. */
        std::span<const std::byte> route_{};
        /** @brief Storage for the generated one-record return route (see `originate`). */
        std::array<std::byte, 16> token_{};
    };

    /**
     * @brief Originate a `READ` or `WRITE` from inside this node, through the SAME ingress
     *        a frame arriving on a link takes — without building wire bytes and without
     *        feeding @ref on_frame under a made-up child name.
     *
     * Builds `FWD{ op, dst, FIELD?, src, payload? }` (RFC-0004 §B) into ONE exactly-sized
     * segment from the injected egress backend (@ref router_planes_t::egress), arms @p slot,
     * and routes the frame with no inbound link: a forward hop strips the mount from `dst`
     * and grows `src` by nothing (this node is the origin), and a `dst` that names no mount
     * is resolved here, its reply handed straight to @p slot. The segment is released before
     * this returns; the router keeps no buffer and no per-request state.
     *
     * An **append** is a `WRITE` whose @p dst carries an append selector —
     * `/…/vertex:subscribers[]` — and so needs no opcode of its own: @p dst's `:field` tail
     * rides as the `FIELD` selector. `AWAIT` is not originated here.
     *
     * The reply is paired by its return route. Without @p reply_to the router spells a fresh
     * one-segment route per request (a reserved `~o<hex>` NAME, valid as a segment and never a
     * mount), so pairing is exact. With @p reply_to the request's `src` is that local path —
     * which a subscribe needs, because the producer delivers to its `src` — and requests
     * sharing one route pair oldest-first, the arrival-order fallback the reference states
     * for replies their route cannot tell apart. A reply no armed record matches goes to the
     * @ref on_reply sink, as before.
     *
     * @param slot     The caller's record; must not already be armed.
     * @param op       `READ` or `WRITE`.
     * @param dst      The target, spelled from THIS node's root (`/net/<module>/<name>/…`),
     *                 optionally with a `:field` tail.
     * @param payload  A `WRITE`'s value — ONE complete TLV, copied into the frame. Empty for
     *                 a `READ`.
     * @param reply_to Optional local return route; must not route through a mount. When
     *                 given, its bytes are borrowed and it must outlive the armed record.
     * @return Success once the frame has been routed (a reply may already have been
     *         delivered); `TYPE_MISMATCH` for any other @p op, or a `WRITE` without a
     *         payload; `INVALID_PATH` for an empty @p dst or an unusable @p reply_to;
     *         `BACKPRESSURE` when @p slot is already armed or the egress backend refuses the
     *         frame.
     */
    [[nodiscard]] graph::result_t<void> originate(origin_t& slot, graph::fwd_op_t op,
                                                  const graph::path_t& dst,
                                                  std::span<const std::byte> payload = {},
                                                  const graph::path_t* reply_to = nullptr);

    /**
     * @brief Disarm @p slot — the caller's deadline expired, or it no longer wants the reply.
     * @return `true` iff @p slot was armed: its sink will not run for this request.
     *         `false` if it was not armed — its reply was, or is being, delivered.
     */
    bool cancel(origin_t& slot) noexcept;

    /** @brief The connection registry (test introspection — the shared demux table). A link
     *         it names is for identity checks: nothing here brackets a send on it, so send
     *         through @ref bound_send instead. */
    [[nodiscard]] const child_registry_t& registry() const noexcept { return registry_; }

    /**
     * @brief How many receiver contexts this router holds (test introspection — the churn
     *        bound of #884): one per NAME ever registered, live or tombstoned.
     *
     * The twin of @ref child_registry_t::size, and asserted against for the same reason: it is
     * the length of the chain every name-keyed and slot-keyed lookup walks, so "create/remove
     * churn does not grow it" is a latency property as well as a memory one. Takes the control
     * lock, so it is a control-plane call — never a per-frame one.
     */
    [[nodiscard]] std::size_t receiver_ctx_count() const {
        const std::lock_guard ctl(ctl_m_);
        std::size_t n = 0;
        for (const child_rx_ctx_t* c = rx_head_.load(std::memory_order_relaxed); c != nullptr;
             c = c->next.load(std::memory_order_relaxed))
            ++n;
        return n;
    }

   private:
    /**
     * @brief Brackets one inbound frame, or one origination, on this thread.
     *
     * Every way into a child link goes through one of these: the child receivers, the bus
     * notifiers, `on_frame`, `originate`, `deliver_remote`,
     * `link_down`, @ref bound_send and a fired AWAIT. A link the transport plane
     * removes is parked on the graph at an epoch closed after it was unrouted
     * (`graph_t::park_release`), and `graph_t::collect()` frees it only once every bracket open at
     * that epoch has closed, so a removed link is never freed while a frame is inside it
     * (`transport_vertex_t::with_link` opens the same bracket). In every
     * build, under every ADR-0080 policy: the bracket is the QSBR domain's read side
     * (`%detail_qsbr::enter` / `leave`), whose nesting depth `fan_out`'s own bracket shares. The
     * outermost exit is a quiescent state (`graph_t::thread_quiescent`, a no-op unless
     * `reclaim_qsbr` is bound).
     */
    using frame_scope_t = graph::detail_qsbr::frame_scope_t;

    /**
     * @brief The egress link a bound element names, after the full §5.1 check.
     *
     * The forwarder's hop and the origin's first hop are the same act — consume element 0,
     * dereference it, egress — so they are the same function. It bounds-checks the index,
     * compares the generation, refuses a saturated one, and evaluates the ACL at the
     * dereferenced vertex for @p right (§6.2: a generation match authorizes nothing).
     *
     * @param e      The element to consume.
     * @param caller The subject context — the inbound link's name at a forwarder, empty for
     *               this node's own local caller at an origin.
     * @param right  The right the operation carries; `nullopt` for an opcode this build names
     *               no right for, which the gate refuses on a graph that enforces an ACL.
     * @retval nullptr Any part of the check failed, or the vertex is not a connection vertex
     *         of a live point-to-point child of this node. The gate is `door_allows`, the
     *         one every spelling runs. The caller MUST then drop —
     *         never repair, never fall through to a different route (§5.3). A bus PEER is
     *         among the refusals and not by omission: a peer has no vertex, so no element
     *         can name one, and egressing over the bus link itself would BROADCAST a
     *         directed operation (ADR-0073 §3 / RFC-0020).
     *
     * Private because the link it answers is valid only inside the frame bracket its caller
     * holds (`frame_scope_t`); the app reaches it through @ref bound_send, which holds one.
     */
    [[nodiscard]] transport_t* bound_egress(wire::path_ref_element_t e, std::string_view caller,
                                            std::optional<graph::acl_right_t> right) const;

    /**
     * @brief A link's stable per-child receiver state — its identity AND its mount run.
     *
     * `mount_tlv` is the child's mount path pre-encoded as a run of NAME TLVs, the same
     * bytes @ref child_registry_t::child_t holds. Carrying a COPY here is what keeps the
     * forward path off the registry entirely: a hop grows `src` from `ctx->mount_tlv`
     * instead of scanning the table for the inbound child, which removes one of the two
     * per-frame linear scans (`docs/performance.md` §2b).
     *
     * A copy rather than a pointer into the registry slot is deliberate — but NOT for the
     * reason this comment used to give. It read: *"Slot addresses are NOT stable across a
     * connection-create — `children_` is a vector and `add` appends (#521) — so a cached slot
     * pointer would be a use-after-free."* That was true when written and is now the exact
     * OPPOSITE of the shipped invariant: ADR-0063 replaced the vector with a chunked list
     * precisely so slot addresses would become permanently stable, and calls that "a deliberate
     * second effect, not a side effect": a holder may keep a `const child_t*` and let the
     * teardown tombstone be the invalidation. Anyone reasoning
     * about slot lifetime from the old sentence would reach the wrong conclusion, which is why
     * it is corrected rather than deleted.
     *
     * The surviving reason for the copy is drift-freedom, not lifetime: the run is a pure
     * function of the child's name, so re-adding a tombstoned name yields identical bytes, and
     * this deque never invalidates a reference either.
     */
    /**
     * @brief The BUS tier's per-peer LINK-TOKEN cache — direct-mapped, fixed, lock-free.
     *
     * The carry (#1266 / #1417) needs the arriving peer's `graph_t::intern_link` token in
     * hand at the subscribe branch, and a bus mount carries MANY peers through one receive
     * context, so the token cannot live on the ctx the way the flat tier's does.
     *
     * Direct-mapped on `peer_handle_t::index` and FIXED rather than grown, deliberately. A
     * growable table would have to be published to a lock-free reader on the transport's own
     * receive thread, which is a retire list and a lifetime argument for a CACHE whose every
     * miss is already correct — the index simply interns the name it was handed. Peer indices
     * are slot numbers (`p<slot>~<gen>`) or bus node ids (`n<node>`), so they are small and dense
     * and the ways collide only past `kWays` live peers on one mount; a collision costs
     * one name lookup and nothing else.
     *
     * WHY THE PEER'S GENERATION IS NOT PART OF THE KEY: a recycled index carries a NEW
     * name (`p<slot>~<gen>`), so its entry must not survive the departure — and it does not:
     * `bus_peer_down` drops the way after the eviction, and an arrival runs that departure
     * first. The stamp the cache holds is the token's own, and `graph_t` validates that
     * itself — an entry left behind by a departed session fails there and degrades to a
     * lookup.
     */
    struct bus_token_cache_t {
        /** @brief Ways in the direct-mapped table. */
        static constexpr std::size_t kWays = 16;
        /** @brief Occupancy tag: the peer index PLUS ONE, so `0` reads as empty. */
        std::atomic<std::uint32_t> key[kWays] = {};
        /** @brief `link_id_t::bits()` for the peer `key` names. 64-bit because the id is an
         *         `(index, generation)` pair read whole — stored and loaded, never an RMW; on
         *         rv32 each access is an `__atomic_*_8` call (#1697). */
        std::atomic<std::uint64_t> val[kWays] = {};

        /**
         * @brief @p peer's cached token, or an invalid one.
         *
         * The tag is read, the value taken, and the tag RE-READ: a writer hides its way
         * before rewriting it, so a tag that reads the same peer index either side of the
         * value pins the value to that peer. Two peers cannot both tag one way — the tag is
         * `index + 1` and indices are distinct — so this cannot hand back another peer's
         * token, and a torn read of one peer's own successive tokens is harmless because
         * both name the same link NAME.
         */
        [[nodiscard]] graph::link_id_t peek(peer_handle_t peer) const noexcept {
            const std::size_t w = peer.index % kWays;
            const std::uint32_t want = peer.index + 1;
            if (key[w].load(std::memory_order_acquire) != want) return {};
            const std::uint64_t bits = val[w].load(std::memory_order_relaxed);
            if (key[w].load(std::memory_order_acquire) != want) return {};
            return graph::link_id_t::from_bits(bits);
        }

        /** @brief Record @p id for @p peer, hiding the way while the pair is inconsistent. */
        void put(peer_handle_t peer, graph::link_id_t id) noexcept {
            const std::size_t w = peer.index % kWays;
            key[w].store(0, std::memory_order_release);
            val[w].store(id.bits(), std::memory_order_relaxed);
            key[w].store(peer.index + 1, std::memory_order_release);
        }

        /** @brief Forget @p peer, if this way is still its — the departure half. */
        void drop(peer_handle_t peer) noexcept {
            const std::size_t w = peer.index % kWays;
            if (key[w].load(std::memory_order_relaxed) == peer.index + 1)
                key[w].store(0, std::memory_order_release);
        }

        /** @brief Forget every peer — a re-add rebinds the whole mount (#884). */
        void clear() noexcept {
            for (std::atomic<std::uint32_t>& k : key) k.store(0, std::memory_order_release);
        }
    };

    /**
     * @brief A bus mount's TENANCY WORDS: per session slot, the `peer_handle_t::bits()` of the
     *        session that holds the slot's anchor, `0` when none does (#2234).
     *
     * What a route to an anchor, and the reverse-direction mint, resolve the session by: one
     * acquire load names the exact tenancy, so a delivery reaches `peer_link_of` for that
     * handle and never a successor's. Held here rather than as the anchor's value, so an
     * arrival or a departure touches no `value_t` and allocates nothing once the table covers
     * the slot. 8 B per slot; the words follow this header in one block from the link plane's
     * source.
     *
     * Grown, never shrunk, under `tenancy_m_`: a table too small for an arriving slot is
     * replaced by one twice as large and the old one PARKED on `parked`, not freed, so a
     * lock-free reader still holding it reads a live (if stale-sized) table. The whole chain
     * goes back in the router's destructor.
     */
    struct alignas(std::atomic<std::uint64_t>) session_tenancy_t {
        std::uint32_t size = 0;              /**< @brief Slots the words cover. */
        session_tenancy_t* parked = nullptr; /**< @brief The table this one replaced. */
        /** @brief The word for @p slot; `slot < size`. */
        [[nodiscard]] std::atomic<std::uint64_t>& word(std::uint32_t slot) noexcept {
            return std::launder(reinterpret_cast<std::atomic<std::uint64_t>*>(this + 1))[slot];
        }
    };

    /**
     * @brief One registered child's receive context: what the frame path reads per inbound
     *        frame without taking `ctl_m_`.
     *
     * **`link_token` is an IDENTITY, not a counter (#1697).** It packs an `(index, generation)`
     * pair that one relaxed load must read whole, and it is never an RMW: the control plane
     * stores, the frame path loads. On rv32 such a load is an `__atomic_load_8` call, and the
     * flat-carry load is on the frame path, so it stays a known per-frame cost on the ESP32-C6
     * until the pair is split or seqlocked. `rmw_counter_t` does not apply to a value that is
     * stored, not bumped.
     */
    struct child_rx_ctx_t {
        fwd_router_t* self;
        /** @brief The child's registered NAME — a view of its registry slot's text, which is
         *         immutable after publish and lives as long as the router (#1779). */
        std::string_view name;
        /** @brief The child's mount run — a view of the same slot's text (#1779). */
        std::span<const std::byte> mount_tlv;
        /**
         * @brief This child's OWN failable-block source; null falls back to the router's.
         *
         * ADR-0067 §3: a `pool_source_t` shared across receive threads is measured-worse
         * than the heap it replaces (ADR-0060 erratum 1 — ~1/15 of its single-thread rate
         * on 12 cores). Each transport has its own receive thread, so a source parked here
         * is touched by exactly one — the per-thread shape, obtained by ownership rather
         * than by a lock. A bounded node gives each child its own slab, which also makes
         * the bound per-peer: one noisy link cannot starve another's decode.
         *
         * Held HERE and not on the link (#1941): it is a fact of the registration, which only
         * a router child has. A bus peer's endpoint, or any link no router registers, carries
         * nothing for it. Atomic because a re-add REBINDS this ctx (#884) — the store runs on
         * the control thread while the departing transport's receive thread may still be
         * reading it. Relaxed on both sides: the value names an object that outlives the
         * router either way, and the ordering edge for the rebind as a whole is `retired`.
         */
        std::atomic<mem::block_source_t*> rx{nullptr};
        /**
         * @brief This child's interned transport-catalog `(kind, role)` — null when it was
         *        registered without one (#1650).
         *
         * The admission context's LINK claim, resolved once per registration: the terminus
         * hands this pointer on with each write the child carries, so a filter can tell a
         * session a `ws` listener accepted from a link this node dialled at the cost of one
         * relaxed load per frame. It points into `kinds_`, which never frees a record before
         * the router, so a reader holding it across a re-add reads a live record. Held here
         * for the reason `rx` is, and atomic and relaxed for the same reason.
         */
        std::atomic<const link_kind_t*> kind{nullptr};
        /**
         * @brief The FLAT tier's resolved-once LINK TOKEN, as `link_id_t::bits()`; `0` ⇒ not
         *        minted yet (#1266 / #1417).
         *
         * One relaxed load is the whole of the carry on a point-to-point child: the link has
         * ONE name, so it has one interned identity for its whole tenancy.
         *
         * Minted LAZILY, at the first remote subscribe over this child rather than at
         * registration. A registered child that never carries a subscription then charges the
         * graph's arena nothing at all, which matters because the arena is the budget #1160 is
         * making configurable on the C6 and a pre-minted slot per child would spend it on
         * links that never use the index. Two threads racing the mint agree by construction:
         * `graph_t::intern_link` is idempotent by NAME.
         *
         * `mutable` and atomic because it is a CACHE written from the frame path through the
         * `const` context the subject seam already hands around, and cleared from the control
         * thread on a re-add (#884) — relaxed, with `retired` carrying the ordering edge for
         * the rebind as a whole.
         */
        mutable std::atomic<std::uint64_t> link_token{0};
        /**
         * @brief The BUS tier's per-peer token cache — null on a point-to-point child.
         *
         * Allocated once, on the first registration of this name as a bus mount, and never
         * freed before the ctx: a re-add CLEARS it rather than replacing it, so a lock-free
         * reader on the receive thread can never hold a pointer to a freed table. Cleared to
         * null (not destroyed) when the same name is re-added as a FLAT child: a name that
         * used to be a bus mount must not keep answering with the facet it no longer has.
         *
         * OWNED: drawn from the link plane's source and returned by the router's destructor
         * (#1779). A refused table refuses the bus registration, with nothing registered.
         */
        bus_token_cache_t* peer_tokens_own = nullptr;
        /** @brief `peer_tokens_own` as the frame path reads it; null ⇒ the FLAT tier. */
        std::atomic<bus_token_cache_t*> peer_tokens{nullptr};
        /**
         * @brief A bus mount's tenancy words (#2234); null until its first arrival.
         *
         * OWNED with every table it replaced (`session_tenancy_t::parked`) and kept across a
         * re-add: a slot anchor outlives its mount's registration, and the word is what the
         * next arrival in that slot finds its predecessor by. `mutable` because the arrival
         * seam hands the ctx around `const`; written under `tenancy_m_`, read lock-free.
         */
        mutable std::atomic<session_tenancy_t*> tenancy{nullptr};
        /**
         * @brief The registry slot this child was registered into — resolved once, here.
         *
         * A bound hop needs the LINK an element names, and the link lives in the registry
         * slot, so the alternative is a `entry_by_name` string scan on every bound frame to
         * re-find a slot whose address never moved. Registry slots are embedded in
         * heap chunks that are appended to and never freed before the registry itself, so a
         * slot pointer is stable for the registry's lifetime — which is what makes caching it
         * sound. (The `mount_tlv` member doc's older aside about an append invalidating a slot
         * pointer describes a shape the registry has not had since it became chunked.)
         *
         * Written ONCE and never rewritten, which is why it stays non-atomic while the
         * per-tenancy words around it are atomics for the rebind (#884). It is also how the
         * receive path reaches the link a frame is inside (`child_t::bound`): the router keeps
         * no copy of the link pointer or its bus facet (#1941), and this slot is the binding
         * to it. A registry slot is keyed by NAME and reused for that name forever — `erase`
         * tombstones the slot in place and `add` rebinds the tombstone rather than appending — so a
         * re-add of this ctx's name always yields the pointer already stored here. `acquire_ctx`
         * asserts exactly that, which is where the coupling to `child_registry_t`'s tombstone
         * contract is pinned.
         */
        const child_registry_t::child_t* entry = nullptr;
        /**
         * @brief Tombstoned: this ctx names no live child, and every lookup SKIPS it (#884).
         *
         * The ctx cannot simply be unlinked on `remove_child` — a lock-free reader may be
         * standing on it — and it must not be left resolving either: `ctx_by_name` answers
         * with the FIRST match, so a re-add that appended a second ctx handed every
         * name-keyed consumer the DEAD one, whose state is the retired tenancy.
         * The registry solved the identical problem the identical way, so this is its scheme
         * one layer out: retire in place, skip on read, revive on re-add.
         *
         * **This is the publication edge for a REVIVED node**, the role `next`'s release-store
         * plays for a fresh one: `acquire_ctx` hides the node, rewrites its per-tenancy words,
         * and `publish_ctx` release-stores `false`; a reader acquire-loads it before reading
         * them, so it sees a whole tenancy or skips the node. The narrow window in between is
         * the same one `child_registry_t::add`'s rebind has, and lands in the same place: a
         * mint that straddles it resolves against a vertex whose registration state
         * `graph_t::vertex_slot_at` re-checks, so the answer is `nullopt` and the caller stays
         * canonical.
         */
        std::atomic<bool> retired{false};
        /**
         * @brief Next published receiver ctx — the LOCK-FREE reader chain (ADR-0063 §3).
         *
         * The bound hop reads this table from a transport RECEIVE thread while `add_child`
         * appends to it from whichever thread a CREATE arrived on, and those are genuinely
         * concurrent (see `ctl_m_`). The chain both OWNS and PUBLISHES the contexts (#1779;
         * a `std::deque` owned them before): each node is one block from the link plane's
         * source, linked by `acquire_ctx` while still `retired`, so a reader that reaches it
         * skips it until `publish_ctx` release-clears the flag — the same edge a revived
         * tombstone takes. The chain is append-only — `remove_child` retires a node where it
         * stands rather than unlinking it, and a re-add revives that same node (`retired`) — so
         * a reader's walk is never invalidated, churn on a stable name set adds no links at
         * all, and the router's destructor is the only place a node is returned.
         */
        std::atomic<child_rx_ctx_t*> next{nullptr};
    };

    /**
     * @brief Route one OWNING inbound frame from a rope-delivering link
     *        (ADR-0042 §1, generalized per ADR-0053).
     *
     * A single-link rope (every current producer) runs the same routing as
     * @ref on_frame over the link's bytes span — the forward hop is untouched
     * (offset-dispatch, zero heap) — and threads the owning view to the
     * terminus, where a trailer-less WRITE payload may be stored as a
     * subview of it (refcount share, zero copy) under the vertex's
     * `share_hint` and the build's block source. A multi-link rope routes a FORWARD hop
     * directly over the rope cursor (ADR-0053 ④b): the dispatch offsets are read
     * through the link-walking @ref wire::grammar::rope_cursor and the egress
     * scatter-gathers the untouched links — no flatten. Only a terminus / reply /
     * control frame, which still needs a contiguous decode (the rope-aware sink is
     * the migration ⑤/⑥ follow-on), pays the one flatten fallback.
     */
    void on_frame_rope(std::string_view inbound_name, view::rope_t frame);
    /**
     * @brief A frame from a bus PEER, carrying the owning child's mount alongside it (#510).
     *
     * A bus link tags each frame with the SENDING peer's name, and a peer has no registry
     * entry of its own — so the receiver alone cannot say which mount the peer hangs under.
     * These overloads carry the owning child's qualified name in from the per-child receiver
     * ctx, which is what lets a forward hop grow `src` by the FULL
     * `net/<module>/<name>/<peer>` path instead of the bare peer segment. Without it, two
     * buses with same-named peers produce identical return routes.
     *
     * `inbound_name` stays the bare PEER for every other purpose (terminus
     * replies, departure) — those key both sides consistently and the registry's peer
     * fallback resolves them.
     */
    void on_frame_bus(const child_rx_ctx_t& ctx, peer_handle_t peer,
                      std::span<const std::byte> frame);
    /** @brief Rope twin of `on_frame_bus`. */
    void on_frame_rope_bus(const child_rx_ctx_t& ctx, peer_handle_t peer, view::rope_t frame);
    /**
     * @brief Resolve an inbound peer HANDLE to the peer NAME the routing plane routes by
     *        (#1294) — the one place the seam's handle becomes a name again.
     *
     * Called once per inbound bus frame, on the transport's receive thread inside the
     * delivery callback, which is exactly where the name used to arrive for free. Every
     * kind answers it as a pure function of the handle (`p<slot>~<gen>`, `n<node>`), so
     * the resolution is a formatting into @p scratch and takes no transport lock.
     * @retval {} The ctx carries no bus facet, or the handle names no peer of that kind —
     *            the frame is then dropped, exactly as an unnamed inbound hop always was.
     */
    /**
     * @brief The peer HANDLE a locally-terminating frame is written under — the WHO half of
     *        the ingress identity (#375 Part 2), resolved at the point of use.
     *
     * On a bus the seam already tagged the frame, and @p peer is simply handed back. On a
     * FLAT link there is no such tag — that is what `peer_named=false` MEANS — so the link
     * itself is asked which peer it is delivering right now (`transport_t::inbound_peer`).
     * Called only where the answer is consumed (a terminus resolve) and never on a forwarding
     * hop, so a relayed frame pays nothing for it.
     * @return The frame's peer, or a handle that is not `valid()` when there is no receive
     *         context, no registered link, or the kind mints no per-peer identity — on which
     *         the subject stays the inbound link's own name.
     */
    [[nodiscard]] static peer_handle_t terminus_peer(const child_rx_ctx_t* ctx,
                                                     peer_handle_t peer) noexcept;
    /**
     * @brief The OVER-WHAT half of the ingress identity (#1650): the catalog `(kind, role)`
     *        @p ctx was registered with, or null when it has none or there is no @p ctx.
     *
     * One relaxed load of what `add_child` resolved once — called where a locally-terminating
     * write is handed to the graph, never on a forwarding hop.
     */
    [[nodiscard]] static const link_kind_t* terminus_kind(const child_rx_ctx_t* ctx) noexcept {
        return ctx != nullptr ? ctx->kind.load(std::memory_order_relaxed) : nullptr;
    }
    /**
     * @brief Derive the SUBJECT a locally-terminating write is gated under, from the peer
     *        handle — the router's own use of the seam the resolver reaches through
     *        `graph::op_resolver_t::on_peer_subject`.
     *
     * Every locally-terminating write a peer makes reaches it through that one supplier, so
     * every spelling of the same peer's write is gated under the same principal.
     * @retval {} No per-peer subject is derivable — the caller keeps the inbound link name.
     */
    [[nodiscard]] static std::string_view peer_subject_of(const child_rx_ctx_t* ctx,
                                                          peer_handle_t peer,
                                                          std::span<char> scratch);
    /** @brief The `graph::op_resolver_t::subject_fn_t` trampoline: reads the receive context
     *         back out of the opaque `inbound_ref_t::origin` the terminus put there. */
    static std::string_view peer_subject_thunk(void* ctx, const graph::inbound_ref_t& inbound,
                                               std::span<char> scratch);
    /**
     * @brief The arriving link's INTERNED TOKEN, from this router's own cache (#1266 /
     *        #1417) — the carry the subscriber index is keyed by.
     *
     * Two tiers, one door, because the 2026-08-14 ruling's `add_child`-only mint point was
     * retired for covering exactly one of them: on the ESP node EVERY WS session is a bus
     * PEER, and its name reaches the graph out of `on_frame_*_bus` without ever passing
     * through `add_child`.
     *
     * - **BUS** (`peer_tokens` non-null): the direct-mapped per-peer cache, then a LAZY MINT
     *   on a miss. The lazy path is mandatory, not an optimisation: an announce-census bus
     *   never calls `set_peer_up_notifier`, so nothing ever fills its entries eagerly and a
     *   cache-only supplier would leave every CAN peer paying the name lookup forever.
     * - **FLAT**: the ctx's own resolved-once word, minted lazily for the same reason.
     *
     * Both mints go through `graph_t::intern_link`, which is idempotent by NAME — so a race
     * between two receive threads, or between this and a redial, agrees by construction.
     *
     * Each tier also answers the PAIR the edge stores (#1941): the slot's connection vertex,
     * and on the bus tier the peer under it. The tier test is the only shape test, so a flat
     * child's per-session handle (`transport_t::inbound_peer`) never reaches a pair.
     *
     * It IS the `graph::link_id_fn_t` the resolver calls: @p ctx is the router (this supplier
     * has state, unlike the subject one), and the receive context comes back out of the
     * opaque `inbound_ref_t::origin` the terminus put there.
     *
     * @retval {} token No receive context (a direct embedder's resolve): the pair alone, by
     *            the name's cold scan.
     */
    [[nodiscard]] static graph::carried_link_t link_id_of(void* ctx,
                                                          const graph::inbound_ref_t& inbound);
    /** @brief The pair of the link a frame arrived on (#1941): its receive context's slot,
     *         or — for a frame handed in with none (the public `on_frame` doors) — the slot
     *         named @p name, found by the cold scan; not valid for neither. @p peer is the
     *         bus tag the frame carried, invalid on a flat child. */
    [[nodiscard]] graph::link_pair_t inbound_pair(const child_rx_ctx_t* ctx, std::string_view name,
                                                  peer_handle_t peer) const {
        const child_registry_t::child_t* const door =
            ctx != nullptr ? ctx->entry : registry_.entry_by_name(name);
        return door != nullptr ? graph::link_pair_t{.conn = door->conn_slot, .peer = peer}
                               : graph::link_pair_t{};
    }
    /** @brief @ref link_down with the departed link's pair in hand — the body every
     *         departure seam runs. */
    void link_gone(std::string_view link_name, graph::link_pair_t link);
    /** @brief The `graph::stats_sampler_fn_t` trampoline: `ctx` is the router
     *         (RFC-0010 Amendment 2). */
    static bool stats_sampler_thunk(void* ctx, std::string_view seam_class,
                                    std::string_view seam_name, graph::stats_block_t* out);
    /** @brief The name @p peer spells under @p door: a bus peer's positional name, or the
     *         door's own when it has no bus facet (a flat link, or the bus module closed). */
    static std::string_view resolve_peer_name(const child_registry_t::child_t& door,
                                              peer_handle_t peer, std::span<char> scratch);
    /** @brief The shared rope routing body; @p inbound_ctx is the link's receiver ctx when the
     *         frame arrived through one (nullptr on the public `on_frame_rope` entry). */
    void on_frame_rope_impl(std::string_view inbound_name, view::rope_t frame,
                            const child_rx_ctx_t* inbound_ctx, bool from_peer,
                            peer_handle_t peer = {});
    /** @brief The shared routing body: @p frame_view is the owning frame when the
     *         link delivers ropes (nullptr on the borrowed-span path). @p bus_child is the
     *         owning child's qualified name when the frame came from a bus peer, else empty. */
    void on_frame_impl(std::string_view inbound_name, std::span<const std::byte> frame,
                       const view::view_t* frame_view, const child_rx_ctx_t* inbound_ctx = nullptr,
                       bool from_peer = false, peer_handle_t peer = {});
    /**
     * @brief Terminus: arena-decode @p frame (ADR-0041) and resolve + reply.
     *
     * The arena draws from the inbound child's failable source, or the router's when
     * the child has none (#588 moved this off the pmr resource; the doc lagged), and is
     * released before returning — the memory policy is entirely the host's. The
     * FWD{REPLY} is sent back over the link the request arrived on. @p frame_view
     * (non-null on the owning-delivery path) is threaded into the resolver for
     * the ADR-0042 §3 referenced WRITE store.
     *
     * @p dst_target is the terminus deref of an escape-headed `dst`: the vertex reference
     * its last element named — an RFC-0029 PAIR carried verbatim — handed to the resolver
     * because the element REPLACED the name it stands for and there is nothing left to look
     * up. Non-null only on the `TERMINUS` arm of `route_pair_forward` (see `head_dst_t`);
     * nullptr for every NAME-spelled frame this node terminates.
     */
    void resolve_terminus(std::string_view inbound_name, std::span<const std::byte> frame,
                          const view::view_t* frame_view,
                          const child_rx_ctx_t* inbound_ctx = nullptr,
                          const wire::path_ref_element_t* dst_target = nullptr,
                          peer_handle_t peer = {});
    /**
     * @brief Terminus over a MULTI-LINK rope: resolve straight off the rope, NO flatten.
     *
     * The owning rope-tier twin of `resolve_terminus` (ADR-0053 ④b / 3c-iii):
     * a request FWD reassembled as a scatter-gather rope (fragmented WS / CAN) is
     * adopted as a lazy @ref wire::tlv_view_t and resolved through the view-tier
     * `op_resolver_t::resolve(tlv_view_t)`. Measured domain (ADR-0053 erratum): this tier
     * earns its place on LARGE, LIGHTLY-FRAGMENTED frames — at 64 KB / 2 links it is ~12%
     * ahead of flatten-then-arena, and behind it everywhere smaller. A single-link rope
     * never reaches here (see the short-circuit below), which is deliberate and measured.
     * The interim flatten this replaces is
     * deleted. The lazy view defers CRC, so integrity is verified here
     * (verify-all-then-apply, ADR-0053 §4) before the op mutates state, matching the
     * arena terminus's `decode_into(VERIFY)`. The ADR-0042 §3 referenced store needs
     * a contiguous frame view, so the rope tier stores its one ownership copy (no
     * `frame_view`). Reply routes back over the inbound link (`reply_link`) exactly as the
     * arena path.
     */
    void resolve_terminus_rope(std::string_view inbound_name, view::rope_t frame,
                               const wire::path_ref_element_t* dst_target = nullptr,
                               const child_rx_ctx_t* inbound_ctx = nullptr,
                               peer_handle_t peer = {});
    /**
     * @brief The link a terminus reply to a frame from @p inbound_name leaves on (#1709).
     *
     * A point-to-point child's frame carries its receive context, and the context holds the
     * link: the reply goes back through it, with no scan of the registry. A bus context (the
     * inbound name is a PEER's), a tombstoned one, or none at all falls back to
     * `child_registry_t::by_name` — the peer resolution ADR-0044 rules for a bus — and counts
     * the lookup in @ref reply_name_lookups.
     * @retval nullptr No link answers to @p inbound_name any more; the reply is dropped.
     */
    [[nodiscard]] transport_t* reply_link(std::string_view inbound_name,
                                          const child_rx_ctx_t* inbound_ctx) noexcept;
    /**
     * @brief The disposition of an outer frame that is not a `FWD`, by its type byte.
     *
     * A RETIRED code (`0x11`-`0x13`, RFC-0032 §6.1) is counted in `retired_rx` and answered
     * with one bare `ERROR{tr::schema::type_mismatch}` on the link it arrived on, never
     * dropped in silence. Any other type is dropped as before; a bare `ERROR` in particular
     * is a report, not a request, so it is never answered and two nodes cannot loop.
     */
    [[gnu::cold]] void refuse_unserved_type(std::byte type, std::string_view inbound_name,
                                            const child_rx_ctx_t* inbound_ctx) noexcept;
    /**
     * @brief Classify ONE inbound FWD frame and dispatch it — the ingress driver, once.
     *
     * The whole top-level sequence a frame arriving on either tier goes through: the FWD
     * type gate, the `dst` peek, the mount descent, and the five dispositions that come out
     * of it — forward / rejected / bound-forward / terminus / reply. Templated over the
     * grammar `Cursor` (ADR-0053 ④b) exactly as `route_fwd_forward` and
     * `route_pair_forward` below already are, so the contiguous and the scatter-gather tier
     * run the SAME classification instead of two hand-written copies of it. The copies had
     * already drifted: a FWD-classified frame whose op VALUE is empty resolved at the
     * terminus contiguously and was dropped as a non-control frame when the identical bytes
     * arrived as a multi-link rope.
     *
     * The three genuinely tier-specific actions are passed in, so the shared body holds no
     * `if constexpr` of its own:
     *
     * @tparam Cursor   A grammar byte-source cursor (span or rope).
     * @param  reject   Reply to a bus NAME + residual hop (ADR-0073 §3 / RFC-0020), called
     *                  with the refusal status and the frame's `fwd_pre_t`, whose offsets
     *                  locate the routes the refusal swaps. Needs contiguous bytes, which the
     *                  rope tier buys with its one COLD flatten.
     * @param  terminus Resolve here: the arena decode on the contiguous tier, the lazy
     *                  view-tier resolve straight off the rope on the other. Takes the
     *                  PAIR-spelled `dst` resolution (`const wire::path_ref_element_t*`, null
     *                  for every NAME-spelled frame), because an element leaves no name to
     *                  look up.
     * @param  reply    Hand a FWD{REPLY} that reached its originator to the reply sink,
     *                  called with the frame's `fwd_pre_t` for the refusal correlation.
     *
     * @pre `cur.size() >= 4` — the callers' own runt-frame gate has already run.
     * @retval true  The frame was FWD-classified and is fully handled; the caller returns.
     * @retval false Not a FWD frame at all — the caller continues to its control arm.
     */
    template <class Cursor, class Reject, class Terminus, class Reply>
    bool route_fwd_ingress(std::string_view inbound_name, const Cursor& cur,
                           const child_rx_ctx_t* inbound_ctx, bool from_peer, Reject&& reject,
                           Terminus&& terminus, Reply&& reply);
    /**
     * @brief The forward hop, read entirely by OFFSET — no decoded tree (ADR-0038 inv. #1).
     *
     * Consumes the `dst` records @p pre names (`strip_at`), prepends the inbound link's mount
     * path to `src` (unless a REPLY), and scatter-gather-sends onward via @p child. @p child
     * is the transport the routing decision already resolved to. Templated over the grammar
     * `Cursor` (ADR-0053 ④b): a @ref wire::grammar::span_cursor reads a contiguous frame
     * (byte-identical to the pre-rope path, zero heap — a stack `iov` array), a @ref
     * wire::grammar::rope_cursor reads a scatter-gather frame (the egress gathers each region's
     * per-link sub-spans into a
     * @ref mem::block_array_t drawn from the injected `rx_` — still no payload copy, and
     * exhaustion DROPS the frame rather than throwing, matching the reply path's own
     * failable-seam gather (#596, #1570)).
     *
     * @tparam Cursor A grammar byte-source cursor (span or rope).
     * @param cur     The cursor positioned at the inbound FWD frame's first byte.
     * @param pre     The routing peek's offsets, with the strip and the outgoing `dst` type
     *                set by the arm that routed the frame.
     */
    template <class Cursor>
    void route_fwd_forward(std::string_view inbound_name, const child_rx_ctx_t* inbound_ctx,
                           bool from_peer, const Cursor& cur, transport_t& child,
                           const fwd_pre_t& pre);
    /**
     * @brief What an escape-headed `dst`'s head arm — RFC-0029's PAIR (`route_pair_forward`)
     *        — decided about one inbound frame.
     *
     * Three answers and not two, because the head element resolves to a `path_ref_element_t`
     * and this node may be either of the things such an element can name: one of its own
     * EGRESS connection vertices (a hop) or an ORDINARY vertex (a terminus). The two are told
     * apart by the element alone, never by the op or the frame, which is what keeps the
     * element spelling agreeing with the string one: a name that descends to a mount is a hop
     * and a name that resolves to a vertex is a terminus, and the element stands for exactly
     * that already-made resolution.
     */
    enum class head_dst_t : std::uint8_t {
        /** @brief Not this arm's element — fall to the canonical path. */
        PASS,
        /** @brief Fully handled here: forwarded over the link the element named, or refused. */
        HANDLED,
        /** @brief A local vertex: resolve HERE, against the element the out-param carries. */
        TERMINUS,
    };
    /**
     * @brief The PAIR hop (RFC-0029 §6) — route @p cur by the owner-issued `(index,
     *        generation)` standing as the HEAD element of its `PATH` `dst`.
     *
     * The whole per-hop algorithm for a PAIR head, and the reason it needs no table: the pair
     * IS this node's own index entry. `deref_vertex_slot` (bounds, generation, registered) is
     * the entire validation, and what the vertex IS decides the rest:
     *
     * - a point-to-point child's **connection vertex with a tail** — a hop: §6.4's gate and the
     *   egress through `bound_egress`, the element consumed, `src` grown canonically;
     * - the **last** element — the terminus (`head_dst_t::TERMINUS`), including a
     *   connection vertex named last, which addresses its own `:`-facets; a WRITE whose last
     *   element names an accepted session's anchor is the reverse-list delivery's last hop;
     * - a **tail below any other vertex** — `INVALID_PATH`, or `NOT_FOUND` for a bus mount
     *   (§10).
     *
     * Every refusal of a well-formed pair answers `NOT_FOUND` from the request's own `src`
     * (§6.3) and nothing is repaired: no re-resolution, no fall-through to the canonical walk,
     * because the pair replaced the name bytes. Zero heap, nothing held across frames.
     *
     * @param op_byte    The frame's op byte as the ingress read it (63 for an empty op).
     * @param out_target Written only on `head_dst_t::TERMINUS`: the pair, for the resolver.
     */
    template <class Cursor, class Reject>
    [[nodiscard]] head_dst_t route_pair_forward(std::string_view inbound_name,
                                                const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                                const Cursor& cur, const fwd_pre_t& pre,
                                                std::uint8_t op_byte, Reject&& reject,
                                                wire::path_pair_t& out_target);
    /**
     * @brief The right a `FWD` op byte carries at a hop's gate — READ for READ/AWAIT, WRITE
     *        for WRITE, `nullopt` for a REPLY or an opcode this build cannot name (masked,
     *        RFC-0024 §9.3).
     */
    [[nodiscard]] static std::optional<graph::acl_right_t> fwd_op_right(
        std::uint8_t op_byte) noexcept;
    /**
     * @brief The ONE gate (RFC-0029 §6.4, §13.2 S6): may @p caller carry an operation needing
     *        @p right through the connection vertex of @p door?
     *
     * Every spelling enters here at the door it reached: the NAME descent's matched mount,
     * the mount a PAIR hop dereferenced to, the mount a PAIR-named session anchor hangs off,
     * and the origin's and a remote delivery's first element. The vertex is the door's own
     * `child_registry_t::child_t::conn_slot`, read lock-free, and it is asked
     * `graph_t::allows(vertex, caller, right)` here and nowhere else in the router, so a hop's
     * verdict cannot depend on how it was spelled.
     *
     * A graph that enforces no ACL passes everything (one relaxed load), and so does a null
     * @p door: no mount was matched, so there is no door to cross and the caller's own
     * refusal (no egress) stands. On a graph that enforces an ACL, a door with no connection
     * vertex and a @p right of `nullopt` (an opcode this build names no right for, an empty op
     * included) are refused: there is nothing to grant the right.
     */
    [[nodiscard]] bool door_allows(const child_registry_t::child_t* door, std::string_view caller,
                                   std::optional<graph::acl_right_t> right) const;
    /**
     * @brief This hop's REVERSE-direction mint element (RFC-0024 §7.1 amendment 1): a
     *        reference for the identity a mint-flagged request ARRIVED on.
     *
     * Point-to-point, the arrival link's connection vertex (`hop_mint` — one identity,
     * two directions, one supplier). Bus-session arrival, the accepted session's identity
     * anchor (#1254). Nullopt — no anchor (an announce-census peer), a saturated
     * generation, a tombstoned ctx — makes the rebuild STRIP the reverse list, the
     * erratum-1 rule direction-reversed.
     */
    [[nodiscard]] std::optional<wire::path_ref_element_t> reverse_hop_ref(
        std::string_view inbound_name, const child_rx_ctx_t* inbound_ctx, bool from_peer) const;
    /**
     * @brief The receiver ctx of @p name whatever its state — live OR tombstoned (#884).
     *
     * The CONTROL-PLANE half of the name → ctx direction, and deliberately a different
     * function from `ctx_by_name`: this one walks the chain under `ctl_m_` and
     * matches a tombstone, because both of its callers exist to change one. A frame path must
     * use `ctx_by_name`, which walks the published chain and skips the dead.
     */
    child_rx_ctx_t* ctl_ctx_by_name(std::string_view name);
    /**
     * @brief A bus mount admitted a session named @p peer — reclaim what the slot's previous
     *        tenancy left, then give the session an identity anchor (#1223 step 2, #1609).
     *
     * An arrival is the RE-TENANT edge: the slot has just changed hands and the new session
     * has sent nothing, so a slot anchor still live is a predecessor's whose departure never
     * reached this router. The slot's tenancy word holds that session (#2234), so the arrival runs
     * exactly its departure (`bus_peer_down`), under the handle and name it held: the anchor, the
     * cached token, and the edges and pending awaits filed under that name. No
     * generation arithmetic is involved, so a generation the slot consumed without an arrival
     * changes nothing. That makes "edges under a session name belong to that session" true by
     * construction rather than by a sweeper's care; when the departure already ran, there is
     * no live anchor and nothing to do. Then it registers (or REVIVES, at the same slot, one
     * generation up) the anchor vertex for `:<mount>/<slot>`, after storing @p handle in the word.
     *
     * Fired ONLY by an accepting listener (`slot_server_t` and the ESP httpd link), never by
     * an announce-census bus, so CAN peers keep ADR-0044 §Decision 1 in full force and grow
     * no vertices at all. Runs on the transport's poll thread with no transport lock held.
     */
    void bus_peer_up(const child_rx_ctx_t& ctx, peer_handle_t handle, std::string_view peer);
    /**
     * @brief A bus mount's session @p peer departed — retire its anchor, THEN evict.
     *
     * Retirement first, on purpose: it is the step that bumps the saturating generation, and
     * it is the one thing that must have happened before the slot can be handed to a
     * successor. Eviction (@ref link_down) is unchanged and still keyed by the peer NAME.
     * An anchor that was never created (an announce-census peer, or a session that departed
     * before its arrival was wired) simply resolves to nothing and the eviction runs alone.
     * A departure that lands after a successor anchored the slot (the tenancy word holds
     * another session's handle, #2234) evicts @p peer's name only: the anchor and the cached token
     * are keyed by slot and belong to the successor by then, pending authentication or not.
     *
     * @p handle is the departing peer's, and it is what the per-peer LINK-TOKEN cache is
     * keyed by (#1417): the eviction below releases the graph's index slot, so the cached
     * token has to go with it or the next session in that slot would ask with a stamp that
     * can never validate again and pay a name lookup for its whole life. Dropping it is
     * self-healing rather than merely tidy — correctness is already covered by `graph_t`
     * refusing the stale stamp.
     */
    void bus_peer_down(const child_rx_ctx_t& ctx, peer_handle_t handle, std::string_view peer);

    /** @brief The session holding @p slot of @p ctx's mount (#2234); invalid when none does.
     *         Lock-free: one acquire load of the table and one of the word. */
    [[nodiscard]] static peer_handle_t session_tenant(const child_rx_ctx_t& ctx,
                                                      std::uint32_t slot) noexcept;
    /**
     * @brief The session holding @p anchor, the anchor of the slot session name @p name
     *        spells on @p ctx's mount, provided the anchor is still at @p generation (#2234);
     *        invalid otherwise. The route to an anchor and the reverse mint both ask this.
     */
    [[nodiscard]] peer_handle_t anchor_tenant(const child_rx_ctx_t* ctx, std::string_view name,
                                              graph::vertex_handle_t anchor,
                                              std::uint32_t generation) const noexcept;
    /** @brief Store @p bits as @p slot's tenancy word, growing the table to cover it;
     *         `false` when the link plane refuses the block. Caller holds `tenancy_m_`. */
    [[nodiscard]] bool set_session_tenant_locked(const child_rx_ctx_t& ctx, std::uint32_t slot,
                                                 std::uint64_t bits);
    /**
     * @brief The receiver ctx @p name is to be registered through — reuse-or-append (#884).
     *
     * The one-ctx-per-NAME rule, enforced where it can be: an existing ctx for @p name (live
     * or tombstoned) is HIDDEN and rebound; only a name this router has never seen appends,
     * linked onto the chain already retired. The returned ctx is not visible to a reader
     * until `publish_ctx`, so the caller may finish filling it first.
     *
     * Control plane, under `ctl_m_`, after `registry_.add(name)` succeeded: a fresh ctx
     * views that slot's name and mount run.
     *
     * @retval nullptr The link plane's source refused a fresh node — nothing was linked.
     */
    child_rx_ctx_t* acquire_ctx(std::string_view name);
    /**
     * @brief Publish @p ctx to the lock-free readers. Control plane, under `ctl_m_`.
     *
     * Clears the node's tombstone with a release-store — fresh and revived nodes alike, since
     * `acquire_ctx` links a fresh one already retired — so a reader that reaches the node
     * sees every field it needs.
     */
    void publish_ctx(child_rx_ctx_t& ctx) noexcept;
    /**
     * @brief The interned record for @p kind, or null for an empty one (#1650). Control
     *        plane, under `ctl_m_`.
     *
     * One record per distinct `(kind, role)` this router has ever registered — a handful on
     * any node — found by a linear scan and appended on a miss, never freed before the router.
     * @return The record, or null when @p kind is empty (a link with no catalog identity);
     *         `nullopt` when the link plane's source refused a new record (#1779).
     */
    [[nodiscard]] std::optional<const link_kind_t*> intern_kind(link_kind_t kind);
    /** @brief The LIVE receiver ctx of child @p link_name, or nullptr — the name → ctx
     *         direction. Tombstoned nodes are skipped: a removed child resolves to nothing,
     *         and a re-added one resolves to its CURRENT tenancy (#884). */
    [[nodiscard]] const child_rx_ctx_t* ctx_by_name(std::string_view link_name) const;
    /** @brief This node's element for the link a REPLY arrived on — the forwarder's mint
     *         contribution (RFC-0024 §7.1 step 2); nullopt when it has none to give. */
    [[nodiscard]] std::optional<wire::path_ref_element_t> hop_mint(
        std::string_view inbound_name, const child_rx_ctx_t* inbound_ctx) const;
    /**
     * @brief The graph remote-delivery sink (#136): emit one producer delivery to @p sub.
     *
     * Sends `FWD{WRITE, dst, src=<empty PATH>, payload}` over the stored PAIR chain when its
     * first element still validates, else over the canonical full route (RFC-0004 §D, RFC-0024
     * §7.1). No hop holds state for the flow (#1951). Fires on the writer thread, outside the
     * vertex lock.
     */
    void deliver_remote(const graph::remote_delivery_t& sub, const graph::value_t& val);

    // The {fn, ctx} context a point-to-point child's receiver is bound with: the
    // router plus the child's inbound NAME (a bus link tags frames with the peer
    // name itself, so its ctx is just the router). Deque for pointer stability —
    // insert-only, like registry_ (the transport holds the address for its life).

    /**
     * @brief Bump one cold-drop counter — OUT OF LINE, and that is measured, not stylistic.
     *
     * Inlined, the seven `fetch_add`s sit inside `route_fwd_forward` and the templated
     * ingress walk, which is the hot forward hop. Each is two or three words on a branch
     * that never runs in steady state, but they still occupy the hop's INSTRUCTION
     * footprint: `bench_forward_demux` charged **+7% at 64 registered links** for them,
     * measured best-of-3 against a same-runner `origin/main` build whose A/A null on that
     * instrument is exactly 0.0%. A control run that added 56 bytes of dead PADDING to this
     * class instead cost nothing at all, which rules out object size and leaves code size.
     *
     * `noinline` + `cold` puts the bump in `.text.unlikely` and leaves ONE call at each drop
     * site, so the hop's hot body is the shipped one plus a branch it already had.
     */
    [[gnu::noinline, gnu::cold]] static void count_drop(std::atomic<std::size_t>& c) noexcept {
        c.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief The failable source a frame from @p ctx decodes through.
     *
     * A branch on a pointer already in a register, on the per-frame path — the whole cost
     * of ADR-0067 §3's per-owner topology. There is deliberately no lookup by name here:
     * resolving the child per frame would put a string scan on the hot path to save a
     * pointer, which is the wrong trade under latency-first.
     */
    [[nodiscard]] mem::block_source_t& rx_for(const child_rx_ctx_t* ctx) const noexcept {
        if (ctx == nullptr) return *rx_;
        mem::block_source_t* const own = ctx->rx.load(std::memory_order_relaxed);
        return own != nullptr ? *own : *rx_;
    }

    /** @brief One deferred remote AWAIT, drawn from its link's rx source (ADR-0084).
     *         Defined in `fwd_router.cpp`. */
    struct pending_await_t;

    /**
     * @brief The router's deferred-AWAIT state (ADR-0084): the list of pending waiters.
     *
     * Holds no buffer: every waiter is a block of the receiving link's own rx source. It
     * holds no thread and reads no clock either: libtracer has no timers. A pending waiter
     * resolves on change, on `link_down` / `remove_child`, or when the router is destroyed. The
     * request's `await_timeout` is ignored: the requester owns the deadline (RFC-0004
     * Amendment 3).
     * Lock order: `m` before a vertex stripe (a cancel disarms under it); a writer fires a
     * waiter with no stripe lock held, so it may take `m`.
     */
    struct await_plane_t {
        mutable std::mutex m;            /**< @brief Guards every field below. */
        std::condition_variable cv;      /**< @brief Teardown drain. */
        pending_await_t* head = nullptr; /**< @brief Pending waiters, doubly linked. */
        std::size_t count = 0;           /**< @brief Length of the `head` list. */
        bool stop = false;               /**< @brief Set by the destructor. */
    };

    /** @brief The resolver's deferral sink: take over a remote AWAIT (ADR-0084). */
    static graph::result_t<void> defer_await_thunk(void* ctx, const graph::deferred_await_t& req);
    /** @brief A waiter's change callback, on the writer's thread. */
    static void await_fired_thunk(void* ctx, graph::await_waiter_t& w) noexcept;
    /** @brief Member body of `defer_await_thunk`. */
    graph::result_t<void> defer_await(const graph::deferred_await_t& req);
    /** @brief Build and send the reply of @p p: the awaited vertex's value. */
    void answer_await(pending_await_t& p);
    /** @brief Unlink and release @p p; `await_plane_t::m` held. */
    void retire_await_locked(pending_await_t* p) noexcept;
    /** @brief Cancel the waiters of @p link_name (all when empty); `await_plane_t::m` held. */
    void cancel_awaits_locked(std::string_view link_name, bool all) noexcept;

    graph::graph_t& graph_;
    graph::op_resolver_t resolver_;
    // The long-lived link-state substrate (#603 defect 1 / #873 family 3): receive contexts,
    // bus token caches, interned kinds and bound-path scratch draw from it.
    mem::block_source_t* link_src_;
    mem::block_source_t* rx_;     // DEFAULT terminus-arena source, NOTHROW (#588);
                                  // a child may carry its own (ADR-0067 §3)
    mem::mem_backend_t* flat_;    // every rope flatten the router performs (#730);
                                  // a null result is answered by value, never stored
    mem::mem_backend_t* egress_;  // terminus REPLY head + mint egress bytes (#795);
                                  // a refusal degrades to addressed BACKPRESSURE
    child_registry_t registry_;   // the one NAME→link demux table (Brick 3a, ADR-0037);
                                  // its chunks draw from `link_src` (#873 phase 1)
    /**
     * @brief One interned link-kind record: the view a `child_rx_ctx_t::kind` points at,
     *        with the owned `kind` bytes right after it in the same block (#1779).
     */
    struct kind_rec_t {
        link_kind_t view;           /**< @brief The record a `child_rx_ctx_t::kind` points at. */
        kind_rec_t* next = nullptr; /**< @brief The next record — control plane only. */
    };
    /** @brief Interned `(kind, role)` records (#1650), under `ctl_m_`: a list of blocks from
     *         the link plane's source, so a record never moves once a ctx points at it, and
     *         returned only by the destructor. */
    kind_rec_t* kinds_ = nullptr;
    await_plane_t awaits_;  // deferred remote AWAITs (ADR-0084)
    /** @brief Head of the LOCK-FREE chain of receiver contexts — it owns them, and it is the
     *         only spelling a frame-path reader may use (see `child_rx_ctx_t::next`). */
    std::atomic<child_rx_ctx_t*> rx_head_{nullptr};
    /** @brief Tail of that chain. Written under `ctl_m_` only, never read by a frame path. */
    child_rx_ctx_t* rx_tail_ = nullptr;
    /**
     * @brief Serializes CONTROL-PLANE mutation of the registry and the receiver-ctx chain
     *        (ADR-0063 §3).
     *
     * `add_child` performs a scan-then-append on the registry and on the receiver chain;
     * neither is atomic, so two concurrent creates could be handed the same empty slot
     * (losing a child) or link two nodes onto one tail. Those creates are genuinely
     * concurrent on an ordinary multi-transport node: `make_connection` runs on whichever
     * transport's RECEIVE thread delivered the CREATE, and each transport has its own.
     *
     * A plain mutex, not a spinlock or an interrupt-disable section: this holds across socket
     * construction and can block for milliseconds, where a spinlock on a single-core FreeRTOS
     * target invites unbounded priority inversion (ADR-0063 erratum 1).
     *
     * **The forward path never takes this** — that lock-freedom is ADR-0038 §3, and the whole
     * point of the chunked list is that readers need no lock. Lock order, where more than one
     * is held: `transport_vertex_t::ctl_m_` → this → `graph_t::map_mutex_` → the vertex stripe.
     */
    mutable std::mutex ctl_m_;
    /**
     * @brief Serializes the reply-sink SETTER — never taken by a reader (#914).
     *
     * A `sink_slot_t` publishes its pair for racing readers but does not serialize two
     * concurrent publishes; this mutex does, which is what keeps the slot three words wide.
     * Deliberately NOT `ctl_m_`: that one is held across socket construction for milliseconds, and
     * installing an observer has no business waiting on a connect. The frame path takes neither.
     */
    mutable std::mutex sink_m_;
    /**
     * @brief The reply-terminus sink, a `sink_slot_t` (#914).
     *
     * The sinks were ten plain members — `fn` and `ctx` stored separately by a setter that
     * takes no lock, read with check-then-call on every transport receive thread. That
     * is a data race by the C++ memory model, and the documented runtime clear
     * ("passing nullptr clears a sink") was its worst case: a torn read hands a new
     * `fn` the previous `ctx`. Every frame-path read is now one `get()` snapshot and
     * the dispatch runs off that snapshot, so no read of a sink can straddle a `set`. The
     * two test-only frame observers that shared this block were deleted (#2277); no frame
     * reads a sink before it is routed.
     */
    sink_slot_t<reply_fn_t> reply_; /**< @brief Reply-terminus sink. */

    /**
     * @name The counted cold-path drops behind @ref drop_stats (#1503 step 3)
     *
     * `std::atomic<std::size_t>` and not plain counters because `on_frame` runs on SEVERAL
     * transport receive threads at once and the router takes no lock on the frame path — a
     * plain `++` here would be a data race by the memory model. Word-sized and RELAXED per
     * `core/STYLE.md` §Introspection clause 5, so rv32 keeps a lock-free `amoadd.w` and pulls
     * no libatomic. Every bump sits on a COLD arm that has already decided to lose the frame;
     * no success arm gains an instruction (ADR-0039's steady-state hop is the referee).
     *
     * **LAST in the class, and that is a measured placement, not tidiness.** Declared among
     * the other members, these seven words push `registry_`, the
     * `rx_head_` chain and the two sink slots the FWD span path reads on every frame apart by
     * a cache line, and `bench_forward_demux` charged ~6% for it at 64 registered links. The
     * sink slots' own doc block records the same hazard from the other direction. Cold state
     * goes behind the hot state it would otherwise displace.
     * @{
     */
    std::atomic<std::size_t> flatten_dropped_{0};
    std::atomic<std::size_t> forward_iov_dropped_{0};
    std::atomic<std::size_t> arena_dropped_{0};
    std::atomic<std::size_t> assemble_dropped_{0};
    std::atomic<std::size_t> reply_iov_dropped_{0};
    std::atomic<std::size_t> delivery_iov_dropped_{0};
    std::atomic<std::size_t> malformed_rx_{0};
    std::atomic<std::size_t> retired_rx_{0};
    /** @} */
    /** @brief @ref reply_name_lookups's counter: cold state, so it sits with the drops. Bumped
     *         only on the by-name arm of `reply_link`, never on a point-to-point reply. */
    std::atomic<std::size_t> reply_name_lookups_{0};

    /**
     * @brief Where a terminating `FWD{REPLY}` goes: the oldest armed @ref origin_t whose return
     *        route the reply's `dst` spells (disarmed as it is taken), else the @ref on_reply
     *        sink (#1645). With nothing armed it is that sink's snapshot and nothing more.
     */
    template <class Cursor>
    [[nodiscard]] sink_slot_t<reply_fn_t>::snapshot_t reply_target(const Cursor& cur);

    /**
     * @name The armed-origin list (#1645)
     *
     * Cold, so behind the drop counters for the reason they sit behind the hot state. The
     * records are the CALLER's storage; the router holds only this head, newest first.
     * `origins_` is written under `origin_m_` and read lock-free only as the "anything
     * armed?" hint that keeps the reply terminus lock-free when nothing is.
     * @{
     */
    mutable std::mutex origin_m_;
    /**
     * @brief Serializes the session-slot transitions: an arrival's store and anchor revival,
     *        a departure's compare, retire and clear, and a tenancy table's growth (#2234).
     *
     * Held across the anchor's retire or registration so a departure that read its own
     * handle cannot retire an anchor a successor has just revived. Cold: only the arrival and
     * departure seams take it, never a frame. Lock order: this → `graph_t::map_mutex_`.
     */
    std::mutex tenancy_m_;
    std::atomic<origin_t*> origins_{nullptr};
    std::atomic<std::uint32_t> origin_seq_{0};
    /** @} */
};

}  // namespace tr::net
