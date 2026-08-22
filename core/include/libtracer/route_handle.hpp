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
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/mem_source.hpp"

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
 *
 * **A NON-OWNING descriptor** (#603 defect 1, ADR-0079). It used to carry a `std::string` and
 * a `std::vector`, which made it the structural reason this store could not leave `std::pmr`:
 * an entry holding those types is neither trivially copyable nor trivially destructible, so
 * `mem::block_array_t` could not hold it and the tables stayed on a throwing allocator that a
 * peer's ADVERTISE reaches. The bytes are now the CALLER's, borrowed for the duration of the
 * @ref route_handle_t::bind_ingress call, and the store copies them into blocks drawn from its
 * injected @ref tr::mem::block_source_t. That inverts the ownership so both halves get what
 * they need: the caller can point at a decoded frame it already holds (no allocation at all),
 * and the store's copy fails by value instead of throwing.
 *
 * @warning The views must outlive only the bind call. Nothing here is retained.
 */
struct handle_binding_t {
    bool terminus = false;      /**< @brief true ⇒ deliver locally; false ⇒ forward + swap. */
    std::string_view down_link; /**< @brief Forward: this node's NAME for the downstream link. */
    std::uint16_t out_label{};  /**< @brief Forward: label to stamp on the downstream COMPACT. */
    std::span<const std::byte>
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
 * @brief A binding read back OUT of the store into CALLER storage — allocation-free (#603).
 *
 * The owning read `lookup_ingress` used to serve, without the owning. It returned a
 * `std::optional<handle_binding_t>` whose `std::string` + `std::vector` were built on the
 * global heap on a peer-provoked arm; this reports the same facts into two spans the caller
 * already has, and reports the SIZES so a caller whose buffer was too small can retry against
 * a larger one rather than mistake truncation for absence.
 */
struct binding_copy_t {
    bool found = false;          /**< @brief false ⇒ no binding for this (link, label). */
    bool terminus = false;       /**< @brief true ⇒ deliver locally; false ⇒ forward + swap. */
    bool truncated = false;      /**< @brief A buffer was too small; the sizes below still hold. */
    std::uint16_t out_label = 0; /**< @brief Forward: label to stamp downstream. */
    std::uint32_t mount_gen = 0; /**< @brief The mount-shape generation (#765). */
    std::string_view down_link;  /**< @brief Forward: a view into the caller's link buffer. */
    std::span<const std::byte>
        local_route;                /**< @brief Terminus: a view into the caller's route buffer. */
    std::size_t down_link_size = 0; /**< @brief The binding's full link-name size. */
    std::size_t local_route_size = 0; /**< @brief The binding's full route size. */
};

/**
 * @brief Per-connection `label ↔ route` tables for ws delivery-compaction (RFC-0004 §E.1).
 *
 * The label state lives PER LINK (ADR-0038 §3 / ADR-0039): each connection owns
 * its own small tables — an INGRESS table (a label arriving on the link → its
 * @ref handle_binding_t), an EGRESS table (a label this node advertised over the
 * link → the route it aliases, retained so a NACK can re-advertise), and a
 * monotonic label allocator — drawn from the injected @ref tr::mem::block_source_t and
 * guarded by the LINK'S OWN mutex, so label traffic on one connection never contends with
 * another. The only cross-link lock is a `shared_mutex` over the link registry,
 * taken exclusively when a link's tables are first CREATED or when @ref clear_link
 * removes them. Each link's tables are REFCOUNTED, so the accessors
 * hand out a PINNING copy: `clear_link` can erase the registry entry while a
 * concurrent writer still holds the tables — the node is destroyed only when the
 * last outstanding reference drops (no dangling reference), and the registry is
 * bounded to LIVE link names instead of growing one empty shell per departed name
 * (#488). State exists only for flows that opted into compaction, so @ref
 * ingress_count on a node forwarding only one-shot/cold traffic is zero.
 *
 * ### Every byte comes from the injected source (#603 defect 1, #873 family 3)
 *
 * This store allocates through NOTHING but @ref tr::mem::block_source_t — no `%std::pmr`, no
 * `%std::string`, no `%std::vector`, no reach for the global heap. That is not hygiene here:
 * `on_advertise` runs on a transport receive thread, pre-ACL, driven entirely by a remote
 * peer, and the shipping profile is `-fno-exceptions` against an aborting `heap_resource_t`,
 * so a `%std::pmr` allocation on this path is a peer-triggerable node reboot. Exhaustion now
 * answers by VALUE at every door — @ref bind_ingress returns `false`, @ref ensure_egress
 * returns `{0, false}`, @ref record_egress returns `false` — and each of those is a refusal
 * the caller ALREADY handles, because it is the same answer a full table (#703) and an
 * exhausted label space (#701) give. A refused compaction degrades to the full-route
 * `FWD{WRITE}` form, which carries its own route and always works.
 *
 * Concretely, the four allocating shapes and where each one now draws from:
 *
 * | what | before | now |
 * | --- | --- | --- |
 * | a link's tables node + its name | `%std::allocate_shared` + `%std::pmr::string` (throwing) |
 * one `try_alloc` block, the name stored INLINE behind the object, intrusive refcount | | the link
 * registry | a `%std::pmr::map` node per link (throwing) | `mem::block_array_t<link_tables_t*>`,
 * linear scan | | the ingress / egress tables | `%std::pmr::vector` growth (throwing) |
 * `mem::block_array_t` growth (`false` on exhaustion) | | a route / link-name byte buffer |
 * `%std::pmr::vector<std::byte>` (throwing) | one exact-size `try_alloc` block per entry |
 *
 * The stored entry types are trivially copyable and trivially destructible, which is what
 * @ref tr::mem::block_array_t requires and what the OLD @ref handle_binding_t (a `%std::string`
 * plus a `%std::vector`) made impossible — see that type's note for the ownership inversion
 * that removed the blocker. Byte blocks are freed explicitly (`block_array_t` runs no
 * destructors), which is why erasure goes through @ref clear_link / @ref release_egress rather
 * than through a container's own `erase`.
 *
 * The pattern this sets for the rest of #873 is written down in
 * `docs/reference/09-memory-substrate.md` (§Migrating a STORE onto the substrate).
 */
class route_handle_t {
   public:
    /**
     * @brief Draw all label state from @p src (ADR-0065 / ADR-0079), bounding each link's
     *        tables at @p max_bindings_per_link entries.
     *
     * A bounded node passes a `mem::pool_source_t` over its slab and the label tables live
     * entirely in host-chosen memory; the default is the process-wide nothrow platform heap.
     * @p src must outlive this object, and must be thread-safe if more than one transport
     * receive thread can reach this store (the RFC-0014 wire-driven paths do).
     *
     * **The bound is injected, never assumed** ([CONTEXT.md §Resource bound]). `0` means
     * unbounded — the default, and the pre-#603 behavior. A bounded host sizes it from its
     * own slab; ADR-0038 §3 calls for exactly this (*"sized by `:settings`"*).
     *
     * Since #603 defect 1 the SOURCE is a bound in its own right, which is ADR-0079's
     * "a bounded node is a property the deployer injects": a full source refuses exactly as a
     * full table does, so a deployment can size the label plane by the slab alone and leave
     * @p max_bindings_per_link at `0`. The two are complementary rather than redundant — the
     * count bounds ONE link's share, the slab bounds the node's total.
     *
     * @param src                    Where all label state is allocated (nothrow, by value).
     * @param max_bindings_per_link  Ceiling on a link's ingress table AND, separately, its
     *                               egress table; `0` ⇒ unbounded.
     */
    explicit route_handle_t(mem::block_source_t* src = &mem::heap_source(),
                            std::size_t max_bindings_per_link = 0)
        : src_(src), max_bindings_(max_bindings_per_link), links_(*src) {}

    /** @brief Release every live link's tables (and the byte blocks they own). */
    ~route_handle_t();

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
     * @param binding Its meaning (forward-swap or local terminus). Its `down_link` /
     *                `local_route` bytes are BORROWED for the call and copied into the store's
     *                own blocks; nothing is retained.
     * @retval false The link's ingress table is full, **or the injected source is exhausted**
     *               — nothing was recorded, and @ref refused_bindings was incremented. A
     *               COMPACT on the unbound label then takes the same drop-and-HANDLE_NACK path
     *               a stale label already takes, which prompts the peer to re-advertise. The
     *               two refusals are deliberately ONE answer: ADR-0079 makes the injected
     *               store's size a bound, so "the slab said no" degrades exactly as "the count
     *               said no" already did, and no caller learns a new shape.
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
     * @brief Copy what a @p label arriving on @p in_link means into CALLER storage —
     *        allocation-free (`found == false` ⇒ stale/unknown).
     *
     * Replaces the owning `lookup_ingress` (#603 defect 1). That returned a
     * `%std::optional<handle_binding_t>` whose `%std::string` + `%std::vector` were built on
     * the throwing global heap — on the COLD COMPACT arm, which a peer provokes by sending a
     * frame on a label whose resolution has not been memoized yet, and on the observed-delivery
     * arm. Both are peer-driven, so both were `abort()` candidates on `-fno-exceptions`. There
     * is no allocation here at all: the bytes land in @p link_out / @p route_out.
     *
     * A buffer that is too small does NOT lose the answer. The binding's real sizes are
     * reported and `truncated` is set, so a caller can retry against a block grown from its own
     * injected source (`fwd_router.cpp` does exactly that for a route wider than its frame
     * buffer). That is the difference between "the route does not fit here" and "there is no
     * binding", which a plain empty result would have conflated.
     *
     * @param in_link   This node's NAME for the inbound link.
     * @param label     The inbound label.
     * @param link_out  Destination for a FORWARD binding's downstream link name. A link name
     *                  is one path segment, so `graph::kMaxSegmentBytes` always suffices.
     * @param route_out Destination for a TERMINUS binding's local route bytes.
     * @return The binding's facts, with views into @p link_out / @p route_out that are valid
     *         only while those buffers are. `found == false` ⇒ drop + NACK, exactly as a
     *         `nullopt` did.
     */
    [[nodiscard]] binding_copy_t copy_binding(std::string_view in_link, std::uint16_t label,
                                              std::span<char> link_out,
                                              std::span<std::byte> route_out) const;

    /**
     * @brief The steady-state lookup: the RESOLVED binding, by value, allocation-free.
     *
     * Prefer this on the COMPACT hot path. The retired owning `lookup_ingress` copied a
     * `std::string` and a `std::vector` out of the table on every call — two allocations per
     * frame, paid before anything checked whether the flow was already resolved, and both on
     * the throwing global heap. This copies ~24 trivially
     * copyable bytes instead, and returns `found=false` for an unknown label so the caller
     * still NACKs identically.
     *
     * A `warm=false` result means the caller must resolve and then call @ref cache_resolution.
     */
    [[nodiscard]] resolved_binding_t resolved(std::string_view in_link, std::uint16_t label) const;

    /**
     * @brief Copy a TERMINUS binding's local route bytes out, without allocating.
     *
     * The warm-COMPACT companion to @ref resolved. `resolved_binding_t` deliberately omits
     * the route bytes because a warm delivery does not need them to WRITE — but an installed
     * @ref fwd_router_t::on_compact_delivery observer is handed them, and serving it through
     * the retired owning `lookup_ingress` made the warm path re-pay the whole owning copy (a
     * `std::string` plus a `std::vector`) that @ref resolved exists to remove, per frame.
     *
     * The bytes are copied under the link's own mutex into caller storage rather than handed
     * to a callback from under it: the observer is host code, and the lock order this class
     * establishes is registry → table, so invoking anything re-entrant while holding a table
     * mutex would invert it against @ref clear_link.
     *
     * @param in_link This node's NAME for the inbound link.
     * @param label   The inbound label.
     * @param out     Destination; written only when the route FITS.
     * @return The route's full size in bytes — `0` when no binding exists (or it is a
     *         forwarding swap, which has no local route). A return greater than
     *         `out.size()` means nothing was written and the route is too long for @p out;
     *         the caller retries against a larger buffer (@ref copy_binding reports the same
     *         size). Callers must therefore test the returned size against @p out's, never
     *         assume a copy.
     */
    [[nodiscard]] std::size_t copy_local_route(std::string_view in_link, std::uint16_t label,
                                               std::span<std::byte> out) const;

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
     * @param route    The (possibly stripped) dst PATH TLV bytes the label aliases. Borrowed
     *                 for the call and copied into the store's own block.
     * @retval false The link's egress table is full, or the injected source is exhausted —
     *               nothing was recorded and @ref refused_bindings was incremented. The caller
     *               must not advertise a label it cannot re-advertise on a NACK.
     */
    [[nodiscard]] bool record_egress(std::string_view out_link, std::uint16_t label,
                                     std::span<const std::byte> route);

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
     *         `{0, false}` ⇒ **no label is available**: this link's label space is
     *         exhausted (see @ref alloc_label), its egress table is at
     *         `max_bindings_per_link`, or the injected source could not serve the route copy.
     *         Nothing was recorded; the caller delivers over the
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
     * @brief Copy the route this node advertised over @p out_link under @p label into @p out —
     *        allocation-free (for re-advertise).
     *
     * Replaces the owning `egress_route` (#603 defect 1). That one was already nothrow, but it
     * got there by probing the GLOBAL heap through `detail::try_assign` — which on
     * `-fno-exceptions` frees the probe block and then runs a throwing `assign` on the
     * inference that the block is still free. This arm is reached from an inbound HANDLE_NACK
     * on a transport receive thread, so a racer in that window is the normal case, and the
     * `assign` hitting exhaustion inside a `noexcept` aborts the node (#850, the #981
     * residual). Copying into caller storage removes the probe, the window and the global-heap
     * reach in one move.
     *
     * @param out_link This node's NAME for the downstream link.
     * @param label    The downstream label.
     * @param out      Destination; written only when the route FITS.
     * @return The route's full size in bytes — `0` when no route is bound for this label. A
     *         return greater than `out.size()` means nothing was written; the caller retries
     *         against a larger buffer (its own injected source) rather than reading a
     *         truncated route. Callers MUST test the returned size against @p out's.
     */
    [[nodiscard]] std::size_t copy_egress_route(std::string_view out_link, std::uint16_t label,
                                                std::span<std::byte> out) const;

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
     * @brief Count of bindings refused because a link's table was at its bound, or because
     *        the injected source was exhausted (diagnostic).
     *
     * ONE counter for both, because ADR-0079 makes them one fact: the store's size IS a bound,
     * so "the slab refused" and "the count refused" are the same operator-visible event —
     * this node is delivering some flows over the full-route form. Splitting them would ask an
     * operator to watch two counters for one symptom.
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

    /**
     * @brief Labels SPENT out of @p link's 16-bit space — used-polarity occupancy of the one
     *        resource this class cannot grow (#1503 finding 3).
     *
     * `capacity` for this seam is the constant 65535 (see @ref alloc_label): the space is
     * per-link and fixed by the wire, so unlike every other bounded resource in the tree
     * there is no injected ceiling to report beside it. Free is `65535 - labels_used(link)`.
     *
     * The allocator is monotonic and saturating — labels are not reclaimed individually, so
     * this only ever rises until @ref clear_link forgets the link and restores its whole
     * space. A value climbing toward 65535 on a long-lived link is the advance warning for
     * @ref labels_exhausted; reading it after the fact only tells you the degrade already
     * happened.
     *
     * @param link This node's NAME for the link.
     * @return Labels issued on @p link, `0` for a link that holds no compaction state, and
     *         65535 once the space is spent.
     */
    [[nodiscard]] std::size_t labels_used(std::string_view link) const;

    /**
     * @brief Times a mint was refused because a link's 16-bit label space was SPENT
     *        (#1503 finding 3) — the previously silent degrade.
     *
     * Distinct from @ref refused_bindings, and deliberately not fused into it: that counter
     * means "a link's table was at its INJECTED bound", which a deployment answers by sizing
     * the table up. This one means the wire's own 65535/link space ran out, which no amount
     * of memory fixes — the answer is a reconnect (@ref clear_link) or fewer distinct flows.
     * One event, one counter, but only where an operator would act differently.
     *
     * The degrade it makes visible is the one #1491 showed matters: a caller handed `0`
     * falls back to the full-route form, which REPLIES per frame — so throughput drops and
     * the reply traffic returns, with nothing on the wire saying why.
     *
     * Counted, never enforced (`core/STYLE.md` §Introspection): the library does not refuse
     * or reconnect on its own account.
     */
    [[nodiscard]] std::size_t labels_exhausted() const noexcept {
        return label_space_exhausted_.load(std::memory_order_relaxed);
    }

   private:
    // An exact-size byte block drawn from `src_`. Trivially copyable BY DESIGN: that is the
    // whole substrate move (#603 defect 1). A `std::pmr::vector` here made the entry types
    // non-trivial, which is what barred `mem::block_array_t` and kept the tables on a throwing
    // allocator a peer's ADVERTISE reaches. Ownership is explicit instead of RAII, because
    // `block_array_t` runs no destructors — every path that drops an entry calls `free_blob`.
    // No capacity field: a route is written once and replaced wholesale, so cap == n, which is
    // also the shape `pool_source_t`'s exact-size classes recycle with zero fragmentation.
    struct blob_t {
        std::byte* p = nullptr;
        std::uint32_t n = 0;
    };

    // One learned binding as STORED — the trivially-copyable twin of the public
    // `handle_binding_t`, with the two owning containers replaced by `blob_t`s the tables own.
    // Field order matches the public type so the copy in/out reads as a field-by-field mirror.
    struct stored_binding_t {
        bool terminus = false;
        bool warm = false;
        std::uint16_t out_label = 0;
        blob_t down_link;
        blob_t local_route;
        const void* down_slot = nullptr;
        std::optional<graph::vertex_handle_t> target;
        std::uint32_t target_gen = 0;
        std::uint32_t mount_gen = 0;
    };

    // One connection's label state (ADR-0038 §3): flat entry arrays over the injected source
    // (a link carries FEW compact flows, so a linear label scan beats a node-based map — no
    // per-entry node allocation, cache-linear) + the link's own mutex. Non-movable (the mutex),
    // constructed in place in a block of its own.
    struct ingress_entry_t {
        std::uint16_t label = 0;
        stored_binding_t binding;
    };
    struct egress_entry_t {
        std::uint16_t label = 0;
        // Declared HERE, between the label and the route, so it lands in the padding the
        // u16 already leaves ahead of the blob — the entry's size is unchanged (#833).
        bool sole_take = false; /**< @brief The mint is still the only take of this label. */
        blob_t route;
    };
    struct link_tables_t {
        explicit link_tables_t(mem::block_source_t& s) noexcept : src(&s), ingress(s), egress(s) {}
        mem::block_source_t* src;  // for freeing this table's blobs and the node itself
        std::mutex m;
        mem::block_array_t<ingress_entry_t> ingress;
        mem::block_array_t<egress_entry_t> egress;
        std::uint16_t next_label = 1;  // 0 is reserved "none"
        // The node-wide clear counter these tables were created at (#827).
        std::uint32_t born_gen = 0;
        // Intrusive refcount, replacing the `std::shared_ptr` control block (#603 defect 1):
        // `std::allocate_shared` is a THROWING allocation and there is no nothrow spelling of
        // it, so the pinning contract (#488) is served by hand. Same semantics, one block
        // instead of two, and the link's name lives inline behind this object.
        std::atomic<std::uint32_t> refs{1};
        std::uint32_t block_bytes = 0;  // the whole allocation, node + inline name
        std::uint32_t name_len = 0;     // the link name, stored immediately after this object
        /** @brief The link name stored inline behind this node. */
        [[nodiscard]] std::string_view name() const noexcept {
            return {reinterpret_cast<const char*>(this) + inline_name_offset(), name_len};
        }
        /** @brief Byte offset of the inline name from the start of the node's block. */
        [[nodiscard]] static constexpr std::size_t inline_name_offset() noexcept {
            return sizeof(link_tables_t);
        }
    };

    // A pinning reference to a link's tables — the intrusive replacement for the shared_ptr
    // copy the accessors used to hand out (#488). Move-only: a pin is taken once and dropped
    // once, and the copy that would need an addref never appears at a call site here.
    class tables_ref_t {
       public:
        tables_ref_t() noexcept = default;
        /** @brief Adopt an ALREADY-counted reference (the accessors retain before returning). */
        explicit tables_ref_t(link_tables_t* t) noexcept : t_(t) {}
        ~tables_ref_t() { drop(); }
        tables_ref_t(const tables_ref_t&) = delete;
        tables_ref_t& operator=(const tables_ref_t&) = delete;
        /** @brief Move the pin; the source is left empty. */
        tables_ref_t(tables_ref_t&& o) noexcept : t_(o.t_) { o.t_ = nullptr; }
        /** @brief Move-assign the pin, dropping this one first. */
        tables_ref_t& operator=(tables_ref_t&& o) noexcept {
            if (this != &o) {
                drop();
                t_ = o.t_;
                o.t_ = nullptr;
            }
            return *this;
        }
        /** @brief True when this pin holds a table. */
        explicit operator bool() const noexcept { return t_ != nullptr; }
        /** @brief The pinned tables. Precondition: non-empty. */
        [[nodiscard]] link_tables_t* operator->() const noexcept { return t_; }
        /** @brief The pinned tables. Precondition: non-empty. */
        [[nodiscard]] link_tables_t& operator*() const noexcept { return *t_; }

       private:
        void drop() noexcept {
            if (t_ != nullptr) release_tables(t_);
            t_ = nullptr;
        }
        link_tables_t* t_ = nullptr;
    };

    /**
     * @brief A link's tables held under the REGISTRY's shared lock — the READ path's access,
     *        with no refcount traffic at all (#603 defect 1).
     *
     * Measured, and the reason it exists. The pinning `tables_ref_t` below pins by an intrusive
     * `std::atomic` increment/decrement pair, which is correct but is TWO ATOMIC RMWs on the
     * per-delivery `COMPACT` path. The `std::shared_ptr` it replaced did not always pay them:
     * libstdc++ dispatches its refcount policy on whether the program is threaded, so a
     * single-threaded process got NON-atomic increments for free. Pinning unconditionally
     * therefore showed up as a per-frame cost the old code did not have — `bench_compact_delivery`
     * `compact-forward` p50 44 ns -> 52 ns — which is exactly the "#897-style atomic on the hot
     * path" that #873's cadence rules a reject.
     *
     * A reader does not need a refcount. `clear_link` erases only under the registry's
     * EXCLUSIVE lock, so holding the SHARED one for the duration of the read already keeps the
     * table alive — and that lock is taken either way, so the whole pin is redundant on this
     * path. The pin stays on the WRITE doors, which must release the registry lock (creating a
     * link's tables needs it exclusively, and a shared holder cannot upgrade).
     *
     * The cost is that a reader now holds the shared lock across the table's own critical
     * section — a linear scan of a few entries — instead of only across the registry lookup,
     * so a concurrent @ref clear_link waits that much longer. The lock ORDER is unchanged
     * (registry then table), which is what @ref bind_ingress_forward and @ref ingress_count
     * already establish, so no new ordering obligation appears.
     */
    class tables_view_t {
       public:
        /** @brief An empty view (no such link). */
        tables_view_t() noexcept = default;
        /** @brief Adopt @p lock and the table it keeps alive. */
        tables_view_t(std::shared_lock<std::shared_mutex>&& lock, link_tables_t* t) noexcept
            : lock_(std::move(lock)), t_(t) {}
        /** @brief True when this view holds a table. */
        explicit operator bool() const noexcept { return t_ != nullptr; }
        /** @brief The held tables. Precondition: non-empty. */
        [[nodiscard]] link_tables_t* operator->() const noexcept { return t_; }
        /** @brief The held tables. Precondition: non-empty. */
        [[nodiscard]] link_tables_t& operator*() const noexcept { return *t_; }

       private:
        std::shared_lock<std::shared_mutex> lock_;
        link_tables_t* t_ = nullptr;
    };

    /** @brief "No such link" for the registry scan. */
    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

    /** @brief Take a fresh reference on @p t and wrap it in a pin. */
    [[nodiscard]] static tables_ref_t pin(link_tables_t* t) noexcept {
        t->refs.fetch_add(1, std::memory_order_relaxed);
        return tables_ref_t{t};
    }
    /** @brief Drop one reference, destroying + freeing the node when the last one goes. */
    static void release_tables(link_tables_t* t) noexcept;
    /** @brief Free every blob an ingress binding owns. */
    static void free_binding(link_tables_t& t, stored_binding_t& b) noexcept;
    /** @brief Return a blob's block to the table's source and null it. */
    static void free_blob(link_tables_t& t, blob_t& b) noexcept;
    /** @brief Copy @p src into a FRESH exact-size block; `false` ⇒ exhausted, @p out untouched.
     */
    [[nodiscard]] static bool set_blob(link_tables_t& t, blob_t& out,
                                       std::span<const std::byte> src) noexcept;

    /** @brief The link's tables, created on first use (exclusive registry lock); empty when the
     *         source cannot serve the node. */
    [[nodiscard]] tables_ref_t tables(std::string_view link);
    /** @brief The link's tables if they exist, held under the registry's SHARED lock (the READ
     *         path — no refcount traffic); empty when the link has none. */
    [[nodiscard]] tables_view_t view_tables(std::string_view link) const;
    /** @brief The link's tables if they exist, PINNED (the write path); else empty. */
    [[nodiscard]] tables_ref_t find_tables(std::string_view link) const;
    /** @brief The registry slot index for @p link with `links_m_` ALREADY held, or `npos`. */
    [[nodiscard]] std::size_t find_slot_locked(std::string_view link) const noexcept;
    /** @brief @ref link_epoch with `links_m_` ALREADY held (either mode). */
    [[nodiscard]] std::uint32_t link_epoch_locked(std::string_view link) const;
    /** @brief The bind-or-refuse body shared by @ref bind_ingress and
     *         @ref bind_ingress_forward, with @p t's own mutex ALREADY held. */
    [[nodiscard]] bool bind_locked(link_tables_t& t, std::uint16_t label,
                                   const handle_binding_t& binding);

    mem::block_source_t* src_;             // the substrate: every byte of label state
    std::size_t max_bindings_ = 0;         // 0 => unbounded; injected, never assumed
    std::atomic<std::size_t> refused_{0};  // counted drops (diagnostic)
    mutable std::shared_mutex links_m_;    // registry only: create/clear, never per delivery
    // The registry: one POINTER per live link, scanned linearly. A node carries a handful of
    // links, so the scan compares a length and a few bytes against a `std::pmr::map`'s node
    // chase — and the map's per-link node was itself a throwing allocation on a peer path.
    mem::block_array_t<link_tables_t*> links_;
    // The node-wide clear counter (#827), guarded by `links_m_` — every clear_link advances
    // it, every newly created link_tables_t stamps it. Saturating, per RFC-0024 §4.4's rule:
    // see clear_link's body for what the ceiling costs and why it is the safe direction.
    std::uint32_t clear_gen_ = 0;
    /**
     * @brief 16-bit label-space exhaustions — see @ref labels_exhausted.
     *
     * Relaxed and bumped only on the COLD mint path (`core/STYLE.md` §Introspection 5).
     *
     * `std::uint32_t` and declared HERE, in `clear_gen_`'s existing tail padding, so
     * `sizeof(route_handle_t)` does not move. This object is a member of `fwd_router_t`,
     * and growing it shifts every router member after `handles_` — including the two sink
     * slots the FWD span path reads on every frame, whose adjacency `bench_forward_demux`
     * already charges ~2% for at 64 registered links. Costing nothing was cheaper than
     * arguing about it. A 32-bit count is enough for a diagnostic; a link that mints four
     * billion times has other news.
     */
    std::atomic<std::uint32_t> label_space_exhausted_{0};
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
