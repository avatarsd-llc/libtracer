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
#include "libtracer/path_element.hpp"
#include "libtracer/path_label_table.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/sink_slot.hpp"
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
     * @param label_src
     *              The nothrow source the `route_handle` LABEL TABLES draw from (#603
     *              defect 1 / #873 family 3, ADR-0065 / ADR-0079 §Decision 4) — the library
     *              holds no buffer of its own. A bounded node injects a
     *              @ref mem::pool_source_t over its static slab (one slab, whole stack —
     *              ADR-0039 §2); the default is the process-wide nothrow platform heap.
     *              Must outlive the router, and must be thread-safe: the label tables are
     *              written from `on_advertise`, which runs on a transport RECEIVE thread and
     *              is driven entirely by a remote peer.
     *
     *              This parameter used to be a `%std::pmr::memory_resource*`. It could not
     *              stay one: a pmr resource cannot report exhaustion by value, so on the
     *              shipping `-fno-exceptions` profile a peer's ADVERTISE storm against an
     *              aborting `heap_resource_t` rebooted the node. Kept in the SAME position
     *              rather than appended, because feeding the label tables was its only job —
     *              a call site that passed `%std::pmr::get_default_resource()` now passes
     *              nothing (or its own source) and gets a compile error rather than a silent
     *              re-route. A call site that passed its OWN resource — a node whose graph
     *              and router historically shared one pmr arena — migrates by pointing
     *              @ref mem::pool_source_t's SPAN CONSTRUCTOR at the same storage that
     *              resource was partitioning (#1493). It must NOT reach for the adapter
     *              that shape invites, a `block_source_t` wrapping the pmr resource: that
     *              wrapper's `try_alloc` cannot answer `nullptr`, so it reinstates the very
     *              abort this parameter change removed. @ref mem::block_source_t's warning
     *              carries the full reasoning, including why a budget-tracking variant is
     *              declined too. Split from @p rx deliberately (ADR-0079's per-plane default):
     *              @p rx is per-frame decode scratch and may legitimately be a
     *              `bump_source_t`, while label state is LONG-LIVED and would monotonically
     *              fill one.
     * @param rx    The nothrow source the TERMINUS ARENA draws from (#588). Split from
     *              @p label_src because the arena is built from a peer's frame, on the RX
     *              path, behind no ACL: a `std::pmr::memory_resource` cannot report
     *              exhaustion by value, so on `-fno-exceptions` its only failure mode
     *              is `abort()`. Drawing the arena from a @ref mem::block_source_t
     *              instead makes an over-large frame a `TLV_NESTING_TOO_DEEP` reject.
     *              Appended with a default, so every existing call site is unchanged;
     *              a bounded node points this at the same slab as @p label_src. Must outlive
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
     *              A bounded node points this at the same slab as @p label_src / @p rx only
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
     *              slab (both folded READs' POINT headers — the composed root's and the
     *              `":children"` listing's — are payload framing and draw from the graph's own
     *              `value_backend` seam instead: #831, closed) — the
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
                          mem::block_source_t* label_src = &mem::heap_source(),
                          mem::block_source_t* rx = &mem::heap_source(),
                          mem::mem_backend_t* flat = &mem::heap_backend(),
                          std::size_t max_label_bindings_per_link = 0,
                          mem::mem_backend_t* egress = &mem::heap_backend())
        : graph_(graph),
          resolver_(graph, flat, egress),  // the terminus tier draws flatten from the SAME
                                           // seam (#766) and reply egress from `egress` (#795)
          label_src_(label_src),
          rx_(rx),
          flat_(flat),
          egress_(egress),
          handles_(label_src, max_label_bindings_per_link) {
        // The captureless {fn, ctx} pair the ADR-0047 doctrine prescribes (#1049) — the same
        // shape as `on_reverse_ref` below. The graph publishes it through a `sink_slot_t`,
        // so `this` must outlive every write that can still reach the producer fan-out;
        // constructing a router against a graph that is ALREADY serving frames is
        // unsupported, which is what `configure_` in the verb's name says.
        graph_.configure_remote_delivery_sink(
            [](void* ctx, const graph::remote_delivery_t& sub, const view::rope_t& value) {
                static_cast<fwd_router_t*>(ctx)->deliver_remote(sub, value);
            },
            this);
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
        resolver_.on_link_id(&fwd_router_t::link_id_thunk, this);
        // The wire `:subscribers[]` door's target descent (RFC-0021, #491): a SUBSCRIBER's
        // PATH child is an address in THIS node's frame, so deciding whether it leaves the
        // node is a mount question — the transport plane's, which L4 cannot name. Installed
        // here, through the same captureless {fn, ctx} pair, so a node that HAS a transport
        // plane resolves mount-path targets and one that does not cannot.
        graph_.configure_wire_target_resolver(
            [](void* ctx, std::span<const std::byte> key) -> graph::wire_target_split_t {
                return static_cast<fwd_router_t*>(ctx)->split_subscriber_target(key);
            },
            this);
    }

    fwd_router_t(const fwd_router_t&) = delete;
    fwd_router_t& operator=(const fwd_router_t&) = delete;

    /**
     * @brief Inject the RFC-0027 PATH-LABEL mint table, turning label switching ON for this
     *        node (§8.3) — or pass `nullptr` (the default) to leave it off.
     *
     * **Off is the conformant default, not a degraded one** (§6.3). A router with no table
     * never mints, so every hop's local part travels as the string it travels as today and
     * nothing on the route notices; a peer that presents a label to such a node gets §7.2's
     * `NOT_FOUND` and falls back to the full-string path it still holds. That is why this is a
     * `configure_` verb and an injected pointer rather than a constructor parameter with a
     * capacity: the bound comes from the embedder's own store (ADR-0079's per-plane axis) and
     * the library chooses neither the capacity nor the ceiling (`CONTEXT.md` §Resource bound).
     *
     * It is also what keeps the judging lens honest. The plain-string forwarding path — the
     * shape every deployment already ships — tests this one pointer against null on a member
     * already in the router's first cache line, and takes the not-taken branch. Nothing else
     * about a string-only hop changes: no field is added to `fwd_rebuild_t` (whose 256-byte
     * ratchet is MEASURED, #1235), no byte is read that was not read before, and the mint work
     * itself is out of line.
     *
     * Call during setup, before frames flow, on the same terms as the constructor's seams.
     * @p labels must outlive the router.
     */
    void configure_path_labels(path_label_table_t* labels) noexcept {
        labels_ = labels;
        // The TERMINUS half of §6.1's rewrite (point 3), installed HERE and not in the
        // constructor, so the seam pointer the resolver tests is null on every node that does
        // not label. A string-only terminus therefore pays one not-taken branch on a member
        // it was already going to read, and never a call — the same cost story the forwarding
        // half's `labels_` null test tells, at the other end of the route.
        const graph::op_resolver_t::path_label_fn_t thunk =
            [](void* ctx, std::string_view inbound_link,
               wire::path_ref_element_t target) -> std::span<const std::byte> {
            return static_cast<fwd_router_t*>(ctx)->terminus_label_record(inbound_link, target);
        };
        resolver_.on_path_label(labels != nullptr ? thunk : nullptr, this);
    }

    /** @brief The injected mint table, or null when this node does not mint (§6.3). */
    [[nodiscard]] path_label_table_t* path_labels() const noexcept { return labels_; }

    /**
     * @brief Labelled `dst` elements this node refused and answered `NOT_FOUND` for (§7.2).
     *
     * The counter `dispatch_edge_target`'s `target_canonical_resolves_` is one seam out
     * (`core/src/graph.cpp`): a refused deref is *"this answer is no longer trustworthy"*, and
     * the discipline that precedent fixes is that the event is COUNTED rather than merely
     * handled. Non-zero means peers are presenting labels this node cannot validate — stale
     * ones after a teardown (the designed, self-correcting case) or labels it never minted.
     * It is never an error: the sender's recovery is the canonical path it still holds.
     */
    [[nodiscard]] std::size_t label_not_found() const noexcept {
        return label_not_found_.load(std::memory_order_relaxed);
    }

    /** @brief Labelled `dst` elements this node dereferenced and forwarded on (§7.2's other
     *         side) — the numerator to @ref label_not_found's denominator. */
    [[nodiscard]] std::size_t label_resolves() const noexcept {
        return label_resolves_.load(std::memory_order_relaxed);
    }

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
     *             for why sharing one across receive threads is the wrong shape.
     * @return false ⇔ @p name is unaddressable, or the registry could not grow — either way
     *         NOTHING was registered.
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
     */
    [[nodiscard]] bool add_child(std::string name, transport_t& link,
                                 mem::block_source_t* rx = nullptr);

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
    //
    // Each pair lives in a `sink_slot_t` (#914), so a setter may run while frames flow:
    // the pair is published as a unit and every frame-path read snapshots it, which is
    // what makes "passing nullptr clears a sink" safe at runtime rather than a race.
    // What a clear does NOT do is stop a dispatch already in flight — a receive thread
    // may be inside the sink, or hold a snapshot taken a moment before — so the ctx must
    // still outlive every possible dispatch, as it must for `receiver_slot_t`.

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
     * Sends an ADVERTISE carrying @p route and records the egress binding (so a NACK can
     * re-advertise). Call when a compact-flagged flow starts or on (re)connect — re-advertising
     * IS the self-heal, and the label is minted once per `(link, route)` then REUSED (#913): the
     * frame goes out on every call, but a re-advertise loop grows no label or table state.
     * @param link_name  This node's NAME for the downstream link to advertise over.
     * @param route_path A complete PATH TLV's bytes — the delivery route to alias.
     * @return The label to stamp on subsequent @ref send_compact, or 0 if @p link_name names no
     *         child, or that link's label space is exhausted / its egress table full (#603 —
     *         see `route_handle_t::ensure_egress`). No ADVERTISE is sent in either case.
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

    /**
     * @brief The node-scoped identity string of the session @p peer holds on bus mount
     *        @p mount — the key `graph_t::register_session_anchor` allocates against (#1223).
     *
     * Deliberately NOT a spellable address. It is `:<mount>/<peer>`, and both `:` and `/` are
     * among the seven characters `path::valid_segment` rejects, so the anchor's rendered key
     * is bytes no registered path in this graph can produce — the property that keeps an
     * anchor out of every listing, every descent and every sweep-set collision. Qualified by
     * the mount because two listeners each name their first session `p0`, and those are two
     * sessions.
     *
     * Exposed because it is what a test (and a later step's mint) needs in order to name the
     * anchor the router created; it is a pure function of its arguments and holds no state.
     */
    [[nodiscard]] static std::string session_anchor_id(std::string_view mount,
                                                       std::string_view peer);

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
     * Runs on the calling (transport RX) thread and takes graph and router locks — the
     * caller must hold no transport-internal locks across this call.
     *
     * @param inbound_name This node's NAME for the link the frame arrived on.
     * @param frame        The complete inbound FWD frame bytes (borrowed; consumed
     *                     before this call returns — sends happen inline).
     */
    void on_frame(std::string_view inbound_name, std::span<const std::byte> frame);

    /** @brief The connection registry (test introspection — the shared demux table). */
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
        return child_rx_.size();
    }

   private:
    /** @brief "This child has no connection vertex" — the unbindable child (RFC-0024 §5.1).
     *         A sentinel rather than an `optional` because it lives in the per-frame ctx and
     *         a `u32` compare is the whole test the bound hop performs against it. */
    static constexpr std::uint32_t kNoConnSlot = 0xFFFFFFFFu;

    /**
     * @brief Reclaim the subscriber edge a refused delivery route names (#1223 step 5).
     *
     * Called on every inbound `FWD{REPLY}` that terminates here, BEFORE the reply sink and
     * whether or not one is installed. A reply that is not an addressed `tr::path::invalid`
     * refusal (RFC-0020's bus-residual reject — op REPLY, kind ERROR, `STATUS{ERROR{0x0021}}`)
     * returns after at most five header reads, allocation-free, so the warm read-reply path
     * pays a peek and nothing else. On a refusal, the reply's `src` — the refused route the
     * rejecting hop echoed back verbatim ("so it can correlate", `reject_bus_name_hop`) — is
     * handed with @p inbound_name to `graph_t::evict_route_edges`, which reclaims exactly the
     * edge(s) that stored that route over that link. This is the producer-side half of the
     * RFC-0020 exchange: the reject already ships; this makes the producer act on it instead
     * of dropping it into a sink that never matched it.
     *
     * @param inbound_name This node's NAME for the link the reply arrived on.
     * @param frame        The reply frame's contiguous bytes.
     */
    void reclaim_refused_route(std::string_view inbound_name, std::span<const std::byte> frame);

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
     * are slot numbers (`p<slot>`) or bus node ids (`n<node>`), so they are small and dense
     * and the ways collide only past `kWays` live peers on one mount; a collision costs
     * one name lookup and nothing else.
     *
     * WHY THE PEER'S GENERATION IS NOT PART OF THE KEY: a peer's NAME is a pure function of
     * its index (`bus_link_t::peer_name` is documented as exactly that), so a recycled index
     * asks about the same link name and therefore the same interned identity. The stamp that
     * DOES matter is the token's own, and `graph_t` validates that itself — an entry left
     * behind by a departed session fails there and degrades to a lookup.
     */
    struct bus_token_cache_t {
        /** @brief Ways in the direct-mapped table. */
        static constexpr std::size_t kWays = 16;
        /** @brief Occupancy tag: the peer index PLUS ONE, so `0` reads as empty. */
        std::atomic<std::uint32_t> key[kWays] = {};
        /** @brief `link_id_t::bits()` for the peer `key` names. */
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
         *
         * Atomic because a re-add REBINDS this ctx (#884) — the store runs on the control
         * thread while the departing transport's receive thread may still be reading it.
         * Relaxed on both sides: the value names an object that outlives the router either
         * way, and the ordering edge for the rebind as a whole is `retired`.
         */
        std::atomic<mem::block_source_t*> rx{nullptr};
        /**
         * @brief The bus facet this ctx's frames arrive through — null for a
         *        point-to-point child (#1294).
         *
         * The inbound seam tags each frame with a `peer_handle_t`, and the NAME the routing
         * plane grows into `src` is resolved from it by `bus_link_t::peer_name`. That
         * resolution needs the link, and the receiver callback is a `{fn, ctx}` pair with no
         * other capture, so the link is recorded here — once per registration, beside the
         * child's other resolved-once facts.
         *
         * Atomic and relaxed for the same reason `rx` is: a re-add REBINDS this ctx
         * (#884) from the control thread while the departing transport's receive thread may
         * still read it, and `retired` carries the ordering edge for the rebind as a whole.
         */
        std::atomic<bus_link_t*> bus{nullptr};
        /**
         * @brief The registered transport itself — the door the TERMINUS asks for the
         *        writer's SUBJECT (#375 Part 2).
         *
         * `bus` above is the ADDRESSING facet and is null on a FLAT child by construction;
         * ADR-0082 rules that the subject is the OTHER claim and must be reachable at
         * `peer_named=false`, so it cannot be asked through that facet. This is the same
         * object, unfacetted: the terminus asks it which peer the in-flight frame came from
         * (`transport_t::inbound_peer`) and what subject that peer writes under
         * (`transport_t::peer_subject`). Both default to "none", so a kind that mints no
         * per-peer identity leaves the subject at the inbound link's own name.
         *
         * Atomic and relaxed for the reason `rx` and `bus` are: a re-add REBINDS this ctx
         * (#884) from the control thread while the departing transport's receive thread may
         * still read it, and `retired` carries the ordering edge for the rebind as a whole.
         */
        std::atomic<transport_t*> link{nullptr};
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
         * thread on a re-add (#884) — the same relaxed discipline `rx`, `bus` and `link` use,
         * with `retired` carrying the ordering edge for the rebind as a whole.
         */
        mutable std::atomic<std::uint64_t> link_token{0};
        /**
         * @brief The BUS tier's per-peer token cache — null on a point-to-point child.
         *
         * Allocated once, on the first registration of this name as a bus mount, and never
         * freed before the ctx: a re-add CLEARS it rather than replacing it, so a lock-free
         * reader on the receive thread can never hold a pointer to a freed table. Cleared to
         * null (not destroyed) when the same name is re-added as a FLAT child, for the reason
         * `bus` is — a name that used to be a bus mount must not keep answering with the
         * facet it no longer has.
         */
        std::unique_ptr<bus_token_cache_t> peer_tokens_own;
        /** @brief `peer_tokens_own` as the frame path reads it; null ⇒ the FLAT tier. */
        std::atomic<bus_token_cache_t*> peer_tokens{nullptr};
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
         *
         * "Once at registration" now means once per REGISTRATION, not once per ctx (#884): a
         * re-add of the same name re-resolves it into this same node, because the answer is a
         * property of the current tenancy and not of the name. Atomic for that store, and read
         * relaxed — `retired` carries the ordering, and on the bound hop the compare is the
         * same single instruction a plain load was.
         */
        std::atomic<std::uint32_t> conn_slot{kNoConnSlot};
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
         * Written ONCE and never rewritten, which is why it alone stays non-atomic while its
         * neighbours became atomics for the rebind (#884). A registry slot is keyed by NAME
         * and reused for that name forever — `erase` tombstones the slot in place and `add`
         * rebinds the tombstone rather than appending — so a re-add of this ctx's name always
         * yields the pointer already stored here. `acquire_ctx` asserts exactly that, which is
         * where the coupling to `child_registry_t`'s tombstone contract is pinned.
         */
        const child_registry_t::child_t* entry = nullptr;
        /**
         * @brief Tombstoned: this ctx names no live child, and every lookup SKIPS it (#884).
         *
         * The ctx cannot simply be unlinked on `remove_child` — a lock-free reader may be
         * standing on it — and it must not be left resolving either: `ctx_by_name` answers
         * with the FIRST match, so a re-add that appended a second ctx handed every
         * name-keyed consumer the DEAD one, whose `conn_slot` indexes the retired tenancy.
         * The registry solved the identical problem the identical way, so this is its scheme
         * one layer out: retire in place, skip on read, revive on re-add.
         *
         * **This is the publication edge for a REVIVED node**, the role `next`'s release-store
         * plays for a fresh one: `acquire_ctx` hides the node, rewrites `rx`/`conn_slot`, and
         * `publish_ctx` release-stores `false`; a reader acquire-loads it before reading either
         * field, so it sees a whole tenancy or skips the node. The narrow window in between is
         * the same one `child_registry_t::add`'s rebind has, and lands in the same place: a
         * mint that straddles it resolves against a vertex whose registration state
         * `graph_t::vertex_slot_at` re-checks, so the answer is `nullopt` and the caller stays
         * canonical.
         */
        std::atomic<bool> retired{false};
        /**
         * @brief Already linked into the published chain — CONTROL PLANE ONLY, under `ctl_m_`.
         *
         * Distinguishes the two things `publish_ctx` must do: LINK a fresh node, or clear the
         * tombstone on one that is already reachable. Never read by a frame path, so it is a
         * plain `bool` rather than an atomic.
         */
        bool linked = false;
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
         * `remove_child` retires a node where it stands rather than unlinking it, and a re-add
         * revives that same node (`retired`) — so a reader's walk is never invalidated, and
         * churn on a stable name set adds no links at all.
         */
        std::atomic<child_rx_ctx_t*> next{nullptr};
        /**
         * @brief This child's per-peer identity for the RFC-0027 §8.3 mint ceiling — the
         *        `kSolePeerHandle` doctrine applied per CHILD rather than per link kind.
         *
         * A point-to-point child has no `peer_handle_t` of its own: the seam mints one only for
         * a bus facet (#1294). But §8.3's ceiling has to be counted against *something* a single
         * far side cannot spend on everyone else's behalf, and `kSolePeerHandle` is one constant
         * for the whole node — every child would share one budget, which is precisely the
         * "one peer consumes the table" case the ceiling exists to prevent. So each ctx mints
         * its own, from the router's `label_peer_seq_`, once per REGISTRATION.
         *
         * Its GENERATION advances on every re-add (#884's tombstone revive), which makes it a
         * validate-on-use stamp in its own right: a label minted for the previous tenancy of
         * this name is owned by a handle that no longer compares equal, so `path_label_table_t`
         * refuses it on owner mismatch even in the window before the slot's own §7.1 bump
         * lands. Two independent stamps for one identity is deliberate — the slot's catches a
         * departed VERTEX, this one catches a departed TENANCY.
         *
         * Control-plane written (under `ctl_m_`), frame-path read; relaxed, because `retired`
         * carries the ordering edge for a rebind as a whole exactly as it does for `conn_slot`.
         */
        std::atomic<std::uint64_t> label_peer{0};
        /**
         * @brief This child's minted PATH LABEL as `tr::wire::path_label_t::bits()`; `0` ⇒ none
         *        (RFC-0027 §6.1).
         *
         * The label stands for this hop's own LOCAL PART — the whole mount run in `mount_tlv`,
         * however many segments that is (§5.3.3, amendment 6) — and it aliases the connection
         * vertex of the link the reply arrived over, which is exactly the element
         * `bound_egress` consumes on the way back in. That is what makes the two directions
         * compose: what a hop mints on a reply is what it dereferences on the next request.
         *
         * **Where §6.2's trigger lives.** Zero until the first REPLY this hop relays over this
         * child, which is the subscription's first fire seen from the forwarder — the moment
         * the hop is *already holding* the resolution and no other condition. No use counter,
         * no hit threshold, no hotness estimate, no timer, no aging: a non-zero value is
         * reused for every later reply, so the table is touched ONCE per child and never on a
         * steady-state frame.
         *
         * Release-stored by the minting receive thread and acquire-loaded before
         * `path_label_tlv` is read, so a reader sees the encoded bytes whole or sees no
         * label at all. It returns to `0` on teardown, beside the §7.1 generation bump.
         */
        std::atomic<std::uint32_t> path_label{0};
        /**
         * @brief The 7 encoded bytes of `path_label` — `00 16 04 <u32 LE>` (§5.3.2).
         *
         * Precomputed for the same reason `mount_tlv` is (#508): the reply-leg rewrite is then
         * a choice between two ALREADY-ENCODED spans at the one place the hop was going to
         * prepend bytes anyway, so §6.1's "replaces, never appends" costs no per-frame encoding
         * and no splice. Valid only while `path_label` is non-zero.
         */
        std::array<std::byte, wire::kPathLabelRecordBytes> path_label_tlv{};
        /**
         * @brief The EGRESS peer `path_label` was minted for, as `peer_handle_t::bits()`.
         *
         * A path label is a function of two things, and conflating them is a mis-delivery: it
         * ALIASES the resolution behind the link the reply arrived on (this ctx), and it is
         * OWNED by the peer the reply is relayed out to — the origin that will present it back.
         * This records the second, so a cached label is reused only for the peer it was minted
         * for and never handed to a different origin, whose `lookup` would refuse it anyway
         * (§4.1's node-scope rule) after the frame had already been shortened on its behalf.
         *
         * A hop relaying replies for one target out to a SECOND origin therefore leaves that
         * origin's part a string (§6.3), rather than mis-spelling it. Labelling both needs one
         * slot per (egress peer, inbound target) pair; this car holds one per inbound child,
         * which is the shape that keeps the table's row count bounded by LINKS on a NARROW
         * node. The pair table is deferred, and the degrade is the designed benign one.
         */
        std::atomic<std::uint64_t> path_label_for{0};
        /**
         * @brief This child's TERMINUS path label (RFC-0027 §6.1 point 3) — `0` ⇒ none.
         *
         * The forwarding half above and this one are two different labels because they stand
         * for two different things, and merging them would be a mis-delivery: `path_label`
         * aliases the CONNECTION VERTEX behind a child (what a hop forwards through), while
         * this one aliases the LOCAL VERTEX a residual resolved to (what a terminus applies an
         * operation at). Both are owned by the peer on the other side of this child — the
         * origin that will present the label back — which is why one ctx holds both.
         *
         * **Where §6.2's trigger lives on the terminus half.** Zero until the first operation
         * this node terminates for this child, and no other condition: no use counter, no
         * threshold, no hotness estimate, no timer, no aging. A non-zero value is reused for
         * every later reply to the SAME residual, so the table is touched once per child.
         *
         * Same release/acquire discipline as `path_label`: release-stored last, so a reader
         * sees the encoded bytes and the owning peer whole or sees no label at all.
         */
        std::atomic<std::uint32_t> terminus_label{0};
        /** @brief The 7 encoded bytes of `terminus_label` — `00 16 04 <u32 LE>` (§5.3.2). */
        std::array<std::byte, wire::kPathLabelRecordBytes> terminus_label_tlv{};
        /** @brief The peer `terminus_label` was minted for, as `peer_handle_t::bits()`. */
        std::atomic<std::uint64_t> terminus_label_for{0};
        /**
         * @brief What `terminus_label` aliases: `(index << 32) | generation` of the local
         *        vertex the residual resolved to, packed so one relaxed load answers "is this
         *        the same residual?".
         *
         * **ONE terminus label per child, first come — and the second residual stays a
         * string.** A child that terminates operations at several vertices could be given a
         * label for each, but that is a per-child MAP, whose row count would scale with how
         * many addresses a peer happens to touch rather than with links — the exact term a
         * NARROW node cannot bound, and the reason §5.3.3 rules one label per whole local
         * part. So this holds one, and a residual that does not match it is left as the string
         * it already is. §6.3 makes that refusal free: the operation proceeds normally and
         * nothing on the route notices. The `(egress peer, target)` pair table that would
         * label them all is the same deferred shape `path_label_for` names.
         */
        std::atomic<std::uint64_t> terminus_label_target{0};
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
     * kind answers it as a pure function of the handle's index (`p<slot>`, `n<node>`), so
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
     * Called only where the answer is consumed (a terminus resolve, a local COMPACT
     * delivery) and never on a forwarding hop, so a relayed frame pays nothing for it.
     * @return The frame's peer, or a handle that is not `valid()` when there is no receive
     *         context, no registered link, or the kind mints no per-peer identity — on which
     *         the subject stays the inbound link's own name.
     */
    [[nodiscard]] static peer_handle_t terminus_peer(const child_rx_ctx_t* ctx,
                                                     peer_handle_t peer) noexcept;
    /**
     * @brief Derive the SUBJECT a locally-terminating write is gated under, from the peer
     *        handle — the router's own use of the seam the resolver reaches through
     *        `graph::op_resolver_t::on_peer_subject`.
     *
     * Shared by that supplier and by the label-switched COMPACT delivery, which resolves no
     * FWD and so never passes through the resolver: two delivery shapes carrying the same
     * peer's write MUST be gated under the same principal, or a deny on one is a bypass
     * through the other.
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
     * @retval {} No context, or an empty name — the index interns what it was handed, which
     *            is exactly what it did before the carry existed.
     */
    [[nodiscard]] graph::link_id_t link_id_of(const child_rx_ctx_t* ctx, peer_handle_t peer,
                                              std::string_view link_name);
    /** @brief The `graph::link_id_fn_t` trampoline: `ctx` is the router (this supplier has
     *         state, unlike the subject one), and the receive context comes back out of the
     *         opaque `inbound_ref_t::origin` the terminus put there. */
    static graph::link_id_t link_id_thunk(void* ctx, const graph::inbound_ref_t& inbound);
    static std::string_view resolve_peer_name(const child_rx_ctx_t& ctx, peer_handle_t peer,
                                              std::span<char> scratch);
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
     * @p dst_label_target is RFC-0027 §7.2's terminus deref: the vertex reference a LABELLED
     * `dst` resolved to through this node's own label table, handed to the resolver because
     * the label REPLACED the name it stands for and there is nothing left to look up.
     * Non-null only on the `TERMINUS` arm of `route_label_forward` (see `label_dst_t`); nullptr for
     * every other frame this node terminates, which is every frame today.
     */
    void resolve_terminus(std::string_view inbound_name, std::span<const std::byte> frame,
                          const view::view_t* frame_view,
                          const child_rx_ctx_t* inbound_ctx = nullptr,
                          const wire::path_ref_element_t* dst_label_target = nullptr,
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
     * `frame_view`). Reply routes back over @p inbound_name exactly as the arena path.
     */
    void resolve_terminus_rope(std::string_view inbound_name, view::rope_t frame,
                               const wire::path_ref_element_t* dst_label_target = nullptr,
                               const child_rx_ctx_t* inbound_ctx = nullptr,
                               peer_handle_t peer = {});
    /**
     * @brief Classify ONE inbound FWD frame and dispatch it — the ingress driver, once.
     *
     * The whole top-level sequence a frame arriving on either tier goes through: the FWD
     * type gate, the `dst` peek, the mount descent, and the five dispositions that come out
     * of it — forward / rejected / bound-forward / terminus / reply. Templated over the
     * grammar `Cursor` (ADR-0053 ④b) exactly as `route_fwd_forward` and
     * `route_bound_forward` below already are, so the contiguous and the scatter-gather tier
     * run the SAME classification instead of two hand-written copies of it. The copies had
     * already drifted: a FWD-classified frame whose op VALUE is empty resolved at the
     * terminus contiguously and was dropped as a non-control frame when the identical bytes
     * arrived as a multi-link rope.
     *
     * The four genuinely tier-specific actions are passed in, so the shared body holds no
     * `if constexpr` of its own:
     *
     * @tparam Cursor   A grammar byte-source cursor (span or rope).
     * @param  observe  The read-only inbound observer (tests/ACL seam), which wants a decoded
     *                  tree and so exists only on the contiguous tier; a no-op on the rope
     *                  tier, where nothing contiguous is in hand to give it.
     * @param  reject   Reply to a bus NAME + residual hop (ADR-0073 §3 / RFC-0020). Needs
     *                  contiguous bytes, which the rope tier buys with its one COLD flatten.
     * @param  terminus Resolve here: the arena decode on the contiguous tier, the lazy
     *                  view-tier resolve straight off the rope on the other. Takes the
     *                  RFC-0027 §7.2 labelled-`dst` resolution (`const
     *                  wire::path_ref_element_t*`, null for every string- and
     *                  `PATH_REF`-spelled frame), because a label leaves no name to look up
     *                  and the deref is made where the label table lives.
     * @param  reply    Hand a FWD{REPLY} that reached its originator to the reply sink.
     *
     * @pre `cur.size() >= 4` — the callers' own runt-frame gate has already run.
     * @retval true  The frame was FWD-classified and is fully handled; the caller returns.
     * @retval false Not a FWD frame at all — the caller continues to its control arm.
     */
    template <class Cursor, class Observe, class Reject, class Terminus, class Reply>
    bool route_fwd_ingress(std::string_view inbound_name, const Cursor& cur,
                           const child_rx_ctx_t* inbound_ctx, bool from_peer, Observe&& observe,
                           Reject&& reject, Terminus&& terminus, Reply&& reply);
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
                           transport_t& child, const fwd_pre_t* pre = nullptr,
                           std::span<const std::byte> reply_label = {});
    /**
     * @brief What the RFC-0027 §7.2 label branch decided about one inbound `dst`.
     *
     * Three answers and not two, because a label resolves to a `path_ref_element_t` and this
     * node may be either of the things such an element can name: one of its own EGRESS
     * connection vertices (a hop — car 4) or an ORDINARY vertex (a terminus — this car). The
     * two are told apart by the element alone, never by the op or the frame, which is what
     * keeps the labelled spelling agreeing with the string one: a name that descends to a
     * mount is a hop and a name that resolves to a vertex is a terminus, and the label stands
     * for exactly that already-made resolution.
     */
    enum class label_dst_t : std::uint8_t {
        /** @brief Not a label at all — run the canonical mount descent, unchanged. */
        NOT_LABELLED,
        /** @brief Fully handled here: forwarded over the link the label named, or refused. */
        HANDLED,
        /** @brief A local vertex: resolve HERE, against the element the out-param carries. */
        TERMINUS,
    };
    /**
     * @brief The LABELLED forward hop (RFC-0027 §7.2) — try to route @p cur by a path label
     *        standing in the first element of its canonical `dst`.
     *
     * The label branch that sits **beside `resolve_mount_at`**, and the whole of it: read the
     * first element of the packed `dst` body, and if it is a label element, turn it straight
     * into the resolution this hop already made — a bounds check, a slot load and a generation
     * compare — instead of folding a digest chain over the leading segments and comparing
     * candidate mount slots. The label covers the hop's ENTIRE mount run (§5.3.3), so what it
     * replaces is the whole descent and not one segment of it.
     *
     * The resolution it yields is a `path_label_target_t`, which IS a
     * @ref wire::path_ref_element_t, so the deref hands straight to @ref bound_egress — the
     * same shipped bounds/generation/ACL sequence a bound hop runs. That reuse is the point
     * rather than a convenience: §8.2 requires a labelled operation to evaluate `acl_allows`
     * at the dereferenced vertex *exactly as the string form does*, and one shared
     * implementation is what makes "byte-identical outcomes" a property instead of a claim.
     *
     * **On a refusal, §7.2 governs and there is no repair of ANY kind**: no re-resolution
     * against a nearest match, no retry against another slot, no guessing, and — the one that
     * separates this from `graph_t::dispatch_edge_target`'s precedent — **no fall-through to
     * the canonical walk**. The precedent falls through because the canonical spelling is still
     * sitting in the edge beside the bound one; here the label REPLACED the string bytes
     * (§6.1), so there is no string left to walk and "falling through" would mean reading a
     * label element as if it were a name. What the precedent does supply, and what is copied
     * verbatim, is that the event is treated as *"this answer is no longer trustworthy"* and is
     * **counted** (@ref label_not_found). The sender's recovery is the full-string path it
     * still holds, re-minted from the next reply.
     *
     * **A label whose target is a LOCAL vertex is this node's TERMINUS residual** (§6.1 point
     * 3's own mint, presented back). It cannot go through @ref bound_egress, which answers
     * only for connection vertices — until this arm existed such a frame took §7.2's counted
     * `NOT_FOUND`, and §12.4 axis 2 had nothing to time. The deref is made here, where the
     * table and the peer identity live, and the RESULT travels to the terminus in @p
     * terminus_target; §8.2's re-check is then the resolver's `graph_t::read` / `write` /
     * `await` at that vertex — the string spelling's own gate, reused rather than restated,
     * exactly as the hop arm reuses `bound_egress`.
     *
     * @param terminus_target Written only when `label_dst_t`'s `TERMINUS` is returned: this
     *                        node's reference to the vertex the label aliased, for
     *                        `resolve_terminus`. Untouched on every other answer.
     */
    template <class Cursor, class Reject>
    [[nodiscard]] label_dst_t route_label_forward(std::string_view inbound_name,
                                                  const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                                  const Cursor& cur, const fwd_pre_t& pre,
                                                  Reject&& reject,
                                                  wire::path_ref_element_t& terminus_target);
    /**
     * @brief RFC-0027 §6.1's reply-leg rewrite: the span this hop prepends to `src`, either its
     *        mount run (the default, and today's behaviour) or the label that replaces it.
     *
     * Deliberately **out of line**, for `rebuild_reply_mint`'s measured reason (#1235): the
     * caller is `[[gnu::flatten]]`-adjacent hot code and a REQUEST hop must see one not-taken
     * branch rather than this body's op-byte read and table call inlined into it.
     *
     * §11.2's mutual exclusion is enforced by the CALL SITE rather than by a flag here: only
     * the canonical-`PATH` legs of `route_fwd_ingress` call this at all, so a `PATH_REF`-spelled
     * leg cannot reach a mint and a host never puts both compressions on one address.
     *
     * @param outbound_name The child the frame is being relayed OUT to — the peer that will
     *                      present the label back, and the identity §8.3's ceiling is counted
     *                      against. Empty answers `{}`.
     * @return The encoded 7-byte element, or EMPTY whenever this hop does not mint — which is
     *         every request leg, every bus child, every node with no injected table, and every
     *         refusal (§6.3 makes all of them one case: the part stays a string).
     */
    template <class Cursor>
    [[gnu::noinline]] std::span<const std::byte> label_src_prefix(std::string_view inbound_name,
                                                                  const child_rx_ctx_t* inbound_ctx,
                                                                  const Cursor& cur,
                                                                  const fwd_pre_t* pre,
                                                                  std::string_view outbound_name);
    /**
     * @brief RFC-0027 §6.1 point 3 — the TERMINUS's label for the residual it just resolved,
     *        minted once per child and reused thereafter.
     *
     * The `op_resolver_t::path_label_fn_t` seam's body. The resolver knows which vertex the
     * residual resolved to and nothing about peers or tables; this knows the peer behind
     * @p inbound_link and owns the table, and neither has to learn the other's half.
     *
     * WHO the label is minted for is the same question the forwarding half answers and takes
     * the same answer: the peer the REPLY goes out to, because that origin is the one that
     * will present the label back. At a terminus the reply leaves over the link the request
     * arrived on, so the inbound child is both what the label is minted for and where it is
     * cached — the two ctxs the forwarding half has to keep apart are one ctx here.
     *
     * §8.1 is the caller's guarantee and it is stated on both sides: this is reached only
     * after the operation's own ACL gate answered success, so no label is ever minted for a
     * destination an ancestor ACL hides.
     *
     * @param inbound_link This node's NAME for the link the request arrived on.
     * @param target       This node's own reference to the vertex the residual resolved to.
     * @return The 7 encoded bytes, valid while this child holds the label — or EMPTY, which
     *         is the conformant answer for every refusal at once (§6.3): no injected table, a
     *         bus child, a retired child, a different residual already holding this child's
     *         one label, or the §8.3 ceiling.
     */
    [[nodiscard]] std::span<const std::byte> terminus_label_record(std::string_view inbound_link,
                                                                   path_label_target_t target);

    /**
     * @brief Mint this child's path label if it has none yet (§6.2's trigger), and answer the
     *        encoded 7-byte element — or `{}` when this hop leaves its part a string (§6.3).
     */
    [[nodiscard]] std::span<const std::byte> child_label_record(std::string_view inbound_name,
                                                                const child_rx_ctx_t& ctx,
                                                                peer_handle_t egress_peer);
    /**
     * @brief Release this child's path label and forget its spelling — §7.1's departure bump.
     *
     * Called wherever a child stops being the thing its label stood for: removal, link-down,
     * and the tombstone-revive of a re-add. There is no withdraw frame and nothing is told
     * (§7.3); the peer's next frame simply fails to validate and it falls back to strings.
     */
    void release_child_label(child_rx_ctx_t& ctx) noexcept;
    /** @brief The next per-child `label_peer` identity — control plane only, under `ctl_m_`. */
    [[nodiscard]] std::uint64_t next_label_peer_bits() noexcept;
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
     * @param reject        The caller's addressed-refusal arm (the RFC-0020-shaped echo) —
     *                      §5.3's NACK for a one-element delivery whose validation failed,
     *                      which is what the producer's step-5 reclaim correlates.
     */
    template <class Cursor, class Reject>
    [[nodiscard]] bool route_bound_forward(std::string_view inbound_name,
                                           const child_rx_ctx_t* inbound_ctx, bool from_peer,
                                           const Cursor& cur, const fwd_pre_t& pre,
                                           std::size_t element_count, Reject&& reject);
    /**
     * @brief The reverse-list delivery's LAST hop (RFC-0024 §7.1 amendment 1, #1223 step 4):
     *        a one-element bound WRITE whose element dereferences to a SESSION ANCHOR is
     *        egressed to that session as the canonical delivery frame.
     *
     * The disclosure fix lives in this function's deref: a dead session's element carries
     * the retired generation, the recycled slot's revived anchor reads one higher, and the
     * frame refuses (§5.1) with §5.3's NACK — the addressed echo the producer's step-5
     * reclaim retires the stale edge on. A one-element WRITE whose element names an
     * ORDINARY vertex answers false and keeps its bound-terminus meaning byte-for-byte.
     */
    template <class Cursor, class Reject>
    [[nodiscard]] bool route_bound_session_delivery(std::string_view inbound_name,
                                                    const child_rx_ctx_t* inbound_ctx,
                                                    bool from_peer, const Cursor& cur,
                                                    const fwd_pre_t& pre, Reject&& reject);
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
     * function from `ctx_by_name`: this one walks the owning deque under `ctl_m_` and
     * matches a tombstone, because both of its callers exist to change one. A frame path must
     * use `ctx_by_name`, which walks the published chain and skips the dead.
     */
    child_rx_ctx_t* ctl_ctx_by_name(std::string_view name);
    /**
     * @brief A bus mount admitted a session named @p peer — give it an identity anchor
     *        (#1223 step 2, ADR-0044 §Amendment 2026-08-13).
     *
     * Registers (or REVIVES, at the same slot) the anchor vertex for `:<mount>/<peer>`. A
     * `PATH_IN_USE` answer means the anchor is already live — a duplicated arrival for a
     * session that never left — and is dropped: the existing anchor stays, generation
     * untouched, which is the only outcome that keeps the stamp honest.
     *
     * Fired ONLY by an accepting listener (`slot_server_t` and the ESP httpd link), never by
     * an announce-census bus, so CAN peers keep ADR-0044 §Decision 1 in full force and grow
     * no vertices at all. Runs on the transport's poll thread with no transport lock held.
     */
    void bus_peer_up(const child_rx_ctx_t& ctx, std::string_view peer);
    /**
     * @brief A bus mount's session @p peer departed — retire its anchor, THEN evict.
     *
     * Retirement first, on purpose: it is the step that bumps the saturating generation, and
     * it is the one thing that must have happened before the slot can be handed to a
     * successor. Eviction (@ref link_down) is unchanged and still keyed by the peer NAME.
     * An anchor that was never created (an announce-census peer, or a session that departed
     * before its arrival was wired) simply resolves to nothing and the eviction runs alone.
     *
     * @p handle is the departing peer's, and it is what the per-peer LINK-TOKEN cache is
     * keyed by (#1417): the eviction below releases the graph's index slot, so the cached
     * token has to go with it or the next session in that slot would ask with a stamp that
     * can never validate again and pay a name lookup for its whole life. Dropping it is
     * self-healing rather than merely tidy — correctness is already covered by `graph_t`
     * refusing the stale stamp.
     */
    void bus_peer_down(const child_rx_ctx_t& ctx, peer_handle_t handle, std::string_view peer);
    /**
     * @brief The receiver ctx @p name is to be registered through — reuse-or-append (#884).
     *
     * The one-ctx-per-NAME rule, enforced where it can be: an existing ctx for @p name (live
     * or tombstoned) is HIDDEN and rebound; only a name this router has never seen appends.
     * The returned ctx is not visible to a reader until `publish_ctx`, so the caller may
     * finish filling it (`conn_slot`) first.
     *
     * Control plane, under `ctl_m_` — it walks the OWNING deque, which no frame path may.
     */
    child_rx_ctx_t& acquire_ctx(const std::string& name, mem::block_source_t* rx);
    /**
     * @brief Publish @p ctx to the lock-free readers. Control plane, under `ctl_m_`.
     *
     * Links a fresh node onto the chain, or clears the tombstone on a revived one — a
     * release-store either way, so a reader that reaches the node sees every field it needs.
     */
    void publish_ctx(child_rx_ctx_t& ctx) noexcept;
    /** @brief The LIVE receiver ctx of child @p link_name, or nullptr — the name → ctx
     *         direction. Tombstoned nodes are skipped: a removed child resolves to nothing,
     *         and a re-added one resolves to its CURRENT tenancy (#884). */
    [[nodiscard]] const child_rx_ctx_t* ctx_by_name(std::string_view link_name) const;
    /** @brief The registered LIVE child whose CONNECTION vertex sits at slot @p index, or
     *         nullptr — tombstoned nodes are skipped, as in `ctx_by_name`. */
    [[nodiscard]] const child_rx_ctx_t* ctx_by_conn_slot(std::uint32_t index) const;
    /** @brief This node's element for the link a REPLY arrived on — the forwarder's mint
     *         contribution (RFC-0024 §7.1 step 2); nullopt when it has none to give. */
    [[nodiscard]] std::optional<wire::path_ref_element_t> hop_mint(
        std::string_view inbound_name, const child_rx_ctx_t* inbound_ctx) const;
    /**
     * @brief The control switch (ADVERTISE / COMPACT / HANDLE_NACK), once, over either tier.
     *
     * Reads the outer type + `u16` label by OFFSET through @p cur — no owning decode of the
     * frame on either tier — and routes the three control types to their handlers. The one
     * thing the two tiers genuinely do differently is how they hand a handler the CHILD
     * window it needs contiguous, so that is the parameter: @p contig maps
     * `(off, total)` to a span, and owns whatever that costs on its tier for the duration
     * of the call. A `HANDLE_NACK` acts on the label alone and never calls it.
     *
     * @tparam Cursor A grammar byte-source cursor (span or rope).
     * @param  contig `std::span<const std::byte>(std::size_t off, std::size_t total)` — the
     *                child window made contiguous, or EMPTY when that was not possible (a
     *                rope materialize that OOM'd or hit a DEVICE link). A contiguous source
     *                subspans, so it never answers empty for a non-empty window.
     */
    template <class Cursor, class Contig>
    void dispatch_control(std::string_view inbound_name, const Cursor& cur, Contig&& contig,
                          const child_rx_ctx_t* inbound_ctx = nullptr, peer_handle_t peer = {});
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
    void on_control_rope(std::string_view inbound_name, view::rope_t frame,
                         const child_rx_ctx_t* inbound_ctx = nullptr, peer_handle_t peer = {});
    /** @brief Learn (or re-advertise downstream) a `label ↔ route` binding (RFC-0004 §E.1). */
    void on_advertise(std::string_view inbound_name, std::uint16_t label, const wire::tlv_t& route);
    /** @brief Forward (swap label) or locally deliver a label-compacted COMPACT payload. */
    void on_compact(std::string_view inbound_name, std::uint16_t label,
                    std::span<const std::byte> payload_bytes,
                    const child_rx_ctx_t* inbound_ctx = nullptr, peer_handle_t peer = {});
    /** @brief Re-advertise an egress binding in response to a downstream HANDLE_NACK. */
    void on_nack(std::string_view inbound_name, std::uint16_t label);
    /** @brief The vertex a bound route names, or nullopt — the memoizable half of
     *         `deliver_local`, shared so a cached handle cannot diverge from it. */
    [[nodiscard]] std::optional<graph::vertex_handle_t> resolve_route_vertex(
        std::span<const std::byte> route_path) const;
    /**
     * @brief Apply a bound route's delivery as a write under @p caller's ACL context.
     *
     * @param caller The ACL subject context — the inbound link's NAME for a COMPACT that
     *               arrived on the wire, matching what the equivalent full-route
     *               `FWD{WRITE}` presents (RFC-0004 §F gates the target vertex at the final
     *               hop, and both spellings of one delivery must present one subject).
     *               Required, not defaulted: an empty caller is the local-trusted
     *               short-circuit in `graph_t::acl_allows`, and #974 was exactly a delivery
     *               path inheriting it by omission.
     */
    [[nodiscard]] bool deliver_local(std::span<const std::byte> route_path,
                                     std::span<const std::byte> payload, std::string_view caller);
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
        if (ctx == nullptr) return *rx_;
        mem::block_source_t* const own = ctx->rx.load(std::memory_order_relaxed);
        return own != nullptr ? *own : *rx_;
    }

    graph::graph_t& graph_;
    graph::op_resolver_t resolver_;
    /** @brief The RFC-0027 §8.3 mint table, or null ⇒ this node never mints (§6.3). Injected,
     *         never owned — the capacity and the per-peer ceiling are the embedder's (ADR-0079). */
    path_label_table_t* labels_ = nullptr;
    /** @brief Source of the per-child `label_peer` handles — control-plane only, under `ctl_m_`.
     *         Starts at 1 because a zero generation never names a peer (`peer_handle_t`). */
    std::uint32_t label_peer_seq_ = 1;
    std::atomic<std::size_t> label_not_found_{0}; /**< @brief §7.2 refusals — see @ref
                                                            label_not_found. */
    std::atomic<std::size_t> label_resolves_{0};  /**< @brief Labelled hops taken. */
    // The label plane's substrate (#603 defect 1 / #873 family 3): the route-handle tables
    // draw from it, and so do the ADVERTISE arm's route re-encodes and the two over-wide
    // route reads on the COMPACT/NACK arms — every one of which was a throwing allocation on
    // a peer-provoked receive thread before. Held here as well as inside `handles_` because
    // those router-side reads are the store's callers, not the store.
    mem::block_source_t* label_src_;
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
    /**
     * @brief Serializes the five sink SETTERS — never taken by a reader (#914).
     *
     * A `sink_slot_t` publishes its pair for racing readers but does not serialize two
     * concurrent publishes; one mutex here does that for all five, which is what keeps a
     * slot three words wide. Deliberately NOT `ctl_m_`: that one is held across socket
     * construction for milliseconds, and installing an observer has no business waiting
     * on a connect. The frame path takes neither.
     */
    mutable std::mutex sink_m_;
    /**
     * @brief The five observer/terminus sinks, each a `sink_slot_t` (#914).
     *
     * They were ten plain members — `fn` and `ctx` stored separately by a setter that
     * takes no lock, read with check-then-call on every transport receive thread. That
     * is a data race by the C++ memory model, and the documented runtime clear
     * ("passing nullptr clears a sink") was its worst case: a torn read hands a new
     * `fn` the previous `ctx`. Every frame-path read is now one `get()` snapshot and
     * the dispatch runs off that snapshot, so no read of a sink can straddle a `set`.
     *
     * Ordered so that the two the FWD span path reads on EVERY frame — `inbound_` and
     * `raw_` — are adjacent: three words per slot puts their hot words within one cache
     * line, which a 64-byte (per-slot-mutex) first cut did not, and `bench_forward_demux`
     * charged ~2% for at 64 registered links.
     */
    sink_slot_t<reply_fn_t> reply_;               /**< @brief Reply-terminus sink. */
    sink_slot_t<inbound_fn_t> inbound_;           /**< @brief Inbound-FWD observer. */
    sink_slot_t<raw_fn_t> raw_;                   /**< @brief Raw-frame observer. */
    sink_slot_t<compact_delivery_fn_t> delivery_; /**< @brief Local COMPACT delivery sink. */
    sink_slot_t<stale_label_fn_t> stale_;         /**< @brief Stale-label observer. */
};

}  // namespace tr::net
