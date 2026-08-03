/*
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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/child_registry.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

/** @brief Offsets a `dst` peek recorded for the head rebuild (defined in fwd_frame_view.hpp).
 *         Forward-declared: the router only passes it through by pointer, so this header does
 *         not pull in the forward-plane cluster. */
struct fwd_pre_t;

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
     * @param mr    The node's container memory resource (ADR-0039 §1) — the
     *              `route_handle` label tables draw from it; the library holds no
     *              buffer of its own. A bounded node injects a pool resource over
     *              its static slab (one slab, whole stack — ADR-0039 §2); the
     *              default is the standard heap. Must outlive the router.
     * @param rx    The nothrow source the TERMINUS ARENA draws from (#588). Split from
     *              @p mr because the arena is built from a peer's frame, on the RX
     *              path, behind no ACL: a `std::pmr::memory_resource` cannot report
     *              exhaustion by value, so on `-fno-exceptions` its only failure mode
     *              is `abort()`. Drawing the arena from a @ref mem::block_source_t
     *              instead makes an over-large frame a `TLV_NESTING_TOO_DEEP` reject.
     *              Appended with a default, so every existing call site is unchanged;
     *              a bounded node points this at the same slab as @p mr. Must outlive
     *              the router.
     * @param flat  The byte backend EVERY rope flatten on the router's forward AND terminus
     *              paths draws its owned `segment` from — the router's own four (#730): the
     *              ingress control-frame sub-rope flattens (`ADVERTISE` route, `COMPACT`
     *              payload), the cold bus-name rejection flatten, and the per-delivery
     *              `COMPACT` egress flatten; PLUS the terminus resolver's rope-tier flattens
     *              one call below `resolve_terminus_rope` (#766) — `view_node::ensure_cache`
     *              (the per-node contiguous span every `wire()`/`body()` read of a multi-link
     *              TLV materializes) and `view_node::own_wire` (the ADR-0053 ⑤ ownership
     *              flatten) — which the router reaches by passing this pointer straight to
     *              its @ref graph::op_resolver_t. Until #766 the terminus half drew from the
     *              global heap, so a fragmented request from a peer escaped the bound; the
     *              honest sentence now is that all rope flattens on the forward and terminus
     *              paths draw from the injected seam. Since #801 that sentence covers the
     *              SPAN-tier terminus too: `arena_node::own_wire`, the ADR-0041 §2 ownership
     *              copy a span-delivered request takes, is this backend's as well — so the
     *              MCU terminus (a synchronous CAN/UART child delivers spans, not ropes) no
     *              longer escapes the bound on its ordinary WRITE. Still NOT every
     *              allocation the router path makes: the reply head segment and the arena
     *              are their own injections (@p egress and @p rx) — the head drew from
     *              `view::heap_alloc`'s global heap until #795 gave it @p egress below, which
     *              is the reading #730 was filed about.
     *              Split from
     *              @p rx because these are BYTE buffers with cache hooks and an owning
     *              refcount (a @ref mem::mem_backend_t), not the arena's raw blocks —
     *              the same split `graph_t` makes between its `ctl` and its
     *              `value_backend` (ADR-0060). Until #730 all of them took
     *              @ref mem::heap_backend by default, so a bounded node's memory bound
     *              did NOT cover them; now a node that points this at its own slab
     *              bounds them all, and every flatten failure is answered by value — the
     *              forward-path frame is dropped (never stored empty), and a refused
     *              terminus flatten answers an addressed `kind=ERROR STATUS{BACKPRESSURE}`
     *              reply (or, when the refusal hit the reply's own route bytes, a drop).
     *
     *              An injected @p flat MUST be thread-safe, with the same force `graph_t`
     *              requires of its `value_backend` (ADR-0060 §2) — and for the same two
     *              reasons, both of which hold here. Three of the four sites run on a
     *              transport child's RECEIVE thread and several children receive
     *              concurrently; the fourth runs on the WRITER thread inside the
     *              remote-delivery fan-out. And the `segment` this backend hands out
     *              self-routes its reclaim on whichever thread drops the last reference,
     *              which is not in general the thread that allocated it. The default
     *              `heap_backend()` already is thread-safe. A bare @ref mem::pool_t is
     *              NOT — its free list is a plain `std::size_t` head and count with no
     *              lock and no atomic, so two receive threads can be handed the same slot
     *              — and must be composed with the target's arch-selected
     *              synchronisation before injection: `mem::synchronized_pool_t` takes it
     *              as a compile-time policy — @ref mem::sync_pool_t is the multi-core
     *              spinlock, `tr::esp::critical_pool_t` the MCU interrupt-disable one.
     *              A bounded node points this at the same slab as @p mr / @p rx only
     *              through such a composition. Must outlive the router.
     * @param max_label_bindings_per_link
     *              Ceiling on one link's ingress table and, separately, its egress table
     *              (#603). `0` ⇒ unbounded, the default and the prior behavior. Without it
     *              the tables are peer-driven and grow to the whole 16-bit label space —
     *              megabytes per link on a 16 KB node. A full table refuses NEW flows, which
     *              then deliver over the full-route `FWD{WRITE}` form; established flows are
     *              untouched. See @ref route_handle_t::refused_bindings for the counter.
     * @param egress
     *              The byte backend the terminus REPLY's egress-construction segments draw
     *              from (#795, ADR-0074): the reply head (peer-driven size — the swapped route
     *              bytes plus the inline tail) and, on a mint, the trailing 12-byte `PATH_REF`.
     *              It is the last *reply-egress* byte source that escaped a bounded node's
     *              slab (the composed-root folded READ still frames POINT headers from the
     *              global heap on the value seam — a separate residual, #831) — the
     *              head was hard-wired to `view::heap_alloc`'s global heap, one allocation on
     *              every reply, peer-drivable and pre-auth reachable (the denied path builds a
     *              head too). A DEDICATED injection, deliberately NOT folded into @p flat: @p
     *              flat is documented and sized against FLATTEN (payload) bytes, and a reply
     *              head is egress construction sized against ROUTE bytes, so widening @p flat's
     *              contract would silently re-scope a slab deployments already set for flattens
     *              (a node could begin refusing replies it used to send). Passed straight to the
     *              @ref graph::op_resolver_t. Appended with a default of the global heap, so
     *              every existing call site is byte-unchanged and only a bounded node that points
     *              it at its slab gets the bound. A refusal degrades through the same
     *              empty-rope → `or_backpressure` → addressed `STATUS{BACKPRESSURE}` path OOM
     *              already takes — answered by value, never an abort. MUST be thread-safe on the
     *              same terms as @p flat. Must outlive the router.
     */
    explicit fwd_router_t(graph::graph_t& graph,
                          std::pmr::memory_resource* mr = std::pmr::get_default_resource(),
                          mem::block_source_t* rx = &mem::heap_source(),
                          mem::mem_backend_t* flat = &mem::heap_backend(),
                          std::size_t max_label_bindings_per_link = 0,
                          mem::mem_backend_t* egress = &mem::heap_backend())
        : graph_(graph),
          resolver_(graph, flat, egress),  // the terminus tier draws flatten from the SAME
                                           // seam (#766) and reply egress from `egress` (#795)
          mr_(mr),
          rx_(rx),
          flat_(flat),
          egress_(egress),
          handles_(mr, max_label_bindings_per_link) {
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
     * @p name is the mount RUN by which THIS node addresses @p link — both the leading
     * `dst` segments that route onward through @p link, and the run prepended to `src`
     * when a frame arrives on @p link (the way back).
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
     * a name **no address could ever name**: empty, containing an empty segment, or wider than
     * `graph::kMaxSegments` (a `dst` cannot carry that many segments, so nothing could ever
     * prefix-match it). Such a name used to append a slot that `size()` and `live_size()`
     * reported as a healthy child while every forward to it missed and fell through to the
     * terminus with no error anywhere — the #516 failure shape, one layer up.
     *
     * @param name This node's local mount name for the link (e.g. "up", "ws-server/up").
     * @param link The transport carrying the next/previous hop.
     * @param rx   Optional per-child failable-block source; null uses the router's. Give
     *             each child its OWN when injecting a bounded one — see ADR-0067 3
     *             for why sharing one across receive threads is the wrong shape.
     * @return false ⇔ @p name is unaddressable and NOTHING was registered. Deliberately not
     *         `[[nodiscard]]`: the return is an upgrade from a debug-only assert, and every
     *         existing call site — which registers a name it composed itself — stays correct
     *         ignoring it.
     */
    bool add_child(std::string name, transport_t& link, mem::block_source_t* rx = nullptr);

    /**
     * @brief Un-register child @p name — it stops resolving, and its routing state goes.
     *
     * The teardown counterpart of @ref add_child (#494). Tombstones the registry entry
     * (@ref child_registry_t::erase) and then runs the full @ref link_down eviction, so
     * the subscriber edges and route-handle labels bound to the name leave with it.
     *
     * **This is removal, not departure.** @ref link_down alone is the right hook for a
     * link that merely dropped: under RFC-0014 a DIAL connection's *vertex* outlives its
     * *socket* and self-heals, so evicting its registry entry on a down notification
     * would permanently unroute a link that is only reconnecting. Call this one when the
     * connection itself is gone — and call it BEFORE destroying the `transport_t`, so no
     * forward can resolve a freed object.
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
     * @param right  The right the operation carries.
     * @retval nullptr Any part of the check failed, or the vertex is not a connection vertex
     *         of a live point-to-point child of this node. The caller MUST then drop —
     *         never repair, never fall through to a different route (§5.3). A bus PEER is
     *         among the refusals and not by omission: a peer has no vertex, so no element
     *         can name one, and egressing over the bus link itself would BROADCAST a
     *         directed operation (ADR-0073 §3 / RFC-0020).
     */
    [[nodiscard]] transport_t* bound_egress(wire::path_ref_element_t e, std::string_view caller,
                                            graph::acl_right_t right) const;

    /**
     * @brief Install the bound form on @p path from the mint answer on a reply (RFC-0024 §7.4).
     *
     * The origin's side of the exchange, and the reason `path_t::bind` had no production
     * caller until now. @p reply is a decoded `FWD{REPLY}`; its LAST child is the accumulated
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
    bool adopt_binding(graph::path_t& path, std::string_view link_name, const wire::tlv_t& reply);

    /** @brief What the origin sends a bound operation as: the link, and the `dst` on the wire. */
    struct bound_dispatch_t {
        transport_t* link = nullptr; /**< @brief The egress link element 0 named. */
        std::vector<std::byte> dst;  /**< @brief The `PATH_REF` TLV carrying the RESIDUAL. */
    };

    /**
     * @brief Spell the next operation over @p path's binding — the origin's own hop (§4.1).
     *
     * The origin consumes element 0 exactly as every forwarder consumes its own: it is this
     * node's reference to its first-hop connection vertex, so it selects the link and does
     * NOT go on the wire. What goes out is the residual, `4 + 8×(H−1)` bytes, and the host
     * that receives it consumes ITS element in turn — the monotone shrink that makes a bound
     * path loop-free for the same reason a canonical `dst` is.
     *
     * @param right The right the operation carries, evaluated at the dereferenced connection
     *              vertex like any other hop's (§6.2).
     * @retval std::nullopt @p path is unbound, or element 0 no longer validates — a link
     *         removed, a connection vertex retired, an ACL revoked. The caller's recovery is
     *         the one that always works: `clear_binding()` and send the canonical path it
     *         still holds, which may then re-mint (§5.3).
     */
    [[nodiscard]] std::optional<bound_dispatch_t> bound_dispatch(const graph::path_t& path,
                                                                 graph::acl_right_t right) const;

    // -- observability / terminus sinks (fn-ptr + context, ADR-0047/ADR-0068 §3) -------
    // Not std::function: these fire on the per-frame RX path, where the erasure
    // machinery (code size, heap capability, exception paths) is the largest avoidable
    // embedded liability — the same call graph_t::subscriber_fn_t already makes at L4.
    // Configure before frames flow; passing nullptr clears a sink.

    /** @brief Reply-terminus sink (@ref on_reply): @p ctx, then the FWD{REPLY} frame. */
    using reply_fn_t = void (*)(void* ctx, const view::rope_t& reply);
    /** @brief Inbound-FWD observer (@ref on_inbound): @p ctx, inbound child, decoded FWD. */
    using inbound_fn_t = void (*)(void* ctx, std::string_view inbound, const wire::tlv_t& fwd);
    /** @brief Raw-frame observer (@ref on_raw): @p ctx, inbound child, the whole frame. */
    using raw_fn_t = void (*)(void* ctx, std::string_view inbound,
                              std::span<const std::byte> frame);
    /** @brief Local COMPACT delivery sink (@ref on_compact_delivery): @p ctx, the bound
     *         local route PATH bytes, the delivered payload TLV bytes. */
    using compact_delivery_fn_t = void (*)(void* ctx, std::span<const std::byte> route,
                                           std::span<const std::byte> payload);
    /** @brief Stale-label observer (@ref on_stale_label): @p ctx, inbound child, label. */
    using stale_label_fn_t = void (*)(void* ctx, std::string_view inbound, std::uint16_t label);

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
     * @param fn  Callback invoked on a transport receive thread; keep it cheap.
     * @param ctx Opaque pointer handed back as @p fn's first argument.
     */
    void on_reply(reply_fn_t fn, void* ctx = nullptr) noexcept;

    /**
     * @brief Set a read-only observer of every inbound FWD (observability/tests).
     *
     * Invoked after decode with the inbound child name and the decoded FWD, before
     * routing. Carries no routing semantics; used to assert the per-hop `dst`-shrink
     * / `src`-grow invariant and as the seam where a per-hop `:acl` forward-right
     * check (RFC-0004 §F) will later hang.
     *
     * @param fn  Callback invoked on a transport receive thread.
     * @param ctx Opaque pointer handed back as @p fn's first argument.
     */
    void on_inbound(inbound_fn_t fn, void* ctx = nullptr) noexcept;

    /**
     * @brief Set a read-only observer of every inbound frame's RAW bytes (any type).
     *
     * Fires before dispatch with the inbound link name and the complete frame span —
     * used by tests to measure the on-wire byte-delta between a lean COMPACT delivery
     * and the equivalent full-route FWD{WRITE} (the point of the route-handle).
     * @param fn  Callback invoked on a transport receive thread.
     * @param ctx Opaque pointer handed back as @p fn's first argument.
     */
    void on_raw(raw_fn_t fn, void* ctx = nullptr) noexcept;

    /**
     * @brief Set the sink for a label-compacted delivery that terminates at this node.
     *
     * Invoked when a COMPACT's label resolves to a LOCAL terminus binding (the
     * established route names a vertex here): the payload has already been written to
     * that vertex (delivery-is-a-write, RFC-0004 §D). Carries the bound local route
     * PATH bytes and the delivered payload TLV bytes (both borrowed for the call).
     * @param fn  Callback invoked on a transport receive thread; keep it cheap.
     * @param ctx Opaque pointer handed back as @p fn's first argument.
     */
    void on_compact_delivery(compact_delivery_fn_t fn, void* ctx = nullptr) noexcept;

    /**
     * @brief Set the observer for a dropped stale/unknown-label COMPACT (RFC-0004 §E.1).
     *
     * Invoked when a COMPACT bears a label with no ingress binding on its link — the
     * frame is dropped and a HANDLE_NACK is sent back to prompt a re-advertise (never
     * a crash). Carries the inbound link name and the stale label.
     * @param fn  Callback invoked on a transport receive thread.
     * @param ctx Opaque pointer handed back as @p fn's first argument.
     */
    void on_stale_label(stale_label_fn_t fn, void* ctx = nullptr) noexcept;

    /**
     * @brief Advertise a `label ↔ route` binding over link @p link_name (producer side).
     *
     * Allocates a fresh per-link label, sends an ADVERTISE carrying it and @p route,
     * and records the egress binding (so a NACK can re-advertise). Call when a
     * compact-flagged flow starts or on (re)connect — re-advertising is the self-heal.
     * @param link_name  This node's NAME for the downstream link to advertise over.
     * @param route_path A complete PATH TLV's bytes — the delivery route to alias.
     * @return The allocated label (to stamp on subsequent @ref send_compact), or 0 if
     *         @p link_name names no child, or if that link's 16-bit label space is
     *         exhausted (`route_handle_t::alloc_label` saturates rather than wrapping onto
     *         a live label — #603). No ADVERTISE is sent in either case.
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
     *
     * On a MID-CHAIN node this also drops every ingress binding held on ANY link whose
     * downstream half crossed @p link_name (#716) — see `tr::net::route_handle_t::clear_link`.
     * Without it the upstream, which never saw the reconnect, keeps streaming COMPACTs onto a
     * dead out-label and the flow drops silently forever; with it the upstream's next COMPACT
     * draws the ordinary stale-label `HANDLE_NACK` and the flow re-advertises itself back up.
     * @param link_name This node's NAME for the link whose label state to drop.
     */
    void clear_link(std::string_view link_name);

    /**
     * @brief The link-departure hook (RFC-0009 §D extended to peer departure): evict
     *        everything the routing plane holds against a link that just died.
     *
     * Two halves, in order: `graph_t::evict_link_edges(link_name)` deactivates and
     * reclaims every subscriber edge whose stored link is @p link_name (write fan-outs
     * stop addressing the dead session, and its ~90 B/edge of route/link/caller state is
     * released — the C6's measured ~27 KB/browser-session leak), then @ref clear_link
     * drops the link's route-handle label state (unchanged self-heal semantics).
     *
     * `add_child` installs this automatically behind every child's departure notifier
     * (`transport_t::set_down_notifier` with the child's registered NAME; the bus facet's
     * `bus_link_t::set_peer_down_notifier` with the departed PEER's name — the same name
     * inbound frames were tagged with, hence the name subscriber edges stored). A host
     * that learns of a departure out-of-band (its own session manager) may call it
     * directly; calling it for a live or unknown link is safe (the next delivery-compact
     * flow re-advertises; eviction of nothing is a no-op).
     *
     * Runs on the calling thread (typically a transport receive/close thread) and takes
     * graph locks — callers must hold no transport-internal locks (the
     * `set_down_notifier` calling discipline).
     * @param link_name The routing plane's inbound NAME for the departed link/peer.
     */
    void link_down(std::string_view link_name);

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
    /** @brief "This child has no connection vertex" — the unbindable child (RFC-0024 §5.1).
     *         A sentinel rather than an `optional` because it lives in the per-frame ctx and
     *         a `u32` compare is the whole test the bound hop performs against it. */
    static constexpr std::uint32_t kNoConnSlot = 0xFFFFFFFFu;

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
     * second effect, not a side effect". ADR-0062's forward cache DEPENDS on it — it holds a
     * `const child_t*` and lets the teardown tombstone be the invalidation. Anyone reasoning
     * about slot lifetime from the old sentence would reach the wrong conclusion, which is why
     * it is corrected rather than deleted.
     *
     * The surviving reason for the copy is drift-freedom, not lifetime: the run is a pure
     * function of the child's name, so re-adding a tombstoned name yields identical bytes, and
     * this deque never invalidates a reference either.
     */
    struct child_rx_ctx_t {
        fwd_router_t* self;
        std::string name;
        std::vector<std::byte> mount_tlv;
        /**
         * @brief This child's OWN failable-block source; null falls back to the router's.
         *
         * ADR-0067 §3: a `pool_source_t` shared across receive threads is measured-worse
         * than the heap it replaces (ADR-0060 erratum 1 — ~1/15 of its single-thread rate
         * on 12 cores). Each transport has its own receive thread, so a source parked here
         * is touched by exactly one — the per-thread shape, obtained by ownership rather
         * than by a lock. A bounded node gives each child its own slab, which also makes
         * the bound per-peer: one noisy link cannot starve another's decode.
         */
        mem::block_source_t* rx = nullptr;
        /**
         * @brief This child's CONNECTION vertex slot index, resolved once at registration
         *        (RFC-0024 §5.1) — `kNoConnSlot` when the child has none.
         *
         * A bound element names a vertex; an egress needs a link; this integer is the whole
         * of the join, and it is recorded here rather than looked up per frame because the
         * lookup runs the other way (vertex → name → child) and there is no reverse index.
         * The child's mount run IS its connection vertex's canonical key — `add_child` is
         * given `net/<module>/<name>` and the vertex is registered at `/net/<module>/<name>`
         * — so the resolution is one `graph_t::find` of bytes the registry already holds.
         *
         * It is NOT a route table: one entry per LINK, sized by the graph and never by the
         * traffic, which is the property RFC-0024 §2.1 credits the design with keeping. A
         * child registered before its connection vertex exists simply has none, and every
         * bound route through it fails validation and falls back to canonical — the shipped
         * wiring registers the vertex first (`transport_vertex_t::make_connection`).
         */
        std::uint32_t conn_slot = kNoConnSlot;
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
         */
        const child_registry_t::child_t* entry = nullptr;
        /**
         * @brief Next published receiver ctx — the LOCK-FREE reader chain (ADR-0063 §3).
         *
         * The bound hop reads this table from a transport RECEIVE thread while `add_child`
         * appends to it from whichever thread a CREATE arrived on, and those are genuinely
         * concurrent (see `ctl_m_`). Walking the owning `std::deque` under no lock is a data
         * race on the deque's own chunk map, however benign a given codegen looks — the same
         * ruling ADR-0063 erratum 3 made about `multi_peer`. So the deque OWNS the contexts and
         * this intrusive chain PUBLISHES them, exactly as `child_registry_t` does with its
         * chunks: the node is fully built before the release-store that links it, and a reader
         * acquires each link before touching the node behind it. The chain is append-only —
         * `remove_child` deliberately leaves the ctx in place — so a reader's walk is never
         * invalidated.
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
     * terminus, where a big trailer-less WRITE payload may be stored as a
     * subview of it (refcount pin, zero copy) under the vertex's
     * `pin_payload_ratio` declaration. A multi-link rope routes a FORWARD hop
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
     * `inbound_name` stays the bare PEER for every other purpose (label tables, terminus
     * replies, departure) — those key both sides consistently and the registry's peer
     * fallback resolves them.
     */
    void on_frame_bus(const child_rx_ctx_t& ctx, std::string_view peer,
                      std::span<const std::byte> frame);
    /** @brief Rope twin of `on_frame_bus`. */
    void on_frame_rope_bus(const child_rx_ctx_t& ctx, std::string_view peer, view::rope_t frame);
    /** @brief The shared rope routing body; @p inbound_ctx is the link's receiver ctx when the
     *         frame arrived through one (nullptr on the public `on_frame_rope` entry). */
    void on_frame_rope_impl(std::string_view inbound_name, view::rope_t frame,
                            const child_rx_ctx_t* inbound_ctx, bool from_peer);
    /** @brief The shared routing body: @p frame_view is the owning frame when the
     *         link delivers ropes (nullptr on the borrowed-span path). @p bus_child is the
     *         owning child's qualified name when the frame came from a bus peer, else empty. */
    void on_frame_impl(std::string_view inbound_name, std::span<const std::byte> frame,
                       const view::view_t* frame_view, const child_rx_ctx_t* inbound_ctx = nullptr,
                       bool from_peer = false);
    /**
     * @brief Terminus: arena-decode @p frame (ADR-0041) and resolve + reply.
     *
     * The arena draws from the inbound child's failable source, or the router's when
     * the child has none (#588 moved this off `mr_`; the doc lagged), and is
     * released before returning — the memory policy is entirely the host's. The
     * FWD{REPLY} is sent back over the link the request arrived on. @p frame_view
     * (non-null on the owning-delivery path) is threaded into the resolver for
     * the ADR-0042 §3 referenced WRITE store.
     */
    void resolve_terminus(std::string_view inbound_name, std::span<const std::byte> frame,
                          const view::view_t* frame_view,
                          const child_rx_ctx_t* inbound_ctx = nullptr);
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
     * @ref mem::block_array_t drawn from the injected `rx_` — still no payload copy, and
     * exhaustion DROPS the frame rather than throwing, matching the reply path's
     * `try_to_iovec` (#596)).
     *
     * @tparam Cursor A grammar byte-source cursor (span or rope).
     * @param cur     The cursor positioned at the inbound FWD frame's first byte.
     */
    template <class Cursor>
    void route_fwd_forward(std::string_view inbound_name, const child_rx_ctx_t* inbound_ctx,
                           bool from_peer, std::size_t strip_k, const Cursor& cur,
                           transport_t& child, const fwd_pre_t* pre = nullptr);
    /**
     * @brief The BOUND forward hop (RFC-0024 §3.4/§5) — try to route @p cur by its `PATH_REF`.
     *
     * The whole of a bound hop: read element 0, bounds-check the index, compare the
     * generation, evaluate the ACL at the dereferenced vertex, and egress the residual over
     * the link that vertex names. No digest fold, no segment compare, no variable-length
     * walk — the `resolve_mount_*` family is not entered at all, which is the structural
     * claim §3.4 makes and §8.4 makes measuring a condition of acceptance.
     *
     * @retval true  The frame was consumed — forwarded, or DROPPED because validation failed
     *               (§5.3: a host that cannot validate element 0 MUST NOT forward, MUST NOT
     *               apply and MUST NOT repair; either way the caller must not fall through
     *               to its terminus arm, or a bound frame this node could not route would be
     *               applied LOCALLY, which is the mis-route the whole design exists to
     *               refuse).
     * @retval false This frame is not a bound FORWARD — the residual is one element and this
     *               node is its terminus, or the op is one no bound hop carries. The caller
     *               continues exactly as it did before bound paths existed.
     *
     * @param pre           The offsets @ref peek_fwd_dst_any already filled for this frame,
     *                      with `dst_ref` set. Passed in rather than re-peeked: the caller has
     *                      to classify the `dst` anyway, and reading the same three headers a
     *                      second time is what made a bound terminus measurably slower than
     *                      the canonical one it is supposed to beat.
     * @param element_count The `PATH_REF` element count that peek reported.
     */
    template <class Cursor>
    [[nodiscard]] bool route_bound_forward(std::string_view inbound_name,
                                           const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                           const Cursor& cur, const fwd_pre_t& pre,
                                           std::size_t element_count);
    /** @brief Publish @p ctx onto the lock-free reader chain. Control plane, under `ctl_m_`. */
    void publish_ctx(child_rx_ctx_t& ctx) noexcept;
    /** @brief The receiver ctx of child @p link_name, or nullptr — the name → ctx direction. */
    [[nodiscard]] const child_rx_ctx_t* ctx_by_name(std::string_view link_name) const;
    /** @brief The registered child whose CONNECTION vertex sits at slot @p index, or nullptr. */
    [[nodiscard]] const child_rx_ctx_t* ctx_by_conn_slot(std::uint32_t index) const;
    /** @brief This node's element for the link a REPLY arrived on — the forwarder's mint
     *         contribution (RFC-0024 §7.1 step 2); nullopt when it has none to give. */
    [[nodiscard]] std::optional<wire::path_ref_element_t> hop_mint(
        std::string_view inbound_name, const child_rx_ctx_t* inbound_ctx) const;
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
    /** @brief The vertex a bound route names, or nullopt — the memoizable half of
     *         `deliver_local`, shared so a cached handle cannot diverge from it. */
    [[nodiscard]] std::optional<graph::vertex_handle_t> resolve_route_vertex(
        std::span<const std::byte> route_path) const;
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

    // The {fn, ctx} context a point-to-point child's receiver is bound with: the
    // router plus the child's inbound NAME (a bus link tags frames with the peer
    // name itself, so its ctx is just the router). Deque for pointer stability —
    // insert-only, like registry_ (the transport holds the address for its life).

    /**
     * @brief The failable source a frame from @p ctx decodes through.
     *
     * A branch on a pointer already in a register, on the per-frame path — the whole cost
     * of ADR-0067 §3's per-owner topology. There is deliberately no lookup by name here:
     * resolving the child per frame would put a string scan on the hot path to save a
     * pointer, which is the wrong trade under latency-first.
     */
    [[nodiscard]] mem::block_source_t& rx_for(const child_rx_ctx_t* ctx) const noexcept {
        return ctx != nullptr && ctx->rx != nullptr ? *ctx->rx : *rx_;
    }

    graph::graph_t& graph_;
    graph::op_resolver_t resolver_;
    std::pmr::memory_resource* mr_;        // route_handle label-table resource (ADR-0039 §1)
    mem::block_source_t* rx_;              // DEFAULT terminus-arena source, NOTHROW (#588);
                                           // a child may carry its own (ADR-0067 §3)
    mem::mem_backend_t* flat_;             // every rope flatten the router performs (#730);
                                           // a null result is answered by value, never stored
    mem::mem_backend_t* egress_;           // terminus REPLY head + mint egress bytes (#795);
                                           // a refusal degrades to addressed BACKPRESSURE
    child_registry_t registry_;            // the one NAME→link demux table (Brick 3a, ADR-0037)
    route_handle_t handles_;               // per-link label tables (compact flows only)
    std::deque<child_rx_ctx_t> child_rx_;  // stable receiver contexts, one per child
    /** @brief Head of the LOCK-FREE published chain through `child_rx_` — the only spelling a
     *         frame-path reader may use (see `child_rx_ctx_t::next`). */
    std::atomic<child_rx_ctx_t*> rx_head_{nullptr};
    /** @brief Tail of that chain. Written under `ctl_m_` only, never read by a frame path. */
    child_rx_ctx_t* rx_tail_ = nullptr;
    /**
     * @brief Serializes CONTROL-PLANE mutation of the registry and `child_rx_` (ADR-0063 §3).
     *
     * `add_child` performs a scan-then-append on the registry and an `emplace_back` on the
     * receiver deque; neither is atomic, so two concurrent creates could be handed the same
     * empty slot (losing a child) or corrupt the deque spine. Those creates are genuinely
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
    reply_fn_t reply_cb_ = nullptr;               /**< @brief Reply-terminus sink. */
    void* reply_ctx_ = nullptr;                   /**< @brief Its opaque context. */
    inbound_fn_t inbound_cb_ = nullptr;           /**< @brief Inbound-FWD observer. */
    void* inbound_ctx_ = nullptr;                 /**< @brief Its opaque context. */
    raw_fn_t raw_cb_ = nullptr;                   /**< @brief Raw-frame observer. */
    void* raw_ctx_ = nullptr;                     /**< @brief Its opaque context. */
    compact_delivery_fn_t delivery_cb_ = nullptr; /**< @brief Local COMPACT delivery sink. */
    void* delivery_ctx_ = nullptr;                /**< @brief Its opaque context. */
    stale_label_fn_t stale_cb_ = nullptr;         /**< @brief Stale-label observer. */
    void* stale_ctx_ = nullptr;                   /**< @brief Its opaque context. */
};

}  // namespace tr::net
