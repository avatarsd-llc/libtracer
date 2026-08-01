/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The connection registry: this node's `NAME → transport link` table — the
 * compositor demux of ADR-0037 (a transport-vertex "resolves the next path segment
 * to a child"). It replaces `fwd_router_t`'s anonymous `children_` field with ONE
 * named, shareable owner, so the connection table is not duplicated between the
 * router and `transport_vertex_t` (Brick 3a of the #83 Stage-2 flip).
 *
 * Layering (ADR-0038 §3b.1): this lives in `tr::net` (L5) and holds `transport_t*` — it is
 * NOT `graph.find` against the L4 vertex map, because an L4 `vertex_t` must never
 * know about a transport (`vertex.hpp`). ADR-0037 §Stage-2 phrased the dissolution as
 * "graph.find(child)"; the layering-safe realization is this single tr::net-owned
 * registry the router consults, which achieves the same "no duplicated children-table"
 * without inverting the L4↔L5 dependency (see ADR-0038 §Brick-3a note).
 */
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

/**
 * @brief This node's `NAME → transport link` table (the compositor demux, ADR-0037).
 *
 * A child is addressed by its **mount path** — `/net/<module>/<name>` (RFC-0014,
 * ADR-0061): the run of `dst` segments this node consumes to route onward, and the run
 * prepended to `src` on the way back. Lookups are lock-free.
 *
 * **Link identity is the QUALIFIED name `"<module>/<name>"`** — one string, so every
 * existing consumer of a link identity (route-handle label tables, subscriber-edge
 * eviction, the departure notifiers) keeps working on an opaque string and needs no
 * signature change. The demux, which holds two raw segment spans and must not allocate
 * on the hot path (`bench_forward_heap`'s `allocs=0` gate), matches through
 * @ref longest_prefix, which compares each slot's key against the `dst` prefix in place
 * and never builds a key.
 *
 * **Shape is per-CONNECTION, not per-module** — a refinement of ADR-0061, which assumed
 * a module declares it. It cannot: `ws-server`'s `peer_named` config decides whether the
 * bus facet is exposed, so two connections in one module may differ. The shape is
 * therefore captured ONCE at @ref add time from `link.bus()` and stored on the slot, which
 * still honours the ADR's actual requirement — no `bus()` probe on the forward path.
 *
 * **Mutation model (#494).** The table was add-only, which left a retired link's
 * `name → transport_t*` resident and dangling. @ref erase closes that, and it does so by
 * **tombstoning in place** — the slot's `link` is nulled and its NAME kept — never by
 * erasing from `children_`. That is deliberate: shifting the vector under a concurrent
 * lock-free reader is a hard use-after-free, whereas a tombstone leaves the slot in place
 * and a racing reader sees either the old pointer or `nullptr`. A
 * later @ref add of the SAME name reuses its tombstone, so create/remove churn on a stable
 * name set does not grow the table; a genuinely new name still appends, so the table's high
 * -water mark is the count of DISTINCT names ever registered. Compaction (and the full
 * mutation-vs-forward concurrency contract) lands with the RFC-0014 S5 liveness engine,
 * where the TSan gate and a safe reclamation scheme arrive together — see ADR-0061
 * (`docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md`).
 *
 * **Slots ARE address-stable (#521, ADR-0063).** The tombstone alone bought stability against
 * ERASE only: `children_` was a `std::vector`, so appending a genuinely new name reallocated
 * and invalidated every slot reference and iterator in the table — which RFC-0014 turned from
 * a dormant caveat into a live hazard by making connection create/remove a RUNTIME operation.
 * The storage is now an append-only CHUNKED LIST: a chunk is never moved, resized, or freed
 * before this object dies, so a slot's address is fixed from the moment it is published.
 * ADR-0062's forward cache builds on exactly that — it holds a `const child_t*` and reads the
 * tombstone as its invalidation.
 *
 * **Writers are serialized by the caller; readers are not (ADR-0063).** @ref add and @ref erase
 * are control-plane calls and must not run concurrently with each other — `add`'s scan-then-
 * append is not atomic, so two racing writers can be handed the SAME empty slot. `fwd_router_t`
 * holds the lock that prevents this. Readers need no lock and take none.
 */
class child_registry_t {
   public:
    child_registry_t() = default;
    child_registry_t(const child_registry_t&) = delete;
    child_registry_t& operator=(const child_registry_t&) = delete;
    /** @brief Frees the chunks. Nothing is reclaimed before this point, by design. */
    ~child_registry_t() {
        for (chunk_t* c = head_.load(std::memory_order_relaxed); c != nullptr;) {
            chunk_t* const nxt = c->next.load(std::memory_order_relaxed);
            delete c;
            c = nxt;
        }
    }

    /**
     * @brief One registered child: its qualified mount name, its link, and its shape.
     *
     * `name` is `"<module>/<name>"` (RFC-0014's `/net/<module>/<name>` minus the constant
     * `net` root). `link == nullptr` marks a TOMBSTONE (#494) — the slot is dead but stays
     * put so a concurrent lock-free reader's iteration remains valid. `multi_peer` is the
     * shape captured once at @ref add time, so the forward path never probes `bus()`.
     */
    struct child_t {
        std::string name; /**< @brief Qualified mount name, `"<module>/<name>"`. */
        /** @brief The link; nullptr marks a TOMBSTONE (#494). ATOMIC because teardown nulls
         *         it in place while a lock-free forward read may be dereferencing the slot
         *         (ADR-0063): the reader sees the old pointer or `nullptr`, never a tear. */
        std::atomic<transport_t*> link{nullptr};
        /** @brief Shape, captured at @ref add time. ATOMIC for the same reason `link` is, and
         *         it was missed the first time: @ref add REBINDS an existing slot on the
         *         tombstone-reuse path that RFC-0014 create/remove churn takes constantly, and
         *         that rebind writes this field while a lock-free forward read is testing it
         *         (`fwd_router.cpp`'s mount descent). A plain write against a plain read is a
         *         data race however benign the codegen looks on a given target (ADR-0063
         *         erratum 3). Relaxed suffices: the value is an independent bool, published
         *         BEFORE the `link` release-store that makes the slot resolvable at all. */
        std::atomic<bool> multi_peer{false};
        /**
         * @brief How many `/`-separated segments @ref name has — the slot's OWN mount width.
         *
         * The field the INVERTED SINGLE-PASS descent turns on (#523). The descent used to try
         * key widths `W..1` and re-scan the whole table at each one — O(W×N) slot visits, which
         * is invisible only while a constant caps W at 3. Storing each slot's own width lets
         * ONE pass match every slot against the prefix of exactly that width, so the descent is
         * O(N) whatever the widths in the table are, and no width needs to be known in advance.
         *
         * Written once, on the append path, before the slot is published — the same contract
         * @ref name_digest has, and for the same reason: it is a pure function of @ref name.
         *
         * Declared HERE, in `multi_peer`'s tail padding, rather than beside `name_digest`:
         * appended at the end it grew `child_t` from 80 to 88 bytes, and 64 slots of that is
         * eight extra cache lines the scan walks per frame. Slotted into the hole the struct
         * already had, the width lift costs the table nothing at all.
         */
        std::uint32_t seg_count = 0;
        /**
         * @brief A cheap digest of @ref name, computed once at @ref add time.
         *
         * A pure function of the slot's own name, so it has NO invalidation contract: a name
         * has exactly one slot and @ref add writes the name only on the append path, before
         * the slot is published. Tombstoning nulls @ref link and leaves this untouched, which
         * is what lets the scan test it BEFORE the acquire-load — a stale-looking hash can
         * only ever cause an extra @ref live check, never a wrong answer.
         *
         * Why it exists: the scan's per-candidate work was an acquire-load plus a string
         * compare, so a wide table paid a real cost per frame even though at most one slot
         * could match. An inline integer discriminator in the slot's own cache line makes the
         * overwhelming majority of candidates cost one compare.
         */
        std::uint64_t name_digest = 0;
        /** @brief True while this slot still resolves — i.e. it is not a tombstone. */
        [[nodiscard]] bool live() const noexcept {
            return link.load(std::memory_order_acquire) != nullptr;
        }
        /** @brief The mount path PRE-ENCODED as a run of NAME TLVs (#508), built once here so
         *         a forward hop emits the grown `src` prefix as ONE span with no per-segment
         *         work — and so no fixed buffer bounds how long a NAME may be.
         *
         *         IMMUTABLE AFTER PUBLISH (ADR-0073's sibling ruling on #684): the encoding
         *         is a pure function of the slot's key (`encode_mount_name(name)`), and a
         *         rebind matches by name — so a live slot's replacement bytes are identical
         *         by construction, and @ref add never reassigns them. That immutability is
         *         what lets the forward path read this vector as a span with NO lock while
         *         @ref add runs concurrently on a control thread; reassigning here would be
         *         a use-after-free on the reader (#684). Slot reuse under a DIFFERENT name
         *         (should teardown ever recycle slots) inherits this invariant. */
        std::vector<std::byte> mount_tlv;
    };

    /**
     * @brief Register the link addressed by qualified name @p name (`"<module>/<name>"`).
     *
     * REBINDS @p name's existing slot when it has one — live or tombstoned — and only
     * appends for a name the table has never held. Captures the link's SHAPE here, once, so
     * the forward path never probes `bus()`. A control-plane call: not for the forward path.
     *
     * **A name has exactly one slot.** Re-adding a LIVE name used to append a second, and
     * that shadow slot reopened precisely the dangling-`transport_t*` hole #494 closed:
     * @ref erase nulled only the first match and returned `true`, so the caller destroyed
     * its transport believing teardown had succeeded while @ref by_name kept resolving the
     * freed link through the shadow. Rebinding makes the name→slot mapping one-to-one, which
     * is what every other operation here already assumes.
     */
    void add(std::string name, transport_t& link) {
        const bool multi_peer = link.bus() != nullptr;
        child_t* hit = nullptr;
        for_each([&](const child_t& c) {
            if (c.name == name) {
                hit = const_cast<child_t*>(&c);
                return true;
            }
            return false;
        });
        if (hit != nullptr) {
            // Rebind updates ONLY the fields a reconnect can change. `mount_tlv` is
            // immutable after publish (see the member doc): a reader on a transport
            // receive thread may hold it as a span right now, and the bytes a rebind
            // would write are identical anyway — `encode_mount_name` is pure and the
            // slot was matched by name. The assert pins that purity invariant.
            assert(hit->mount_tlv == encode_mount_name(name));
            hit->multi_peer.store(multi_peer, std::memory_order_relaxed);
            hit->link.store(&link, std::memory_order_release);
            // A tombstone coming back to life changes what a `dst` prefix resolves to, so it
            // moves the mount shape exactly as a fresh append does (#765).
            bump_generation();
            return;
        }
        std::vector<std::byte> mount = encode_mount_name(name);
        child_t* const slot = append();
        if (slot == nullptr) return;  // allocation failure — soft-fail, nothing registered
        slot->name = std::move(name);
        slot->name_digest = digest_name(slot->name);
        slot->seg_count = static_cast<std::uint32_t>(segment_count(slot->name));
        slot->multi_peer.store(multi_peer, std::memory_order_relaxed);
        slot->mount_tlv = std::move(mount);
        slot->link.store(&link, std::memory_order_release);
        publish(slot);
        bump_generation();
    }

    /**
     * @brief Segments in a qualified mount name — `"a/b"` is 2, `""` is 0.
     *
     * Counts separators rather than splitting: a count is all any caller wants, and building a
     * vector of pieces to learn one would be the wrong shape even on a control-plane path.
     */
    [[nodiscard]] static constexpr std::size_t segment_count(std::string_view name) noexcept {
        if (name.empty()) return 0;
        std::size_t n = 1;
        for (const char c : name) {
            if (c == '/') ++n;
        }
        return n;
    }

    /**
     * @brief The MOUNT-SHAPE generation — bumped whenever a `dst` prefix could start or stop
     *        resolving to a different mount (#765).
     *
     * The third validate-on-use stamp, beside `graph_t::retire_generation` (a revived vertex)
     * and the slot tombstone (a departed link). Neither of those two can see the hazard this
     * one exists for: bind a label through mount `net/ws/s`, then register `net/ws/s/rack`, and
     * a full `FWD` resolves against the NEW, deeper mount while a `COMPACT` riding the old
     * label still dereferences the binding made against the old split. Both targets are alive
     * and both are the vertex/link they always were — what moved is the POINT at which the
     * address divides into "local mount" and "remote residual".
     *
     * Until #523 the two planes agreed about a deeper mount only because NEITHER could reach it
     * — the descent capped its width, so the deeper registration was unroutable to both. That
     * is agreement by mutual failure, and lifting the width bound ends it.
     *
     * Coarse ON PURPOSE: it counts mount-table mutations, not the mounts a given label depends
     * on. A mutation that could not have changed one label's split still restamps it, and that
     * label takes the RFC-0004 §E.1 self-heal — drop, observe, `HANDLE_NACK`, re-advertise. A
     * per-label dependency set would be a reverse index, which is the option ADR-0062 already
     * rejected: it moves work onto the control plane's lock to serve the minority flow, and it
     * is a SECOND invalidation mechanism beside one that works.
     */
    [[nodiscard]] std::uint32_t mount_generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    /**
     * @brief Walks a `dst`'s leading segments once, forward, keeping the digest chain with it.
     *
     * The state the single-pass descent needs to test a slot of ANY width for one integer
     * compare. @ref fold_segment is an ACCUMULATOR — `h_k = fold(h_{k-1}, seg_k)` — so the
     * digests of every prefix form a chain, and @ref reach walks it forward on demand: a
     * table whose slots share a width folds it exactly once and every slot after the first
     * costs two integer compares, the same per-slot work the old exact-match scan paid.
     *
     * @tparam SegAt `std::optional<std::string_view>(std::size_t)` — segment `i` of the
     *               `dst`, `std::nullopt` when the `dst` has no segment there. An EMPTY
     *               string means "present but not routable" (over-long, or unreadable off a
     *               rope), which is a different answer: it stops the chain without meaning
     *               the address ended.
     */
    template <class SegAt>
    class prefix_walk_t {
       public:
        /** @brief Walk the segments @p at yields, folding as it goes. */
        explicit prefix_walk_t(SegAt& at) noexcept : at_(at) {}

        /**
         * @brief Advance the chain to cover the first @p want segments.
         * @return false if the `dst` has no usable run that long — and then no LONGER run
         *         is usable either, which `limit_` remembers so the rest of the pass
         *         costs one compare per slot instead of one walk.
         */
        [[nodiscard]] bool reach(std::size_t want) {
            if (want > limit_) return false;
            if (want < k_) {  // a narrower slot after a wider one — restart the chain
                k_ = 0;
                h_ = kDigestSeed;
            }
            while (k_ < want) {
                const std::optional<std::string_view> s = at_(k_);
                if (!s || s->empty()) {
                    limit_ = k_;
                    return false;
                }
                h_ = fold_segment(h_, *s);
                ++k_;
            }
            return true;
        }

        /** @brief Digest of the first `reach`ed segments — comparable to `name_digest`. */
        [[nodiscard]] std::uint64_t digest() const noexcept { return h_; }

       private:
        SegAt& at_;
        std::size_t k_ = 0;
        std::uint64_t h_ = kDigestSeed;
        std::size_t limit_ = static_cast<std::size_t>(-1);
    };

    /**
     * @brief The live child whose qualified name is the LONGEST prefix of the `dst` segments
     *        @p at yields — the forward demux's entry point (#523).
     *
     * ONE pass over the table. Each slot is matched against the prefix of ITS OWN
     * @ref child_t::seg_count, so a mount of any width resolves and the descent never retries
     * a width: **O(N) slot visits, independent of how wide the widest mount is**. It replaces
     * a `k = W..1` loop that re-scanned the whole table at every width — O(W×N), measured at
     * 270 ns vs 25 ns for one scan at W=12, N=64 — and which, worse, could only ever match the
     * widths a compile-time constant enumerated (`kMountPeekMax`, deleted with this).
     *
     * Segments are read through @p at and compared against the stored key **in place**, so no
     * key is ever built and the hot path stays allocation-free (`bench_forward_heap`'s
     * `allocs=0` gate). Nothing here is sized by a width: there is no per-width array, no
     * peek window, and no constant to raise.
     *
     * LONGEST-MATCH-WINS is the contract, preserved exactly: the old loop started at the
     * widest key and returned the first hit, so a more specific mount always beat a shorter
     * one. Here the incumbent's width is the filter (`k <= best_k` cannot win), which is also
     * what keeps the pass cheap when a wide mount matches early.
     *
     * @tparam SegAt See @ref prefix_walk_t.
     * @return The matched slot, or nullptr when no registered mount prefixes the `dst`.
     */
    template <class SegAt>
    [[nodiscard]] const child_t* longest_prefix(SegAt&& at) const {
        // TWO PHASES, and the split is the whole performance story. Phase 1 is a scan whose
        // body is two loads and two compares and NOTHING else — no call, no string access, no
        // acquire load. Phase 2 confirms the single slot it picked.
        //
        // Written as one phase, with the confirm inline, the compiler stops inlining the scan
        // body and the WHOLE pass slows down — not just the matching slot. Measured on the
        // shipped `W = 3` shape: ~24 ns, i.e. ~20% of a forward hop, paid on a comparison that
        // by construction can only matter for one slot in the table.
        prefix_walk_t<SegAt> walk(at);
        const child_t* best = nullptr;
        std::size_t best_k = 0;
        // The chain state for the width the PREVIOUS slot had, held as plain locals. Registry
        // tables are overwhelmingly uniform in width — every RFC-0014 mount is
        // `net/<module>/<name>` — so the chain is folded once and every slot after the first
        // costs the two compares.
        std::size_t at_k = 0;
        std::uint64_t at_digest = 0;
        for_each([&](const child_t& c) {
            const std::size_t k = c.seg_count;
            if (k != at_k) {
                if (k == 0 || k <= best_k || !walk.reach(k)) return false;
                at_k = k;
                at_digest = walk.digest();
            }
            if (c.name_digest != at_digest) return false;
            best = &c;
            best_k = k;
            return false;  // keep going: a WIDER slot later in the table still wins
        });
        // Phase 2. The digest is a FILTER over three facts per segment, so this compare is the
        // DECISION, not a formality — and its closing total-length check is what rules that
        // the key is a SEGMENT prefix of the address rather than merely a byte prefix.
        if (best == nullptr) return nullptr;
        if (best->live() && matches_prefix(best->name, best_k, at)) return best;
        // The filter picked a slot that is not actually the answer: a digest collision, or a
        // slot tombstoned between the scan and here. Both are rare enough to be worth nothing
        // in the hot loop and both must still give the right answer, so the fallback repeats
        // the pass with the confirm inline — the shape phase 1 exists to keep OUT of the hot
        // loop, run only when the hot loop was wrong.
        return longest_prefix_confirmed(at);
    }

    /**
     * @brief @ref longest_prefix with the confirm INSIDE the pass — the cold fallback.
     *
     * Same contract, same answer, and it is the definition the fast path is an optimisation
     * of. Reached only when the digest filter's pick fails to confirm.
     */
    template <class SegAt>
    [[nodiscard]] const child_t* longest_prefix_confirmed(SegAt& at) const {
        prefix_walk_t<SegAt> walk(at);
        const child_t* best = nullptr;
        std::size_t best_k = 0;
        for_each([&](const child_t& c) {
            const std::size_t k = c.seg_count;
            if (k == 0 || k <= best_k || !walk.reach(k)) return false;
            if (c.name_digest != walk.digest()) return false;
            if (!c.live() || !matches_prefix(c.name, k, at)) return false;
            best = &c;
            best_k = k;
            return false;
        });
        return best;
    }

    /**
     * @brief @ref longest_prefix over a ready-made segment list — the control plane's form.
     *
     * `on_advertise` holds decoded `NAME` children, not a frame cursor, and `subscribe_toward`
     * holds a parsed `path_t`. Same descent, same answer: the two planes resolving a mount by
     * different rules is precisely what #516 was.
     */
    [[nodiscard]] const child_t* longest_prefix(std::span<const std::string_view> segs) const {
        return longest_prefix([segs](std::size_t i) -> std::optional<std::string_view> {
            if (i >= segs.size()) return std::nullopt;
            return segs[i];
        });
    }

    /**
     * @brief Resolve @p peer within THIS endpoint's own peer table (ADR-0061).
     *
     * The per-endpoint replacement for `by_name`'s global cross-bus scan: a peer segment
     * is resolved against the multi-peer child it was addressed *through*, so two servers'
     * same-named peers stay distinct and a peer is never reachable through the wrong
     * module. A point-to-point child resolves no peer at all.
     * @return The directed per-peer endpoint, or nullptr if this child has no such peer.
     */
    [[nodiscard]] static transport_t* resolve_peer(const child_t& child, std::string_view peer) {
        transport_t* const l = child.link.load(std::memory_order_acquire);
        if (!child.multi_peer.load(std::memory_order_relaxed) || l == nullptr) return nullptr;
        bus_link_t* const bus = l->bus();
        return bus == nullptr ? nullptr : bus->peer_link(peer);
    }

    /**
     * @brief Tombstone the link addressed by @p name — it stops resolving.
     *
     * Call in the same step the link/vertex is torn down, and BEFORE the `transport_t`
     * is destroyed, so no forward can resolve a freed object. The slot itself is kept
     * (see the class docs).
     * @return true if @p name named a live child, false if it named none.
     */
    bool erase(std::string_view name) {
        bool erased = false;
        for_each([&](const child_t& c) {
            if (c.live() && c.name == name) {
                const_cast<child_t&>(c).link.store(nullptr, std::memory_order_release);
                erased = true;  // keep going: belt-and-braces against any shadow slot
            }
            return false;
        });
        // A departed mount moves the split for every `dst` that used to descend through it,
        // so it restamps the label plane exactly as a registration does (#765).
        if (erased) bump_generation();
        return erased;
    }

    /**
     * @brief The link addressed by @p name (nullptr if none).
     *
     * Resolution order (ADR-0044): an exact static child NAME wins; otherwise each
     * registered BUS child (a link exposing @ref transport_t::bus) is asked to
     * resolve @p name as a currently-audible peer (@ref bus_link_t::peer_link),
     * yielding a DIRECTED per-peer endpoint. So an announced bus peer's name is a
     * routable next-hop segment with no registry mutation and no stored peer state
     * — the peer table lives inside the bus transport and expires with its traffic.
     */
    /** @brief The live child slot registered under exactly @p name (nullptr if none). */
    [[nodiscard]] const child_t* entry_by_name(std::string_view name) const {
        const child_t* hit = nullptr;
        for_each([&](const child_t& c) {
            if (c.live() && c.name == name) {
                hit = &c;
                return true;
            }
            return false;
        });
        return hit;
    }

    /**
     * @brief The link addressed by @p name (nullptr if none), peer fallback included.
     *
     * The identity lookup used off the mount-descent path (reply/advertise plumbing, which
     * addresses a link by its qualified name). Resolution order (ADR-0044): an exact child
     * NAME wins; otherwise each registered BUS child is asked to resolve @p name as a
     * currently-audible peer. Prefer @ref longest_prefix on the forward path, and
     * @ref resolve_peer for scoped peer resolution.
     */
    [[nodiscard]] transport_t* by_name(std::string_view name) const {
        transport_t* hit = nullptr;
        for_each([&](const child_t& c) {
            if (c.live() && c.name == name) {
                hit = c.link.load(std::memory_order_acquire);
                return true;
            }
            return false;
        });
        if (hit != nullptr) return hit;
        for_each([&](const child_t& c) {
            transport_t* const l = c.link.load(std::memory_order_acquire);
            if (l == nullptr) return false;  // tombstone (#494) — no link to ask
            if (bus_link_t* const bus = l->bus()) {
                if (transport_t* const peer = bus->peer_link(name)) {
                    hit = peer;
                    return true;
                }
            }
            return false;
        });
        return hit;
    }

    /** @brief The link whose NAME equals the raw segment bytes @p seg (nullptr if none). */
    [[nodiscard]] transport_t* by_segment(std::span<const std::byte> seg) const {
        return by_name(detail::as_string_view(seg));
    }

    /**
     * @brief Qualified name @p name pre-encoded as a run of NAME TLVs — the mount run.
     *
     * The same bytes a slot's `mount_tlv` holds, exposed so a link's receiver ctx can carry
     * its OWN copy and a forward hop need not scan the table to find them. It is a pure
     * function of @p name, so the copy cannot drift from the slot's. A control-plane call.
     */
    [[nodiscard]] static std::vector<std::byte> mount_run_for(std::string_view name) {
        return encode_mount_name(name);
    }

    /** @brief Number of slots — live children PLUS tombstones (test introspection). */
    [[nodiscard]] std::size_t size() const noexcept {
        std::size_t n = 0;
        for_each([&](const child_t&) {
            ++n;
            return false;
        });
        return n;
    }

    /** @brief Number of children that still resolve (test introspection). */
    [[nodiscard]] std::size_t live_size() const noexcept {
        std::size_t n = 0;
        for_each([&](const child_t& c) {
            if (c.live()) ++n;
            return false;
        });
        return n;
    }

    // The three digest helpers below are PUBLIC for one reason: `digest_name` and
    // `digest_segments` must agree exactly, and a disagreement is silent — the registry
    // would simply stop resolving. `registry_teardown_test` therefore pins them against
    // each other directly, which it cannot do through a lookup.
    /**
     * @brief A per-segment digest of a qualified name — the scan's cheap discriminator.
     *
     * Deliberately NOT a general hash. A general hash (FNV-1a was tried) walks every byte in
     * a serial xor-multiply chain, and on a short name that chain is ~24 dependent `imul`s —
     * MEASURED at ~30 ns, which is ~20% of a whole forward hop and is paid on EVERY lookup,
     * including the single-child case that has nothing to scan. It made the fixed cost worse
     * to make the scan cost better.
     *
     * This reads three cheap facts per segment — its length and its first and last byte — and
     * folds them with one multiply per segment. For a two-segment mount that is ~3 multiplies
     * instead of ~24, and it costs the same whether the names are 8 bytes or 80.
     *
     * It is a FILTER, never a decision: a collision costs one full compare, which is what the
     * scan did unconditionally before. The length pre-filter runs alongside it and catches a
     * different axis, so the two together leave very little for the compare to reject.
     */
    [[nodiscard]] static constexpr std::uint64_t fold_segment(std::uint64_t h,
                                                              std::string_view seg) noexcept {
        const std::uint64_t first = seg.empty() ? 0 : static_cast<unsigned char>(seg.front());
        const std::uint64_t last = seg.empty() ? 0 : static_cast<unsigned char>(seg.back());
        return (h + (seg.size() | (first << 8) | (last << 16))) * 0x9E37'79B9'7F4A'7C15ULL;
    }

    /** @brief The empty name's digest. */
    static constexpr std::uint64_t kDigestSeed = 0;

    /**
     * @brief Digest a stored qualified name (`"<module>/<name>"`) by splitting it on `/`.
     *
     * Runs once per @ref add — control plane — so the split costs nothing that matters. It
     * MUST produce what @ref digest_segments produces for the same name; `child_registry_test`
     * pins that agreement directly rather than only through a lookup, because a silent
     * disagreement would not fail loudly: it would simply stop resolving.
     */
    [[nodiscard]] static constexpr std::uint64_t digest_name(std::string_view name) noexcept {
        std::uint64_t h = kDigestSeed;
        std::size_t at = 0;
        while (true) {
            const std::size_t slash = name.find('/', at);
            h = fold_segment(h,
                             name.substr(at, slash == std::string_view::npos ? slash : slash - at));
            if (slash == std::string_view::npos) return h;
            at = slash + 1;
        }
    }

    /** @brief Digest @p segs — the same value @ref digest_name gives for them joined by `/`. */
    [[nodiscard]] static constexpr std::uint64_t digest_segments(
        std::span<const std::string_view> segs) noexcept {
        std::uint64_t h = kDigestSeed;
        for (const std::string_view seg : segs) h = fold_segment(h, seg);
        return h;
    }

   private:
    /**
     * @brief Encode qualified name @p name (`"a/b"`) as a run of NAME TLVs, one per segment.
     *
     * Canonical form (`opt = 0`, `u16` length) is chosen deliberately: this is EMITTING, so
     * there is no encoding variance to be wrong about — unlike MATCHING an inbound path,
     * where a peer may legally send `opt.LL=1` and byte comparison would break conformance
     * (ADR-0062 §Considered options). A segment too long for the length field yields an empty
     * run, and the hop falls back to encoding the name per-frame.
     */
    [[nodiscard]] static std::vector<std::byte> encode_mount_name(std::string_view name) {
        std::vector<std::byte> out;
        std::size_t at = 0;
        while (at <= name.size()) {
            const std::size_t slash = name.find('/', at);
            const std::string_view seg = name.substr(
                at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
            // Refuse before emitting: `emit_tlv` would auto-widen a body past 0xFFFF to a
            // 4-byte length, and this mount run is read back by offset against a fixed 4-byte
            // NAME header. With the check first the bytes match the hand-rolled push exactly
            // (ADR-0048 §3 — one representation of the header layout).
            if (seg.size() > 0xFFFFu) return {};
            wire::emit_name(out, seg);
            if (slash == std::string_view::npos) break;
            at = slash + 1;
        }
        return out;
    }

    /**
     * @brief True iff @p key equals the first @p k segments @p at yields, joined by `/`.
     *
     * Compared in place — no key is built, so the descent allocates nothing. This is the
     * CONFIRMATION, reached only for a candidate whose digest already matched — and it is a
     * real decision, not a formality: the digest is a filter over three facts per segment, so
     * the byte compare here and its closing `pos == key.size()` are what actually rule that
     * this key is this address's prefix.
     */
    template <class SegAt>
    [[nodiscard]] static bool matches_prefix(const std::string& key, std::size_t k,
                                             SegAt& at) noexcept {
        std::size_t pos = 0;
        for (std::size_t i = 0; i < k; ++i) {
            if (i != 0) {
                if (pos >= key.size() || key[pos] != '/') return false;
                ++pos;
            }
            const std::optional<std::string_view> s = at(i);
            if (!s) return false;
            // A plain bounded memcmp, NOT `key.compare(pos, n, sv)`: that overload range-checks
            // `pos` against a throwing precondition and then computes a min-length before it
            // compares anything, and on the confirm — which runs once per frame over every
            // segment of the matched mount — that measured several ns per segment against a
            // descent whose whole budget is ~130 ns.
            if (pos + s->size() > key.size()) return false;
            if (!s->empty() && std::memcmp(key.data() + pos, s->data(), s->size()) != 0)
                return false;
            pos += s->size();
        }
        return pos == key.size();
    }

    /** @brief Publish a mount-shape change (#765). Control plane only. */
    void bump_generation() noexcept { generation_.fetch_add(1, std::memory_order_release); }

    /**
     * @brief An append-only chunked list — the ADR-0063 container (#521).
     *
     * The table is NEVER erased from or reordered (teardown tombstones in place), so the
     * only mutations are APPEND and an in-place pointer null. That makes a chunked list the
     * natural shape: a slot's address is stable for this object's lifetime, a reader walking
     * it is unaffected by a concurrent append, and there is **no reclamation problem at all**
     * because nothing is ever freed before teardown.
     *
     * It replaces a `std::vector`, whose `push_back` reallocated and invalidated every slot
     * reference in the table — sound only while the registry was "immutable after setup", a
     * premise RFC-0014 ended by making connection create/remove a runtime operation.
     *
     * `kChunk` is an **allocation granularity, not a bound**: the list grows without limit,
     * so this introduces no synthetic cap (RFC-0006/0007, ADR-0051). Chunks are heap-allocated
     * on demand, so a node with no connections carries one pointer and nothing else.
     *
     * Publication: a slot is fully constructed BEFORE `used` is bumped with release, and a
     * reader acquires `used` before touching any slot — so a reader never observes a
     * half-built entry. The same release/acquire pairing publishes each new chunk.
     */
    static constexpr std::size_t kChunk = 4;

    struct chunk_t {
        child_t slots[kChunk];
        std::atomic<std::size_t> used{0};    /**< @brief Slots published in this chunk. */
        std::atomic<chunk_t*> next{nullptr}; /**< @brief Next chunk, or null. */
    };

    /** @brief Walk every published slot in order; @p fn returning true stops the walk. */
    template <class Fn>
    void for_each(Fn&& fn) const {
        for (chunk_t* c = head_.load(std::memory_order_acquire); c != nullptr;
             c = c->next.load(std::memory_order_acquire)) {
            const std::size_t n = c->used.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < n; ++i) {
                if (fn(c->slots[i])) return;
            }
        }
    }

    /** @brief Append a slot and publish it. Control plane only — serialized by the caller. */
    child_t* append() {
        chunk_t* c = head_.load(std::memory_order_relaxed);
        if (c == nullptr) {
            c = new (std::nothrow) chunk_t();
            if (c == nullptr) return nullptr;
            head_.store(c, std::memory_order_release);
        }
        for (;;) {
            const std::size_t n = c->used.load(std::memory_order_relaxed);
            if (n < kChunk) return &c->slots[n];  // caller fills, then calls publish()
            chunk_t* nxt = c->next.load(std::memory_order_relaxed);
            if (nxt == nullptr) {
                nxt = new (std::nothrow) chunk_t();
                if (nxt == nullptr) return nullptr;
                c->next.store(nxt, std::memory_order_release);
            }
            c = nxt;
        }
    }

    /** @brief Make the slot `append` handed back visible to readers (release). */
    void publish(const child_t* slot) {
        for (chunk_t* c = head_.load(std::memory_order_relaxed); c != nullptr;
             c = c->next.load(std::memory_order_relaxed)) {
            const std::size_t n = c->used.load(std::memory_order_relaxed);
            if (n < kChunk && slot == &c->slots[n]) {
                c->used.store(n + 1, std::memory_order_release);
                return;
            }
        }
    }

    std::atomic<chunk_t*> head_{nullptr};
    /** @brief The mount-shape generation (#765) — see @ref mount_generation. */
    std::atomic<std::uint32_t> generation_{1};
};

}  // namespace tr::net
