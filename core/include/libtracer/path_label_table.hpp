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
#include <memory_resource>
#include <mutex>
#include <optional>
#include <vector>

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
 * - **The store is injected.** Every allocation is drawn from the `memory_resource` the
 *   embedder passes — ADR-0079's per-plane composition axis, not a static array and not a
 *   library-chosen capacity. The standing no-synthetic-limits rule (`CONTEXT.md` §Resource
 *   bound): a bound comes from an injected resource, never from a prediction.
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
     * @brief Build a table over @p mr, holding at most @p capacity slots, at most
     *        @p max_per_peer of them for any one peer.
     *
     * @param mr           Where every slot and every per-peer counter is allocated (ADR-0079).
     *                     Must outlive this object.
     * @param capacity     Slots this host may ever hold live at once. Clamped to
     *                     @ref tr::wire::kPathLabelSlotSpace, which is where the 16-bit index
     *                     stops; `0` (@ref kMintsNothing) means this host never mints.
     * @param max_per_peer The §8.3 per-peer ceiling; `0` (@ref kNoPeerCeiling) leaves one peer
     *                     able to reach @p capacity.
     */
    explicit path_label_table_t(std::pmr::memory_resource* mr = std::pmr::get_default_resource(),
                                std::size_t capacity = kMintsNothing,
                                std::size_t max_per_peer = kNoPeerCeiling);

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

    mutable std::mutex m_;
    std::size_t capacity_ = 0;
    std::size_t max_per_peer_ = 0;
    std::size_t live_ = 0;
    std::size_t retired_ = 0;
    std::size_t refused_ = 0;
    // Grown on demand up to capacity_ rather than reserved up front: a host that mints a
    // handful of parts pays for a handful of slots, and the injected store is not asked for
    // 65 536 of them because the index could name that many. Freed slots come back through
    // free_head_, so the vector's size is the high-water mark of live labels, never traffic.
    std::pmr::vector<slot_t> slots_;
    std::uint32_t free_head_ = kNoSlot;
    // Linear, and deliberately: a node carries FEW peers, the entry dies when its count hits
    // zero, and a scan over a flat pmr vector beats a node-based map with no per-entry
    // allocation (route_handle_t's per-link tables took the same shape for the same reason).
    std::pmr::vector<peer_census_t> peers_;
};

}  // namespace tr::net
