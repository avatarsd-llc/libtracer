/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0004 / ADR-0035 slice 4 — the route-handle: ws delivery-compaction. The ws
 * (full-TLV) counterpart of transport_can's `identity↔path` map (#55/ADR-0030).
 *
 * Taken literally, "a delivery *is* a FWD WRITE" (RFC-0004 §D) makes every streamed
 * sample re-carry its full return route — ~16x overhead on a small high-rate
 * sample (RFC-0004 §E.1). The fix is header-elision generalized: a per-link LABEL
 * that aliases an established delivery route. A label is meaningful only on the
 * link it was bound for; each forwarding hop SWAPS it (MPLS-style), exactly as a
 * CAN-ID is re-resolved against each bus. Binding is advertise-driven: the upstream
 * advertises `label ↔ route` in-band when a compact-flagged flow starts; each hop
 * learns `label → (downstream link, out-label)` and re-advertises downstream with
 * its OWN label. Re-advertise on (re)connect is the self-heal (ADR-0030); a
 * delivery bearing an unknown/stale label is dropped with a NACK that prompts a
 * re-advertise — never a crash.
 *
 * `route_handle_t` is the per-node label store: a small `label ↔ binding` map kept
 * per link, scoped to the flows explicitly flagged `delivery_compact`. A
 * cold/one-shot/non-compact flow allocates NO entry here, preserving the slice-3
 * stateless-forwarder property. The orchestration (advertise propagation + COMPACT
 * swap) lives in fwd_router_t, which owns one route_handle_t.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/graph.hpp"

namespace tr::net {

/**
 * @brief The RESOLVED form of a binding — everything a steady-state COMPACT frame needs.
 *
 * ADR-0062's point, made concrete: an established flow must not re-derive what it already
 * knows. This is trivially copyable and allocation-free, so @ref route_handle_t::resolved
 * can hand it out from under the link's mutex without the `std::string` + `std::vector`
 * copy `lookup_ingress` pays on EVERY frame today — a cost that landed before anything even
 * looked at whether a resolution was cached.
 *
 * The route bytes are deliberately absent. A warm binding never touches them: the terminus
 * dereferences `target` and writes; the forwarding hop sends on `down`. They are
 * needed only to RE-resolve, which is the cold path and keeps the owning accessor.
 *
 * Both cached forms are self-invalidating rather than notified — no callback fires from
 * under a lock:
 *   - `target` is paired with `target_gen`, compared against
 *     `graph_t::retire_generation`; a retired-and-revived vertex bumps it (#511), so a
 *     stale handle is detected on use (RFC-0009 §B.6 re-virginize).
 *   - `down` is read from the registry SLOT, whose `link` teardown nulls in place
 *     (ADR-0063 made slot addresses permanently stable) — so a departed link reads
 *     `nullptr`: the same clean miss as an unresolved lookup. The tombstone IS the
 *     invalidation.
 */
struct resolved_binding_t {
    bool found = false;          /**< @brief false ⇒ no binding for this (link, label). */
    bool terminus = false;       /**< @brief true ⇒ deliver locally; false ⇒ forward + swap. */
    bool warm = false;           /**< @brief true ⇒ the resolution below is populated. */
    std::uint16_t out_label = 0; /**< @brief Forward: label to stamp downstream. */
    const void* down_slot = nullptr; /**< @brief Forward: cached registry slot. */
    /** @brief Terminus: the cached vertex. `std::optional` rather than a defaulted handle —
     *         ADR-0056 keeps `vertex_handle_t` opaque and ALWAYS valid, so "no resolution yet"
     *         must be expressed outside the handle, not as an invalid one. */
    std::optional<graph::vertex_handle_t> target;
    std::uint32_t target_gen = 0; /**< @brief Terminus: generation `target` was resolved at. */
    /** @brief The MOUNT-SHAPE generation this binding was resolved against (#765) — see
     *         @ref handle_binding_t::mount_gen. Carried into the allocation-free view so the
     *         warm COMPACT path can validate it without taking the owning form. */
    std::uint32_t mount_gen = 0;
};

/**
 * @brief One learned per-link label binding — what an inbound label means here.
 *
 * Either a forwarding swap (rewrite to `out_label` and re-emit over `down_link`) or a
 * local terminus (resolve `local_route` and apply the write). The trailing fields memoize
 * what that resolution produced (ADR-0062), so an established flow stops re-deriving it; the
 * allocation-free view of them is @ref resolved_binding_t.
 */
struct handle_binding_t {
    bool terminus = false;     /**< @brief true ⇒ deliver locally; false ⇒ forward + swap. */
    std::string down_link;     /**< @brief Forward: this node's NAME for the downstream link. */
    std::uint16_t out_label{}; /**< @brief Forward: label to stamp on the downstream COMPACT. */
    std::vector<std::byte>
        local_route; /**< @brief Terminus: the local dst PATH TLV bytes to resolve + write. */
    /** @brief ADR-0062: the resolution, filled on first use and re-filled when it goes stale.
     *         `warm == false` means "never resolved"; the two cached forms carry their own
     *         staleness signal (see @ref resolved_binding_t). */
    bool warm = false;               /**< @brief true ⇒ the cached fields below are filled. */
    const void* down_slot = nullptr; /**< @brief Forward: cached `child_registry_t::child_t*`. */
    std::optional<graph::vertex_handle_t> target; /**< @brief Terminus: the cached vertex. */
    std::uint32_t target_gen = 0; /**< @brief Terminus: generation `target` was resolved at. */
    /**
     * @brief The mount-shape generation this binding's SPLIT was decided against (#765).
     *
     * The THIRD validate-on-use stamp, and it exists because the other two cannot see this
     * hazard. `target_gen` catches a retired-and-revived vertex; the slot tombstone catches a
     * departed link. Neither catches the split MOVING: bind a label through mount `net/ws/s`,
     * then register `net/ws/s/rack`, and a full `FWD` resolves against the new, deeper mount
     * while a `COMPACT` riding this label still dereferences the binding made against the old
     * one. Both targets are alive and both are what they always were — what changed is the
     * point at which the address divides into "local mount" and "remote residual", and the
     * two planes now disagree about the same address with nothing reporting it.
     *
     * Until #523 the disagreement was unreachable, but only by accident: the descent capped
     * its width, so a deeper mount was unroutable to BOTH planes. That is agreement by mutual
     * failure, and it stopped holding the moment the width bound was lifted.
     *
     * Compared against @ref child_registry_t::mount_generation on use; a mismatch takes the
     * SAME RFC-0004 §E.1 self-heal a stale label already takes — drop, fire the stale-label
     * observer, `HANDLE_NACK` upstream to prompt a re-advertise. No new error code, and no
     * second invalidation mechanism (the objection that killed ADR-0062's reverse index).
     */
    std::uint32_t mount_gen = 0;
};

/**
 * @brief Per-connection `label ↔ route` tables for ws delivery-compaction (RFC-0004 §E.1).
 *
 * The label state lives PER LINK (ADR-0038 §3 / ADR-0039): each connection owns
 * its own small tables — an INGRESS table (a label arriving on the link → its
 * @ref handle_binding_t), an EGRESS table (a label this node advertised over the
 * link → the route it aliases, retained so a NACK can re-advertise), and a
 * monotonic label allocator — drawn from the injected memory resource and guarded
 * by the LINK'S OWN mutex, so label traffic on one connection never contends with
 * another. The only cross-link lock is a `shared_mutex` over the link registry,
 * taken exclusively when a link's tables are first CREATED or when @ref clear_link
 * removes them. Each link's tables are owned by a `shared_ptr`, so the accessors
 * hand out a PINNING copy: `clear_link` can erase the registry entry while a
 * concurrent writer still holds the tables — the node is destroyed only when the
 * last outstanding reference drops (no dangling reference), and the registry is
 * bounded to LIVE link names instead of growing one empty shell per departed name
 * (#488). State exists only for flows that opted into compaction, so @ref
 * ingress_count on a node forwarding only one-shot/cold traffic is zero.
 */
class route_handle_t {
   public:
    /**
     * @brief Draw all label state from @p mr (ADR-0039 §1), bounding each link's tables at
     *        @p max_bindings_per_link entries.
     *
     * A bounded node passes a pool resource over its slab and the label tables
     * live entirely in host-chosen memory; the default is the standard heap.
     * @p mr must outlive this object.
     *
     * **The bound is injected, never assumed** ([CONTEXT.md §Resource bound]). `0` means
     * unbounded — the default, and the pre-#603 behavior. A bounded host sizes it from its
     * own slab; ADR-0038 §3 calls for exactly this (*"sized by `:settings`"*).
     *
     * Without a bound the tables are peer-driven and grow to the whole label space: a link
     * can accumulate 65535 ingress bindings, each a `handle_binding_t` carrying a
     * `std::string` and a `std::vector`. That is megabytes per link on a node whose whole
     * budget is 16 KB.
     *
     * @param mr                     Where all label state is allocated.
     * @param max_bindings_per_link  Ceiling on a link's ingress table AND, separately, its
     *                               egress table; `0` ⇒ unbounded.
     */
    explicit route_handle_t(std::pmr::memory_resource* mr = std::pmr::get_default_resource(),
                            std::size_t max_bindings_per_link = 0)
        : mr_(mr), max_bindings_(max_bindings_per_link), links_(mr) {}

    route_handle_t(const route_handle_t&) = delete;
    route_handle_t& operator=(const route_handle_t&) = delete;

    /**
     * @brief Record an ingress binding: a @p label arriving on @p in_link means @p binding.
     *
     * Rebinding a label already present always succeeds — it replaces in place and adds no
     * entry. Only a NEW label can be refused, and only when the link is at
     * `max_bindings_per_link`.
     *
     * @param in_link This node's NAME for the link the ADVERTISE/COMPACT arrives on.
     * @param label   The label as seen on that inbound link.
     * @param binding Its meaning (forward-swap or local terminus).
     * @retval false The link's ingress table is full — nothing was recorded, and
     *               @ref refused_bindings was incremented. A COMPACT on the unbound label
     *               then takes the same drop-and-HANDLE_NACK path a stale label already
     *               takes, which prompts the peer to re-advertise.
     */
    [[nodiscard]] bool bind_ingress(std::string_view in_link, std::uint16_t label,
                                    handle_binding_t binding);

    /**
     * @brief The CLEAR EPOCH of @p link — the token that says "this link's tables have not been
     *        reconnected since" (#827).
     *
     * Sampled by a forwarding hop BEFORE it mints anything against its downstream link, and
     * handed back to @ref bind_ingress_forward when the swap is finally bound. Every
     * @ref clear_link advances a node-wide counter, and a link's tables carry the value they
     * were created at, so once the link HAS tables its epoch changes only when THIS link is
     * cleared — an unrelated link's reconnect leaves it alone. Before the tables exist there
     * is nothing to stamp, so a sample reads the shared counter: an unrelated clear_link
     * landing between that sample and the table creation makes the fresh tables stamp the
     * bumped value and the bind is refused SPURIOUSLY. The refusal direction is safe — the
     * upstream's next COMPACT draws a stale-label NACK and the re-advertise binds normally —
     * a liveness nit confined to a link's first-contact window, never a stale binding.
     *
     * @param link This node's NAME for the link.
     * @return An opaque token, meaningful only when compared against a later sample of the
     *         SAME link. A link with no tables reads the current counter, so creating them
     *         changes nothing and the first advertise on a link is refused only in the
     *         first-contact window above.
     */
    [[nodiscard]] std::uint32_t link_epoch(std::string_view link) const;

    /**
     * @brief @ref bind_ingress for a FORWARDING swap, refused if @p down_epoch went stale.
     *
     * The store-under-inbound / point-at-outbound asymmetry again (#716), now in its racing
     * form (#827). @ref clear_link's cross-link sweep can only erase bindings that already
     * exist; an `on_advertise` running on another link's rx thread mints its out-label and
     * retains its egress route against the PRE-clear downstream table, and binds the swap
     * afterwards. A reconnect landing between those two steps sweeps the inbound link before
     * the binding is there, and the binding lands after the table it aims into is gone —
     * reproducing the exact #716 state the sweep exists to prevent, through a window
     * measured in microseconds and with a permanent outcome.
     *
     * So the swap is bound only if @p down_epoch — sampled from @ref link_epoch before the
     * mint — still names the downstream tables the label was minted against. The epoch read
     * and the insert are ONE critical section against @ref clear_link, so the sweep cannot
     * interleave between them. Refusing takes the path a full ingress table already takes
     * (`false`, nothing recorded): the peer's COMPACT misses, draws the ordinary stale-label
     * HANDLE_NACK, and the flow re-advertises from a clean slate. Nothing new goes on the
     * wire, and this is the COLD advertise path — the per-delivery path is untouched.
     *
     * A refusal is deliberately NOT counted in @ref refused_bindings, which means "a link's
     * table was at its injected bound". This is not a resource refusal and it is not silent:
     * the very next COMPACT on the unbound label fires the stale-label observer, which is
     * where the event is already visible.
     *
     * @param in_link    This node's NAME for the link the ADVERTISE arrived on.
     * @param label      The label as seen on that inbound link.
     * @param binding    The forwarding swap; `binding.down_link` names the link @p down_epoch
     *                   was sampled from. A terminus binding has no downstream half and must
     *                   use @ref bind_ingress instead.
     * @param down_epoch The @ref link_epoch of `binding.down_link`, sampled BEFORE the
     *                   out-label was minted.
     * @retval false Nothing was recorded — either the ingress table is at its bound, or the
     *               downstream link was reconnected inside the window.
     */
    [[nodiscard]] bool bind_ingress_forward(std::string_view in_link, std::uint16_t label,
                                            handle_binding_t binding, std::uint32_t down_epoch);

    /**
     * @brief Look up what a @p label arriving on @p in_link means (nullopt ⇒ stale/unknown).
     * @param in_link This node's NAME for the inbound link.
     * @param label   The inbound label.
     * @return The learned binding, or `std::nullopt` if no binding exists (drop + NACK).
     */
    [[nodiscard]] std::optional<handle_binding_t> lookup_ingress(std::string_view in_link,
                                                                 std::uint16_t label) const;

    /**
     * @brief The steady-state lookup: the RESOLVED binding, by value, allocation-free.
     *
     * Prefer this on the COMPACT hot path. @ref lookup_ingress copies a `std::string` and a
     * `std::vector` out of the table on every call — two allocations per frame, paid before
     * anything checks whether the flow was already resolved. This copies ~24 trivially
     * copyable bytes instead, and returns `found=false` for an unknown label so the caller
     * still NACKs identically.
     *
     * A `warm=false` result means the caller must resolve and then call @ref cache_resolution.
     */
    [[nodiscard]] resolved_binding_t resolved(std::string_view in_link, std::uint16_t label) const;

    /**
     * @brief Record the resolution @p r against (@p in_link, @p label) so later frames skip it.
     *
     * Idempotent and best-effort: a binding that vanished between resolve and cache is simply
     * not updated, and the next frame resolves again. Never invalidates a live delivery.
     */
    void cache_resolution(std::string_view in_link, std::uint16_t label,
                          const resolved_binding_t& r);

    /**
     * @brief Remember the @p route advertised over @p out_link under @p label.
     *
     * Lets this node re-advertise the binding on a HANDLE_NACK (or reconnect) without
     * re-deriving the route. Idempotent — re-recording the same key replaces it.
     * @param out_link This node's NAME for the downstream link the ADVERTISE went out on.
     * @param label    The label this node assigned for that downstream flow.
     * @param route    The (possibly stripped) dst PATH TLV bytes the label aliases.
     * @retval false The link's egress table is full — nothing was recorded and
     *               @ref refused_bindings was incremented. The caller must not advertise a
     *               label it cannot re-advertise on a NACK.
     */
    [[nodiscard]] bool record_egress(std::string_view out_link, std::uint16_t label,
                                     std::vector<std::byte> route);

    /**
     * @brief Find this link's label for @p route, or allocate + record a fresh one (#136).
     *
     * The producer-origin lazy-advertise primitive (RFC-0004 §E.1, Q5): the first compact
     * delivery on a `(out_link, route)` flow has no binding, so a new label is allocated,
     * recorded as egress, and returned with `fresh == true` (the caller must send the
     * ADVERTISE once); subsequent deliveries find the same label and return `fresh ==
     * false` (send only the COMPACT). @ref clear_link drops the binding so a post-reconnect
     * delivery re-advertises — the self-heal, with no transport "up" event. Since #913 the
     * forwarding-hop swap and `fwd_router_t::advertise` mint here too, in place of an
     * @ref alloc_label + @ref record_egress pair that minted unconditionally and burned one
     * label per re-advertise cycle.
     *
     * A fresh entry is born RECLAIMABLE and the first *reuse* of it clears that (#833): while
     * the mint is the only take of a label, the minter may still hand it back with
     * @ref release_egress. The steady-state delivery path pays one byte-load and a
     * not-taken branch for that — the store happens at most once per entry, on the first
     * reuse — and never a store on an entry a second caller has already taken.
     *
     * @param out_link This node's NAME for the downstream link.
     * @param route    A complete PATH TLV's bytes — the delivery route the label aliases.
     * @return `{label, fresh}` — the (reused or new) label, and whether it was just created.
     *         `{0, false}` ⇒ **no label is available**: either this link's label space is
     *         exhausted (see @ref alloc_label) or its egress table is at
     *         `max_bindings_per_link`. Nothing was recorded; the caller delivers over the
     *         full-route FWD path. A flow ALREADY in the table is never refused — reuse is
     *         checked before the bound, so a full table degrades new flows only.
     */
    [[nodiscard]] std::pair<std::uint16_t, bool> ensure_egress(std::string_view out_link,
                                                               std::span<const std::byte> route);

    /**
     * @brief Hand back a label + egress route taken from @ref ensure_egress and **never put
     *        on the wire** — the refused-bind unwind (#833).
     *
     * A forwarding hop mints its out-label and retains its stripped egress route BEFORE it
     * binds the ingress swap, because the binding names the label. When that bind refuses —
     * a full ingress table, or the #827 epoch guard — the hop returns without advertising,
     * so the label it minted aliases a route no ingress binding aims at and no peer has ever
     * seen. Nothing reclaimed it short of the downstream link's next @ref clear_link. This
     * gives it back: the entry is erased, and the label itself returns to the allocator when
     * it is still the most recently minted one.
     *
     * **Only the MINT is reclaimable.** Since #913 an egress entry is SHARED — one label
     * serves every ingress flow whose stripped route is identical — so erasing it on one
     * claimant's refusal would strand every other. The entry therefore carries "the mint is
     * still the only take of this label", set when @ref ensure_egress creates it and cleared
     * by the first reuse, and this call erases nothing once that is false. That is what
     * makes the two-thread interleaving safe without holding a lock across the bind: an
     * advertise on another link's rx thread that reuses the label between this caller's mint
     * and its refusal has already cleared the flag, so the entry it now depends on survives.
     * An established flow is untouched by construction — its take was a reuse.
     *
     * @param out_link This node's NAME for the downstream link the label was minted on.
     * @param label    The label @ref ensure_egress returned. `0` is ignored.
     * @param route    The route bytes that were passed to @ref ensure_egress; an entry whose
     *                 route has since been replaced (@ref record_egress) is left alone.
     * @note A no-op when the link has no tables — a release must not CREATE a link shell,
     *       which is the state @ref link_count bounds (#488). So the common companion of a
     *       refusal, a downstream reconnect that erased the whole table, costs nothing here.
     */
    void release_egress(std::string_view out_link, std::uint16_t label,
                        std::span<const std::byte> route);

    /**
     * @brief The route this node advertised over @p out_link under @p label (for re-advertise).
     * @param out_link This node's NAME for the downstream link.
     * @param label    The downstream label.
     * @return The retained route PATH bytes, or `std::nullopt` if unknown.
     */
    [[nodiscard]] std::optional<std::vector<std::byte>> egress_route(std::string_view out_link,
                                                                     std::uint16_t label) const;

    /**
     * @brief Allocate a fresh, per-link, monotonic label (≥1; 0 is reserved "none").
     *
     * The allocator SATURATES rather than wrapping (#603). It issues 1..65535 in order and
     * is then permanently exhausted, returning 0 — because a wrapped counter re-issues
     * labels that still alias live routes, and a delivery on a reused label resolves the
     * WRONG route (a misroute, not a drop). Labels are not reclaimed individually today;
     * @ref clear_link drops a link's whole table and restores its space, which is the
     * self-heal a (re)connect already performs.
     *
     * @param link This node's NAME for the link the label is scoped to.
     * @return A label unique among this link's currently allocated labels, or **0** when the
     *         link's 16-bit space is exhausted — callers MUST treat 0 as "cannot compact"
     *         and fall back to the full-route form, never stamp it on a frame.
     */
    [[nodiscard]] std::uint16_t alloc_label(std::string_view link);

    /**
     * @brief Drop ALL state (ingress, egress, allocator) for @p link, AND every ingress
     *        binding on any OTHER link whose downstream half crossed @p link — the self-heal hook.
     *
     * A transport calls this on (re)connect/disconnect of @p link so a subsequent
     * re-advertise rebinds from a clean slate; a delivery on a now-cleared label is
     * stale and is NACK'd rather than misrouted.
     *
     * The cross-link sweep is what makes that true on a MID-CHAIN node (#716). A forwarding
     * binding is stored under the **inbound** link while `handle_binding_t::down_link` names
     * the **outbound** one, so clearing only @p link's own tables leaves an ingress binding
     * elsewhere still pointing at an out-label that died with them. The upstream never saw
     * the reconnect and so never re-advertises: it keeps streaming COMPACTs, this node keeps
     * forwarding the dead out-label, the downstream keeps NACKing, and the NACK is answered
     * from the very table that was erased — a permanent, silent drop of the whole flow.
     * Erasing those bindings makes the upstream's next COMPACT miss, which draws the ordinary
     * stale-label `HANDLE_NACK` and prompts the upstream to re-advertise; nothing new is put
     * on the wire. Terminus bindings have no downstream half and are never swept.
     *
     * The sweep can only erase bindings that already EXIST, which is why it is only half the
     * guard (#827): a forwarding swap still in flight on another rx thread is bound after the
     * sweep has already scanned its inbound link. That half is @ref bind_ingress_forward's —
     * this call advances the clear epoch @ref link_epoch reports, so a swap minted against the
     * tables erased here is refused rather than bound.
     *
     * Cost is O(links x bindings) on this COLD (re)connect path; the per-delivery path is
     * untouched.
     * @param link This node's NAME for the link whose state to forget.
     */
    void clear_link(std::string_view link);

    /** @brief Count of live ingress bindings (tests assert a non-compact flow holds 0). */
    [[nodiscard]] std::size_t ingress_count() const;

    /** @brief Count of live egress (advertised) bindings. */
    [[nodiscard]] std::size_t egress_count() const;

    /**
     * @brief Count of live per-link table shells in the registry (diagnostic).
     *
     * One shell per link name that currently holds compaction state. @ref clear_link
     * reclaims a departed link's shell, so a workload that churns through many distinct
     * link names returns here to its steady-state live-name count rather than growing
     * unboundedly (#488). Tests assert this reclamation.
     */
    [[nodiscard]] std::size_t link_count() const;

    /**
     * @brief Count of bindings refused because a link's table was at its bound (diagnostic).
     *
     * The counted-drop half of the bounded-resource contract (`can_reassembly_t` keeps
     * `dropped_groups` for the same reason): a bound that silently discards work is
     * indistinguishable from one that is never reached. A non-zero value here means some
     * flows on this node are delivering over the full-route form rather than compacted —
     * degraded throughput, never a wrong or missing delivery.
     */
    [[nodiscard]] std::size_t refused_bindings() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }

   private:
    // One connection's label state (ADR-0038 §3): flat pmr entry arrays (a link
    // carries FEW compact flows, so a linear label scan beats a node-based map —
    // no per-entry allocation, cache-linear) + the link's own mutex. Non-movable
    // (the mutex), constructed in place in the node-based registry map below.
    struct ingress_entry_t {
        std::uint16_t label = 0;
        handle_binding_t binding;
    };
    struct egress_entry_t {
        std::uint16_t label = 0;
        // Declared HERE, between the label and the route, so it lands in the padding the
        // u16 already leaves ahead of the vector — the entry's size is unchanged (#833).
        bool sole_take = false; /**< @brief The mint is still the only take of this label. */
        std::pmr::vector<std::byte> route;
    };
    struct link_tables_t {
        explicit link_tables_t(std::pmr::memory_resource* mr) : ingress(mr), egress(mr) {}
        std::mutex m;
        std::pmr::vector<ingress_entry_t> ingress;
        std::pmr::vector<egress_entry_t> egress;
        std::uint16_t next_label = 1;  // 0 is reserved "none"
        // The node-wide clear counter these tables were created at (#827). Appended AFTER the
        // hot fields on purpose: `resolved()` reads `ingress` and nothing else, and keeping
        // its offset put keeps the per-delivery lookup byte-for-byte what it was.
        std::uint32_t born_gen = 0;
    };

    // Each link's tables are heap-owned via a shared_ptr and the registry stores that
    // pointer, so an accessor hands out a PINNING copy: a table stays alive for as long
    // as any caller holds its shared_ptr, even after clear_link erases the registry entry
    // (#488). std::map node stability is no longer relied on for reference validity.
    /** @brief The link's tables, created on first use (exclusive registry lock). */
    [[nodiscard]] std::shared_ptr<link_tables_t> tables(std::string_view link);
    /** @brief The link's tables if they exist (shared registry lock), else nullptr. */
    [[nodiscard]] std::shared_ptr<link_tables_t> find_tables(std::string_view link) const;
    /** @brief @ref link_epoch with `links_m_` ALREADY held (either mode). */
    [[nodiscard]] std::uint32_t link_epoch_locked(std::string_view link) const;
    /** @brief The bind-or-refuse body shared by @ref bind_ingress and
     *         @ref bind_ingress_forward, with @p t's own mutex ALREADY held. */
    [[nodiscard]] bool bind_locked(link_tables_t& t, std::uint16_t label,
                                   handle_binding_t&& binding);

    std::pmr::memory_resource* mr_;
    std::size_t max_bindings_ = 0;         // 0 => unbounded; injected, never assumed
    std::atomic<std::size_t> refused_{0};  // counted drops (diagnostic)
    mutable std::shared_mutex links_m_;    // registry only: create/clear, never per delivery
    std::pmr::map<std::pmr::string, std::shared_ptr<link_tables_t>, std::less<>> links_;
    // The node-wide clear counter (#827), guarded by `links_m_` — every clear_link advances
    // it, every newly created link_tables_t stamps it. Saturating, per RFC-0024 §4.4's rule:
    // see clear_link's body for what the ceiling costs and why it is the safe direction.
    std::uint32_t clear_gen_ = 0;
};

/**
 * @brief Encode an ADVERTISE frame: `ADVERTISE{ VALUE label(u16), PATH route }`.
 *
 * A frame BUILDER, not an emitter. It returns an owning vector and so allocates through the
 * throwing global heap; on the shipping `-fno-exceptions` profile that is an `abort()`. Since
 * #885 nothing in the router calls it: a label-plane frame is put on a link by
 * `fwd_router.cpp`'s scatter-gather emitters, which write the 12-byte head on the stack and
 * reference the route. Use this only where a frame is wanted as a VALUE and the caller is not
 * on a peer-provoked path — conformance vectors, tests, tools.
 * @param label      The per-link label being bound (this hop's outbound label).
 * @param route_path A complete PATH TLV's bytes — the dst route the label aliases.
 * @return The framed ADVERTISE TLV bytes.
 */
[[nodiscard]] std::vector<std::byte> encode_advertise(std::uint16_t label,
                                                      std::span<const std::byte> route_path);

/**
 * @brief Encode a COMPACT delivery: `COMPACT{ VALUE label(u16), <payload TLV> }`.
 *
 * A frame BUILDER on the same terms as `encode_advertise` — owning, throwing, and called by
 * no production path since #885.
 * @param label   The per-link label naming the established route (no route bytes ride).
 * @param payload A complete payload TLV's bytes (the delivered VALUE).
 * @return The framed COMPACT TLV bytes.
 */
[[nodiscard]] std::vector<std::byte> encode_compact(std::uint16_t label,
                                                    std::span<const std::byte> payload);

/**
 * @brief The 6-byte `VALUE label(u16)` TLV that opens every route-handle frame.
 *
 * The label child is a fixed-shape run — opaque (`opt.PL=0`), 2-byte length — so it needs no
 * header emitter and no growable buffer. Returning it lets a SCATTER-GATHER egress build a
 * frame head entirely on the stack (`tr::net::stack_writer`) while keeping the byte layout
 * at ONE locus: the builders below emit it through here too, so a gathered frame and a built
 * one cannot drift apart. Since #885 every ADVERTISE, COMPACT and HANDLE_NACK the router
 * sends is gathered off a head that starts with these six bytes.
 * @param label The per-link label naming the established route.
 * @return `{VALUE, opt=0, len=2 (u16 LE), label (u16 LE)}`.
 */
[[nodiscard]] std::array<std::byte, 6> label_tlv(std::uint16_t label) noexcept;

/**
 * @brief Encode a HANDLE_NACK: `HANDLE_NACK{ VALUE label(u16) }` (stale-label signal).
 *
 * A frame BUILDER on the same terms as `encode_advertise` — owning, throwing, and called by
 * no production path since #885. The router answers a stale label from a stack buffer instead:
 * the frame is a fixed ten bytes, so there is nothing to allocate.
 * @param label The unknown/stale label that prompted the NACK.
 * @return The framed HANDLE_NACK TLV bytes.
 */
[[nodiscard]] std::vector<std::byte> encode_handle_nack(std::uint16_t label);

}  // namespace tr::net
