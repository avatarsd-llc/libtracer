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
 * Layering (ADR-0016): this lives in `tr::net` (L5) and holds `transport_t*` — it is
 * NOT `graph.find` against the L4 vertex map, because an L4 `vertex_t` must never
 * know about a transport (`vertex.hpp`). ADR-0037 §Stage-2 phrased the dissolution as
 * "graph.find(child)"; the layering-safe realization is this single tr::net-owned
 * registry the router consults, which achieves the same "no duplicated children-table"
 * without inverting the L4↔L5 dependency (see ADR-0038 §Brick-3a note).
 */
#pragma once

#include <atomic>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv.hpp"
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
 * @ref by_segments, which compares the two parts in place and never builds a key.
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
        /** @brief True while this slot still resolves — i.e. it is not a tombstone. */
        [[nodiscard]] bool live() const noexcept {
            return link.load(std::memory_order_acquire) != nullptr;
        }
        /** @brief The mount path PRE-ENCODED as a run of NAME TLVs (#508), built once here so
         *         a forward hop emits the grown `src` prefix as ONE span with no per-segment
         *         work — and so no fixed buffer bounds how long a NAME may be. */
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
        std::vector<std::byte> mount = encode_mount_name(name);
        child_t* hit = nullptr;
        for_each([&](const child_t& c) {
            if (c.name == name) {
                hit = const_cast<child_t*>(&c);
                return true;
            }
            return false;
        });
        if (hit != nullptr) {
            hit->multi_peer.store(multi_peer, std::memory_order_relaxed);
            hit->mount_tlv = std::move(mount);
            hit->link.store(&link, std::memory_order_release);
            return;
        }
        child_t* const slot = append();
        if (slot == nullptr) return;  // allocation failure — soft-fail, nothing registered
        slot->name = std::move(name);
        slot->multi_peer.store(multi_peer, std::memory_order_relaxed);
        slot->mount_tlv = std::move(mount);
        slot->link.store(&link, std::memory_order_release);
        publish(slot);
    }

    /**
     * @brief The live child whose qualified name equals @p segs joined by `/` (null if none).
     *
     * The forward demux's entry point. @p segs are raw segment spans read straight out of
     * the inbound frame (`<module>`, `<name>`, and for a bus peer `<peer>`); they are
     * compared against the stored key **in place**, so no key is ever built and the hot
     * path stays allocation-free (`bench_forward_heap`'s `allocs=0` gate).
     */
    [[nodiscard]] const child_t* by_segments(std::span<const std::string_view> segs) const {
        // The joined length is a property of `segs`, not of any child — so it is computed
        // ONCE here rather than per candidate. It was inside `matches`, which meant every
        // slot in the table re-ran a loop over the segments just to derive the number it
        // would compare its own size against. Hoisting it leaves the per-slot work at a
        // single integer compare for the (overwhelming) majority that cannot match.
        std::size_t need = segs.empty() ? 0 : segs.size() - 1;
        for (const std::string_view s : segs) need += s.size();
        const child_t* hit = nullptr;
        for_each([&](const child_t& c) {
            if (c.live() && c.name.size() == need && matches(c.name, segs)) {
                hit = &c;
                return true;
            }
            return false;
        });
        return hit;
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
     * currently-audible peer. Prefer @ref by_segments on the forward path, and
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
            if (seg.size() > 0xFFFFu) return {};
            out.push_back(static_cast<std::byte>(std::to_underlying(wire::type_t::NAME)));
            out.push_back(std::byte{0});
            out.push_back(static_cast<std::byte>(seg.size() & 0xFF));
            out.push_back(static_cast<std::byte>((seg.size() >> 8) & 0xFF));
            for (const char c : seg) out.push_back(static_cast<std::byte>(c));
            if (slash == std::string_view::npos) break;
            at = slash + 1;
        }
        return out;
    }

    /**
     * @brief True iff @p key equals @p segs joined by `/`, compared without allocating.
     *
     * Callers that scan a table pre-filter on the joined LENGTH (see @ref by_segments) —
     * this still re-derives it, so the function is correct standalone, but on the scan path
     * it only ever runs for a candidate whose size already matched.
     */
    [[nodiscard]] static bool matches(const std::string& key,
                                      std::span<const std::string_view> segs) noexcept {
        std::size_t need = segs.empty() ? 0 : segs.size() - 1;
        for (const std::string_view s : segs) need += s.size();
        if (key.size() != need) return false;
        std::size_t at = 0;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (i != 0 && key[at++] != '/') return false;
            if (key.compare(at, segs[i].size(), segs[i]) != 0) return false;
            at += segs[i].size();
        }
        return true;
    }

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
};

}  // namespace tr::net
