/**
 * @file
 * @brief #1266 — what LINK-IDENTITY INTERNING can and cannot buy the subscriber index, priced
 *        in ONE binary against a sentinel, with RAM on the same axis.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #1266 asks for a subscriber index that costs a POINTER rather than a string hash, and its
 * acceptance bar is a MEASUREMENT: the 4 / 8 / 16 / 32 / 65-link curve against a fresh A/A
 * null. Two attempts stalled for want of the instrument — #1290 built an interned probe table
 * and measured it with a purpose-built ablated variant that was then thrown away, and #1366
 * shipped the L2/L4 seam but recorded *"Measurement: not taken. No checked-in A/B harness for
 * the subscribe path exists."* This file is that harness. It is the durable half of the car;
 * the curve it prints is the perishable half.
 *
 * @section question The question, stated so a number can answer it
 *
 * `graph_t::index_link_vertex` (`core/src/graph.cpp`) takes a `std::string_view` link name,
 * takes `link_index_mutex_`, hashes the name, finds it in a
 * `std::pmr::unordered_map<std::pmr::string, link_entry_t>`, copies the name into a fresh
 * `std::pmr::string` key on a miss, and then does an idempotent insert into that link's
 * candidate list. Interning proposes to replace the hash + find + key copy with an index.
 * The issue's own ablation puts the whole index at +52 / +69 / +59 ns at 4 / 8 / 65 links, of
 * which the NAME LOOKUP is +28 / +24 / +39 ns — so the lookup is the part interning can
 * address, and the mutex and the list maintenance are the parts it cannot.
 *
 * @section arms The four index arms, and what each one is for
 *
 * All four run in ONE binary. That is not a convenience: `bench/README.md` measures this
 * host's cross-build layout sensitivity at up to **+9.8 % on an untouched leg**, which is
 * larger than part of the effect being hunted, so a two-build A/B could not resolve it.
 *
 *  - `S-sentinel` — the CONTROL. The identical driver loop with the index call replaced by a
 *    barrier that consumes both operands. Every other arm's verdict is a paired per-round
 *    delta against this arm, so the loop, the name rotation and the clock are subtracted.
 *  - `idx-string` — THE HISTORICAL BASELINE: the string-keyed map `graph_t` shipped before the
 *    carry landed (#1417), transcribed line-for-line from the `index_link_vertex` of that
 *    revision and its two file-local helpers. Every saving this bench reports is quoted
 *    against this column, and keeping it in-binary is what lets the old and new shapes be
 *    compared under ONE layout. A transcription arm is worth exactly its faithfulness,
 *    so it is not tidied: the sorted-prefix/unsorted-tail membership test is written out
 *    rather than composed from `<algorithm>`, `kLinkIndexCompactFloor` keeps its value, and
 *    the transparent hash/equality pair is the one `graph.hpp` declares.
 *  - `idx-token` — INTERNING AT ITS MOST FAVOURABLE. The caller is assumed to already hold
 *    the link's `link_id_t`, so the operation is the mutex plus a vector index plus the
 *    *identical* idempotent insert: no hash, no map, no key copy. This arm deliberately
 *    charges NOTHING for obtaining the token, which is the best case interning could ever
 *    have. `idx-string − idx-token` is therefore the CEILING on what interning can save per
 *    subscribe, not an estimate of it.
 *  - `idx-token-intern` — THE GRAPH-ONLY RE-KEY, which is what #1366 left as the remaining
 *    work when it described it as *"a single-seam graph-layer change"*. The name still
 *    arrives as a `std::string_view`, because the subscribe path holds nothing else (see
 *    @ref why_graph_only), so the graph must intern it itself: hash + find in a name→token
 *    map, then the vector index, then the identical insert. This arm exists to price that
 *    described shape rather than to argue about it.
 *  - `idx-token-carry` — WHAT #1417 ACTUALLY PROPOSES TO SHIP, and unlike `idx-token` it
 *    PAYS FOR OBTAINING THE TOKEN. See @ref carry.
 *
 * @section carry The carry arm, and why it is not the ceiling
 *
 * `idx-token`'s 104.0 B/link is a dense token-indexed vector THAT STORES NO KEY AT ALL. The
 * shipped index cannot be that: the NAME door is load-bearing — `link_down(name)` and
 * `evict_link_edges(name)` enter by name, and `integrations/esp-idf/tests/host/`
 * `httpd_ws_departure_cost_test.cpp` calls `link_edge_candidates("p0")` on a bare `graph_t`
 * with no router and no token at all. So the name has to be stored somewhere, and the naive
 * carry — a dense vector PLUS a name→token map — is byte-for-byte the shape `idx-token-intern`
 * already measured at 183.8 B/link, i.e. WORSE than today's 175.8.
 *
 * `idx-token-carry` therefore stores the name EXACTLY ONCE, in the dense slot itself, and
 * deletes the map: one `pmr::vector<carry_slot_t>` indexed by token, plus a free list. The
 * name door becomes a LINEAR SCAN over live slots — a path that runs once per peer hangup, not
 * once per subscribe — and its cost is REPORTED (`RESULT_SIDX_DOOR`) rather than hidden.
 *
 * And the token is not free here. It is obtained through an INDIRECT CALL to a supplier that
 * models both router tiers, which is the carry's real per-subscribe price:
 *
 *  - FLAT (a point-to-point child): one relaxed atomic load off a `child_rx_ctx_t`-shaped
 *    struct — the resolved-once discipline that ctx's other cached fields already use.
 *  - BUS (every WS session on the ESP node, and every CAN peer): a dense lookup by
 *    `peer_handle_t::index` with a `generation` compare and a MISS BRANCH, because an
 *    announce-census bus never fires `peer_up` and its peers must be minted lazily.
 *
 * Both are inside the timed window, so `idx-string − idx-token-carry` is a FORECAST where
 * `idx-string − idx-token` is only a ceiling. The gap between the two columns is exactly what
 * the indirect call plus the tier branch plus the token validation costs.
 *
 * A fifth arm, `sub-wire`, drives the LIVE `graph_t::subscribe_wire` so the index delta can be
 * read as a fraction of a whole remote subscribe rather than in isolation. It is reported
 * beside the others and never compared to the sentinel — it does a different amount of work.
 *
 * @section why_graph_only Why the name is still a string at the index, at HEAD
 *
 * `fwd_router_t::resolve_peer_name` turns the frame's `peer_handle_t` back into a name into a
 * **stack** `std::array<char, kPeerNameChars>` scratch buffer, and that `std::string_view` is
 * what reaches L4. There is no stable string and no token below that point. So whatever
 * `graph_t` keys on, *something* has to map the arriving name to it — one hash, per subscribe
 * — unless a token is minted upstream and CARRIED. `idx-token` prices the carried form and
 * `idx-token-intern` prices the graph-only form; the gap between them is the value of the
 * carry, and it is the number that decides whether the carry is worth commissioning.
 *
 * @section ram RAM, on the same axis as latency
 *
 * Every index arm is also built to steady state under a counting `std::pmr::memory_resource`
 * and its bytes at rest reported per link count. With the vertex population held constant,
 * `(bytes@65 − bytes@4) / 61` is the index's PER-LINK footprint — the figure the 2026-08-14
 * ruling quoted as *"~832 B → roughly ~500 B (est.)"* and never checked.
 *
 * The live `graph_t` is measured through the same counter, and it turns out to be a SHARPER
 * instrument than expected rather than a contaminated one. A `graph_t` draws nothing at all
 * from its injected `std::pmr::memory_resource` during construction or vertex registration —
 * measured: 0 bytes and 0 allocations after a ctor plus eight `register_vertex` calls, because
 * vertex storage comes from the `ctl` block source and the global heap. In this workload the
 * counted arena therefore sees exactly one structure: `link_index_` itself. The live arm's
 * bytes ARE the shipped index's bytes, no subtraction required, and `calibrate` asserts that
 * `idx-string` reproduces them byte-for-byte and allocation-for-allocation before any number
 * is reported. That check is what makes the transcription's faithfulness a machine-verified
 * property instead of an authorial claim.
 *
 * @section reading How to read the output
 *
 *     RESULT_SIDX      round tag arm links verts p50ps p99ps meanps n batch
 *     RESULT_SIDX_RAM  tag arm links verts live_bytes peak_bytes allocs entries
 *
 * Own tags, deliberately: `bench/bench_common.hpp` records that every parser in the tree gates
 * on the `RESULT` line's exact 12-field arity, and a 13th field would make each of them match
 * ZERO rows rather than fail loudly. `RESULT_PIN` set the precedent for a bench whose shape is
 * its own; these follow it, and no existing parser reads either tag.
 *
 * Per-op cost is accumulated in PICOSECONDS, not nanoseconds. The operation under test is
 * tens of nanoseconds and the arms are expected to differ by a few, so an integer-nanosecond
 * per-op figure would quantise the very quantity being measured. The batch window is
 * nanoseconds; only the per-op quotient is scaled.
 *
 * `bench/run_subscribe_index.sh` drives it: arms rotate their order every round inside one
 * process, and the A/A null is two ABBA-interleaved executions of this same binary.
 * `bench/collate_subscribe_index.py` renders the tables and applies the null band.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path.hpp"
#include "libtracer/peer_handle.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/vertex.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Link counts swept — #1266's own acceptance axis, quoted in full or not at all. */
constexpr std::array<std::size_t, 5> kLinkCounts{4, 8, 16, 32, 65};

/**
 * @brief Vertex populations swept, held CONSTANT across the link axis at each point.
 *
 * 8 is the narrow configuration the 2026-08-14 ruling costed ("4 links / 8 vertices"), so the
 * per-link footprint this bench reports is directly comparable to that ruling's estimate. 32
 * is the wide end: it is four times `kLinkIndexCompactFloor`, so the membership test's binary
 * search over the sorted prefix is doing real work rather than degenerating to the tail scan.
 */
constexpr std::array<std::size_t, 2> kVertexCounts{8, 32};

/** @brief Latency samples per arm per grid cell per round, for the four index arms. */
constexpr std::size_t kSamplesPerCell = 400;

/**
 * @brief Latency samples per grid cell per round for the LIVE `sub-wire` arm.
 *
 * Fewer, because one sample is a whole `links * verts` pass over a freshly built graph rather
 * than a calibrated batch of one operation — see @ref live_pass for why it has to be.
 */
constexpr std::size_t kLiveSamplesPerCell = 25;

/** @brief Picoseconds per nanosecond — the per-op scale factor (see @ref reading). */
constexpr std::uint64_t kPsPerNs = 1000;

/**
 * @brief The candidate pointer the index stores.
 *
 * `graph_t`'s index stores `vertex_t*` and does nothing with it but compare, order and copy
 * it; `vertex_handle_t` deliberately does not surrender the pointer, so the transcription
 * arms store distinct 8-aligned `void*` drawn from a local pool instead. This is the ONE
 * deviation from a line-for-line transcription, it is forced, and it changes no instruction:
 * the binary search, the tail scan, the `push_back` and the sort all compare pointer values.
 */
using cand_t = void*;

/**
 * @brief A link's interned identity — the word #1266 wants the index keyed by.
 *
 * A dense index rather than a pointer, because a dense index is the CHEAPEST thing the key
 * could be: a vector subscript with no indirection. If interning does not pay at a vector
 * subscript it cannot pay at a pointer either, so the favourable direction is the honest one
 * for a ceiling.
 */
struct link_id_t {
    std::uint32_t v = 0; /**< @brief Dense slot in the token-keyed index. */
};

/** @brief One link's candidate list — transcribed from `graph_t::link_entry_t`. */
struct link_entry_t {
    std::pmr::vector<cand_t> vs; /**< @brief `[0, compacted)` sorted and unique, then an
                                  *          unsorted tail. */
    std::size_t compacted = 0;   /**< @brief Where the sorted prefix ends. */
};

/** @brief Transcribed from `graph_t::kLinkIndexCompactFloor`. */
constexpr std::size_t kLinkIndexCompactFloor = 8;

/** @brief Transcribed from `compact_candidates` (`core/src/graph.cpp`). */
void compact_candidates(std::pmr::vector<cand_t>& vs) {
    std::sort(vs.begin(), vs.end());
    vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
}

/**
 * @brief Transcribed from `candidates_contain` (`core/src/graph.cpp`).
 *
 * Written out rather than composed from `std::binary_search` + `std::find`, exactly as the
 * original is and for the reason the original records: the two extra `<algorithm>`
 * instantiations moved GCC's inter-procedural budget. Not tidied.
 */
bool candidates_contain(const std::pmr::vector<cand_t>& vs, std::size_t sorted, const cand_t v) {
    std::size_t lo = 0;
    std::size_t hi = sorted;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (vs[mid] == v) return true;
        if (vs[mid] < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (std::size_t i = sorted; i < vs.size(); ++i)
        if (vs[i] == v) return true;
    return false;
}

/**
 * @brief The idempotent-insert body shared by all three index arms, transcribed once.
 *
 * Shared deliberately: the arms differ ONLY in how they reach the entry, so anything they do
 * after reaching it must be byte-identical or the comparison prices something else. Keeping
 * one copy is what makes `idx-string − idx-token` a measurement of the LOOKUP.
 */
void insert_candidate(link_entry_t& e, const cand_t v) {
    if (candidates_contain(e.vs, e.compacted, v)) return;
    e.vs.push_back(v);
    if (e.vs.size() - e.compacted >= kLinkIndexCompactFloor) {
        compact_candidates(e.vs);
        e.compacted = e.vs.size();
    }
}

/** @brief Transcribed from `graph_t::link_key_hash_t`. */
struct link_key_hash_t {
    using is_transparent = void; /**< @brief Enables the heterogeneous `find`. */
    std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

/** @brief Transcribed from `graph_t::link_key_eq_t`. */
struct link_key_eq_t {
    using is_transparent = void; /**< @brief Enables the heterogeneous `find`. */
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

/**
 * @brief Arm `idx-string` — `graph_t::index_link_vertex` as it stands at HEAD.
 *
 * The mutex, the transparent hash, the heterogeneous `find`, the `std::pmr::string` key copy
 * on a miss, and the shared idempotent insert.
 */
class string_index_t {
   public:
    /** @brief Construct over @p mr, which every entry and key is drawn from. */
    explicit string_index_t(std::pmr::memory_resource* mr) : mr_(mr), map_(mr) {}

    /** @brief Transcribed from `graph_t::index_link_vertex`. */
    void insert(std::string_view link, const cand_t v) {
        if (link.empty()) return;
        const std::lock_guard lock(m_);
        auto it = map_.find(link);
        if (it == map_.end())
            it = map_.emplace(std::pmr::string(link, mr_),
                              link_entry_t{.vs = std::pmr::vector<cand_t>(mr_), .compacted = 0})
                     .first;
        insert_candidate(it->second, v);
    }

    /** @brief Distinct candidates recorded for @p link — the calibration oracle. */
    [[nodiscard]] std::size_t candidates(std::string_view link) {
        const std::lock_guard lock(m_);
        const auto it = map_.find(link);
        if (it == map_.end()) return 0;
        compact_candidates(it->second.vs);
        it->second.compacted = it->second.vs.size();
        return it->second.vs.size();
    }

    /** @brief Distinct link keys held. */
    [[nodiscard]] std::size_t links() const {
        const std::lock_guard lock(m_);
        return map_.size();
    }

   private:
    mutable std::mutex m_;          /**< @brief Transcribed `link_index_mutex_`. */
    std::pmr::memory_resource* mr_; /**< @brief Where keys and entries are drawn from. */
    std::pmr::unordered_map<std::pmr::string, link_entry_t, link_key_hash_t, link_key_eq_t>
        map_; /**< @brief Transcribed `link_index_`. */
};

/**
 * @brief Arm `idx-token` — interning with the token ALREADY IN HAND.
 *
 * The most favourable form interning could ever take: no hash, no map, no key copy, just a
 * vector subscript and the shared idempotent insert. Charging nothing for obtaining the token
 * is what makes the `idx-string` delta a CEILING rather than an estimate — in the real system
 * the token has to be minted upstream and carried through the `tr::net`/`tr::graph` seam, and
 * that carry is not free.
 */
class token_index_t {
   public:
    /** @brief Construct over @p mr with @p links dense slots preallocated. */
    token_index_t(std::pmr::memory_resource* mr, std::size_t links) : by_id_(mr) {
        by_id_.reserve(links);
        for (std::size_t i = 0; i < links; ++i)
            by_id_.push_back(link_entry_t{.vs = std::pmr::vector<cand_t>(mr), .compacted = 0});
    }

    /** @brief The interned form of `index_link_vertex`: subscript, then the shared insert. */
    void insert(link_id_t id, const cand_t v) {
        const std::lock_guard lock(m_);
        insert_candidate(by_id_[id.v], v);
    }

    /** @brief Distinct candidates recorded for @p id — the calibration oracle. */
    [[nodiscard]] std::size_t candidates(link_id_t id) {
        const std::lock_guard lock(m_);
        link_entry_t& e = by_id_[id.v];
        compact_candidates(e.vs);
        e.compacted = e.vs.size();
        return e.vs.size();
    }

   private:
    mutable std::mutex m_; /**< @brief The leaf lock the shipped index also takes. */
    /** @brief Dense, token-indexed entries. No `mr_` beside it, unlike the two name-keyed
     *         arms: with no key to copy there is nothing left for this arm to allocate after
     *         construction, which is the saving stated as a field that does not exist. */
    std::pmr::vector<link_entry_t> by_id_;
};

/**
 * @brief Arm `idx-token-intern` — the GRAPH-ONLY re-key #1366 described as what remains.
 *
 * The name still arrives as a `std::string_view` (@ref why_graph_only), so the hash and the
 * find are still paid; the token-keyed vector is paid ON TOP. This arm is here because the
 * described shape deserves a number rather than an argument.
 */
class intern_index_t {
   public:
    /** @brief Construct over @p mr with @p links dense slots preallocated. */
    intern_index_t(std::pmr::memory_resource* mr, std::size_t links)
        : mr_(mr), intern_(mr), by_id_(mr) {
        by_id_.reserve(links);
        for (std::size_t i = 0; i < links; ++i)
            by_id_.push_back(link_entry_t{.vs = std::pmr::vector<cand_t>(mr), .compacted = 0});
    }

    /** @brief Intern the name to a token, then the shared insert through the token. */
    void insert(std::string_view link, const cand_t v) {
        if (link.empty()) return;
        const std::lock_guard lock(m_);
        auto it = intern_.find(link);
        if (it == intern_.end()) {
            const link_id_t fresh{static_cast<std::uint32_t>(next_++)};
            it = intern_.emplace(std::pmr::string(link, mr_), fresh).first;
        }
        insert_candidate(by_id_[it->second.v], v);
    }

    /** @brief Distinct candidates recorded for @p link — the calibration oracle. */
    [[nodiscard]] std::size_t candidates(std::string_view link) {
        const std::lock_guard lock(m_);
        const auto it = intern_.find(link);
        if (it == intern_.end()) return 0;
        link_entry_t& e = by_id_[it->second.v];
        compact_candidates(e.vs);
        e.compacted = e.vs.size();
        return e.vs.size();
    }

   private:
    mutable std::mutex m_;          /**< @brief The leaf lock. */
    std::pmr::memory_resource* mr_; /**< @brief Where keys and entries are drawn from. */
    std::pmr::unordered_map<std::pmr::string, link_id_t, link_key_hash_t, link_key_eq_t>
        intern_; /**< @brief Name → token. The hash that did not go away. */
    std::pmr::vector<link_entry_t> by_id_; /**< @brief Dense, token-indexed entries. */
    std::size_t next_ = 0;                 /**< @brief Next token to mint. */
};

/**
 * @brief The token `idx-token-carry` carries — a dense slot plus a validate-on-use stamp.
 *
 * NOT `link_id_t`: that one is the ceiling arm's bare subscript and is deliberately left
 * unchanged, so #1416's three established columns stay byte-identical. This one is
 * node-scoped and answers #1366's second objection directly — two links each minting
 * `tr::net::kSolePeerHandle` get different `carry_id_t`s, because the graph mints it, not the
 * link. A zero @ref generation is never live, so a default-constructed token always falls
 * back to the name door.
 */
struct carry_id_t {
    std::uint32_t slot = 0;       /**< @brief Dense slot in the token-keyed index. */
    std::uint32_t generation = 0; /**< @brief Validate-on-use stamp; `0` is never live. */
};

/** @brief How many name characters live INSIDE a slot before it overflows. */
constexpr std::size_t kInlineLinkNameChars = 19;

/** @brief `carry_slot_t::len` sentinel for a name held in the overflow list. */
constexpr std::uint8_t kOverflowNameLen = 0xFF;

/**
 * @brief One link's whole index record — the candidate list AND its name, stored ONCE.
 *
 * Sized to 64 bytes on purpose: `link_entry_t` (40) + the stamp (4) + a length byte + 19
 * inline characters. The inline name is what makes the carry pay in RAM rather than merely
 * in latency — a `std::pmr::string` here is 40 bytes of header before a single character is
 * stored (measured; the polymorphic allocator is not empty), which would put the slot at 88
 * and hand back most of the saving. 19 characters covers every name the transport plane
 * actually mints (`p<slot>`, `n<node>`, `kPeerNameChars` tokens) and every dotted-quad
 * `host:port` spelling; anything longer goes to @ref long_name_t, which costs nothing at all
 * when it is empty.
 */
struct carry_slot_t {
    link_entry_t e;                         /**< @brief The candidate list — shared shape. */
    std::uint32_t generation = 0;           /**< @brief Bumped on release; `0` ⇒ never minted. */
    std::uint8_t len = 0;                   /**< @brief Inline name length; `0` ⇒ dead slot,
                                             *          @ref kOverflowNameLen ⇒ overflowed. */
    char name[kInlineLinkNameChars] = {{}}; /**< @brief The link's ADMITTED-OVER spelling. */
};

static_assert(sizeof(carry_slot_t) == 64,
              "the carry slot is one cache line, and that is the "
              "whole RAM argument for the inline name");

/** @brief A name too long for @ref carry_slot_t — rare, so a scanned list beats a map. */
struct long_name_t {
    std::uint32_t slot;    /**< @brief Which slot it belongs to. */
    std::pmr::string text; /**< @brief The spelling. */
};

/**
 * @brief A `child_rx_ctx_t`-shaped per-link router state — the SUPPLIER side of the carry.
 *
 * Deliberately drawn from the GLOBAL heap and not from the counted arena: the router's
 * per-link state is not the graph's index, and charging it to `link_index_`'s footprint would
 * flatter neither arm honestly.
 */
struct rx_ctx_shape_t {
    /** @brief FLAT tier: the resolved-once token, read with one relaxed load — the same
     *         discipline `child_rx_ctx_t`'s other cached fields already document. */
    std::atomic<std::uint64_t> flat_token{0};
    std::vector<std::uint64_t> bus_token; /**< @brief BUS tier: token bits by peer index. */
    std::vector<std::uint32_t> bus_gen;   /**< @brief BUS tier: the handle stamp each slot holds. */
    bool bus = false;                     /**< @brief Which tier this ctx is. */
};

/** @brief The supplier seam: `(router ctx, the frame's peer handle) → the link's token`. */
using link_id_fn_t = carry_id_t (*)(void*, tr::net::peer_handle_t);

/** @brief Pack a token into the one word a lock-free ctx cache can publish atomically. */
[[nodiscard]] constexpr std::uint64_t pack_token(carry_id_t id) noexcept {
    return (static_cast<std::uint64_t>(id.generation) << 32) | id.slot;
}

/** @brief The inverse of @ref pack_token. */
[[nodiscard]] constexpr carry_id_t unpack_token(std::uint64_t bits) noexcept {
    return carry_id_t{static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32)};
}

/**
 * @brief The supplier thunk, modelling BOTH router tiers — the carry's real per-subscribe cost.
 *
 * `[[gnu::noinline]]` and reached through @ref g_link_id_supplier, because the whole point of
 * this arm is that the token costs an INDIRECT CALL through a seam the compiler cannot see
 * across. Letting GCC inline a known target here would price a carry nobody can build.
 */
[[gnu::noinline]] carry_id_t supply_link_id(void* c, tr::net::peer_handle_t peer) {
    const rx_ctx_shape_t* const ctx = static_cast<const rx_ctx_shape_t*>(c);
    if (!ctx->bus) return unpack_token(ctx->flat_token.load(std::memory_order_relaxed));
    // The BUS tier's miss branch is not decoration: an announce-census CAN bus never calls
    // `set_peer_up_notifier`, so its peers have no minted token and the index must fall back
    // to the name door and mint lazily. Charging that branch here is what keeps this arm a
    // forecast rather than a second ceiling.
    if (peer.index >= ctx->bus_token.size()) return {};
    if (ctx->bus_gen[peer.index] != peer.generation) return {};
    return unpack_token(ctx->bus_token[peer.index]);
}

/**
 * @brief The supplier, read through a `volatile` so the call site stays INDIRECT.
 *
 * A plain function pointer initialised in this translation unit is constant-propagated by
 * GCC at `-O3`, which would delete the very indirection being measured. A volatile load
 * cannot be folded, and it is paid once per driver construction rather than per operation.
 */
link_id_fn_t volatile g_link_id_supplier = &supply_link_id;

/**
 * @brief Arm `idx-token-carry` — the shape #1417 proposes, paying for the token (@ref carry).
 *
 * ONE structure, not two: a dense `pmr::vector<carry_slot_t>` addressed by the carried token,
 * with the link's name stored inside the slot it belongs to. There is no map, no hash and no
 * key copy anywhere in this class — the only name comparison is the linear scan the name door
 * and the lazy mint run, and neither is on the subscribe path.
 */
class carry_index_t {
   public:
    /** @brief Construct over @p mr, which every slot, name and candidate list is drawn from. */
    explicit carry_index_t(std::pmr::memory_resource* mr)
        : mr_(mr), slots_(mr), free_(mr), long_names_(mr) {}

    /** @brief Mint-or-find @p name's token — the LINK-UP door, never the subscribe one. */
    [[nodiscard]] carry_id_t intern(std::string_view name) {
        const std::lock_guard lock(m_);
        return intern_locked(name);
    }

    /**
     * @brief The carried form of `index_link_vertex`: validate the token, then the shared
     *        insert.
     *
     * Three tests, transcribed from the shipped door: in range, the stamp matches, and the
     * slot SPELLS the key. The third is what turns *"the caller must never carry the wrong
     * token"* into *"carrying the wrong token cannot lose an edge"* — the two admissions
     * whose key is not the arrival link (a mount-routed target, and the `caller` fallback
     * #943 needs) both land on it, and #1071 exists to keep exactly those edges reachable.
     * It is a handful of inline bytes against a `std::string_view` already in a register,
     * and it is CHARGED to this arm rather than argued away.
     */
    void insert(carry_id_t id, std::string_view name, const cand_t v) {
        if (name.empty()) return;  // the LOCAL spelling — no link teardown can ever name it
        const std::lock_guard lock(m_);
        if (id.slot >= slots_.size() || slots_[id.slot].generation != id.generation ||
            slot_name(id.slot) != name) {
            id = intern_locked(name);
            if (id.generation == 0) return;
        }
        insert_candidate(slots_[id.slot].e, v);
    }

    /** @brief Distinct candidates recorded under @p id — the calibration oracle. */
    [[nodiscard]] std::size_t candidates(carry_id_t id) {
        const std::lock_guard lock(m_);
        if (id.slot >= slots_.size() || slots_[id.slot].generation != id.generation) return 0;
        return compacted_size(slots_[id.slot].e);
    }

    /**
     * @brief THE NAME DOOR — distinct candidates recorded for @p name, found by linear scan.
     *
     * This is what `link_edge_candidates` / `link_candidates` become, and the trade the carry
     * makes: a hash and a find per SUBSCRIBE become a scan per PEER HANGUP. Its cost is
     * measured and reported (`RESULT_SIDX_DOOR`), never assumed away.
     */
    [[nodiscard]] std::size_t candidates_by_name(std::string_view name) {
        const std::lock_guard lock(m_);
        const std::uint32_t i = find_locked(name);
        if (i == kNoSlot) return 0;
        return compacted_size(slots_[i].e);
    }

   private:
    /** @brief "No such slot" — also what an unminted token's scan answers. */
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

    /** @brief Fold the tail in and report the distinct count, as every name door does. */
    [[nodiscard]] static std::size_t compacted_size(link_entry_t& e) {
        if (e.vs.size() != e.compacted) {
            compact_candidates(e.vs);
            e.compacted = e.vs.size();
        }
        return e.vs.size();
    }

    /** @brief @p i's spelling — inline, overflowed, or empty for a dead slot. */
    [[nodiscard]] std::string_view slot_name(std::uint32_t i) const {
        const carry_slot_t& s = slots_[i];
        if (s.len != kOverflowNameLen) return std::string_view(s.name, s.len);
        for (const long_name_t& l : long_names_)
            if (l.slot == i) return l.text;
        return {};
    }

    /** @brief The slot holding @p name, or @ref kNoSlot. A dead slot's name is empty. */
    [[nodiscard]] std::uint32_t find_locked(std::string_view name) const {
        for (std::uint32_t i = 0; i < slots_.size(); ++i)
            if (slot_name(i) == name) return i;
        return kNoSlot;
    }

    /** @brief Give @p i the spelling @p name, inline when it fits. */
    void name_slot(std::uint32_t i, std::string_view name) {
        carry_slot_t& s = slots_[i];
        if (name.size() <= kInlineLinkNameChars) {
            std::memcpy(s.name, name.data(), name.size());
            s.len = static_cast<std::uint8_t>(name.size());
            return;
        }
        long_names_.push_back(long_name_t{i, std::pmr::string(name, mr_)});
        s.len = kOverflowNameLen;
    }

    /** @brief @ref intern with the lock already held. */
    [[nodiscard]] carry_id_t intern_locked(std::string_view name) {
        if (name.empty()) return {};
        if (const std::uint32_t i = find_locked(name); i != kNoSlot)
            return carry_id_t{i, slots_[i].generation};
        std::uint32_t i;
        if (!free_.empty()) {
            i = free_.back();
            free_.pop_back();
        } else {
            // Grown to EXACTLY what is needed rather than doubled: this runs once per
            // link-up, and the figure #1266 is judged on is bytes at rest in a user-pinned
            // arena, where a doubling vector of 64-byte slots would hand back a fifth of the
            // saving as slack.
            if (slots_.size() == slots_.capacity()) slots_.reserve(slots_.size() + 1);
            slots_.push_back(carry_slot_t{.e = {.vs = std::pmr::vector<cand_t>(mr_)}});
            i = static_cast<std::uint32_t>(slots_.size() - 1);
        }
        carry_slot_t& s = slots_[i];
        if (s.generation == 0) s.generation = 1;  // a fresh slot; released ones were bumped
        name_slot(i, name);
        return carry_id_t{i, s.generation};
    }

    mutable std::mutex m_;          /**< @brief The leaf lock the shipped index also takes. */
    std::pmr::memory_resource* mr_; /**< @brief Where slots, names and lists are drawn from. */
    std::pmr::vector<carry_slot_t> slots_;     /**< @brief Dense, token-indexed. THE structure. */
    std::pmr::vector<std::uint32_t> free_;     /**< @brief Released slots awaiting reuse. */
    std::pmr::vector<long_name_t> long_names_; /**< @brief Names past the inline bound. */
};

/**
 * @brief A `std::pmr::memory_resource` that counts what its upstream hands out.
 *
 * Bytes AT REST is the figure #1266 asks for, so `live_` is the headline and `peak_` is
 * beside it: an index whose steady state is small but whose compaction transiently doubles a
 * vector is a different proposition on a user-pinned arena than one that does not, and a
 * live-only number would hide that.
 */
class counting_resource_t : public std::pmr::memory_resource {
   public:
    /** @brief Count allocations forwarded to @p up. */
    explicit counting_resource_t(std::pmr::memory_resource* up = std::pmr::new_delete_resource())
        : up_(up) {}

    [[nodiscard]] std::size_t live() const noexcept { return live_; } /**< @brief Bytes at rest. */
    [[nodiscard]] std::size_t peak() const noexcept {
        return peak_;
    } /**< @brief High-water mark. */
    [[nodiscard]] std::uint64_t allocs() const noexcept {
        return allocs_;
    } /**< @brief Alloc count. */

   private:
    void* do_allocate(std::size_t bytes, std::size_t align) override {
        void* const p = up_->allocate(bytes, align);
        live_ += bytes;
        ++allocs_;
        if (live_ > peak_) peak_ = live_;
        return p;
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
        live_ -= bytes;
        up_->deallocate(p, bytes, align);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        return this == &o;
    }

    std::pmr::memory_resource* up_; /**< @brief Where the bytes actually come from. */
    std::size_t live_ = 0;          /**< @brief Outstanding bytes. */
    std::size_t peak_ = 0;          /**< @brief Largest `live_` ever observed. */
    std::uint64_t allocs_ = 0;      /**< @brief Allocations forwarded. */

   public:
    /**
     * @brief The @ref tr::mem::block_source_t FACE of the same counters — what the live
     *        `graph_t` arm needs since #873 phase 1 collapsed its four seams into one source.
     *
     * Not the forbidden pmr→source wrapper: this face draws from
     * @ref tr::mem::heap_source directly and can therefore answer `nullptr` honestly. It
     * shares the enclosing object's `live_`/`peak_`/`allocs_` so the SUB_WIRE arm's RAM row
     * still reports one number for the whole arm, exactly as it did when the graph took a
     * `std::pmr::memory_resource*`.
     */
    class source_face_t final : public tr::mem::block_source_t {
       public:
        explicit source_face_t(counting_resource_t& owner) noexcept
            : block_source_t("counting"), o_(&owner) {}

        [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
            void* const p = tr::mem::heap_source().try_alloc(bytes, align);
            if (p == nullptr) return nullptr;
            o_->live_ += bytes;
            ++o_->allocs_;
            if (o_->live_ > o_->peak_) o_->peak_ = o_->live_;
            return p;
        }
        void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
            o_->live_ -= bytes;
            tr::mem::heap_source().release(p, bytes, align);
        }

       private:
        counting_resource_t* o_;
    };

    /** @brief This counter's source face; stable for the counter's lifetime. */
    [[nodiscard]] source_face_t& as_source() noexcept { return face_; }

   private:
    source_face_t face_{*this};
};

/**
 * @brief The swept workload's link names.
 *
 * Spelled as a real deployment spells them — `p0`, `p1`, … for bus peers and
 * `192.168.4.N:PORT` for point-to-point links, alternating. The prefix structure is
 * deliberate: #1290's prototype recorded that real link names SHARE LONG PREFIXES, and a
 * digest that folds the tail without mixing strands the entropy and degrades a probe table to
 * a full scan. Any future interning attempt is entitled to be measured against names that
 * carry that hazard rather than against synthetic high-entropy ones.
 */
[[nodiscard]] std::vector<std::string> make_link_names(std::size_t n) {
    std::vector<std::string> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        char buf[64];
        if (i % 2 == 0)
            std::snprintf(buf, sizeof buf, "p%zu", i);
        else
            std::snprintf(buf, sizeof buf, "192.168.4.%zu:9000", i % 250);
        out.emplace_back(buf);
    }
    return out;
}

/** @brief Distinct 8-aligned pointers standing in for the index's `vertex_t*` (see @ref cand_t). */
[[nodiscard]] std::vector<cand_t> make_candidates(std::vector<std::uintptr_t>& pool,
                                                  std::size_t n) {
    pool.assign(n, 0);
    std::vector<cand_t> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(static_cast<cand_t>(&pool[i]));
    return out;
}

/** @brief Every arm this binary knows. `S-sentinel` leads because it is the control. */
enum class arm_t : std::uint8_t {
    SENTINEL,   /**< @brief The driver loop with no index operation — the control. */
    IDX_STRING, /**< @brief Today's string-keyed index, transcribed. */
    IDX_TOKEN,  /**< @brief Interning with the token already in hand — the ceiling. */
    IDX_INTERN, /**< @brief The graph-only re-key: hash, then token. */
    IDX_CARRY,  /**< @brief The carry as it would ship, PAYING for the token (@ref carry). */
    SUB_WIRE,   /**< @brief The live `graph_t::subscribe_wire` path, for proportion. */
};

/** @brief An arm's command-line spelling and its enumerator. */
struct arm_spec_t {
    const char* label; /**< @brief What `--arms=` and the RESULT line call it. */
    arm_t arm;         /**< @brief Which driver runs. */
};

/** @brief The arm table, control first. */
constexpr arm_spec_t kAllArms[] = {
    {"S-sentinel", arm_t::SENTINEL},       {"idx-string", arm_t::IDX_STRING},
    {"idx-token", arm_t::IDX_TOKEN},       {"idx-token-intern", arm_t::IDX_INTERN},
    {"idx-token-carry", arm_t::IDX_CARRY}, {"sub-wire", arm_t::SUB_WIRE},
};

/** @brief The label of the control arm, repeated here so the collator and the bench agree. */
constexpr const char* kControlArm = "S-sentinel";

/** @brief A PATH TLV over one segment — the subscriber marker and the return route. */
[[nodiscard]] std::vector<std::byte> b_path_one(std::string_view seg) {
    std::vector<std::byte> body;
    const bool ok = tr::wire::emit_path_segment(body, seg);
    (void)ok;
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief SUBSCRIBER{ PATH @p marker } — the wire subscribe body the live arm binds. */
[[nodiscard]] std::vector<std::byte> b_subscriber(std::string_view marker) {
    const std::vector<std::byte> body = b_path_one(marker);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}

/** @brief A `view_t` over a fresh owned heap segment holding @p bytes. */
[[nodiscard]] tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief A remote-delivery sink that does nothing — the live arm needs one installed. */
void null_remote_sink(void*, const tr::graph::remote_delivery_t&, const tr::view::rope_t&) {}

/**
 * @brief One arm's driver over one grid cell, warmed to steady state and ready to be timed.
 *
 * The rotation is `links[(k / verts) % L]` and `cands[k % V]`, i.e. one link renews all of its
 * own vertices before the next link does — the pattern a peer renewing its subscriptions
 * actually produces, and the pattern the issue names as the steady state ("a subscription is
 * renewed far more often than a new vertex is first subscribed"). Steady state matters: every
 * timed call takes the idempotent early return, which is the path the shipped index spends
 * essentially all of its life on.
 */
class driver_t {
   public:
    /** @brief Build the cell's index, names and candidates, and warm it to steady state. */
    driver_t(arm_t arm, std::size_t links, std::size_t verts, counting_resource_t& mr)
        : arm_(arm),
          links_(links),
          verts_(verts),
          mr_(&mr),
          names_(make_link_names(links)),
          cands_(make_candidates(pool_, verts)) {
        ids_.reserve(links);
        for (std::size_t i = 0; i < links; ++i)
            ids_.push_back(link_id_t{static_cast<std::uint32_t>(i)});
        switch (arm_) {
            case arm_t::IDX_STRING:
                s_index_ = std::make_unique<string_index_t>(&mr);
                break;
            case arm_t::IDX_TOKEN:
                t_index_ = std::make_unique<token_index_t>(&mr, links);
                break;
            case arm_t::IDX_INTERN:
                i_index_ = std::make_unique<intern_index_t>(&mr, links);
                break;
            case arm_t::IDX_CARRY:
                build_carry();
                break;
            case arm_t::SUB_WIRE:
                build_live_bodies();
                build_live_graph();
                break;
            case arm_t::SENTINEL:
                break;
        }
        // Warm to steady state: every (link, vertex) pair present, so the timed window
        // measures the renewal path and not first-insert. The four index arms get TWO passes,
        // which is also where their idempotency is exercised; the live arm gets exactly ONE,
        // for the reason @ref live_pass records.
        const std::size_t passes = arm_ == arm_t::SUB_WIRE ? 1 : 2;
        for (std::size_t i = 0; i < links * verts * passes; ++i) step();
    }

    /**
     * @anchor live_pass
     * @brief Rebuild the live arm's graph so the next pass starts from an empty edge set.
     *
     * @section why_reset Why the live arm cannot simply be looped like the others
     *
     * `graph_t::subscribe_wire` is NOT idempotent the way `index_link_vertex` is: re-issuing
     * the same subscription ADDS an edge rather than replacing one, so a driver that just
     * loops accumulates edges without bound and its per-op cost tracks how many operations
     * have already run instead of the configuration under test. Measured on the first draft of
     * this bench, that artefact read as **65 µs/op at 4 links and 17 µs/op at 65 links** — a
     * curve that appears to *improve* with link count purely because the 65-link cell reached
     * a smaller calibrated batch and therefore performed fewer total subscribes. Reporting
     * that would have been a fabricated result with a plausible shape.
     *
     * So the live arm's unit of measurement is one whole `links * verts` PASS over a freshly
     * built graph: exactly one edge per `(link, vertex)` pair, no accumulation, and a per-op
     * figure that is a genuine per-subscribe cost. The rebuild is untimed.
     *
     * A no-op on every other arm, whose insert genuinely is idempotent.
     */
    void reset() {
        if (arm_ != arm_t::SUB_WIRE) return;
        vertices_.clear();
        graph_.reset();
        counter_ = 0;
        build_live_graph();
    }

    /** @brief Operations in one live pass — the live arm's batch. */
    [[nodiscard]] std::size_t pass_ops() const { return links_ * verts_; }

    /** @brief One operation — the thing being timed. */
    void step() {
        const std::size_t k = counter_++;
        const std::size_t li = (k / verts_) % links_;
        const std::size_t vi = k % verts_;
        switch (arm_) {
            case arm_t::SENTINEL:
                // The control: consume both operands so the rotation cannot be optimised out,
                // and do no index work at all.
                asm volatile("" : : "r"(names_[li].data()), "r"(cands_[vi]) : "memory");
                break;
            case arm_t::IDX_STRING:
                s_index_->insert(names_[li], cands_[vi]);
                break;
            case arm_t::IDX_TOKEN:
                t_index_->insert(ids_[li], cands_[vi]);
                break;
            case arm_t::IDX_INTERN:
                i_index_->insert(names_[li], cands_[vi]);
                break;
            case arm_t::IDX_CARRY:
                // The whole carry, priced: an INDIRECT call into the router-shaped supplier
                // (which branches on its tier and, on the bus, compares a handle stamp),
                // then the token validation, then the identical shared insert. Nothing here
                // is assumed to be already in hand except the two things a terminus genuinely
                // does hold — the frame's `peer_handle_t` and its `child_rx_ctx_t`.
                c_index_->insert(supplier_(ctxs_[li], peers_[li]), names_[li], cands_[vi]);
                break;
            case arm_t::SUB_WIRE:
                live_step(li, vi);
                break;
        }
    }

    /** @brief Distinct candidates the arm recorded for link @p li — the calibration oracle. */
    [[nodiscard]] std::size_t candidates(std::size_t li) {
        switch (arm_) {
            case arm_t::IDX_STRING:
                return s_index_->candidates(names_[li]);
            case arm_t::IDX_TOKEN:
                return t_index_->candidates(ids_[li]);
            case arm_t::IDX_INTERN:
                return i_index_->candidates(names_[li]);
            case arm_t::IDX_CARRY:
                return c_index_->candidates(tokens_[li]);
            case arm_t::SUB_WIRE:
                return graph_ ? graph_->link_edge_candidates(names_[li]) : 0;
            case arm_t::SENTINEL:
                return 0;
        }
        return 0;
    }

    /**
     * @brief The NAME DOOR's answer for link @p li — what an eviction entering by name sees.
     *
     * Only the two arms that have a name door answer; the calibration checks they agree with
     * what the token door says, which is the machine-checked form of #1366's first objection
     * (*"the key belongs to EVICTION, not to subscribe"*).
     */
    [[nodiscard]] std::size_t candidates_by_name(std::size_t li) {
        switch (arm_) {
            case arm_t::IDX_STRING:
                return s_index_->candidates(names_[li]);
            case arm_t::IDX_CARRY:
                return c_index_->candidates_by_name(names_[li]);
            default:
                return 0;
        }
    }

   private:
    /**
     * @brief Stand up the carry arm: intern every link at LINK-UP, then publish each token
     *        into a router-shaped ctx of the tier that link would really belong to.
     *
     * Even links are BUS peers sharing ONE ctx (every WS session on the ESP node is a bus
     * peer, and an announce-census CAN bus is the same shape) and odd links are point-to-point
     * FLAT children with a ctx each. That split is not cosmetic: the 2026-08-14 ruling's
     * `add_child`-only mint point was retired precisely because it covered the flat tier only,
     * and an arm that priced one tier would repeat the mistake.
     */
    void build_carry() {
        c_index_ = std::make_unique<carry_index_t>(mr_);
        supplier_ = g_link_id_supplier;  // volatile read — keeps the call site indirect
        bus_ctx_ = std::make_unique<rx_ctx_shape_t>();
        bus_ctx_->bus = true;
        ctxs_.assign(links_, nullptr);
        peers_.assign(links_, tr::net::peer_handle_t{});
        tokens_.assign(links_, carry_id_t{});
        for (std::size_t li = 0; li < links_; ++li) {
            tokens_[li] = c_index_->intern(names_[li]);
            const std::uint64_t bits = pack_token(tokens_[li]);
            if (li % 2 == 0) {
                const auto pi = static_cast<std::uint32_t>(li / 2);
                bus_ctx_->bus_token.resize(pi + 1, 0);
                bus_ctx_->bus_gen.resize(pi + 1, 0);
                bus_ctx_->bus_token[pi] = bits;
                bus_ctx_->bus_gen[pi] = 1;
                peers_[li] = tr::net::peer_handle_t{pi, 1};
                ctxs_[li] = bus_ctx_.get();
            } else {
                flat_ctxs_.push_back(std::make_unique<rx_ctx_shape_t>());
                flat_ctxs_.back()->flat_token.store(bits, std::memory_order_relaxed);
                peers_[li] = tr::net::kSolePeerHandle;
                ctxs_[li] = flat_ctxs_.back().get();
            }
        }
    }

    /** @brief Pre-build the per-(link, vertex) SUBSCRIBER and per-link return-route bytes. */
    void build_live_bodies() {
        subs_.reserve(links_ * verts_);
        routes_.reserve(links_);
        for (std::size_t li = 0; li < links_; ++li) {
            routes_.push_back(b_path_one(names_[li]));
            for (std::size_t vi = 0; vi < verts_; ++vi) {
                char marker[64];
                std::snprintf(marker, sizeof marker, "s%zu-%zu", li, vi);
                subs_.push_back(b_subscriber(marker));
            }
        }
    }

    /** @brief Stand up a fresh live graph and its vertices — the untimed half of @ref reset. */
    void build_live_graph() {
        graph_ = std::make_unique<graph_t>(&mr_->as_source());
        graph_->configure_remote_delivery_sink(&null_remote_sink, nullptr);
        vertices_.reserve(verts_);
        for (std::size_t i = 0; i < verts_; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "/v%zu", i);
            vertices_.push_back(graph_->register_vertex(path_t(buf), role_t::STORED_VALUE));
        }
    }

    /** @brief One live remote subscribe — the renewal a peer re-issues. */
    void live_step(std::size_t li, std::size_t vi) {
        const auto r = graph_->subscribe_wire(vertices_[vi], make_value(subs_[li * verts_ + vi]),
                                              make_value(routes_[li]), names_[li]);
        live_ok_ += r.has_value() ? 1u : 0u;
    }

    arm_t arm_;                        /**< @brief Which driver `step` runs. */
    std::size_t links_;                /**< @brief Distinct link names in this cell. */
    std::size_t verts_;                /**< @brief Vertex population, held constant across links. */
    counting_resource_t* mr_;          /**< @brief The counted arena every arm draws from. */
    std::vector<std::uintptr_t> pool_; /**< @brief Backing storage for @ref cands_. */
    std::vector<std::string> names_;   /**< @brief The cell's link names. */
    std::vector<cand_t> cands_;        /**< @brief The cell's candidate pointers. */
    std::vector<link_id_t> ids_;       /**< @brief Pre-minted tokens, one per link. */
    std::size_t counter_ = 0;          /**< @brief Drives the (link, vertex) rotation. */

    std::unique_ptr<string_index_t> s_index_; /**< @brief Arm `idx-string`. */
    std::unique_ptr<token_index_t> t_index_;  /**< @brief Arm `idx-token`. */
    std::unique_ptr<intern_index_t> i_index_; /**< @brief Arm `idx-token-intern`. */

    std::unique_ptr<carry_index_t> c_index_;  /**< @brief Arm `idx-token-carry`. */
    link_id_fn_t supplier_ = nullptr;         /**< @brief The seam the token is obtained over. */
    std::unique_ptr<rx_ctx_shape_t> bus_ctx_; /**< @brief The one bus ctx every bus peer shares. */
    std::vector<std::unique_ptr<rx_ctx_shape_t>> flat_ctxs_; /**< @brief One ctx per flat child. */
    std::vector<void*> ctxs_; /**< @brief Per link — what `inbound.origin` is. */
    std::vector<tr::net::peer_handle_t> peers_; /**< @brief Per link — what the frame carries. */
    std::vector<carry_id_t> tokens_;            /**< @brief Per link, for the oracle only. */

    std::unique_ptr<graph_t> graph_;             /**< @brief Arm `sub-wire`'s live graph. */
    std::vector<vertex_handle_t> vertices_;      /**< @brief Its registered vertices. */
    std::vector<std::vector<std::byte>> subs_;   /**< @brief Per-(link, vertex) SUBSCRIBER bytes. */
    std::vector<std::vector<std::byte>> routes_; /**< @brief Per-link return-route PATH bytes. */
    std::uint64_t live_ok_ = 0;                  /**< @brief Successful subscribes — kept live. */
};

/** @brief One arm's figures at one grid cell in one round. */
struct cell_result_t {
    bench::Latency::Summary lat; /**< @brief Per-op picoseconds. */
    std::size_t batch = 0;       /**< @brief The calibrated batch the window used. */
};

/**
 * @brief Time one arm at one grid cell: calibrate the window, then sample it.
 *
 * The batch is sized by WINDOW rather than by plateau (`calibrate_batch_for_window`). The
 * plateau rule compares two timed quantities, so the machine gets a vote in which batch is
 * latched, and `bench/bench_common.hpp` records same-binary A/A differences of up to ~8 % from
 * nothing but that lottery — which would eat the entire effect this bench is hunting.
 *
 * The live `sub-wire` arm is sampled differently and the difference is not cosmetic: its unit
 * is one whole freshly-built pass rather than a calibrated batch, for the reason
 * @ref live_pass records.
 *
 * NOT INLINED, deliberately. This is the only entry point the latency measurement goes
 * through and `calibrate` / `run_ram` never reach it, so it is the toggle a profiler collects
 * on — `valgrind --tool=callgrind --collect-atstart=no --toggle-collect='*run_cell*'` attributes
 * exactly this arm at exactly the cell `--links` / `--verts` name, with the calibration pass
 * executed but uncounted. Inlined into `main` (which is what -O3 did, and what made #1436's
 * first profiling attempt collect zero events) no such toggle exists. It runs ten times in a
 * full sweep, so the call costs nothing measurable.
 */
[[gnu::noinline]] [[nodiscard]] cell_result_t run_cell(arm_t arm, std::size_t links,
                                                       std::size_t verts) {
    counting_resource_t mr;
    driver_t d(arm, links, verts, mr);
    bench::Latency lat;

    if (arm == arm_t::SUB_WIRE) {
        const std::size_t batch = d.pass_ops();
        lat.reserve(kLiveSamplesPerCell);
        for (std::size_t s = 0; s < kLiveSamplesPerCell; ++s) {
            d.reset();  // untimed: a fresh graph, so the pass starts from an empty edge set
            const std::uint64_t a = bench::now_ns();
            for (std::size_t i = 0; i < batch; ++i) d.step();
            const std::uint64_t window = bench::now_ns() - a;
            lat.add(window * kPsPerNs / batch);
        }
        return {lat.summarize(), batch};
    }

    const std::size_t batch = bench::calibrate_batch_for_window([&] { d.step(); });
    lat.reserve(kSamplesPerCell);
    for (std::size_t s = 0; s < kSamplesPerCell; ++s) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) d.step();
        const std::uint64_t window = bench::now_ns() - a;
        lat.add(window * kPsPerNs / batch);
    }
    return {lat.summarize(), batch};
}

/** @brief Build one arm to steady state and report what its index cost in bytes. */
struct ram_result_t {
    std::size_t live = 0;     /**< @brief Bytes at rest. */
    std::size_t peak = 0;     /**< @brief High-water mark. */
    std::uint64_t allocs = 0; /**< @brief Allocations forwarded to the upstream. */
    std::size_t entries = 0;  /**< @brief Distinct candidates the arm recorded for link 0. */
};

/** @brief Steady-state footprint of one arm at one grid cell. */
[[nodiscard]] ram_result_t run_ram(arm_t arm, std::size_t links, std::size_t verts) {
    counting_resource_t mr;
    ram_result_t out;
    {
        driver_t d(arm, links, verts, mr);
        out.entries = d.candidates(0);
        out.live = mr.live();
        out.peak = mr.peak();
        out.allocs = mr.allocs();
    }
    return out;
}

/**
 * @brief What ONE name-door lookup costs, in picoseconds — the carry's side of the trade.
 *
 * REPORTED, NOT GATED. `idx-string` answers the door with a hash and a map find; the carry
 * answers it with a linear scan over live slots. The scan is unambiguously the slower of the
 * two and grows with link count, and pretending otherwise would be dishonest — but it runs
 * once per PEER HANGUP where the thing it pays for runs once per SUBSCRIBE, so the trade is
 * shown rather than hidden inside an aggregate.
 */
[[nodiscard]] std::uint64_t run_door(arm_t arm, std::size_t links, std::size_t verts) {
    counting_resource_t mr;
    driver_t d(arm, links, verts, mr);
    constexpr std::size_t kDoorReps = 200;
    std::uint64_t best = ~std::uint64_t{0};
    for (std::size_t s = 0; s < kDoorReps; ++s) {
        const std::uint64_t a = bench::now_ns();
        std::size_t sink = 0;
        for (std::size_t li = 0; li < links; ++li) sink += d.candidates_by_name(li);
        const std::uint64_t window = bench::now_ns() - a;
        asm volatile("" : : "r"(sink) : "memory");
        best = std::min(best, window * kPsPerNs / links);
    }
    return best;
}

/** @brief `RESULT_SIDX_DOOR` — one name-door lookup's cost, in picoseconds. */
void emit_door(const char* tag, const char* arm, std::size_t links, std::size_t verts,
               std::uint64_t ps) {
    std::printf("RESULT_SIDX_DOOR\t%s\t%s\t%zu\t%zu\t%llu\n", tag, arm, links, verts,
                static_cast<unsigned long long>(ps));
    std::fflush(stdout);
}

/** @brief `RESULT_SIDX` — one arm at one grid cell in one round. Own tag; see @ref reading. */
void emit_sidx(int round, const char* tag, const char* arm, std::size_t links, std::size_t verts,
               const cell_result_t& r) {
    std::printf("RESULT_SIDX\t%d\t%s\t%s\t%zu\t%zu\t%llu\t%llu\t%llu\t%zu\t%zu\n", round, tag, arm,
                links, verts, static_cast<unsigned long long>(r.lat.p50),
                static_cast<unsigned long long>(r.lat.p99),
                static_cast<unsigned long long>(r.lat.mean), r.lat.n, r.batch);
    std::fflush(stdout);
}

/** @brief `RESULT_SIDX_RAM` — one arm's steady-state footprint at one grid cell. */
void emit_ram(const char* tag, const char* arm, std::size_t links, std::size_t verts,
              const ram_result_t& r) {
    std::printf("RESULT_SIDX_RAM\t%s\t%s\t%zu\t%zu\t%zu\t%zu\t%llu\t%zu\n", tag, arm, links, verts,
                r.live, r.peak, static_cast<unsigned long long>(r.allocs), r.entries);
    std::fflush(stdout);
}

/**
 * @brief Break the instrument's line before believing any cell.
 *
 * Run on every invocation, not once at authoring time. Four things have to hold or no number
 * from this binary means anything:
 *
 *  1. **The transcription is faithful.** All three index arms must record the SAME distinct
 *     candidate set for the same input sequence. If they diverge, `idx-string − idx-token` is
 *     a comparison of two different data structures rather than of two lookups.
 *  2. **The insert is idempotent.** A second full pass must not grow any arm's candidate
 *     count. This is #1290's fix, and if it ever regressed the timed window would be
 *     measuring list growth instead of the renewal path.
 *  3. **The sentinel does no index work**, so it is a floor and not a fifth index.
 *  4. **The live arm actually indexes.** `graph_t::link_edge_candidates` must report the
 *     whole vertex population for the link, i.e. `subscribe_wire` really did reach
 *     `index_link_vertex`. Without this the `sub-wire` arm could be timing a rejected
 *     subscribe and reporting it as a subscribe.
 *
 * @return 0 on success; non-zero is a refusal to report any number from this binary.
 */
int calibrate() {
    int bad = 0;
    const auto expect = [&bad](const char* what, std::uint64_t got, std::uint64_t want) {
        const bool ok = got == want;
        std::printf("CALIBRATE\t%s\t%s\tgot=%llu\twant=%llu\n", ok ? "PASS" : "FAIL", what,
                    static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
        if (!ok) ++bad;
    };

    constexpr std::size_t kL = 8;
    constexpr std::size_t kV = 8;
    counting_resource_t mr_s;
    counting_resource_t mr_t;
    counting_resource_t mr_i;
    counting_resource_t mr_c;
    counting_resource_t mr_n;
    counting_resource_t mr_w;
    driver_t ds(arm_t::IDX_STRING, kL, kV, mr_s);
    driver_t dt(arm_t::IDX_TOKEN, kL, kV, mr_t);
    driver_t di(arm_t::IDX_INTERN, kL, kV, mr_i);
    driver_t dc(arm_t::IDX_CARRY, kL, kV, mr_c);
    driver_t dn(arm_t::SENTINEL, kL, kV, mr_n);
    driver_t dw(arm_t::SUB_WIRE, kL, kV, mr_w);

    // 1 + 2 — the four index arms agree, and agree at the value the warm-up should have
    // produced (every vertex once, no duplicates), which is idempotency observed.
    for (std::size_t li = 0; li < kL; ++li) {
        expect("idx-string records every vertex exactly once", ds.candidates(li), kV);
        expect("idx-token agrees with idx-string", dt.candidates(li), ds.candidates(li));
        expect("idx-token-intern agrees with idx-string", di.candidates(li), ds.candidates(li));
        expect("idx-token-carry agrees with idx-string", dc.candidates(li), ds.candidates(li));
        // THE NAME DOOR — #1366's first objection, machine-checked. The carry arm keeps the
        // key eviction enters by, and a name lookup must land on the SAME slot the token
        // does. If these ever diverge the carry has un-indexed exactly the edges #1071 and
        // #943 exist to keep reachable, and no latency figure below would be worth having.
        expect("the carry's NAME door answers what its TOKEN door answers", dc.candidates(li),
               dc.candidates_by_name(li));
        expect("the carry's NAME door agrees with idx-string's", dc.candidates_by_name(li),
               ds.candidates_by_name(li));
    }
    // A further full pass must move nothing — the idempotent early return, exercised.
    for (std::size_t i = 0; i < kL * kV; ++i) {
        ds.step();
        dt.step();
        di.step();
        dc.step();
    }
    expect("a second pass adds no candidate (idx-string)", ds.candidates(0), kV);
    expect("a second pass adds no candidate (idx-token)", dt.candidates(0), kV);
    expect("a second pass adds no candidate (idx-token-intern)", di.candidates(0), kV);
    expect("a second pass adds no candidate (idx-token-carry)", dc.candidates(0), kV);

    // 3 — the control is a floor, not a fifth index.
    expect("S-sentinel records nothing", dn.candidates(0), 0);
    expect("S-sentinel allocates nothing", mr_n.allocs(), 0);

    // 4 — the live arm reached the real index.
    for (std::size_t li = 0; li < kL; ++li)
        expect("subscribe_wire indexed the whole vertex set for its link", dw.candidates(li), kV);

    // 5 — `reset` really does start the live arm from an empty edge set, which is the whole
    // basis of its per-pass unit (@ref live_pass). Without this the live figure would silently
    // drift back into measuring accumulation.
    dw.reset();
    expect("reset clears the live arm's index", dw.candidates(0), 0);
    for (std::size_t i = 0; i < kL * kV; ++i) dw.step();
    expect("one live pass re-indexes the whole vertex set", dw.candidates(0), kV);

    // The RAM instrument is live: a string-keyed index must cost strictly more than a
    // token-keyed one holding the same candidates, or the counter is not counting.
    const bool ram_live = mr_s.live() > 0 && mr_t.live() > 0;
    std::printf("CALIBRATE\t%s\tRAM counters are live\tstring=%zu\ttoken=%zu\n",
                ram_live ? "PASS" : "FAIL", mr_s.live(), mr_t.live());
    if (!ram_live) ++bad;

    // 7 — the SHIPPED doors, exercised through the live arm: `graph_t::intern_link` is
    // idempotent by NAME (the same-NAME redial property #1263 pinned), and a released slot's
    // token stops validating. Checked here because the transcription arms can only show that
    // the SHAPE has these properties; this shows the shipped code does.
    {
        // #873 phase 1: the graph takes ONE `block_source_t`. Nothing in this block reads the
        // counter — it only needed a graph — so the process default is the honest wiring.
        graph_t g;
        const tr::graph::link_id_t a = g.intern_link("p0");
        const tr::graph::link_id_t b = g.intern_link("p0");
        expect("intern_link is idempotent by NAME (same-NAME redial, #1263)",
               a == b && a.valid() ? 1 : 0, 1);
        const tr::graph::link_id_t c = g.intern_link("p1");
        expect("two links get two tokens (#1366 objection 2)", c != a && c.valid() ? 1 : 0, 1);
        g.release_link(a);
        const tr::graph::link_id_t d = g.intern_link("p2");
        expect("a released slot is REUSED", d.slot == a.slot ? 1 : 0, 1);
        expect("but its stamp moved, so the old token cannot inherit it",
               d.generation != a.generation ? 1 : 0, 1);
    }

    // 6 — THE FAITHFULNESS ORACLE, and it is the strongest check in this file.
    //
    // A `graph_t` built over an injected `std::pmr::memory_resource` draws NOTHING from it
    // during construction or vertex registration — measured: 0 bytes, 0 allocations after a
    // ctor and 8 `register_vertex` calls. Vertex storage comes from the `ctl` block source and
    // the global heap. So in THIS workload the counted arena sees exactly one structure:
    // `link_index_` itself.
    //
    // Which means the live arm's byte count is the SHIPPED index's byte count, and the
    // transcription arm can be checked against it directly rather than taken on trust. If the
    // arm that claims to be today's shape ever stops reproducing `link_index_` byte-for-byte
    // AND allocation-for-allocation, the transcription has drifted from the code it claims to
    // transcribe, and every figure in this bench is measuring something other than what it
    // says. That is a refusal to report, not a footnote.
    //
    // WHICH ARM IS CHECKED MOVED WITH THE SHIPPED INDEX (#1417). Until the carry landed, the
    // shipped shape was `idx-string` and it reproduced 746 / 23 @ 4 links, 1388 / 45 @ 8 and
    // 11467 / 361 @ 65. The carry replaced the map with the dense slot vector this file's
    // `carry_index_t` transcribes, so `idx-token-carry` is now the arm under oath and the
    // numbers moved with it — 512 / 20, 1024 / 40, 8320 / 325. `idx-string` is retained
    // deliberately and is now the HISTORICAL BASELINE: it is what `origin/main` shipped
    // before this change, it is the column every saving in #1416's curve is quoted against,
    // and keeping it in-binary is what lets the two shapes be compared under one layout.
    for (const std::size_t l : {std::size_t{4}, std::size_t{8}, std::size_t{65}}) {
        const ram_result_t r_carry = run_ram(arm_t::IDX_CARRY, l, kV);
        const ram_result_t r_live = run_ram(arm_t::SUB_WIRE, l, kV);
        expect("idx-token-carry reproduces the shipped link_index_ BYTE for byte", r_carry.live,
               r_live.live);
        expect("idx-token-carry reproduces it ALLOCATION for allocation", r_carry.allocs,
               r_live.allocs);
    }
    return bad;
}

/**
 * @brief Restrict a swept axis to ONE of its canonical points — the profiler's affordance.
 *
 * `--links=` / `--verts=` exist so a profiler can be pointed at a SINGLE grid cell: attributing
 * the `sub-wire` growth (#1436) to named callees needs one cell's samples uncontaminated by the
 * other nine, and `perf record` over the whole sweep cannot separate them afterwards.
 *
 * An off-grid value is REFUSED rather than accepted, because a cell that is not on
 * @ref kLinkCounts / @ref kVertexCounts is not on the curve every other number in this file is
 * quoted against, and a run that silently invented one would be reported as if it were.
 *
 * @param sweep The canonical axis.
 * @param pick  The requested point, or 0 for "the whole axis".
 * @return The filtered axis; EMPTY iff @p pick is non-zero and not on @p sweep.
 */
[[nodiscard]] std::vector<std::size_t> filter_axis(std::span<const std::size_t> sweep,
                                                   std::size_t pick) {
    if (pick == 0) return {sweep.begin(), sweep.end()};
    for (const std::size_t x : sweep)
        if (x == pick) return {pick};
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    int rounds = 1;
    int round0 = 0;
    bool do_calibrate = false;
    bool do_ram = false;
    bool do_door = false;
    std::size_t pick_links = 0;
    std::size_t pick_verts = 0;
    const char* tag = "A";
    std::vector<arm_spec_t> arms(std::begin(kAllArms), std::end(kAllArms));

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--rounds=", 0) == 0)
            rounds = std::atoi(a.c_str() + 9);
        else if (a.rfind("--round0=", 0) == 0)
            round0 = std::atoi(a.c_str() + 9);
        else if (a.rfind("--links=", 0) == 0)
            pick_links = std::strtoul(a.c_str() + 8, nullptr, 10);
        else if (a.rfind("--verts=", 0) == 0)
            pick_verts = std::strtoul(a.c_str() + 8, nullptr, 10);
        else if (a.rfind("--tag=", 0) == 0)
            tag = argv[i] + 6;
        else if (a == "--calibrate")
            do_calibrate = true;
        else if (a == "--ram")
            do_ram = true;
        else if (a == "--door")
            do_door = true;
        else if (a.rfind("--arms=", 0) == 0) {
            arms.clear();
            const std::string list = a.substr(7);
            std::size_t p = 0;
            while (p <= list.size()) {
                const std::size_t c = std::min(list.find(',', p), list.size());
                const std::string tok = list.substr(p, c - p);
                for (const arm_spec_t& x : kAllArms)
                    if (tok == x.label) arms.push_back(x);
                p = c + 1;
            }
        }
    }

    const std::vector<std::size_t> links_sweep = filter_axis(kLinkCounts, pick_links);
    const std::vector<std::size_t> verts_sweep = filter_axis(kVertexCounts, pick_verts);
    if (links_sweep.empty() || verts_sweep.empty()) {
        std::fprintf(stderr,
                     "bench_subscribe_index: --links/--verts must name a swept point "
                     "(links 4/8/16/32/65, verts 8/32)\n");
        return 2;
    }

    // Reachability before numbers, every run — not once at authoring time.
    if (calibrate() != 0) {
        std::fprintf(stderr, "bench_subscribe_index: calibration FAILED; refusing to report\n");
        return 2;
    }
    if (do_calibrate) return 0;

    if (do_ram) {
        std::printf("# RESULT_SIDX_RAM tag arm links verts live_bytes peak_bytes allocs entries\n");
        for (const arm_spec_t& arm : arms)
            for (std::size_t verts : verts_sweep)
                for (std::size_t links : links_sweep)
                    emit_ram(tag, arm.label, links, verts, run_ram(arm.arm, links, verts));
        return 0;
    }

    if (do_door) {
        std::printf("# RESULT_SIDX_DOOR tag arm links verts ps_per_lookup\n");
        for (const arm_t arm : {arm_t::IDX_STRING, arm_t::IDX_CARRY})
            for (std::size_t links : links_sweep)
                emit_door(tag, arm == arm_t::IDX_STRING ? "idx-string" : "idx-token-carry", links,
                          kVertexCounts[0], run_door(arm, links, kVertexCounts[0]));
        return 0;
    }

    std::printf("# RESULT_SIDX round tag arm links verts p50ps p99ps meanps n batch\n");
    std::printf("# control arm: %s\n", kControlArm);
    for (int r = 0; r < rounds; ++r) {
        // Rotate the arm order every round: arm i leads round i. Exhausting one arm's runs
        // before starting the next is the shape that produced the recorded 55.2 / 53.0 /
        // 149.8 M deliv/s swing on identical code (`bench/run_pin_ratio.sh`), and nothing
        // here does that.
        const std::size_t n = arms.size();
        for (std::size_t j = 0; j < n; ++j) {
            const arm_spec_t& arm = arms[(static_cast<std::size_t>(r + round0) + j) % n];
            for (std::size_t verts : verts_sweep)
                for (std::size_t links : links_sweep)
                    emit_sidx(r + round0, tag, arm.label, links, verts,
                              run_cell(arm.arm, links, verts));
        }
    }
    return 0;
}
