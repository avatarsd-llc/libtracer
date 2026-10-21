/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A vertex's SUBSCRIPTION edges (M3b) — the whole fan-out data plane below `vertex_t`:
 * the per-subscription delivery policy, the hot/cold subscriber record split (#380 §3),
 * the minted local target binding (#830), the dispatch snapshot types, and the published,
 * immutable-after-publish edge array with its edge-pin retire machinery (#635).
 *
 * Extracted from `vertex.hpp` (#868). The split follows the one seam that already existed
 * in that header: everything here is edge state, reachable from `edge_block_t`, and
 * `vertex_t` owns exactly one lazily-allocated block of it. The types are ordered
 * SLOT-SIDE first (what a subscribe writes under the stripe lock) then PUBLISH-SIDE (what
 * a delivery reads under an edge pin), because that is the direction the data flows.
 */
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "libtracer/config.hpp"
#include "libtracer/edge_pin.hpp"
#include "libtracer/link_id.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/mem_string.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/value.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief `tr::graph` subscription edges: slot table, dispatch snapshots and the
 *        published edge array (M3b, #380 §3, #635).
 */

namespace tr::graph {

/**
 * @brief The per-subscription DELIVERY CLASS — bits 6–7 of the packed `delivery_policy` word
 *        (RFC-0025 §4.1, as amended).
 *
 * The class says how a producer's fan-out edge treats this ONE subscriber's deliveries. It is
 * a subscription property, never a vertex flag: the signal plane is demand-driven and
 * init-free, so no vertex ever declares "I am a stream" (RFC-0025 §3, claim 5).
 */
enum class delivery_class_t : std::uint8_t {
    /** @brief `0` — last-wins. Delivery MAY coalesce to the newest value. The LKV contract,
     *         today's behaviour, and the default an absent word decodes to. */
    CONFLATE = 0,
    /** @brief `1` — every write delivered as its own event; order-preserving, never
     *         conflated, no producer-side accumulation. */
    IMMEDIATE = 1,
    /** @brief `2` — the wire encoding of RFC-0008's `assign`/`propagate` flush (RFC-0025
     *         §4.1.2): a flush emits the SNAPSHOT on a plain value and the FULL since-flush
     *         LIST on a STREAM, as one BATCH record (`batch.hpp`). Accumulation is the source
     *         vertex's own state — never a per-subscriber buffer at the fan-out edge. */
    BATCH = 2,
    /** @brief `3` — append-preserving: every write delivered in order, none conflated, with
     *         the §4.4 pressure contract binding at the RECEIVING vertex's ring. */
    STREAM = 3,
};

/**
 * @brief One subscription's DELIVERY policy — a packed 16-bit field carried in the
 *        `SUBSCRIBER` TLV's `SETTINGS` child (RFC-0022 §3.A).
 *
 * Delivery policy describes **one producer→subscriber relationship**, not the producer: a
 * vertex that fans out to a CAN peer and a WebSocket peer at once has no single
 * `reliability` or `priority` to hold, which is why these lived on the vertex for a year
 * without anything ever consuming them. DDS puts the same three on the reader/writer pair
 * for the same reason.
 *
 * | bits | field | values |
 * | ---: | --- | --- |
 * | 0–1 | reliability | 0 = best-effort, 1 = reliable; 2–3 reserved |
 * | 2–4 | priority | 0–7, 0 = default |
 * | 5 | durability_request | 1 = deliver the latched last value on join |
 * | 6–7 | delivery_class | 0 = conflate (default), 1 = immediate, 2 = batch, 3 = stream |
 * | 8–15 | reserved | MUST be written 0, MUST be ignored on read |
 *
 * Absent from the wire ⇒ all-zero ⇒ today's default behaviour, byte-identically — and the
 * class field costs no wire byte for the same reason: `0` is conflate, which is what every
 * pre-RFC-0025 subscriber wrote into those bits when they were reserved. Old subscribers are
 * conflate-class BY CONSTRUCTION.
 *
 * Only @ref durability_request is consumed today (the transient-local latch at
 * `graph_t::admit_subscriber`). @ref priority and @ref delivery_class are stored and read
 * back, awaiting the work that honours them — the honest shape RFC-0022 §3.E chose over
 * moving dead per-vertex fields. @ref reliability is **not** awaiting anything: RFC-0025's
 * 2026-08-24 §4.4 selector erratum (#1204) rules the pressure arm to be the RECEIVING
 * vertex's own declaration (`vertex_policy_t::ring_source`), so these two bits are **carried
 * verbatim and read by nothing** — decoded, stored, read back from `:subscribers[]` and
 * re-emitted unchanged, with no behaviour behind them and none promised.
 *
 * **Flags only, never a magnitude.** A deadline or a queue bound added later is a magnitude
 * and belongs in the subscription's cold half as a full-width field, never in these bits.
 */
struct delivery_policy_t {
    std::uint16_t bits = 0; /**< @brief The packed field, as it arrived off the wire. */

    static constexpr std::uint16_t kReliabilityMask = 0x0003;   /**< @brief Bits 0–1. */
    static constexpr std::uint16_t kPriorityMask = 0x001C;      /**< @brief Bits 2–4. */
    static constexpr int kPriorityShift = 2;                    /**< @brief Bits 2–4 offset. */
    static constexpr std::uint16_t kDurabilityRequest = 0x0020; /**< @brief Bit 5. */
    static constexpr std::uint16_t kDeliveryClassMask = 0x00C0; /**< @brief Bits 6–7. */
    static constexpr int kDeliveryClassShift = 6;               /**< @brief Bits 6–7 offset. */

    /** @brief 0 = best-effort, 1 = reliable (2–3 reserved; stored, never interpreted). */
    [[nodiscard]] constexpr std::uint8_t reliability() const noexcept {
        return static_cast<std::uint8_t>(bits & kReliabilityMask);
    }
    /** @brief 0–7, 0 = default. */
    [[nodiscard]] constexpr std::uint8_t priority() const noexcept {
        return static_cast<std::uint8_t>((bits & kPriorityMask) >> kPriorityShift);
    }
    /** @brief True iff THIS subscriber asked for the latched last value on join. */
    [[nodiscard]] constexpr bool durability_request() const noexcept {
        return (bits & kDurabilityRequest) != 0;
    }
    /**
     * @brief Bits 6–7 — how the fan-out edge treats this subscriber's deliveries
     *        (RFC-0025 §4.1).
     *
     * Every two-bit pattern is an assigned class, so this accessor is total: there is no
     * "unknown class" to reject, and a word from a future sender still decodes to one of the
     * four. Reading the field is not honouring it — the classes beyond `CONFLATE` land with
     * the fan-out-edge mechanics and the receiving vertex's ring.
     */
    [[nodiscard]] constexpr delivery_class_t delivery_class() const noexcept {
        return static_cast<delivery_class_t>((bits & kDeliveryClassMask) >> kDeliveryClassShift);
    }

    /** @brief Memberwise equality on the raw bits (reserved bits included — they are
     *         carried verbatim, so two policies differing only there are not the same
     *         bytes). */
    bool operator==(const delivery_policy_t&) const = default;
};

/**
 * @brief The in-process per-edge delivery sink: a plain `{fn, ctx}` pair (the ADR-0047
 *        hot-path shape, same doctrine as `tr::net::receiver_slot_t`).
 *
 * Snapshotting one under the fan-out lock is a trivial copy — no per-publish
 * `std::function` copy (which heap-allocates once captures exceed the SBO). The value
 * crosses as the published block it is (`value_t`, RFC-0028 §5.1) — no wrapper, no copy;
 * the sink reads its links in place and may clone them (`value_t::rope`, refcount bumps)
 * if it keeps them past the call.
 */
using subscriber_fn_t = void (*)(void* ctx, const value_t& value);

/**
 * @brief The COLD wire/gate half of a subscription edge (#380 §3), lazily allocated and
 *        **refcount-shared, immutable after admission** (#1442): the in-process edge — the
 *        common MCU wiring shape (callback or local target, empty caller) — keeps
 *        `subscriber_t::remote` null and pays one pointer instead of ~90 B of route/link/
 *        caller state per edge.
 *
 * **Build then freeze.** An admission door fills one of these through
 * @ref subscriber_t::ensure_remote while the record is still private to its stack-local
 * `subscriber_t`; the slot verb then moves it in, and from the first
 * `%vertex_t::try_publish_edges` onward the record is READ-ONLY. Nothing in the tree
 * writes it after admission — `index_link_vertex`'s key choice, `evict_link_edges`' link
 * compare, `evict_route_edges`' route compare, `edge_view_of` and the dispatch snapshot
 * (`%vertex_t::copy_published`, #1448) are all reads — which is what makes sharing it
 * correct rather than merely cheap.
 *
 * That immutability is the whole fix for #1442. Before it, a republish DEEP-COPIED this
 * record into a fresh `pub_remote_t` per pre-existing entry, per admission — one nothrow
 * `operator new` plus up to two `std::string` heap copies each — and `%scan_retired_edges`
 * freed them all again on the next pass. Measured at ~940 instructions per pre-existing edge
 * against a ~158 inherent floor at 65 links (`bench/README.md`, *Where the whole-subscribe
 * growth goes*), i.e. ~83 % of the constant spent reproducing bytes byte-identical to the
 * ones being retired. A republish now copies a pointer and increments @ref refs.
 */
struct subscriber_remote_t {
    /** @brief An empty record whose name and own block come from @p src (#1885). */
    explicit subscriber_remote_t(mem::block_source_t& src) noexcept : caller(src) {}

    /**
     * @brief The link the subscribe arrived on, as a pair (`%link_pair_t`: its connection
     *        vertex's slot, and the bus peer under it) — the edge's SESSION, the key its
     *        departure evicts it by (#1941, #1622 L6).
     *
     * Every edge a link carried in stores it: a `subscribe_wire` edge delivers over it (or
     * over the mount its target routes through), and a `:subscribers[N]` replace a link
     * carried stores it with NO route, because it delivers to a local target (#2135). A pair
     * here therefore does not by itself make an edge remote — @ref return_route does.
     *
     * It used to be this node's NAME for the link, a `mem::string_t` the delivery sink then
     * resolved by scanning the registry's names and, for a bus peer, every bus's peers. It is
     * now what that scan was looking for: twelve bytes, no block of their own, and a pair
     * deref at the sink. The NAME is asked of the link object when it is needed, which is
     * never on the delivery path: by the session seam's per-edge `RELEASE`
     * (`graph_hooks_t::link_name`), and by the departure index, which the admitting door keys
     * by the name it was handed.
     *
     * FIRST, where the retired `pub_remote_t` put its link, so the DELIVERY path
     * (`graph_t::dispatch_edge_remote`) reads its first word where it always did. The record
     * is returned to the source its @ref caller draws from (`caller.source()`), so it carries
     * no pointer of its own for it.
     */
    link_pair_t link{};
    /**
     * @brief The consumer's accumulated return route (a complete PATH TLV's bytes — the FWD
     *        `src` the subscribe arrived with).
     *
     * A write hands (@ref link, this route, value) to the graph's injected remote-delivery
     * sink, which emits the `FWD{WRITE}` back over the link (RFC-0004 §D, #136).
     *
     * THIS field is the remote-leg discriminator (#2135): `graph_t::dispatch_edge` takes its
     * remote leg on a non-empty route, and only then reads @ref link to deliver over.
     * `graph_t::subscribe_wire` refuses an empty route as `INVALID_PATH` (#1055), so every
     * routed edge has both; the `:subscribers[]` field-write arm binds no route, so its edge
     * has no remote leg even when it stores its arrival link as its session. Before #2135
     * the link was the discriminator, which is why that arm could not store one: a name with
     * no route would have bought one `FWD{WRITE}` per publish with an empty `dst`. Held as a view
     * over a REFCOUNTED segment (ADR-0041 §2): copied once at subscribe, then every delivery
     * snapshot is a refcount clone — O(1) copies over the subscription's life, and an in-flight
     * delivery keeps the route alive across a concurrent unsubscribe. An opaque view, so L4 never
     * depends on tr::net.
     */
    view::view_t return_route{};
    /**
     * @brief The COMPLETED reverse-direction bound route (RFC-0024 §7.1 amendment 1) — a
     *        `PATH_REF` TLV whose element 0 is THIS node's own reference to the connection
     *        vertex the subscribe arrived on; empty ⇒ the subscription is canonical-only.
     *
     * Stored at admission by `graph_t::subscribe_wire` when the mint-flagged subscribe
     * carried a reverse list the responder could complete. On every delivery the producer
     * consumes element 0 locally — validates it against its OWN vertex map (§6.2's re-check)
     * and egresses through the vertex it dereferences to — and puts elements `1..` on the
     * wire as the delivery's bound `dst`. A failed local validation (the link re-dialled;
     * the generation moved) falls back to the canonical @ref return_route, which is always
     * stored alongside — the reverse binding is an optimisation plus a liveness check, never
     * the only route. Same ownership shape as @ref return_route — one refcounted copy at
     * subscribe, refcount clones per delivery snapshot.
     */
    view::view_t reverse_route{};
    /**
     * @brief The caller context this edge was created under (#81, ADR-0026 fan-in gate).
     *
     * The inbound link NAME for a remote subscribe, empty for a locally-wired edge. A
     * fan-out re-dispatch into a LOCAL target vertex is gated by the TARGET's `:acl` WRITE
     * right under this context — the subscription's creator is the "writer" the target
     * authorizes. A REMOTE subscriber's fan-in gate runs on the peer instead (its
     * `FWD{WRITE}` terminus checks the same right).
     */
    mem::string_t caller;
    /**
     * @brief Intrusive refcount (#1442): how many holders name this record — the slot, plus
     *        one per PUBLISHED edge array whose entry points at it.
     *
     * **Rides the record's existing TAIL PADDING and therefore costs zero bytes.**
     * @ref caller ends at offset 96 and the record is 8-aligned, so a 4-byte counter lands
     * at 96 and `sizeof` stays the pinned 104 B (120 B before #1941 stored the link as a
     * pair rather than a name). That is why the shape is an
     * intrusive count and not a `std::shared_ptr`: a 16-byte handle would have widened
     * @ref subscriber_t (pinned at 80 B) AND @ref pub_edge_t, whose width was measured at
     * **+23 %** on the fan-out-1024 publish the last time it grew — the fix would have been
     * paid for out of the delivery path.
     *
     * Not a synchronization primitive for the PAYLOAD. The payload is written once, before
     * the record is ever named by a published array, and the seq_cst exchange that publishes
     * that array is what makes those bytes visible to a pinned reader — exactly the ordering
     * the deep copy relied on. The COUNT is atomic because it is genuinely contended: two
     * mutators can be inside `%scan_retired_edges` at the same time (each pops a disjoint
     * retire list after releasing the stripe lock) and both may drop the last reference to
     * the same record — and since #1448 the pinned reader touches it too: the dispatch
     * snapshot (`%vertex_t::copy_published`) CLONES the handle, a relaxed increment taken
     * while the pin guarantees the entry's own reference still holds the count above zero.
     *
     * `%tr::view::detail::ref_count_t` is the in-tree primitive @ref tr::view::segment_ptr_t
     * already uses, guarded binding included (a core with no atomic RMW, #1722).
     */
    view::detail::ref_count_t refs{1};
    /**
     * @brief The local vertex's retention depth when this edge was admitted, saturated at
     *        65535 — the depth of the per-peer ring its deliveries wait in (ADR-0087 §2,
     *        #2144; @ref delivery_lane_t). `1` is a SAMPLE.
     *
     * Rides the tail padding after @ref refs with @ref lane_reliable, so `sizeof` stays the
     * pinned 104 B. Written once at admission, like every other field here.
     */
    std::uint16_t lane_depth = 1;
    /** @brief The local vertex's §4.4 arm when this edge was admitted (see @ref lane_depth). */
    bool lane_reliable = false;
};

/**
 * @brief Intrusive owning handle for a @ref subscriber_remote_t — ONE pointer, copy = clone.
 *
 * The `%tr::view::segment_ptr_t` shape, applied to the subscription cold half (#1442): copy is
 * a relaxed increment, destruction is an acq_rel decrement, and the last holder out frees the
 * record. Eight bytes, so it drops into @ref subscriber_t and @ref pub_edge_t exactly where
 * the `std::unique_ptr` it replaces sat and neither pinned width moves.
 *
 * Const by default. `operator->` yields a `const` record because every holder except the
 * admitting door is a reader; the one mutable escape (@ref mutable_get) is named so that a
 * write to a published record cannot be typed by accident.
 */
class remote_ptr_t {
   public:
    /** @brief An empty handle — the plain in-process edge's cold half (none). */
    remote_ptr_t() noexcept = default;

    /** @brief Adopt a freshly built record's INITIAL reference without bumping (`refs == 1`).
     *  @param p The record, or null (which yields an empty handle). */
    [[nodiscard]] static remote_ptr_t adopt(subscriber_remote_t* p) noexcept {
        remote_ptr_t h;
        h.p_ = p;
        return h;
    }

    /** @brief Clone — one more reference to the same immutable record (relaxed increment). */
    remote_ptr_t(const remote_ptr_t& other) noexcept : p_(other.p_) {
        if (p_ != nullptr) p_->refs.inc_relaxed();
    }
    /** @brief Transfer @p other's reference, leaving it empty. */
    remote_ptr_t(remote_ptr_t&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    /** @brief Copy-and-swap assignment — one operator covers copy- and move-assign. */
    remote_ptr_t& operator=(remote_ptr_t other) noexcept {
        std::swap(p_, other.p_);
        return *this;
    }
    /**
     * @brief Release this reference; the last one out frees the record.
     *
     * Always inlined for the same reason @ref reset is: the fan-out destroys one handle per
     * edge per delivery. With only `reset` pinned, GCC 13.3 still outlined the destructor
     * itself when graph.cpp's inline budget moved, which put a call back on every edge of the
     * wide-fan-out teardown loop (+10 % p50 at `inproc/64/1024/1`, #1951).
     */
    [[gnu::always_inline]] ~remote_ptr_t() { reset(); }

    /**
     * @brief Drop this reference (acq_rel) and empty the handle; frees the record at zero.
     *
     * Always inlined, and small enough for that to be free: the null test and the decrement
     * only, with the record's destructor out of line in `%free_record`. Every `edge_view_t`
     * the fan-out copies and destroys runs this once per edge per DELIVERY, mostly on an
     * empty handle (a local edge, or a moved-from temporary). Left to the inliner, with the
     * `delete` inline, GCC outlined `reset()` whenever graph.cpp's inline budget moved, and
     * that put a call on every edge of the copy and teardown loops (+78 % p50 at fan-out
     * 1024 on `inproc/64/1024/1`, #1816).
     */
    [[gnu::always_inline]] void reset() noexcept {
        if (p_ != nullptr && p_->refs.dec_acq_rel() == 1) free_record(p_);
        p_ = nullptr;
    }

    /** @brief The shared record, read-only; null when this edge carries no cold half. */
    [[nodiscard]] const subscriber_remote_t* get() const noexcept { return p_; }
    /** @brief Member access on the shared record (read-only). */
    [[nodiscard]] const subscriber_remote_t* operator->() const noexcept { return p_; }
    /** @brief True iff this handle names a record. */
    [[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }
    /** @brief Null test, so `remote == nullptr` reads exactly as it did under
     *         `std::unique_ptr` at every call site that tests for the in-process edge. */
    [[nodiscard]] friend bool operator==(const remote_ptr_t& h, std::nullptr_t) noexcept {
        return h.p_ == nullptr;
    }
    /** @brief Holder count — the build-then-freeze assertion and diagnostics ONLY, never a
     *         synchronization primitive (an acquire load of a value another thread may be
     *         changing). */
    [[nodiscard]] std::uint_least32_t use_count() const noexcept {
        return p_ != nullptr ? p_->refs.load_acquire() : 0;
    }

    /**
     * @brief The shared record, MUTABLE — the admission door's BUILD phase and nothing else.
     *
     * Legal only while the caller is the record's sole holder, which is what
     * @ref subscriber_t::ensure_remote asserts. Once a publish has cloned the handle, the
     * record is bytes a pinned reader may be copying with no lock and no pin on the record
     * itself — a write here would be a data race the deep copy used to make impossible.
     */
    [[nodiscard]] subscriber_remote_t* mutable_get() const noexcept { return p_; }

   private:
    /** @brief The last holder's free, out of line and cold: it runs once per record
     *         lifetime (an unsubscribe), never per delivery, so its body stays off the
     *         per-edge path that inlines @ref reset. */
    [[gnu::noinline, gnu::cold]] static void free_record(subscriber_remote_t* p) noexcept {
        mem::drop_in(p->caller.source(), p);  // the record and its name share one source
    }

    subscriber_remote_t* p_ = nullptr; /**< @brief The shared record, or null. */
};

/**
 * @brief A subscription edge's canonical PATH key — **immutable and refcount-shared**, held
 *        in one block of the graph's table source (#1912).
 *
 * Shared rather than owned because the dispatch snapshot must outlive a concurrent
 * unsubscribe: @ref vertex_t::snapshot_edges copies each active slot out under an edge pin
 * so the graph can dispatch after releasing it, and the slot may be cleared in between. A
 * deep copy satisfied that and cost a **malloc + free per edge per delivery**. Refcounting
 * satisfies it for one count update instead, exactly as @ref edge_view_t::remote does one
 * field over for the same hazard (#1448).
 *
 * The `%remote_ptr_t` shape with the key's length carried in the handle: the count and the
 * owning source head the block and the key bytes follow them, so admission is ONE draw from
 * the source the edge's other state uses, and a refused draw is a value (null), never an
 * abort. Until #1912 this was a `std::shared_ptr` around a `std::vector`: two platform-heap
 * draws per admission, outside the seam. The handle is 16 B on a 64-bit host and 8 B on rv32,
 * the `std::shared_ptr`'s width, so @ref subscriber_t, @ref edge_view_t and @ref pub_edge_t
 * keep their pinned sizes. The count is `%tr::view::detail::ref_count_t`, which skips the
 * locked RMW while the process is single-threaded, as `std::shared_ptr` does.
 *
 * Null ⇒ no local re-dispatch target (the callback-only or remote-only edge). The key is
 * built once at admission and never mutated, so sharing it needs no synchronization beyond
 * the count.
 */
class target_key_t {
   public:
    /** @brief An empty handle — no local target. */
    target_key_t() noexcept = default;
    /** @brief An empty handle, spelled as the null it tests equal to. */
    target_key_t(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)

    /** @brief The one way to make a key: @ref try_make_target_key. */
    friend target_key_t try_make_target_key(mem::block_source_t& src,
                                            std::span<const std::byte> key) noexcept;

    /** @brief Clone — one more reference to the same immutable key. */
    target_key_t(const target_key_t& other) noexcept : p_(other.p_), size_(other.size_) {
        if (p_ != nullptr) p_->refs.inc_relaxed();
    }
    /** @brief Transfer @p other's reference, leaving it empty. */
    target_key_t(target_key_t&& other) noexcept
        : p_(std::exchange(other.p_, nullptr)), size_(std::exchange(other.size_, 0)) {}
    /** @brief Copy-and-swap assignment — one operator covers copy- and move-assign. */
    target_key_t& operator=(target_key_t other) noexcept {
        std::swap(p_, other.p_);
        std::swap(size_, other.size_);
        return *this;
    }
    /** @brief Release this reference; the last one out returns the block. */
    ~target_key_t() { reset(); }

    /** @brief Drop this reference and empty the handle. Always inlined for the reason
     *         `%remote_ptr_t::reset` gives: the fan-out runs it once per edge per delivery. */
    [[gnu::always_inline]] void reset() noexcept {
        if (p_ != nullptr && p_->refs.dec_acq_rel() == 1) free_block(p_, size_);
        p_ = nullptr;
        size_ = 0;
    }

    /** @brief The key bytes; empty for a null handle. */
    [[nodiscard]] std::span<const std::byte> operator*() const noexcept {
        if (p_ == nullptr) return {};
        return {reinterpret_cast<const std::byte*>(p_ + 1), size_};
    }
    /** @brief True iff this handle names a key. */
    [[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }
    /** @brief Null test. */
    [[nodiscard]] friend bool operator==(const target_key_t& h, std::nullptr_t) noexcept {
        return h.p_ == nullptr;
    }

   private:
    /** @brief The block's head: the count and the source the block returns to. */
    struct head_t {
        view::detail::ref_count_t refs; /**< @brief Holders of this key. */
        mem::block_source_t* source;    /**< @brief Where the block came from. */
    };

    /** @brief The last holder's release, out of line and cold (once per key lifetime). */
    [[gnu::noinline, gnu::cold]] static void free_block(head_t* p, std::uint32_t size) noexcept {
        mem::block_source_t* const src = p->source;
        p->~head_t();
        src->release(p, sizeof(head_t) + size, alignof(head_t));
    }

    head_t* p_ = nullptr;    /**< @brief The shared block, or null. */
    std::uint32_t size_ = 0; /**< @brief The key's length in bytes. */
};

/**
 * @brief The sentinel slot index meaning "this edge carries no binding" (#830).
 *
 * Not `0`: slot 0 is the graph root, a real index. A separate `bool` would cost the same
 * word after padding and admits the state "bound, but to nothing".
 */
inline constexpr std::uint32_t kUnboundTargetSlot = 0xFFFFFFFFu;

/**
 * @brief A subscription edge's minted binding to its local target vertex (#830, RFC-0024
 *        machinery used node-locally).
 *
 * `dispatch_edge_target` re-resolved `target_key` from the root on EVERY delivery, and
 * `find_ptr` is linear in address depth (+5.75 ns/segment measured; the full terminus leg
 * +21.3 ns/segment, because a deep dst also scales the arena decode, the mount peek and the
 * descent). A slot deref is flat at ~11 ns. So the edge carries the pair minted at admission
 * and the canonical walk becomes the FALLBACK, taken whenever the binding is absent (the
 * target did not exist yet, or was unmintable) or stale (`bound_generation_matches` refuses
 * it after a retire).
 *
 * This is a cache of an ANSWER, never of a permission: the deref revalidates the generation
 * and the registered bit, and `dispatch_edge_target` still evaluates `acl_allows` at the
 * deref'd vertex per delivery — the RFC-0024 §6.2 hot-path rule. Mismatch drops to canonical
 * resolution rather than delivering, so the edge can never misroute into a successor tenant.
 */
struct target_binding_t {
    std::uint32_t index = kUnboundTargetSlot; /**< @brief Node-scoped vertex slot index. */
    std::uint32_t generation = 0;             /**< @brief The slot generation at mint time. */

    /** @brief True iff a mint succeeded for this edge. */
    [[nodiscard]] constexpr bool bound() const noexcept { return index != kUnboundTargetSlot; }
};

/**
 * @brief Copy @p key into one block of @p src as a shared `target_key_t`, NOTHROW — null on
 *        refusal or empty input (#1912).
 *
 * Admission is reachable from a peer's bytes (RFC-0014 made registration wire-driven), so a
 * refused draw answers by value and the caller refuses the admission as BACKPRESSURE.
 * @param src The graph's table source; the block returns to it when the last holder drops.
 * @param key The canonical PATH key bytes; an empty span yields null (no target).
 */
[[nodiscard]] inline target_key_t try_make_target_key(mem::block_source_t& src,
                                                      std::span<const std::byte> key) noexcept {
    target_key_t h;
    if (key.empty()) return h;
    void* const raw =
        src.try_alloc(sizeof(target_key_t::head_t) + key.size(), alignof(target_key_t::head_t));
    if (raw == nullptr) return h;
    h.p_ = ::new (raw) target_key_t::head_t{view::detail::ref_count_t{1}, &src};
    std::memcpy(reinterpret_cast<std::byte*>(h.p_ + 1), key.data(), key.size());
    h.size_ = static_cast<std::uint32_t>(key.size());
    return h;
}

/**
 * @brief One subscription edge as an admission door builds it (M3b) — a stack record, never
 *        stored whole (#1622).
 *
 * A write to the owning vertex fans out to a target vertex (@ref target_key —
 * spec-faithful re-dispatch) and/or an in-process @ref callback (sugar), per
 * docs/reference/02 §dispatch + 04 §write fanout. The wire/gate members live in the
 * lazily-allocated @ref remote half (#380 §3).
 *
 * `vertex_t::add_edge` and `replace_edge` take one and split it between the vertex's two
 * tables, so each field is kept exactly once: the dispatch fields (callback pair, target key,
 * binding, cold half) go into the slot's @ref pub_edge_t, which the fan-out reads, and the rest
 * (@ref source_view, @ref policy) into its @ref subscriber_slot_t. Until #1622 the slot table
 * kept this whole record as well, 80 B per slot of which 48 B repeated the published entry.
 */
struct subscriber_t {
    target_key_t target_key;            /**< @brief Canonical PATH key (null ⇒ callback-only). */
    target_binding_t binding{};         /**< @brief Minted slot for @ref target_key (#830). */
    subscriber_fn_t callback = nullptr; /**< @brief In-process sink fn; null ⇒ target-only
                                             (ADR-0053 §6 rope value). */
    void* callback_ctx = nullptr;       /**< @brief Caller-owned context passed back to
                                             @ref callback; must outlive every delivery. */
    /**
     * @brief The original SUBSCRIBER TLV view this slot was written from, retained zero-copy
     *        (a refcount clone of the field-write payload).
     *
     * Empty for in-process callback sugar that carries no TLV (the local target sugar DOES
     * carry one — ADR-0049 encodes through the field-write door). A `:subscribers[]` read
     * ropes these slot views into the `FWD{REPLY}` with no byte copy (RFC-0004 §D /
     * ADR-0035 slice 2 zero-copy reply rule). Stays HOT (outside @ref remote) precisely
     * because local field-write-door edges carry it.
     */
    view::view_t source_view{};
    /** @brief The cold wire/gate half (#380 §3) — null for the plain in-process edge;
     *         allocated by @ref ensure_remote when a route/link/caller is
     *         stored (pay-for-what-you-use, ADR-0021). SHARED with every published entry
     *         that names this slot (#1442), never copied into one. */
    remote_ptr_t remote;
    /** @brief This subscription's DELIVERY policy (RFC-0022 §3.A) — the packed 16 bits its
     *         `SUBSCRIBER.SETTINGS{ NAME "delivery_policy" }` carried, or all-zero when it
     *         carried none. HOT, not in the cold `remote` half: `durability_request` is read
     *         under the same lock hold that appends the slot, and it rides free in the
     *         record's tail padding. */
    delivery_policy_t policy{};

    /** @brief A blank edge (a door's scratch record). */
    subscriber_t() = default;
    /** @brief NOT copyable, exactly as it was while the cold half was a `std::unique_ptr`
     *         (#380 §3). The handle that replaced it IS copyable — that is the point — so
     *         the ban is stated rather than inherited: a copied slot would share a cold half
     *         that @ref ensure_remote is then entitled to write. */
    subscriber_t(const subscriber_t&) = delete;
    /** @brief NOT copy-assignable — see the copy constructor. */
    subscriber_t& operator=(const subscriber_t&) = delete;
    /** @brief Movable: a door hands its record to the edge verb by value. */
    subscriber_t(subscriber_t&&) = default;
    /** @brief Move-assignable, for symmetry with the move constructor. */
    subscriber_t& operator=(subscriber_t&&) = default;
    /** @brief Releases this record's reference to the shared cold half. */
    ~subscriber_t() = default;

    /**
     * @brief The cold half, allocated on first use (admission-time only — never on a
     *        dispatch path), and MUTABLE only because the caller is still its sole holder.
     *
     * The build phase of build-then-freeze (#1442). Every in-tree door fills a stack-local
     * @ref subscriber_t and only then hands it to `vertex_t::add_edge` / `replace_edge`, so
     * nothing has cloned the handle yet; the assertion states that rather than trusting it,
     * because a write reached after a publish would mutate bytes a pinned reader may be
     * copying under no lock.
     *
     * The record and its two names draw from @p src, the graph's table source (#1885).
     * @retval nullptr @p src refused the record; the slot is unchanged (BACKPRESSURE).
     */
    [[nodiscard]] subscriber_remote_t* ensure_remote(mem::block_source_t& src) noexcept {
        if (remote == nullptr)
            remote = remote_ptr_t::adopt(mem::make_in<subscriber_remote_t>(src, src));
        assert(remote.use_count() <= 1 &&
               "the subscription cold half is immutable once published (#1442)");
        return remote.mutable_get();
    }
};

/**
 * @brief The cold remainder of one `:subscribers[N]` slot — what the slot table keeps, and
 *        only what no publisher reads (#1622).
 *
 * The dispatch fields of an active slot live in exactly one place, the slot's entry in the
 * vertex's published `%edge_pub_t` array, which a mutation rebuilds from the array it
 * displaces. What remains here is read only under the stripe lock: the retained SUBSCRIBER
 * view a `:subscribers[N]` read hands back, the delivery policy, and whether the slot is
 * occupied at all. A cleared slot is a default-constructed one; the table never shrinks, because
 * RFC-0009 §D.2 makes the indices stable.
 */
struct subscriber_slot_t {
    /** @brief A view, a policy word and a flag: nothing points into the record, so the slot
     *         table grows by `memcpy` and its growth releases nothing
     * (`%mem::bitwise_relocatable`). */
    using bitwise_relocatable_t = void;
    /** @brief The SUBSCRIBER TLV view the slot was written from (@ref subscriber_t::source_view),
     *         empty for in-process sugar that carries none. */
    view::view_t source_view{};
    /** @brief The slot's delivery policy (@ref subscriber_t::policy). */
    delivery_policy_t policy{};
    /** @brief Is the slot occupied? Then the current published array holds exactly one entry
     *         naming it; a cleared slot may still have a dead one until the next republish. */
    bool active = false;
};

/**
 * @brief The edge records' WIDTHS, pinned (#1266, #1622).
 *
 * Every one of these is per-edge storage on a node whose subscriber arena is measured in
 * kilobytes. These numbers moved silently more than once (a member added to the wrong record
 * costs nothing a test can see until the RAM census runs), so they are stated where a change to
 * any of them has to walk past them. @ref subscriber_t is a door's stack record now, so its pin
 * prices the admission frame, not a table.
 *
 * #1442 moved the cold half from owned-per-holder to refcount-shared and left both of its
 * numbers unchanged: the handle is one pointer, as the `std::unique_ptr` was, and the intrusive
 * count fits the cold record's pre-existing tail padding. A future member that pushes
 * @ref subscriber_remote_t past 120 B evicts the counter into a word of its own and costs 8,
 * not 4 — that is the growth this pin is here to price.
 */
static_assert(sizeof(void*) != 8 || sizeof(subscriber_t) == 80,
              "the door's edge record is 80 B — see #380 §3; move new members to the cold half");
static_assert(sizeof(void*) != 8 || sizeof(subscriber_slot_t) == 32,
              "a slot is 32 B on LP64 — dispatch fields belong in pub_edge_t, once (#1622)");
static_assert(sizeof(void*) != 4 || sizeof(subscriber_slot_t) == 16,
              "a slot is 16 B on ILP32 — dispatch fields belong in pub_edge_t, once (#1622)");
static_assert(std::is_nothrow_move_constructible_v<subscriber_slot_t>,
              "the slot table grows by MOVE");
static_assert(sizeof(void*) != 8 || sizeof(subscriber_remote_t) == 104,
              "the COLD edge half is 104 B since its link is a pair, not a name (#1941) — "
              "price any growth against the per-edge RAM");
static_assert(sizeof(void*) != 8 || alignof(subscriber_remote_t) == 8,
              "the COLD edge half is 8-aligned; it is refcount-owned off to the side, so its "
              "address is stable across a slot-vector reallocation");
static_assert(sizeof(void*) != 8 || sizeof(remote_ptr_t) == 8,
              "the cold-half handle is ONE pointer — a wider one (a std::shared_ptr) is paid "
              "for out of pub_edge_t, on the delivery path");

/**
 * @brief The dispatch-relevant snapshot of one ACTIVE subscription edge — **four words and
 *        two refcounts**, no byte copy of anything (#1448).
 *
 * What @ref vertex_t::snapshot_edges copies out under an edge pin so the graph can dispatch
 * with the pin released (callbacks / re-dispatch re-enter the graph): the `{fn, ctx}`
 * callback pair, the minted binding, and refcount SHARES of the two owned records — the
 * target key and the whole cold half.
 *
 * **Why the cold half is shared here and not copied (#1448).** #1442 made
 * @ref subscriber_remote_t refcount-shared and immutable after admission, and #1447 spent
 * that on the SUBSCRIBE path (`%vertex_t::try_publish_edges`). This snapshot is the same
 * copy on the DELIVERY path, where it is paid once per edge **per write** rather than once
 * per admission: it used to own `std::string` copies of the link and the caller plus
 * refcount clones of the two routes, i.e. two probe-guarded assignments and two atomics for
 * every remote edge of every fan-out. It is now ONE relaxed increment, and the record it
 * names is exactly the bytes the copy used to reproduce. The lifetime guarantee the copies
 * bought — the slot may be cleared while dispatch runs outside the pin — is bought instead
 * by the share itself: this handle is a holder, so the record outlives the unsubscribe that
 * drops the slot's.
 *
 * That also makes the whole snapshot INFALLIBLE. Nothing in it can allocate (a `shared_ptr`
 * clone and an intrusive increment do not), so `%vertex_t::copy_published` no longer has a
 * per-edge OOM leg at all and @ref vertex_t::snapshot_drops_t no longer carries an
 * `out_of_memory` count — the shed it used to describe cannot happen.
 *
 * ADR-0041 §2 is satisfied more strongly than before, not stretched: its remote-subscriber
 * row asks for **one copy at subscribe into a refcounted segment** with every later delivery
 * *roping* the stored route rather than copying it. The routes already complied as `view_t`
 * clones; now the delivery does not even clone them, and nothing here is a borrowed span —
 * the handle owns.
 *
 * The width matters on its own: 160 B → 48 B. @ref edge_snapshot_t is
 * `kCapacity` of these on the publishing thread's stack, and a wide fan-out streams `F` of them
 * through the overflow vector, which is the `F * sizeof(edge_view_t)` term `bench/bench_common.hpp`
 * names as the reason the mid fan-out ladder exists.
 */
struct edge_view_t {
    subscriber_fn_t callback = nullptr; /**< @brief The in-process sink fn (null ⇒ none). */
    void* callback_ctx = nullptr;       /**< @brief The sink's caller-owned context. */
    target_key_t target_key; /**< @brief Local re-dispatch target (refcount share, not a copy). */
    target_binding_t binding{}; /**< @brief The minted slot for that target, or unbound (#830). */
    /** @brief The shared, immutable cold half (#1442) — null for the plain in-process edge.
     *         A HOLDER, not a borrow: it keeps the record alive for the whole dispatch, which
     *         is what the owning string copies used to do. */
    remote_ptr_t remote;

    /**
     * @brief The link this edge was admitted over (its session, @ref subscriber_remote_t::link);
     *        not valid for an in-process edge. It is the delivery link only when
     *        @ref has_remote_leg holds.
     */
    [[nodiscard]] link_pair_t link() const noexcept {
        const subscriber_remote_t* r = remote.get();
        return r != nullptr ? r->link : link_pair_t{};
    }
    /** @brief The edge's stored ACL fan-in context (#81), borrowed from the held record;
     *         empty for a locally-wired edge. */
    [[nodiscard]] std::string_view caller() const noexcept {
        const subscriber_remote_t* r = remote.get();
        return r != nullptr ? std::string_view(r->caller) : std::string_view{};
    }
    /**
     * @brief Does this edge have a REMOTE-delivery leg — i.e. a stored return route?
     *
     * The gate `graph_t::dispatch_edge` takes per edge, kept as one named test because it is
     * on the always-inlined per-edge body of the wide fan-out loop. The null check
     * short-circuits, so the in-process edge — the bulk of any fan-out — pays one load and
     * one branch, exactly what `link.empty()` cost when the link was an inline string. The
     * route, not the link, since #2135: a `:subscribers[N]` replace a link carried stores that link
     * as its session and delivers locally (`subscriber_remote_t::return_route`).
     */
    [[nodiscard]] bool has_remote_leg() const noexcept {
        const subscriber_remote_t* r = remote.get();
        return r != nullptr && !r->return_route.empty();
    }
};

/**
 * @brief The dispatch snapshot's WIDTH, pinned — the delivery path's bandwidth (#1448).
 *
 * `snapshot_edges` writes one of these per active edge on every fan-out, `kInlineFanout` of
 * them live on the publishing thread's stack, and a wide fan-out streams `F` through the
 * overflow vector. #844's mid ladder exists because that array outgrows L1 somewhere in the
 * 128→1024 gap, so the width is a measured hot-path quantity and not a housekeeping detail.
 * 160 B before #1448 (two `std::string`s and two `view_t`s inline), 48 B after.
 */
static_assert(sizeof(void*) != 8 || sizeof(edge_view_t) == 48,
              "the dispatch snapshot is 48 B — it is written once per edge per DELIVERY; "
              "put new per-edge state in the shared cold half, not here");

/**
 * @brief A transient-local durability latch (RFC-0004 §D / Q4): the LKV plus the freshly
 *        admitted edge's dispatch view, both snapshotted atomically with the append.
 *
 * @ref value stays null when no latch fired (the subscriber requested no durability —
 * RFC-0022 §3.A — or the producer holds no LKV yet).
 */
struct edge_latch_t {
    value_ref_t value; /**< @brief The latched LKV; empty ⇒ no latch. */
    edge_view_t edge;  /**< @brief The admitted edge's dispatch view. */
};

/**
 * @brief The fixed-capacity stack buffer of @ref edge_view_t dispatch views — the
 *        no-heap small-fan-out half of @ref vertex_t::snapshot_edges.
 *
 * The element storage is RAW (uninitialized) bytes: declaring one on the publish hot
 * path costs nothing, and only the views actually snapshotted are placement-constructed
 * (and destroyed). A default-constructed `std::array<edge_view_t, 8>` here instead
 * zeroed ~900 bytes of stack per publish — GCC lowers that to eight `rep stos` blocks
 * whose microcode startup latency dominated single-subscriber fan-out (the post-v0.3.0
 * fan1 delivery regression). Non-copyable; reused via @ref clear.
 */
class edge_snapshot_t {
   public:
    /** @brief The snapshot width: the build's `%tr::graph::kInlineFanout` (mirrored as
     *         `vertex_t::kInlineFanout`). */
    static constexpr std::size_t kCapacity = kInlineFanout;

    /** @brief An empty snapshot; the element storage stays uninitialized (the point). */
    edge_snapshot_t() noexcept = default;
    /** @brief Non-copyable — a transient dispatch buffer, never a value. */
    edge_snapshot_t(const edge_snapshot_t&) = delete;
    /** @brief Non-assignable — a transient dispatch buffer, never a value. */
    edge_snapshot_t& operator=(const edge_snapshot_t&) = delete;
    /** @brief Destroy the constructed views (only those actually snapshotted). */
    ~edge_snapshot_t() { clear(); }

    /** @brief Placement-construct @p v as the next view; the caller (the snapshot loop)
     *         keeps the count ≤ @ref kCapacity. */
    void push_back(edge_view_t v) {
        ::new (static_cast<void*>(raw_ + n_ * sizeof(edge_view_t))) edge_view_t(std::move(v));
        ++n_;
    }
    /** @brief Destroy every constructed view; the buffer is reusable afterwards.
     *
     *  Out of line, so the fan-out's frame keeps one call for the whole teardown and the
     *  inliner's choice for this loop cannot move `graph_t::fan_out`'s code (#1622). */
    [[gnu::noinline]] void clear() noexcept {
        for (std::size_t i = 0; i < n_; ++i) (*this)[i].~edge_view_t();
        n_ = 0;
    }
    /** @brief The number of views constructed. */
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    /** @brief The @p i-th snapshotted view (@p i < @ref size). */
    [[nodiscard]] edge_view_t& operator[](std::size_t i) noexcept {
        return *std::launder(reinterpret_cast<edge_view_t*>(raw_ + i * sizeof(edge_view_t)));
    }
    /** @brief The @p i-th snapshotted view (@p i < @ref size), const. */
    [[nodiscard]] const edge_view_t& operator[](std::size_t i) const noexcept {
        return *std::launder(reinterpret_cast<const edge_view_t*>(raw_ + i * sizeof(edge_view_t)));
    }

   private:
    /** @brief Uninitialized element storage (constructed views live at the front). */
    alignas(edge_view_t) std::byte raw_[kCapacity * sizeof(edge_view_t)];
    std::size_t n_ = 0; /**< @brief Constructed-view count. */
};

/**
 * @brief One entry of a PUBLISHED edge array: the hot dispatch fields, a liveness bit and the
 *        slot index the entry mirrors.
 *
 * **The ONE copy of the slot's dispatch fields (#1622).** The slot table keeps only the cold
 * remainder (@ref subscriber_slot_t); a republish copies the surviving entries out of the array
 * it displaces, plus the one edge an add or a replace brings, so no second copy of these fields
 * exists anywhere to fall out of step with this one.
 *
 * Written once, before the array is published, and never touched again — that is what lets a
 * reader copy it out with no lock. The `active` bit is the ONE mutable word. For a removed
 * edge it is MONOTONE: an unsubscribe or an eviction (@ref vertex_t::clear_edge,
 * @ref vertex_t::evict_link_edges) flips it to false under the stripe lock and nothing sets it
 * again. A suspend and a resume (@ref vertex_t::set_edge_suspended, #1533) flip it both ways,
 * also under that lock, and that is sound because the entry still names a LIVE edge: its
 * fields are immutable, nothing is released while it is suspended, and an occupied slot's entry
 * is always its current occupant's, because an add or a replace either publishes or changes
 * nothing. A reader loads the bit and skips the entry when it is clear.
 *
 * That single mutable bit is not a hedge on immutability, it removes a failure mode. Without
 * it every unsubscribe would have to BUILD a smaller array, and an unsubscribe that cannot
 * allocate would be left publishing an edge the caller has already torn its `callback_ctx`
 * down behind. With it, dropping an edge is allocation-free and therefore infallible; the
 * compaction that actually reclaims the dropped entry's refcount clones rides the next
 * successful publish, where a failure costs nothing but a delayed release.
 *
 * **The array carries only ACTIVE slots (#1533).** A cleared slot contributes no entry once the
 * next republish compacts it out, so the copy loop walks a dead one only in the window an
 * unsubscribe's refused compaction leaves open. A suspended slot KEEPS its entry, with the bit
 * clear: that
 * is what makes a toggle a flip — no republish, no allocation, no failure — at the price of one
 * skipped entry per write for each suspended edge (the load-and-skip the loop already does).
 * That is why the entry names its slot: the array no longer mirrors the slot table one-for-one,
 * so the flip finds its entry by @ref slot (entries are in slot order). The index rides the
 * padding after
 * `active` (32 bits on LP64, 16 on ILP32, see @ref slot_index_t), so the entry's width — the
 * copy loop's bandwidth, 56 B and 32 B — and the loop's own loads are unchanged on both.
 */
struct pub_edge_t {
    /**
     * @brief The slot-index width: whatever fits the padding after @ref active (#1533).
     *
     * Seven spare bytes on LP64 hold 32 bits; three on ILP32 (rv32, Cortex-M) hold 16 only, so
     * a narrow target caps a vertex at @ref kMaxSlot + 1 subscriber slots (`add_edge` refuses
     * past it) rather than widen every entry the copy loop walks by 4 B.
     */
    using slot_index_t = std::conditional_t<(sizeof(void*) >= 8), std::uint32_t, std::uint16_t>;
    /** @brief The highest slot index an entry can name. */
    static constexpr std::size_t kMaxSlot = std::numeric_limits<slot_index_t>::max();

    subscriber_fn_t callback = nullptr; /**< @brief In-process sink fn (null ⇒ target-only). */
    void* callback_ctx = nullptr;       /**< @brief The sink's caller-owned context. */
    target_key_t target_key;            /**< @brief Local re-dispatch target (refcount share). */
    target_binding_t binding{};         /**< @brief The minted slot for that target (#830). */
    /**
     * @brief The cold wire half (null for a local edge) — a refcount SHARE of the admitting
     *        slot's @ref subscriber_remote_t, never a copy of it (#1442).
     *
     * The entry's WIDTH is the fan-out copy loop's bandwidth, which is why this half is out of
     * line at all: inlining its members made an entry 136 B against `subscriber_t`'s 72 and
     * cost **+23 % on the fan-out-1024 publish** — measured, not predicted. Sharing keeps that
     * width exactly where it was (one pointer, as the `std::unique_ptr` here was) while making
     * a republish's per-entry cost a pointer copy and an increment instead of a heap
     * allocation and two `std::string` copies.
     */
    remote_ptr_t remote;
    std::atomic<bool> active{true}; /**< @brief Monotone true -> false liveness bit. */
    /** @brief The slot index this entry mirrors (#1533). Written before publish, never after;
     *         read only by the liveness flip, under the stripe lock. */
    slot_index_t slot = 0;
};

static_assert(sizeof(void*) != 8 || sizeof(pub_edge_t) == 56,
              "a published entry is 56 B on LP64 — the slot index rides the liveness padding");
static_assert(sizeof(void*) != 4 || sizeof(pub_edge_t) == 32,
              "a published entry is 32 B on ILP32 — the slot index rides the liveness padding");

/**
 * @brief A vertex's published, immutable-after-publish edge array (#635) — what
 *        @ref vertex_t::snapshot_edges copies out under an edge pin instead of the stripe
 *        mutex.
 *
 * One allocation: this header immediately followed by `count` inline @ref pub_edge_t, in slot
 * order, at most one per slot index: one per occupied slot (its bit clear while suspended),
 * plus a dead entry for any slot cleared since the last successful republish. The
 * array is never resized, never reordered and never freed while any participant announces it
 * (`%edge_pin.hpp`); a control-plane mutation publishes a NEW array and pushes this one onto
 * the owning block's retire list. `retire_next` is touched only once the array is off the
 * slot, so it never races the readers.
 */
struct alignas(pub_edge_t) edge_pub_t {
    edge_pub_t* retire_next = nullptr; /**< @brief Retire-list link (off-slot only). */
    std::uint32_t count = 0;           /**< @brief Constructed entries following this header. */
    std::uint32_t capacity = 0; /**< @brief Entries the block was sized for — what the sized
                                     release returns (#1778); rides the header's padding. */

    /** @brief The inline entry storage. */
    [[nodiscard]] pub_edge_t* entries() noexcept {
        return std::launder(reinterpret_cast<pub_edge_t*>(this + 1));
    }
    /** @brief The inline entry storage, const. */
    [[nodiscard]] const pub_edge_t* entries() const noexcept {
        return std::launder(reinterpret_cast<const pub_edge_t*>(this + 1));
    }
};

/**
 * @brief Allocate an UNFILLED edge array of @p n entries, NOTHROW.
 *
 * The caller placement-constructs the entries and bumps `count` as it goes, so a partially
 * filled array is always destroyable by `destroy_edge_pub`.
 * @return Null on OOM (#477 — the control-plane verb soft-fails; nothing is published).
 */
[[nodiscard]] inline edge_pub_t* alloc_edge_pub(mem::block_source_t& src, std::size_t n) noexcept {
    void* raw = src.try_alloc(sizeof(edge_pub_t) + n * sizeof(pub_edge_t), alignof(edge_pub_t));
    if (raw == nullptr) return nullptr;
    edge_pub_t* const p = ::new (raw) edge_pub_t{};
    p->capacity = static_cast<std::uint32_t>(n);
    return p;
}

/** @brief Destroy an edge array's constructed entries and return it to @p src, which served
 *         it. Null-safe. */
inline void destroy_edge_pub(mem::block_source_t& src, edge_pub_t* p) noexcept {
    if (p == nullptr) return;
    pub_edge_t* e = p->entries();
    for (std::uint32_t i = p->count; i-- > 0;) e[i].~pub_edge_t();
    const std::size_t bytes = sizeof(edge_pub_t) + p->capacity * sizeof(pub_edge_t);
    p->~edge_pub_t();
    src.release(p, bytes, alignof(edge_pub_t));
}

/**
 * @brief A vertex's edge state, allocated on FIRST subscribe and freed with the vertex.
 *
 * Pay-for-what-you-use, the #361 §1 discipline: an edgeless vertex — the overwhelming
 * majority on an MCU node — owns a single null pointer, where it used to own an empty
 * `std::vector` (24 B on a host, 12 on rv32). The block itself is never displaced or
 * reclaimed, only its published arrays are, which is why `snapshot_edges` may load it with a
 * plain acquire and no pin at all.
 *
 * Each slot field is stored ONCE (#1622). @ref slots is the `:subscribers[N]` slot table,
 * index-stable per RFC-0009 §D.2 and mutated only under the vertex stripe mutex; it holds the
 * cold remainder of each slot. @ref pub holds every occupied slot's dispatch fields, and is
 * what publishers read without any lock. The two agree because the only verbs that bring a new
 * edge (`vertex_t::add_edge`, `replace_edge`) publish it or change nothing.
 */
struct edge_block_t {
    /** @brief The slot table (stripe-locked). Its source served this block and serves every
     *         published array too (#1778). */
    mem::block_array_t<subscriber_slot_t> slots;
    std::atomic<edge_pub_t*> pub{nullptr}; /**< @brief The published array (null ⇒ no edges). */
    std::atomic<edge_pub_t*> retired{nullptr}; /**< @brief Displaced arrays awaiting a scan. */

    /** @brief The current array's entries, in slot order; empty when nothing is published.
     *         Call with the stripe lock held. */
    [[nodiscard]] std::span<pub_edge_t> published() const noexcept {
        edge_pub_t* const p = pub.load(std::memory_order_relaxed);
        return p == nullptr ? std::span<pub_edge_t>{}
                            : std::span<pub_edge_t>(p->entries(), p->count);
    }

    /** @brief An empty block drawing from @p src. */
    explicit edge_block_t(mem::block_source_t& src) noexcept : slots(src) {}
    edge_block_t(const edge_block_t&) = delete;
    edge_block_t& operator=(const edge_block_t&) = delete;

    /**
     * @brief The teardown flush: free the published array AND everything still parked.
     *
     * The contract this states, in the same shape ADR-0069 §6 states for the LKV domain: the
     * graph — and therefore every vertex — must OUTLIVE the threads that published through
     * it. A thread still inside `snapshot_edges` when its vertex is destroyed is a
     * use-after-free with or without this mechanism, and the ASan/LSan legs exercise the flush
     * on the joined-threads side of that line.
     */
    ~edge_block_t() {
        destroy_edge_pub(slots.source(), pub.exchange(nullptr, std::memory_order_acq_rel));
        for (edge_pub_t* p = retired.exchange(nullptr, std::memory_order_acq_rel); p != nullptr;) {
            edge_pub_t* next = p->retire_next;
            destroy_edge_pub(slots.source(), p);
            p = next;
        }
    }
};

/** @brief Push @p p onto @p head — a wait-free-on-x86 Treiber push; the mutator's thread
 *         runs it OUTSIDE every lock, so no free ever happens under the stripe mutex. */
inline void retire_push(std::atomic<edge_pub_t*>& head, edge_pub_t* p) noexcept {
    edge_pub_t* old = head.load(std::memory_order_relaxed);
    do {
        p->retire_next = old;
    } while (
        !head.compare_exchange_weak(old, p, std::memory_order_acq_rel, std::memory_order_relaxed));
}

/**
 * @brief Free every parked array no participant announces; park the rest again.
 *
 * NEVER waits. An array still pinned stays parked until the next mutation's scan or the
 * block's teardown flush, so parked memory is bounded by (arrays displaced while some reader
 * is mid-copy) — a control-plane rate times a sub-microsecond window. Run by the MUTATOR, on
 * its own thread, after the stripe lock is released: the destructors that run here belong to
 * library types only (an entry owns a target-key share and a @ref remote_ptr_t share — a
 * `subscriber_t::callback_ctx` is caller-owned and never destroyed by us), so no embedder code
 * can execute inside this machinery.
 *
 * TWO mutators can be in here at once — each `exchange`s the whole retire list, so they hold
 * disjoint arrays, but those arrays may share a cold half with each other and with the live
 * slot. That is precisely what @ref subscriber_remote_t::refs is atomic for; the free happens
 * once, on whichever thread drops the last reference.
 */
inline void scan_retired_edges(edge_block_t& b) noexcept {
    edge_pub_t* p = b.retired.exchange(nullptr, std::memory_order_acq_rel);
    edge_pub_t* keep = nullptr;
    while (p != nullptr) {
        edge_pub_t* next = p->retire_next;
        if (detail_ep::is_pinned(p)) {
            p->retire_next = keep;
            keep = p;
        } else {
            destroy_edge_pub(b.slots.source(), p);
        }
        p = next;
    }
    while (keep != nullptr) {
        edge_pub_t* next = keep->retire_next;
        retire_push(b.retired, keep);
        keep = next;
    }
}

/**
 * @brief A retired vertex's edges, detached whole: its slot table and the published array that
 *        holds the edges' dispatch fields (#1816, #1622).
 *
 * `vertex_t::revert_to_placeholder` MOVES both out under the stripe lock, which cannot fail and
 * releases nothing there; `graph_t::retire` then reads the routed edges' links with every graph
 * lock released, to give their holds back. The array is off the vertex but not yet on its retire
 * list, so nothing frees it while it is read here; a reader may still be copying it under a pin,
 * which is why the destructor retires it through the block's list instead of freeing it.
 */
class detached_edges_t {
   public:
    /** @brief Pointers and an array of them, none into the object, so the retire batch grows
     *         by `memcpy` and its growth reaches no destructor (`%mem::bitwise_relocatable`). */
    using bitwise_relocatable_t = void;
    /** @brief Nothing detached; the slot table, when one is moved in, keeps @p src. */
    explicit detached_edges_t(mem::block_source_t& src) noexcept : slots_(src) {}
    detached_edges_t(const detached_edges_t&) = delete;
    detached_edges_t& operator=(const detached_edges_t&) = delete;
    /** @brief Take @p other's edges, leaving it with none. */
    detached_edges_t(detached_edges_t&& other) noexcept
        : block_(std::exchange(other.block_, nullptr)),
          pub_(std::exchange(other.pub_, nullptr)),
          slots_(std::move(other.slots_)) {}
    detached_edges_t& operator=(detached_edges_t&&) = delete;
    /** @brief Hand the array back to its block's retire list and scan it. */
    ~detached_edges_t() {
        if (pub_ == nullptr) return;
        retire_push(block_->retired, pub_);
        scan_retired_edges(*block_);
    }

    /** @brief Detach @p b's array and slot table into this (empty) record. Call with the
     *         stripe lock held. */
    void take(edge_block_t& b) noexcept {
        block_ = &b;
        // seq_cst, as every exchange of the slot is (see `pin_t::acquire`).
        pub_ = b.pub.exchange(nullptr, std::memory_order_seq_cst);
        // The move leaves b's table empty with its source untouched: `scan_retired_edges`
        // reads that source outside the stripe lock, so nothing here may write it (#1919).
        slots_ = std::move(b.slots);
    }
    /** @brief Call @p fn once per edge that was occupying a slot (suspended ones included),
     *         skipping a cleared slot's dead entry. */
    template <typename F>
    void for_each_edge(F fn) const {
        if (pub_ == nullptr) return;
        for (const pub_edge_t& e : std::span<const pub_edge_t>(pub_->entries(), pub_->count))
            if (slots_[e.slot].active) fn(e);
    }

   private:
    edge_block_t* block_ = nullptr;               /**< @brief The block the array came from. */
    edge_pub_t* pub_ = nullptr;                   /**< @brief The detached array, or null. */
    mem::block_array_t<subscriber_slot_t> slots_; /**< @brief The detached slot table. */
};

}  // namespace tr::graph
