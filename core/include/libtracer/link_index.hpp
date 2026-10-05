/**
 * @file
 * @brief The per-link departure index `graph_t` keeps (#1071): which vertices may hold a
 *        subscriber edge admitted over a given link, keyed by the interned link identity.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * An IMPLEMENTATION header of `graph_t`, not an application surface: `graph.hpp` includes it
 * because `graph_t` holds the index by value, and nothing outside the graph unit names the
 * type. Applications reach the index only through `graph_t`'s link doors (`intern_link`,
 * `intern_link_hinted`, `release_link`, `link_edge_candidates`, `link_index_name_lookups`)
 * and through `graph_t::evict_link_edges` / `graph_t::evict_route_edges`, which stay on
 * `graph_t` because they also walk the vertex tree under the map lock.
 *
 * Extracted from `graph.cpp` (#1710) so the per-link record the RFC-0029 slices key on has
 * a seam of its own, and so a slice that edits the graph unit does not collide with it. The
 * move changes no behaviour: the same slots, the same carry, the same scan, the same lock.
 *
 * NOT a leaf: it names `vertex_t*` (forward-declared), because the candidate list is a list
 * of vertices.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

#include "libtracer/link_id.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/mem_string.hpp"

namespace tr::graph {

class vertex_t;

/**
 * @brief Which vertices hold a subscriber edge for a given LINK NAME — the index that makes
 *        a peer's departure cost its OWN edges instead of the graph's (#1071).
 *
 * Keyed by the edge's ADMITTED-OVER spelling, which is what `vertex_t::evict_link_edges`
 * matches on: `remote->link`, falling back to `remote->caller` when the former is empty
 * (a `field_write` admission stores the inbound link only as the gate context — #943).
 * Computing the key any other way at the caller would silently un-index exactly the edges
 * that fix was about.
 *
 * A SUPERSET, deliberately, and that asymmetry is the whole safety argument. An entry is
 * added when an edge is admitted and removed only when the whole link is evicted, so an
 * individual unsubscribe, a replace that displaces the last edge of a link, and a failed
 * admission all leave a STALE vertex behind. A stale entry costs one
 * `vertex_t::evict_link_edges` that matches nothing and reports 0 — the same no-op the
 * old whole-tree walk performed on every unsubscribed vertex it visited. A MISSING entry
 * would instead leak a live edge past a departure, so every path that can create one
 * indexes, and no path except whole-link eviction removes.
 *
 * Insertion happens at the `note_subscriber_added` bump, NOT after the slot lands:
 * that bump is precisely the predicate (`own_subs() > 0`) the replaced whole-tree walk
 * keyed on, so the index becomes visible to a concurrent eviction no later than the walk
 * would have seen the vertex. The subscribe-racing-its-own-link's-teardown window is
 * therefore exactly the pre-existing one `graph_t::evict_link_edges` documents, neither
 * widened nor narrowed.
 *
 * Drawn from the graph's TABLE source (`graph_t::table_source`, ADR-0083, #1778): every
 * table here is a core container whose growth refuses by value, so an exhausted source
 * answers a failed intern or a refused admission instead of a throw. The departure path
 * allocated a global-heap `std::vector` sized to the graph's whole subscribed set on every
 * peer hangup, which is the allocation #1071 called out. It now allocates nothing at all —
 * the candidate list IS this entry, moved out.
 *
 * @section link_index_carried Why this is a DENSE SLOT VECTOR and not a map (#1266 / #1417)
 *
 * It was a `std::pmr::unordered_map<std::pmr::string, link_entry_t>` with a transparent
 * hash, so every remote subscribe hashed the arriving link name and found it. PR #1416
 * measured what that costs and what removing it could buy, four arms in one binary
 * against a fresh A/A null: **a token that costs nothing to obtain removes ~78 % of the
 * index operation's cost net of the control, and a GRAPH-ONLY re-key buys nothing at
 * all** — `within-null`
 * at nine of ten cells, and its footprint goes UP. The reason is structural: the peer
 * NAME is materialized per frame into a stack scratch buffer, so something must map that
 * name to whatever the index keys on unless a token is minted upstream and carried.
 *
 * So the token is carried (`subscribe_wire`'s `link_token`), and this is what it
 * addresses. One structure, not two: the name lives INSIDE the slot it belongs to, which
 * is what makes the carry pay in bytes as well as in nanoseconds. A dense vector PLUS a
 * name→token map is byte-for-byte the shape #1416 measured at 183.8 B/link against the
 * map's 175.8 — i.e. worse — and this shape measures **128.0 B/link**.
 *
 * MEASURED on the same harness, with the shipped arm held to the calibration oracle: the
 * index operation goes **14.1–17.2 ns → 9.4 ns, FLAT in link count** at 8 vertices
 * (15.5–18.4 → 10.4 at 32), which is **42–56 % of its cost net of the control at every
 * one of the ten cells**, and its footprint **175.8 → 128.0 B per link**. The gap to the
 * free-token ceiling is the indirect supplier call, its tier branch, and the name compare
 * in @ref index_vertex.
 *
 * The NAME door survives unchanged, which is #1366's first objection answered rather
 * than waived: @ref candidate_count, @ref candidates and therefore `graph_t::evict_link_edges`
 * all still take a `std::string_view` and all still work with no token in sight (the ESP
 * departure-cost test calls one on a bare `graph_t` with no router at all). They pay a
 * LINEAR SCAN over live slots instead of a hash. `run_subscribe_index.sh` reports that
 * scan's cost every run (`RESULT_SIDX_DOOR`) rather than leaving the half of the trade
 * that got slower unstated: it is CHEAPER than the hash up to about 32 live links and
 * about 2x dearer at 65.
 *
 * **Which paths pay it, stated correctly (#1437).** The argument this block used to make
 * — "a path that runs once per peer hangup where the one it pays for runs once per
 * subscribe" — is true of the three doors above and was NOT true of the index insert
 * itself. @ref index_vertex falls back to the same scan whenever the carry misses, and
 * the carry misses on doors that run per SUBSCRIBE: a host-side `subscribe_toward`, a
 * mount-routed target whose key is the mount's name and not the arrival's (RFC-0021
 * §4.B.1), and the `field_write` `caller` fallback (#943). The trade as documented was
 * narrower than the trade as shipped, which is the whole of #1437.
 *
 * The answer is to CLOSE those doors, not to speed the scan up: a name→token map is the
 * shape #1416 already measured at 183.8 B/link against the 128.0 this one costs, and a
 * per-slot digest would spend the C6's arena on a WIDE-side latency point. So the two
 * doors with somewhere to keep a word now keep one — @ref intern_hinted, whose slot
 * hint the transport plane parks on the registered mount it already resolved — and the
 * genuinely token-less doors keep the scan, which is the trade the paragraph above
 * argues and can now be read as arguing.
 *
 * @section link_index_locking Locking
 *
 * Every public member takes the index's own mutex, which guards the slot vector, the free
 * list and the long names ONLY. It is a leaf: never held across a map, stripe, or sweep
 * acquisition, so it orders against nothing else in `graph_t`.
 */
class link_index_t {
   public:
    /** @brief An empty index whose tables, per-link lists and names all draw from @p src. */
    explicit link_index_t(mem::block_source_t& src) noexcept : slots_(src), long_names_(src) {}

    /** @brief Non-copyable — the graph holds its one index by value. */
    link_index_t(const link_index_t&) = delete;
    /** @brief Non-assignable. */
    link_index_t& operator=(const link_index_t&) = delete;

    /**
     * @brief Mint-or-find @p name's interned token — `graph_t::intern_link`'s body.
     *
     * Idempotent by name: the same spelling always answers the same live token (#1263).
     * @return The token, or a default-constructed one for an empty name (#1056) or when the
     *         table source refused a new slot (#1778).
     */
    [[nodiscard]] link_id_t intern(std::string_view name);

    /**
     * @brief @ref intern with a caller-held slot hint — `graph_t::intern_link_hinted`'s body.
     *
     * A hint that does not name a live slot spelling @p name costs one compare and then the
     * scan; it can never produce a wrong token. Written back only on a valid token.
     */
    [[nodiscard]] link_id_t intern_hinted(std::string_view name, std::uint32_t& hint);

    /**
     * @brief Retire @p token's slot so it can be reused — `graph_t::release_link`'s body.
     *
     * Invalid, out-of-range, stale and already-released tokens are no-ops.
     */
    void release(link_id_t token);

    /**
     * @brief Record that @p v may hold an edge admitted over @p link (#1071).
     *
     * An empty @p link is a no-op: the LOCAL spelling is not reachable by any link teardown
     * (#1056). Idempotent at the insert (#1266). @p token is the CARRIED identity (#1417):
     * used only when it names a live slot that spells @p link, otherwise @p link is interned
     * by name and the fallback is counted in @ref name_lookups.
     * @retval false The table source refused the entry: @p v is NOT indexed, so the caller
     *               must refuse the admission (a live unindexed edge would outlive its link).
     */
    [[nodiscard]] bool index_vertex(std::string_view link, link_id_t token, vertex_t* v);

    /**
     * @brief The DISTINCT candidate count for @p link — `graph_t::link_edge_candidates`'s body.
     *
     * Compacts the entry first, so the number is not an artefact of where the amortized
     * compaction last landed. Empty name or unknown link ⇒ 0.
     */
    [[nodiscard]] std::size_t candidate_count(std::string_view link) const;

    /**
     * @brief The candidate vertices for @p link — the index entry, or empty.
     *
     * @param take When true the entry is REMOVED and its slot released, transferring its
     *             vector to the caller: what a whole-link eviction wants, since every one of
     *             that link's edges is about to be gone and the entry would otherwise be a
     *             permanent stale list. The route-scoped sibling passes false — it reclaims
     *             only SOME of the link's edges, so the entry must survive for the link's
     *             eventual teardown. The copy is drawn from the table source; when it is
     *             refused the answer is empty, and the route-scoped eviction reclaims
     *             nothing until the link's own teardown takes the entry whole.
     */
    [[nodiscard]] mem::block_array_t<vertex_t*> candidates(std::string_view link, bool take);

    /**
     * @brief How many index inserts fell back to a NAME LOOKUP —
     *        `graph_t::link_index_name_lookups`'s body.
     */
    [[nodiscard]] std::size_t name_lookups() const;

   private:
    /** @brief One link's candidate list, plus where its sorted prefix ends. */
    struct link_entry_t {
        mem::block_array_t<vertex_t*> vs; /**< @brief The link's DISTINCT candidate vertices:
                                           *          `[0, compacted)` sorted, then an unsorted
                                           *          tail (@ref index_vertex). */
        std::size_t compacted = 0;        /**< @brief Where the sorted prefix ends — `vs.size()`
                                           *          as of the last compaction. On a DEAD
                                           *          slot, the next free slot (`free_head_`). */
    };
    /** @brief How long a link's UNSORTED tail may get before it is merged into the sorted
     *         prefix — so the membership test's linear half stays a handful of pointers and
     *         the sort is paid per NEW vertex, never per subscribe. */
    static constexpr std::size_t kCompactFloor = 8;

    /** @brief How many name characters live INSIDE a slot before it overflows to
     *         `long_names_`.
     *
     * Chosen so the whole slot is 64 bytes — one cache line, and the difference between a
     * measured 128.0 B/link and the 160.8 B a `std::pmr::string` here would cost (that type
     * is 40 bytes of header before a single character is stored, because the polymorphic
     * allocator is not empty). It covers every name the transport plane mints — `p<slot>`,
     * `n<node>`, the `tr::net::kPeerNameChars` tokens — and every dotted-quad `host:port`.
     * A longer registered child name still works; it simply costs one entry in the overflow
     * list and one extra indirection on the doors that scan by name. */
    static constexpr std::size_t kInlineNameChars = 19;
    /** @brief `link_slot_t::len` sentinel for a name held in `long_names_`. */
    static constexpr std::uint8_t kOverflowNameLen = 0xFF;

    /**
     * @brief One link's whole index record: its candidate list AND its name, stored ONCE.
     *
     * A dead slot has `len == 0`, which no live link can have — @ref index_vertex and
     * @ref intern both refuse the empty spelling (the #1056 empty-key rule) — so a name
     * scan skips dead slots without a second predicate.
     */
    struct link_slot_t {
        link_entry_t e;                   /**< @brief The candidate list. */
        std::uint32_t generation = 0;     /**< @brief Bumped on release; `0` ⇒ never minted. */
        std::uint8_t len = 0;             /**< @brief Inline name length; `0` ⇒ DEAD,
                                           *          `kOverflowNameLen` ⇒ overflowed. */
        char name[kInlineNameChars] = {}; /**< @brief The ADMITTED-OVER spelling. */
    };
    // The property worth pinning is that the name rides INSIDE the slot for free — no padding
    // beyond the stamp and the length byte — NOT the literal 64, which is a 64-bit host's
    // arithmetic. On the 32-bit RISC-V C6 this same expression is 44 bytes, because
    // `link_entry_t` is 20 there and not 40. Spelling the assert `== 64` broke the ESP build
    // outright, which would have been an odd way to ship the change whose whole justification
    // is the C6's user-pinned arena.
    static_assert(sizeof(link_slot_t) ==
                      sizeof(link_entry_t) + sizeof(std::uint32_t) + 1 + kInlineNameChars,
                  "the index slot carries its name inline and adds no padding for it — 64 bytes, "
                  "one cache line, on a 64-bit host; see kInlineNameChars");

    /** @brief A link name too long for `link_slot_t` — rare, so a scanned list beats a
     *         second hashed container, and it costs nothing at all while it is empty. */
    struct link_long_name_t {
        std::uint32_t slot; /**< @brief Which slot it belongs to. */
        mem::string_t text; /**< @brief The spelling. */
    };

    /** @brief "No such slot" — what a name scan answers when nothing matches. */
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

    // The five private doors below all run with `mutex_` HELD.
    /** @brief Slot @p i's spelling — inline, overflowed, or empty for a dead slot. */
    [[nodiscard]] std::string_view slot_name(std::uint32_t i) const;
    /** @brief The live slot holding @p name, or `kNoSlot`. */
    [[nodiscard]] std::uint32_t find_slot(std::string_view name) const;
    /** @brief Give slot @p i the spelling @p name, inline when it fits. */
    [[nodiscard]] bool name_slot(std::uint32_t i, std::string_view name);
    /** @brief @ref intern with the lock already held. */
    [[nodiscard]] link_id_t intern_locked(std::string_view name);
    /** @brief Retire slot @p i: drop its name, bump its stamp, free its list, list it for
     *         reuse. The shared body of @ref release and the take-arm of @ref candidates. */
    void release_slot(std::uint32_t i);

    // `mutable` because the entries are a CACHE of where a link's edges may be: the
    // diagnostic reader compacts one in place to report a distinct count, which changes no
    // observable graph state. Guarded by the mutable mutex below.
    /** @brief The dense slot vector, addressed by `link_id_t::slot`. */
    mutable mem::block_array_t<link_slot_t> slots_;
    /** @brief The first released slot awaiting reuse, or `kNoSlot` — what keeps a churning
     *         node's slot space bounded by its CONCURRENT link count instead of by its
     *         lifetime's. The list is INTRUSIVE: a dead slot's `e.compacted` holds the next
     *         one, so releasing a slot draws nothing and cannot be refused (#1778). */
    std::uint32_t free_head_ = kNoSlot;
    /** @brief Names past `kInlineNameChars`. Empty on every shipped deployment. */
    mem::block_array_t<link_long_name_t> long_names_;
    /** @brief @ref name_lookups's counter. A plain word, not an atomic: every read and write
     *         of it is already inside `mutex_`. */
    std::size_t name_lookups_ = 0;
    /** @brief Guards `slots_`, `free_head_` and `long_names_` ONLY. A leaf: never held across a
     *         map, stripe, or sweep acquisition, so it orders against nothing else. */
    mutable std::mutex mutex_;
};

}  // namespace tr::graph
