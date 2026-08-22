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
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "libtracer/edge_pin.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief `tr::graph` subscription edges: slot table, dispatch snapshots and the
 *        published edge array (M3b, #380 §3, #635).
 */

namespace tr::graph {

// L1 types this layer consumes (upward dependency on tr::view, docs/adr/0016 §2).
using view::rope_t;
using view::view_t;

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
 * `graph_t::admit_subscriber`); @ref reliability, @ref priority and @ref delivery_class are
 * stored and read back, awaiting the fan-out-edge and receiver-ring work that honours them —
 * the honest shape RFC-0022 §3.E chose over moving dead per-vertex fields.
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
 * crosses as the rope it is (ADR-0053 §6); the sink may clone links (refcount bumps).
 */
using subscriber_fn_t = void (*)(void* ctx, const rope_t& value);

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
    /**
     * @brief This node's NAME for the link the subscribe arrived on.
     *
     * FIRST, and the member ORDER below is the retired `pub_remote_t`'s, not this record's
     * historical one. That is deliberate and load-bearing: unifying the two halves means the
     * DELIVERY path (`graph_t::dispatch_edge_remote` since #1448; `vertex_t`'s published-entry
     * copy before it) reads this record instead of a published copy, and keeping the offsets
     * it reads at exactly where they were is what kept that loop's instruction stream
     * identical across #1442. The slot-side readers (`edge_view_of`, `evict_link_edges`,
     * `evict_route_edges`) move their displacements instead — control-plane paths, none of
     * them pinned.
     */
    std::string link;
    /**
     * @brief The consumer's accumulated return route (a complete PATH TLV's bytes — the FWD
     *        `src` the subscribe arrived with).
     *
     * A write hands (@ref link, this route, @ref delivery_compact, value) to the graph's
     * injected remote-delivery sink, which emits the `FWD{WRITE}` (or auto-promoted COMPACT)
     * back over the link (RFC-0004 §D/§E.1, ADR-0035 slice 4 / #136).
     *
     * @ref link is the discriminator, not this field: `graph_t::dispatch_edge` takes its
     * remote leg on a non-empty link and reads this route without testing it. The two agree
     * because the admitting door enforces it — `graph_t::subscribe_wire` refuses an empty
     * route as `INVALID_PATH` (#1055), and the `:subscribers[]` field-write arm, which binds
     * no route, deliberately leaves @ref link empty (see the note at that door: assigning a
     * link there would manufacture exactly the routeless delivery this invariant excludes).
     * So on a published edge the two are populated together or not at all, and testing either
     * one answers "is this subscriber remote?". Held as a view over a REFCOUNTED segment (ADR-0041
     * §2): copied once at subscribe, then every delivery snapshot is a refcount clone —
     * O(1) copies over the subscription's life, and an in-flight delivery keeps the route
     * alive across a concurrent unsubscribe. An opaque view, so L4 never depends on tr::net.
     */
    view_t return_route{};
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
    view_t reverse_route{};
    /**
     * @brief The caller context this edge was created under (#81, ADR-0026 fan-in gate).
     *
     * The inbound link NAME for a remote subscribe, empty for a locally-wired edge. A
     * fan-out re-dispatch into a LOCAL target vertex is gated by the TARGET's `:acl` WRITE
     * right under this context — the subscription's creator is the "writer" the target
     * authorizes. A REMOTE subscriber's fan-in gate runs on the peer instead (its
     * `FWD{WRITE}` terminus checks the same right).
     */
    std::string caller;
    /**
     * @brief Route-handle opt-in (`SUBSCRIBER.qos_settings.delivery_compact`, RFC-0004
     *        §E.1 / ADR-0035 slice 4).
     *
     * When true the consumer requests label-compacted deliveries: the producer MAY
     * advertise a per-link label aliasing this subscriber's return route and thereafter
     * stream lean COMPACT frames instead of full-route `FWD{WRITE}` deliveries. Default
     * false ⇒ stateless full-route delivery, so a cold/one-shot flow allocates no label
     * state.
     */
    bool delivery_compact = false;
    /**
     * @brief Intrusive refcount (#1442): how many holders name this record — the slot, plus
     *        one per PUBLISHED edge array whose entry points at it.
     *
     * **Rides the record's existing TAIL PADDING and therefore costs zero bytes.**
     * @ref delivery_compact ends at offset 113 and the record is 8-aligned, so a 4-byte
     * counter lands at 116 and `sizeof` stays the pinned 120 B. That is why the shape is an
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
     * already uses, `LIBTRACER_NO_ATOMIC` fallback included.
     */
    view::detail::ref_count_t refs{1};
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
    /** @brief Release this reference; the last one out frees the record. */
    ~remote_ptr_t() { reset(); }

    /** @brief Drop this reference (acq_rel) and empty the handle; frees the record at zero. */
    void reset() noexcept {
        if (p_ != nullptr && p_->refs.dec_acq_rel() == 1) delete p_;
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
    subscriber_remote_t* p_ = nullptr; /**< @brief The shared record, or null. */
};

/**
 * @brief A subscription edge's canonical PATH key — **immutable and refcount-shared**.
 *
 * Shared rather than owned because the dispatch snapshot must outlive a concurrent
 * unsubscribe: @ref vertex_t::snapshot_edges copies each active slot out under an edge pin
 * so the graph can dispatch after releasing it, and the slot may be cleared in between. A
 * deep copy satisfied that and cost a **malloc + free per edge per delivery** — a
 * `std::vector` has no small-buffer optimisation, so every non-null key allocated, which is
 * the ordinary local-binding case (`/sensor/temp:subscribers[] -> /dev/ctrl0/in/temp`).
 * Refcounting satisfies it for an atomic increment instead, exactly as
 * @ref edge_view_t::remote does one field over for the same hazard (#1448 — the whole cold
 * half went the same way, so the snapshot now takes two refcounts and copies no bytes).
 *
 * Null ⇒ no local re-dispatch target (the callback-only or remote-only edge). The key is
 * built once at admission and never mutated, so sharing it needs no synchronization beyond
 * the control block's own refcount.
 */
using target_key_t = std::shared_ptr<const std::vector<std::byte>>;

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
 * @brief Wrap @p key as a shared `target_key_t`, NOTHROW — null on OOM or empty input.
 *
 * Mirrors `vertex_t::try_make_lkv`'s probe-then-commit discipline (#477): under the MCU
 * profile a `bad_alloc` is an `abort()`, and admission is reachable from a peer's bytes
 * (RFC-0014 made registration wire-driven), so this soft-fails by value instead.
 * @param key The canonical PATH key bytes; an empty span yields null (no target).
 */
[[nodiscard]] inline target_key_t try_make_target_key(std::vector<std::byte>&& key) noexcept {
    if (key.empty()) return nullptr;
#if defined(__cpp_exceptions)
    try {
        return std::make_shared<const std::vector<std::byte>>(std::move(key));
    } catch (...) {
        return nullptr;  // only the control-block allocation can throw (the move is noexcept)
    }
#else
    // Declared inside the branch that uses it: at -Wextra an unconditional definition is an
    // unused variable on every exception-enabled build, which is most of CI.
    static constexpr std::size_t kCtrlSlack = 4 * sizeof(void*);  // >= both mainstream ABIs
    if (!tr::detail::probe_bytes(sizeof(std::vector<std::byte>) + kCtrlSlack)) return nullptr;
    return std::make_shared<const std::vector<std::byte>>(std::move(key));
#endif
}

/**
 * @brief One subscription edge (M3b).
 *
 * A write to the owning vertex fans out to a target vertex (@ref target_key —
 * spec-faithful re-dispatch) and/or an in-process @ref callback (sugar), per
 * docs/reference/02 §dispatch + 04 §write fanout. An inactive slot models an unsubscribe
 * (a cleared `:subscribers[N]`). The wire/gate members live in the lazily-allocated
 * @ref remote half (#380 §3), so the plain in-process edge costs 80 B, not 160.
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
    view_t source_view{};
    /** @brief The cold wire/gate half (#380 §3) — null for the plain in-process edge;
     *         allocated by @ref ensure_remote when a route/link/caller/compact-flag is
     *         stored (pay-for-what-you-use, ADR-0021). SHARED with every published entry
     *         that names this slot (#1442), never copied into one. */
    remote_ptr_t remote;
    /** @brief This subscription's DELIVERY policy (RFC-0022 §3.A) — the packed 16 bits its
     *         `SUBSCRIBER.SETTINGS{ NAME "delivery_policy" }` carried, or all-zero when it
     *         carried none. HOT, not in the cold `remote` half: `durability_request` is read
     *         under the same lock hold that appends the slot, and it rides free in the
     *         padding beside @ref active. */
    delivery_policy_t policy{};
    /** @brief Active flag; an active edge receives every propagated value (delivery is
     *         value-agnostic — WHICH vertices a sweep propagates is the vertex's
     *         `delivery_mode_t`, never a per-subscriber byte comparison). */
    bool active = true;

    /** @brief A blank edge (an inert slot shell, or a door's scratch record). */
    subscriber_t() = default;
    /** @brief NOT copyable, exactly as it was while the cold half was a `std::unique_ptr`
     *         (#380 §3). The handle that replaced it IS copyable — that is the point — so
     *         the ban is stated rather than inherited: a copied slot would share a cold half
     *         that @ref ensure_remote is then entitled to write. */
    subscriber_t(const subscriber_t&) = delete;
    /** @brief NOT copy-assignable — see the copy constructor. */
    subscriber_t& operator=(const subscriber_t&) = delete;
    /** @brief Movable: what the slot verbs do (append, reuse, reclaim-in-place). */
    subscriber_t(subscriber_t&&) = default;
    /** @brief Move-assignable: `subs[idx] = std::move(...)` is the reclaim. */
    subscriber_t& operator=(subscriber_t&&) = default;
    /** @brief Releases this slot's reference to the shared cold half. */
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
     */
    subscriber_remote_t& ensure_remote() {
        if (remote == nullptr) remote = remote_ptr_t::adopt(new subscriber_remote_t{});
        assert(remote.use_count() == 1 &&
               "the subscription cold half is immutable once published (#1442)");
        return *remote.mutable_get();
    }
};

/**
 * @brief The edge record's WIDTH, pinned (#1266).
 *
 * Both halves are per-edge storage on a node whose subscriber arena is measured in
 * kilobytes, and the hot half is what the fan-out loop streams — #380 §3 split the cold
 * members out for exactly that reason, and the split is only worth anything while the hot
 * record stays narrow. These numbers moved silently more than once (a member added to the
 * wrong half costs nothing a test can see until the RAM census runs), so they are stated
 * where a change to either struct has to walk past them.
 *
 * 64-bit hosts only: the widths are pointer-sized-member sums, so an MCU build legitimately
 * differs and a `sizeof` pin there would be a false alarm rather than a guard.
 *
 * #1442 moved the cold half from owned-per-holder to refcount-shared and **both numbers are
 * unchanged**: the handle is one pointer, as the `std::unique_ptr` was, and the intrusive
 * count fits the cold record's pre-existing tail padding. A future member that pushes
 * @ref subscriber_remote_t past 120 B evicts the counter into a word of its own and costs 8,
 * not 4 — that is the growth this pin is here to price.
 */
static_assert(sizeof(void*) != 8 || sizeof(subscriber_t) == 80,
              "the HOT edge record is 80 B — see #380 §3; move new members to the cold half");
static_assert(sizeof(void*) != 8 || alignof(subscriber_t) == 8,
              "the HOT edge record is 8-aligned; it lives in a vector");
static_assert(std::is_nothrow_move_constructible_v<subscriber_t>,
              "the slot vector grows by MOVE; a throwing move would copy — and the copy is "
              "deleted");
static_assert(sizeof(void*) != 8 || sizeof(subscriber_remote_t) == 120,
              "the COLD edge half is 120 B — price any growth against the per-edge RAM");
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
 * The width matters on its own: 160 B → 48 B. @ref edge_snapshot_t is `kCapacity` of these
 * on the publishing thread's stack, and a wide fan-out streams `F` of them through the
 * overflow vector, which is the `F * sizeof(edge_view_t)` term `bench/bench_common.hpp`
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
     * @brief This edge's remote-delivery link NAME; empty ⇒ no remote leg.
     *
     * Borrowed from the record this view holds, so it is valid for as long as the view is —
     * which is exactly as long as the `std::string` member it replaces was. Empty for an
     * in-process edge AND for the `:subscribers[]` field-write arm, which binds a caller
     * context but deliberately no link (there is no return route to deliver over).
     */
    [[nodiscard]] std::string_view link() const noexcept {
        const subscriber_remote_t* r = remote.get();
        return r != nullptr ? std::string_view(r->link) : std::string_view{};
    }
    /** @brief The edge's stored ACL fan-in context (#81), borrowed from the held record;
     *         empty for a locally-wired edge. */
    [[nodiscard]] std::string_view caller() const noexcept {
        const subscriber_remote_t* r = remote.get();
        return r != nullptr ? std::string_view(r->caller) : std::string_view{};
    }
    /**
     * @brief Does this edge have a REMOTE-delivery leg — i.e. a non-empty @ref link?
     *
     * The gate `graph_t::dispatch_edge` takes per edge, kept as one named test because it is
     * on the always-inlined per-edge body of the wide fan-out loop. The null check
     * short-circuits, so the in-process edge — the bulk of any fan-out — pays one load and
     * one branch, exactly what `link.empty()` cost when the string was inline.
     */
    [[nodiscard]] bool has_remote_leg() const noexcept {
        const subscriber_remote_t* r = remote.get();
        return r != nullptr && !r->link.empty();
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
    std::shared_ptr<const rope_t> value; /**< @brief The latched LKV; null ⇒ no latch. */
    edge_view_t edge;                    /**< @brief The admitted edge's dispatch view. */
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
    /** @brief The snapshot width (mirrored as `vertex_t::kInlineFanout`). */
    static constexpr std::size_t kCapacity = 8;

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
    /** @brief Destroy every constructed view; the buffer is reusable afterwards. */
    void clear() noexcept {
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
 * @brief One entry of a PUBLISHED edge array: the hot dispatch fields plus a liveness bit.
 *
 * Written once, before the array is published, and never touched again — that is what lets a
 * reader copy it out with no lock. The `active` bit is the ONE mutable word, and it is
 * MONOTONE: it starts true and an unsubscribe (@ref vertex_t::clear_edge,
 * @ref vertex_t::evict_link_edges, retirement) flips it to false under the stripe lock. A
 * reader loads it and skips the entry.
 *
 * That single mutable bit is not a hedge on immutability, it removes a failure mode. Without
 * it every unsubscribe would have to BUILD a smaller array, and an unsubscribe that cannot
 * allocate would be left publishing an edge the caller has already torn its `callback_ctx`
 * down behind. With it, dropping an edge is allocation-free and therefore infallible; the
 * compaction that actually reclaims the dropped entry's refcount clones rides the next
 * successful publish, where a failure costs nothing but a delayed release.
 */
struct pub_edge_t {
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
};

/**
 * @brief A vertex's published, immutable-after-publish edge array (#635) — what
 *        @ref vertex_t::snapshot_edges copies out under an edge pin instead of the stripe
 *        mutex.
 *
 * One allocation: this header immediately followed by `count` inline @ref pub_edge_t. The
 * array is never resized, never reordered and never freed while any participant announces it
 * (`%edge_pin.hpp`); a control-plane mutation publishes a NEW array and pushes this one onto
 * the owning block's retire list. `retire_next` is touched only once the array is off the
 * slot, so it never races the readers.
 */
struct alignas(pub_edge_t) edge_pub_t {
    edge_pub_t* retire_next = nullptr; /**< @brief Retire-list link (off-slot only). */
    std::uint32_t count = 0;           /**< @brief Constructed entries following this header. */

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
[[nodiscard]] inline edge_pub_t* alloc_edge_pub(std::size_t n) noexcept {
    void* raw = ::operator new(sizeof(edge_pub_t) + n * sizeof(pub_edge_t), std::nothrow);
    if (raw == nullptr) return nullptr;
    return ::new (raw) edge_pub_t{};
}

/** @brief Destroy an edge array's constructed entries and free it. Null-safe. */
inline void destroy_edge_pub(edge_pub_t* p) noexcept {
    if (p == nullptr) return;
    pub_edge_t* e = p->entries();
    for (std::uint32_t i = p->count; i-- > 0;) e[i].~pub_edge_t();
    p->~edge_pub_t();
    ::operator delete(static_cast<void*>(p));
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
 * @ref slots is the MASTER: the `:subscribers[N]` slot table, index-stable per RFC-0009 §D.2,
 * mutated only under the vertex stripe mutex exactly as it was before #635. @ref pub is the
 * dispatch-side projection of it that publishers read without any lock.
 */
struct edge_block_t {
    std::vector<subscriber_t> slots;       /**< @brief The master slot table (stripe-locked). */
    std::atomic<edge_pub_t*> pub{nullptr}; /**< @brief The published array (null ⇒ no edges). */
    std::atomic<edge_pub_t*> retired{nullptr}; /**< @brief Displaced arrays awaiting a scan. */

    edge_block_t() = default;
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
        destroy_edge_pub(pub.exchange(nullptr, std::memory_order_acq_rel));
        for (edge_pub_t* p = retired.exchange(nullptr, std::memory_order_acq_rel); p != nullptr;) {
            edge_pub_t* next = p->retire_next;
            destroy_edge_pub(p);
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
            destroy_edge_pub(p);
        }
        p = next;
    }
    while (keep != nullptr) {
        edge_pub_t* next = keep->retire_next;
        retire_push(b.retired, keep);
        keep = next;
    }
}

}  // namespace tr::graph
