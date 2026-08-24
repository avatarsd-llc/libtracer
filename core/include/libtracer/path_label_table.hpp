/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0027 §§4, 7, 8 — the PATH-LABEL MINT TABLE: the per-host state a minting hop
 * holds so a labelled path element can be turned straight into a resolution it
 * already made, with no digest fold, no segment compare and no name hash.
 *
 * This is the piece RFC-0027 §11.1 collision 3 surrenders RFC-0024's "no hop holds
 * anything" for, knowingly and on the record. It is deliberately NOT the shape
 * RFC-0024 §12 rejected: it is not keyed on canonical bytes (it is an array indexed
 * by a number the peer supplies — a bounds check and a load), it adds no second
 * invalidation mechanism (the generation bump on departure IS the invalidation), it
 * is bounded by an INJECTED resource (ADR-0079's per-plane axis) and it REFUSES
 * rather than evicts, so its worst case is exactly today's behaviour: a host that
 * cannot mint forwards strings.
 *
 * Lives in `tr::net` (the transport plane): it is keyed by @ref tr::net::peer_handle_t,
 * the per-peer identity the receiver seam already carries (#1294), and per §9 no
 * label ever reaches a host-API call, a `path_t` the application builds, or a local
 * resolution. The VALUE it stores and hands back is a wire type (`tr::wire`), and
 * what that value means at L4 — the vertex-map bounds check, the retirement-generation
 * compare, the §8.2 ACL re-check — stays with the router, not here.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "libtracer/mem_source.hpp"
#include "libtracer/path_label.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/transport.hpp"

/**
 * @file
 * @brief The RFC-0027 mint table: `(peer, resolution) -> path label`, ceilinged and injected.
 */

namespace tr::net {

/**
 * @brief What a path label ALIASES on the minting host — a node-scoped vertex reference.
 *
 * The same `(u32 index, u32 generation)` pair a `PATH_REF` element carries, and the type is
 * reused rather than re-declared: a label and a vref are two spellings of one primitive at two
 * granularities (RFC-0027 §11.1), so the doc set gets one definition of the pair rather than a
 * second that drifts. The two generations are DIFFERENT stamps and must not be confused — the
 * one inside this value is the target vertex's retirement generation, the one in the
 * @ref tr::wire::path_label_t naming it is the table SLOT's.
 */
using path_label_target_t = wire::path_ref_element_t;

/**
 * @brief The per-host path-label table of RFC-0027 §8.3 — injected, per-peer ceilinged,
 *        refuse-on-exhaustion, and never evicting a live label.
 *
 * A slot is minted for one peer against one already-made resolution (§6.2: the trigger is a
 * subscription's first fire — the moment the hop is *already holding* the answer, and no other
 * condition; no use counters, no hotness estimate, no timers, no aging). @ref lookup turns a
 * label the peer hands back into that resolution, and refuses anything it cannot validate, so
 * the caller answers a `NOT_FOUND`-class error and the peer falls back to the full-string path
 * it still holds (§7.2). Nothing here is load-bearing: a host that never mints, refuses every
 * mint, or exhausts its table is a host that forwards strings, which is what every host does
 * today (§6.3).
 *
 * **Four bounds, all of them rulings** (§8.3):
 *
 * - **The store is injected, and it is the FAILABLE seam.** Every allocation is drawn from the
 *   @ref tr::mem::block_source_t the embedder passes — ADR-0079's per-plane composition axis,
 *   not a static array and not a library-chosen capacity. The standing no-synthetic-limits rule
 *   (`CONTEXT.md` §Resource bound): a bound comes from an injected resource, never from a
 *   prediction. It is `block_source_t` and NOT `std::pmr` for the reason ADR-0065 gives and
 *   #1478 made concrete here: this table is fed by REMOTE PEERS, so its allocations are the
 *   reachable-from-the-network class the failable seam exists to bound, and
 *   `%std::pmr::memory_resource::allocate` can only report exhaustion by throwing — which on
 *   the `-fno-exceptions` shipping profile reaches ESP-IDF's link-wrapped `__cxa_throw`
 *   `abort()` stub. Exhaustion here is a REFUSAL by value, counted, and nothing else.
 * - **A per-peer ceiling.** One peer cannot consume the table. It is per-target configuration,
 *   never a magic number, and it is counted against the peer handle the receiver seam already
 *   mints (§10 — the substrate this table hangs its accounting on with no new bookkeeping).
 * - **On exhaustion the host REFUSES a new mint.** It does not evict, does not grow, and does
 *   not fail the operation; @ref mint answers `std::nullopt` and the caller leaves the part a
 *   string. A refusal is invisible to correctness, which is exactly why refusing is affordable.
 * - **Live labels are NEVER evicted by pressure.** An established label is not a cache entry:
 *   evicting one converts a bounded-resource condition into a stream of avoidable `NOT_FOUND`
 *   round trips on flows that were working. The only way a label stops validating is
 *   @ref release / @ref release_peer — a departure, which is §7.1's generation bump.
 *
 * **Saturate and retire, never wrap (§4.3.1).** A release bumps its slot's generation. When
 * that bump reaches @ref tr::wire::kPathLabelMaxGeneration the generation stops advancing and
 * the slot is **retired permanently** — removed from the mintable set for the lifetime of the
 * table, not after reclamation, not after the peer departs, not after the table empties. This
 * preserves RFC-0024 §4.4 rule 3 verbatim across both `(slot, generation)` fields at zero wire
 * cost, and closes #603's mis-delivery class by construction: the generation never returns to a
 * value a peer might still be holding, so no stale label can ever validate falsely. Retirement
 * is invisible to correctness because it degrades to §8.3's already-accepted refuse-new path.
 *
 * All calls may race the transport's receive thread and synchronize internally, on the pattern
 * `route_handle_t` established for the §E.1 label plane.
 */
class path_label_table_t {
   public:
    /**
     * @brief A table that mints nothing — the default capacity, and a conformant host.
     *
     * §6.3 is what makes an unconfigured node the SAFE default rather than a degraded one: a
     * host that never mints leaves every part a string and nothing on the route notices. A
     * deployment that wants compaction sizes the table from its own slab, which is the only
     * place the size may come from.
     */
    static constexpr std::size_t kMintsNothing = 0;

    /**
     * @brief No per-peer ceiling — every slot of @p capacity is reachable by one peer.
     *
     * Spelled, and spelled as a hazard: §8.3 requires a ceiling, and this value is here only
     * for the single-peer and test cases where "the table's own capacity" already IS the
     * per-peer bound. A multi-peer deployment that leaves it unset lets one peer spend the
     * table on everyone else's behalf.
     */
    static constexpr std::size_t kNoPeerCeiling = 0;

    /**
     * @brief No explicit ceiling on DISTINCT peers — derive it from @ref capacity.
     *
     * A peer only occupies a census entry while it holds at least one live label, so the
     * number of entries can never exceed the number of live labels, which is @ref capacity.
     * Deriving is therefore the bound that was always true, not a new one, and it is what
     * keeps this migration a no-behaviour-change on every existing deployment.
     */
    static constexpr std::size_t kPeersFollowCapacity = 0;

    /**
     * @brief Build a table over @p src, holding at most @p capacity slots, at most
     *        @p max_per_peer of them for any one peer and at most @p max_peers distinct peers.
     *
     * **The slot array is reserved to @p capacity here**, which is the point of the seam
     * rather than a detail of it: after construction a mint takes a slot out of storage the
     * table already owns, so the peer-provoked path performs NO allocation at all and cannot
     * be the thing that finds the source empty. It also makes the injected bound REAL at the
     * moment the deployment states it, instead of discovered later under peer traffic — the
     * capacity was already chosen from the embedder's own slab (§6.3), so charging it up front
     * changes when it is paid, not how much. (This reverses the earlier grow-on-demand shape,
     * which spread the same total across the traffic that provoked it.) The per-peer census
     * takes a small floor instead and grows on demand up to @p max_peers, because a node
     * carries FEW peers and reserving one entry per possible slot would charge a large table
     * for peers it will never see; a census growth that the source refuses is a counted mint
     * refusal, exactly like reaching the ceiling.
     *
     * A source that cannot serve the reservation leaves the table at
     * @ref kMintsNothing — the §6.3 conformant host that forwards strings — rather than
     * throwing or aborting: @ref capacity then reads `0` and every @ref mint refuses and
     * counts into @ref refused_mints. Construction never fails, because a failure a
     * constructor cannot report by value is exactly the abort this seam exists to remove.
     *
     * @param src          Where the slot array and the per-peer census are allocated
     *                     (ADR-0065, ADR-0079). Must outlive this object. Defaults to the
     *                     process-wide platform heap, which is nothrow-failable like every
     *                     other source.
     * @param capacity     Slots this host may ever hold live at once. Clamped to
     *                     @ref tr::wire::kPathLabelSlotSpace, which is where the 16-bit index
     *                     stops; `0` (@ref kMintsNothing) means this host never mints.
     * @param max_per_peer The §8.3 per-peer ceiling; `0` (@ref kNoPeerCeiling) leaves one peer
     *                     able to reach @p capacity.
     * @param max_peers    Distinct peers that may hold a census entry at once; `0`
     *                     (@ref kPeersFollowCapacity) derives it from @p capacity. Clamped to
     *                     @p capacity, which no census can exceed anyway.
     */
    explicit path_label_table_t(mem::block_source_t* src = &mem::heap_source(),
                                std::size_t capacity = kMintsNothing,
                                std::size_t max_per_peer = kNoPeerCeiling,
                                std::size_t max_peers = kPeersFollowCapacity);

    path_label_table_t(const path_label_table_t&) = delete;
    path_label_table_t& operator=(const path_label_table_t&) = delete;

    /**
     * @brief Mint a label for @p peer aliasing the resolution @p target — or REFUSE (§8.3).
     *
     * The mint is POST-AUTH by construction and MUST stay so (§8.1): a caller reaches here only
     * on an operation that already ran its own gates, so no label is ever minted for a
     * destination an ancestor ACL hides. Probing a labelled route therefore yields what probing
     * the string form yields — *exists + denied*, never *exists + here is a handle to it*.
     *
     * @param peer   The peer the label is minted FOR, and the identity its ceiling is counted
     *               against. An invalid handle is refused: a label with no owner could be
     *               presented by anyone, and @ref lookup's owner check is half of §4.1's
     *               node-scope rule.
     * @param target The resolution this label aliases, as the minting host spells it.
     * @return The fresh label, or `std::nullopt` when the mint is refused — the peer is at its
     *         ceiling, the table is at its capacity, or every free slot has retired. A refusal
     *         is NOT an error: the caller leaves the part a string (§6.3) and the operation
     *         proceeds normally. @ref refused_mints counts it, because a bound that silently
     *         discards work is indistinguishable from one that is never reached.
     */
    [[nodiscard]] std::optional<wire::path_label_t> mint(peer_handle_t peer,
                                                         path_label_target_t target);

    /**
     * @brief Resolve @p label presented by @p peer — `std::nullopt` ⇒ answer `NOT_FOUND` (§7.2).
     *
     * Four ways to fail, and all four take the same answer, because a receiver that cannot
     * validate a label MUST NOT forward it, MUST NOT apply the operation, and MUST NOT attempt
     * any repair of its own — no re-resolution against a nearest match, no retry against a
     * different slot, no guessing (RFC-0024 §5.3's drop-never-mis-route rule, binding here
     * identically): the index is out of range, the slot holds no live label, the generation
     * does not match, or the label was minted for a DIFFERENT peer. The last is §4.1's scope
     * rule in force — a label means something only to the host that minted it and only to the
     * peer it was minted for, so a leaked label buys an attacker one `NOT_FOUND` and no state.
     *
     * @param peer  The peer the frame carrying @p label arrived from.
     * @param label The label as it arrived on the wire.
     * @return The resolution this label aliases, or `std::nullopt`.
     */
    [[nodiscard]] std::optional<path_label_target_t> lookup(peer_handle_t peer,
                                                            wire::path_label_t label) const;

    /**
     * @brief Retire the label at @p label's slot — §7.1's departure bump.
     *
     * Called when the part a label resolves to departs: retirement, connection-vertex removal,
     * link teardown. The slot's generation advances, so the label the peer still holds compares
     * unequal on its next frame and is refused; the peer falls back to strings and re-mints
     * from the next reply. There is **no withdraw frame, no unbind, no lease and no TTL**
     * (§7.3) — nothing is told, and the next frame discovers it.
     *
     * When the bump reaches @ref tr::wire::kPathLabelMaxGeneration the slot **retires
     * permanently** (§4.3.1) and is never returned to the free list; otherwise it becomes
     * mintable again. A stale @p label — one whose generation is already behind the slot's — is
     * ignored, so a late release can never retire a successor's live label.
     *
     * @param label The label to invalidate.
     * @retval false Nothing was released: the index is out of range, the slot holds no live
     *               label, or @p label is stale.
     */
    bool release(wire::path_label_t label);

    /**
     * @brief Release every label minted for @p peer — the peer-departure sweep (§7.1).
     *
     * The bulk form of @ref release, on the identity the seam hands down when a peer goes away.
     * Each slot takes the same generation bump and the same §4.3.1 saturation check, so a peer
     * that reconnects can never inherit a label its predecessor's counterpart still holds.
     * @param peer The departed peer.
     * @return How many labels were released.
     */
    std::size_t release_peer(peer_handle_t peer);

    /** @brief Slots this table may hold live at once — the injected capacity, post-clamp. */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /** @brief The §8.3 per-peer ceiling (`0` ⇒ @ref kNoPeerCeiling). */
    [[nodiscard]] std::size_t max_per_peer() const noexcept { return max_per_peer_; }

    /** @brief Distinct peers that may hold a census entry at once — post-derive, post-clamp. */
    [[nodiscard]] std::size_t max_peers() const noexcept { return max_peers_; }

    /** @brief Labels currently live across every peer. */
    [[nodiscard]] std::size_t live_count() const;

    /** @brief Labels currently live for @p peer — what its ceiling is compared against. */
    [[nodiscard]] std::size_t live_count_for(peer_handle_t peer) const;

    /**
     * @brief Slots retired permanently by §4.3.1's saturation (diagnostic).
     *
     * The measurement §15 clause 3 names as the design's one surviving falsifier: retirement is
     * affordable only if a real deployment does not churn slots fast enough to retire them, and
     * a table whose retired count climbs is a table on its way to minting nothing. It is not an
     * error — the degrade is §8.3's benign one — but it is the number that says so.
     */
    [[nodiscard]] std::size_t retired_slots() const;

    /**
     * @brief Mints refused for want of a slot or a peer's ceiling (diagnostic).
     *
     * Non-zero means some parts on this host are travelling as strings that could have been
     * labelled — degraded compaction, never a wrong or missing delivery (§8.3).
     */
    [[nodiscard]] std::size_t refused_mints() const;

   private:
    /** @brief The free-list terminator — no slot index can collide with it. */
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFU;

    /** @brief Census entries reserved at construction — a floor, not the ceiling. A node
     *         carries FEW peers, so this is what almost every deployment ever needs, and
     *         the array still grows on demand up to `max_peers_` for the ones that do not. */
    static constexpr std::size_t kPeerPrereserve = 8;

    /**
     * @brief One table slot: the peer that owns it, what it aliases, and its stamp.
     *
     * A retired slot (§4.3.1) is spelled by `generation == kPathLabelMaxGeneration` rather than
     * a flag — the saturation IS the retirement, so there is one fact and nothing to keep
     * consistent with it.
     */
    struct slot_t {
        peer_handle_t owner{};        /**< @brief The peer this slot was minted for. */
        path_label_target_t target{}; /**< @brief The resolution the label aliases. */
        std::uint16_t generation = 1; /**< @brief The current stamp; max ⇒ retired forever. */
        bool live = false;            /**< @brief False ⇒ free (or retired), never resolvable. */
        std::uint32_t next_free = kNoSlot; /**< @brief Free-list link; `kNoSlot` ends it. */
    };

    /** @brief One peer's live-label count — the §8.3 ceiling's accumulator. */
    struct peer_census_t {
        std::uint64_t peer = 0; /**< @brief `peer_handle_t::bits()` — the whole identity. */
        std::size_t live = 0;   /**< @brief Live labels for that peer; the entry dies at 0. */
    };

    /** @brief Release the live label in @p s (mutex held): bump, and retire on saturation. */
    void release_locked(slot_t& s, std::uint32_t index);

    /** @brief The census entry for @p key, or `nullptr` (mutex held). */
    [[nodiscard]] peer_census_t* find_census(std::uint64_t key) noexcept;

    /** @brief Remove the census entry @p c — its peer holds no live label (mutex held). */
    void drop_census(peer_census_t* c) noexcept;

    mutable std::mutex m_;
    std::size_t capacity_ = 0;
    std::size_t max_per_peer_ = 0;
    std::size_t max_peers_ = 0;
    std::size_t live_ = 0;
    std::size_t retired_ = 0;
    std::size_t refused_ = 0;
    // RESERVED to capacity_ at construction, not grown on demand, so a peer-provoked mint
    // allocates nothing. Freed slots come back through free_head_, so `size()` is the
    // high-water mark of live labels and never tracks traffic; past that mark the array is
    // already big enough and push_back cannot fail.
    mem::block_array_t<slot_t> slots_;
    std::uint32_t free_head_ = kNoSlot;
    // Linear, and deliberately: a node carries FEW peers, the entry dies when its count hits
    // zero, and a scan over a flat array beats a node-based map with no per-entry allocation
    // (route_handle_t's per-link tables took the same shape for the same reason). Reserved to
    // max_peers_ for the same reason slots_ is.
    mem::block_array_t<peer_census_t> peers_;
};

}  // namespace tr::net
