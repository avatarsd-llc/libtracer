/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * An L4 graph vertex: a named, addressable position holding a value, a bounded
 * history, or a user handler (docs/reference/11 §roles). Pinned in place (the
 * atomic LKV slot + mutex + condvar are non-movable); always handled via a
 * vertex_handle_t returned by graph_t::register_vertex (ADR-0056). The read/write LKV hot path is
 * lock-free (an atomic shared_ptr swap, the orderings M2 already pays for); the
 * mutex guards only the history ring, the subscriber list (M3b), and the await
 * waiter accounting.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "libtracer/path.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/status.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

// L1 types this layer consumes (upward dependency on tr::view, docs/adr/0016 §2).
using view::rope_t;
using view::segment_ptr_t;
using view::view_t;

/** @brief A vertex's behavioral role (docs/reference/11 §roles). */
enum class role_t {
    STORED_VALUE, /**< @brief Role 1: last-writer-wins; holds the last-written value. */
    STREAM,       /**< @brief Role 2: bounded history ring sized by `settings.history_keep_last`. */
    HANDLER,      /**< @brief Roles 3-7: user `on_read` / `on_write` supplies the behavior. */
};

/** @brief The mandatory core QoS fields of a vertex (docs/reference/02 §core writable fields). */
struct settings_t {
    std::uint8_t reliability = 0;        /**< @brief 0=best-effort, 1=reliable. */
    std::uint8_t durability = 0;         /**< @brief 0=volatile, 1=transient-local. */
    std::uint32_t history_keep_last = 1; /**< @brief Stream ring depth (>=1). */
    std::uint64_t deadline_ns = 0;       /**< @brief 0=off; max ns between writes before a
                                              liveness fault. */
    std::uint8_t priority = 0;           /**< @brief 0=low .. 255=critical (transport hint, not a
                                              wire bit). */
    std::uint32_t queue_max_bytes = 0; /**< @brief 0=unbounded; per-subscriber back-pressure cap. */
    /**
     * @brief Store-by-reference threshold (ADR-0042 §3): a view-delivered WRITE whose
     *        payload TLV is >= this many bytes (and carries no trailer bits) is stored as a
     *        SUBVIEW of the inbound frame (refcount pin, zero copy) instead of the one-copy
     *        trailer-sliced store. 0 (the default) DISABLES referencing — pinning
     *        amplification is a per-vertex deployment call.
     */
    std::uint32_t store_ref_min_bytes = 0;

    /** @brief Memberwise equality — lets registration detect all-defaults settings (which
     *         need no @ref vertex_ext_t allocation, ADR-0021 pay-for-what-you-use). */
    bool operator==(const settings_t&) const = default;
};

/** @brief The all-defaults @ref settings_t a vertex without a @ref vertex_ext_t reports —
 *         one shared constant, never a per-vertex copy. */
inline constexpr settings_t kDefaultSettings{};

/**
 * @brief User behavior for a Handler-role vertex.
 *
 * `on_children` additionally applies to ANY role: when set, a read of the vertex's
 * `:children[]` field serves this synthesized member listing (a complete POINT TLV view)
 * INSTEAD of enumerating registered child vertices — the ADR-0044 seam by which a
 * transport/connection vertex lists its live bus peers without ever creating a vertex for
 * them. The value seam is rope-typed (ADR-0053 §6): `on_read` supplies the vertex value as
 * the rope it is (a contiguous scalar is the single-link case), `on_write` receives the
 * written value without a flatten copy.
 */
struct handlers_t {
    std::function<result_t<rope_t>()> on_read; /**< @brief Supplies the vertex value on read. */
    std::function<result_t<void>(const rope_t&)>
        on_write;                                  /**< @brief Receives the written value. */
    std::function<result_t<view_t>()> on_children; /**< @brief Synthesized `:children[]` listing. */
};

/**
 * @brief One right bit of an ACE `access_mask` (docs/reference/05 §0x0A, ADR-0020).
 *
 * Single-bit values so a gate tests exactly one right; a stored mask may carry any
 * OR of them. `WRITE_ACL` is precisely the `admin` right (modify the ACL / delegate).
 */
enum class acl_right_t : std::uint32_t {
    READ = 0x01,        /**< @brief Read the vertex value / control fields. */
    WRITE = 0x02,       /**< @brief Write the vertex value / control fields (fan-in gate). */
    SUBSCRIBE = 0x04,   /**< @brief Append a `:subscribers[]` edge (fan-out gate). */
    CREATE = 0x08,      /**< @brief Create a child via `:children[]` (ADR-0017). */
    DELETE = 0x10,      /**< @brief Remove a child (reserved; no core surface yet). */
    READ_ACL = 0x20,    /**< @brief Read the `:acl` field. */
    WRITE_ACL = 0x40,   /**< @brief Modify the `:acl` field — the `admin` right. */
    WRITE_OWNER = 0x80, /**< @brief Transfer ownership (reserved; no core surface yet). */
};

/** @brief The one ACE flag the core subset honors: propagate to the subtree (ADR-0020). */
inline constexpr std::uint8_t kAceInherit = 0x1;

/** @brief An ACE's type (ADR-0020): ALLOW grants; DENY refuses (full policy only). */
enum class ace_type_t : std::uint8_t {
    ALLOW = 0, /**< @brief The ACE grants its mask's rights. */
    DENY = 1,  /**< @brief The ACE refuses them — evaluated only by `full_acl_policy_t`
                    (ADR-0050); the ALLOW-only profile rejects DENY at parse time. */
};

/**
 * @brief One parsed ACE of a vertex's `:acl` (ADR-0020 / #81).
 *
 * Evaluation is the pure per-target policy of ADR-0050 (`security_acl.hpp`): the
 * default ALLOW-only MCU profile rejects a DENY ACE (or any flag bit beyond
 * `kAceInherit`) at write time with TYPE_MISMATCH, so stored ACEs never carry
 * semantics the selected evaluator would silently weaken; the full `security_acl`
 * host policy (LIBTRACER_ACL_FULL) stores DENY and evaluates ordered
 * first-match-per-bit.
 */
struct ace_t {
    ace_type_t type = ace_type_t::ALLOW; /**< @brief ALLOW or DENY (policy-gated at parse). */
    std::uint8_t flags = 0;              /**< @brief ACE flags; only `kAceInherit` is accepted. */
    std::vector<std::byte> subject;      /**< @brief Opaque subject token (ADR-0018); the special
                                              subject `"EVERYONE@"` matches any resolved subject. */
    std::uint32_t access_mask = 0; /**< @brief Granted rights (an OR of `acl_right_t` bits). */
    std::uint64_t expires_ns = 0;  /**< @brief Absolute expiry, ns since the UNIX epoch;
                                        0 = never expires. An expired ACE grants nothing. */
};

/**
 * @brief Per-VERTEX propagation policy (value-agnostic; RFC-0008 §C).
 *
 * Governs whether an ANCESTOR's propagate sweep includes this vertex — NOT a
 * per-subscriber value filter (there is no byte comparison; ADR-0053 §1, a vertex never
 * parses its bytes). `assign` and a DIRECT propagate on the vertex itself are never gated
 * by it. Held as vertex state (default IF_NEWER); wire config via the vertex `:settings`
 * is deferred. Numeric filtering (deadband) remains an application filter vertex (ADR-0021
 * sibling), never a field here.
 */
enum class delivery_mode_t : std::uint8_t {
    /** @brief Default: an ancestor sweep includes this vertex only if it was assigned since
     *         the last covering sweep — the structural coalescing flush (RFC-0008 §B). */
    IF_NEWER = 0,
    /** @brief An ancestor sweep ALWAYS includes this vertex's current value (a sweep-driven
     *         keepalive; the producer's timer sets the rate). */
    UNCONDITIONAL = 1,
    /** @brief An ancestor sweep NEVER includes it; deliverable only by a direct propagate
     *         on the vertex itself. */
    EXPLICIT = 2,
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
 * @brief One subscription edge (M3b).
 *
 * A write to the owning vertex fans out to a target vertex (@ref target_key —
 * spec-faithful re-dispatch) and/or an in-process @ref callback (sugar), per
 * docs/reference/02 §dispatch + 04 §write fanout. An inactive slot models an unsubscribe
 * (a cleared `:subscribers[N]`).
 */
struct subscriber_t {
    std::vector<std::byte> target_key;  /**< @brief Canonical PATH key (empty ⇒ callback-only). */
    subscriber_fn_t callback = nullptr; /**< @brief In-process sink fn; null ⇒ target-only
                                             (ADR-0053 §6 rope value). */
    void* callback_ctx = nullptr;       /**< @brief Caller-owned context passed back to
                                             @ref callback; must outlive every delivery. */
    /** @brief Active flag; an active edge receives every propagated value (delivery is
     *         value-agnostic — WHICH vertices a sweep propagates is the vertex's
     *         `delivery_mode_t`, never a per-subscriber byte comparison). */
    bool active = true;
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
     * @brief The consumer's accumulated return route (a complete PATH TLV's bytes — the FWD
     *        `src` the subscribe arrived with).
     *
     * Empty together with @ref link ⇒ an in-process slot (callback/target sugar), ignored
     * for remote delivery. Populated ⇒ a REMOTE subscriber: a write hands (@ref link, this
     * route, @ref delivery_compact, value) to the graph's injected remote-delivery sink,
     * which emits the `FWD{WRITE}` (or auto-promoted COMPACT) back over the link (RFC-0004
     * §D/§E.1, ADR-0035 slice 4 / #136). Held as a view over a REFCOUNTED segment (ADR-0041
     * §2): copied once at subscribe, then every delivery snapshot is a refcount clone —
     * O(1) copies over the subscription's life, and an in-flight delivery keeps the route
     * alive across a concurrent unsubscribe. An opaque view, so L4 never depends on tr::net.
     */
    view_t return_route{};
    std::string link; /**< @brief This node's NAME for the link the subscribe arrived on. */
    /**
     * @brief The original SUBSCRIBER TLV view this slot was written from, retained zero-copy
     *        (a refcount clone of the field-write payload).
     *
     * Empty for in-process callback/target sugar that carries no TLV. A `:subscribers[]`
     * read ropes these slot views into the `FWD{REPLY}` with no byte copy (RFC-0004 §D /
     * ADR-0035 slice 2 zero-copy reply rule).
     */
    view_t source_view{};
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
};

/**
 * @brief The dispatch-relevant snapshot of one ACTIVE subscription edge.
 *
 * What @ref vertex_t::snapshot_edges copies out under the vertex lock so the graph can
 * dispatch OUTSIDE it (callbacks / re-dispatch re-enter the graph): the `{fn, ctx}`
 * callback pair, owning copies of the target key / link / caller (the slot may be
 * cleared concurrently once dispatch runs outside the lock), and a refcount CLONE of
 * the stored return route (ADR-0041 §2 — a bump, not a byte copy; the clone keeps the
 * route alive across a concurrent unsubscribe).
 */
struct edge_view_t {
    subscriber_fn_t callback = nullptr; /**< @brief The in-process sink fn (null ⇒ none). */
    void* callback_ctx = nullptr;       /**< @brief The sink's caller-owned context. */
    std::vector<std::byte> target_key;  /**< @brief Local re-dispatch target (owning copy). */
    std::string link;      /**< @brief Remote-delivery link NAME (owning copy; empty ⇒ local). */
    view_t return_route{}; /**< @brief Consumer return route (refcount clone). */
    bool delivery_compact = false; /**< @brief RFC-0004 §E.1 label-compaction opt-in. */
    std::string caller; /**< @brief The edge's stored ACL fan-in context (#81, owning copy). */
};

/**
 * @brief A transient-local durability latch (RFC-0004 §D / Q4): the LKV plus the freshly
 *        admitted edge's dispatch view, both snapshotted atomically with the append.
 *
 * @ref value stays null when no latch fired (volatile producer, or no LKV yet).
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
 * @brief The lazily-allocated COLD half of a vertex (issue #361 §1): every member a plain
 *        STORED_VALUE leaf with default QoS, no handlers, and no `:acl` never touches.
 *
 * ADR-0021 rule 2 ("the machinery is pay-for-what-you-use") applied to RAM: the common
 * MCU leaf keeps `vertex_t::ext_` null and pays nothing here. Allocated at most once —
 * at registration when the identity needs it (STREAM role, user handlers, non-default
 * settings), or later under the vertex mutex on the first `:acl` / `:settings` write —
 * and never freed before the vertex (the insert-only ADR-0057 lifetime), so a published
 * pointer stays valid for every reader.
 */
struct vertex_ext_t {
    handlers_t handlers{}; /**< @brief User behavior (Handler role + the `on_children` seam). */
    /** @brief STREAM ring (docs/reference/11 role 2); guarded by the vertex mutex. */
    std::deque<std::shared_ptr<const rope_t>> history;
    /** @brief Raw `:acl` TLV bytes, served back verbatim (#81-A, ADR-0018/0020); guarded by
     *         the vertex mutex. Empty ⇒ no `:acl` set. */
    std::vector<std::byte> acl;
    /** @brief The `:acl` bytes parsed into core-subset ACEs at write time (#81); guarded by
     *         the vertex mutex. `graph_t::acl_allows` evaluates these. */
    std::vector<ace_t> aces;
    /** @brief The ADR-0050 cached effective-ACE merge (own + INHERIT-flagged ancestor ACEs,
     *         pre-merged in evaluation order); guarded by the vertex mutex, rebuilt lazily
     *         when @ref acl_cache_dirty is raised. Only the MERGE is cached, never a
     *         verdict — expiry evaluates at check time against the caller's now. */
    std::vector<ace_t> eff_aces;
    /** @brief Raised ⇒ @ref eff_aces is stale (rebuild lazily; ADR-0050 cache protocol). */
    std::atomic<bool> acl_cache_dirty{true};
    settings_t settings{}; /**< @brief QoS settings; single-field mutation under the vertex
                                mutex (`vertex_t::update_settings`). */
    /** @brief STREAM drain cursor (RFC-0008 §E): the write seq at the last flush, so a
     *         propagate drains only the entries appended since; guarded by the vertex
     *         mutex. */
    std::uint64_t last_flushed_seq = 0;
};

/**
 * @brief An L4 graph vertex: a named, addressable position holding a value, a bounded
 *        history, or a user handler (docs/reference/11 §roles).
 *
 * Pinned in place (the atomic last-known-value slot + mutex + condvar are non-movable) and
 * always handled via a `vertex_handle_t` returned by `graph_t::register_vertex` (ADR-0056). The
 * read/write hot path is lock-free (an atomic shared_ptr swap); the mutex guards only the
 * history ring, the subscriber list, and the await waiter accounting. Non-copyable.
 *
 * The public surface is a VERB interface — storage (@ref store / @ref read_stored),
 * readiness (@ref note_write / @ref wait_for_change / the seq cursors), edges
 * (@ref add_edge / @ref clear_edge / @ref snapshot_edges), and ACL state (@ref set_acl /
 * @ref with_aces / @ref with_effective_aces) — each verb taking the vertex mutex
 * internally (the LKV slot stays
 * lock-free). `graph_t` keeps what SPANS vertices: routing, ancestor walks, fan-out
 * dispatch legs, the effective-ACL walk, admission, and the field surface.
 */
class vertex_t {
   public:
    /** @brief The no-heap small-fan-out snapshot width (@ref snapshot_edges buffer size). */
    static constexpr std::size_t kInlineFanout = edge_snapshot_t::kCapacity;
    /** @brief Inline child slots before the child list spills to the sorted heap vector
     *         (ADR-0057 Composite tree — most vertices are leaves or narrow composites). */
    static constexpr std::size_t kInlineChildren = 2;

    /** @brief Construct a vertex with its role, own canonical NAME record (ADR-0057 — one
     *         segment, not the full key), QoS settings, and handlers. The cold extension
     *         block is allocated only if this identity needs one (#361 §1). */
    vertex_t(role_t role, path_key_t name, settings_t settings, handlers_t handlers)
        : role_(role), name_(std::move(name)) {
        adopt_identity(role, settings, std::move(handlers));
    }

    vertex_t(const vertex_t&) = delete;
    vertex_t& operator=(const vertex_t&) = delete;

    /** @brief Free the cold extension block (allocated at most once, ADR-0057 lifetime). */
    ~vertex_t() { delete ext_.load(std::memory_order_acquire); }

    /** @brief This vertex's behavioral role. */
    [[nodiscard]] role_t role() const noexcept { return role_; }
    /** @brief This vertex's own canonical NAME record (its single path segment, ADR-0057);
     *         empty at the root. The full key is a parent-walk concatenation
     *         (`graph_t`'s `build_key`). */
    [[nodiscard]] const path_key_t& name() const noexcept { return name_; }
    /** @brief This vertex's QoS settings (`kDefaultSettings` when no extension block —
     *         a default-QoS vertex stores no copy of them). */
    [[nodiscard]] const settings_t& settings() const noexcept {
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr ? e->settings : kDefaultSettings;
    }
    /** @brief This vertex's user handlers (Handler role behavior + the `on_children` seam);
     *         an all-empty shared constant when no extension block. */
    [[nodiscard]] const handlers_t& handlers() const noexcept {
        static const handlers_t kNoHandlers{};
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr ? e->handlers : kNoHandlers;
    }

    // -- Composite tree links (ADR-0057) -------------------------------------------------
    //
    // The graph is a Composite of vertices: each node owns its children (one non-moving
    // unique_ptr allocation per child, so vertex_t* stay stable for the graph's lifetime)
    // and points at its parent. Children/registered are guarded by graph_t's map lock
    // (unique for mutation, shared for walks); the parent pointer and name bytes are
    // immutable after construction, so parent-chain walks (bubbling, the ACL inheritance
    // walk) run LOCK-FREE.

    /** @brief The owning parent node (`nullptr` only at the graph root).
     *  @note Immutable once linked — safe to walk without any lock. */
    [[nodiscard]] vertex_t* parent() const noexcept { return parent_; }

    /** @brief True once a registration filled this node; false for a placeholder — a
     *         structural intermediate level that `find` / `read_children` must not surface
     *         (matching the flat-map behavior where missing intermediates did not exist).
     *  @note Read/written under the graph's map lock. */
    [[nodiscard]] bool registered() const noexcept { return registered_; }

    /**
     * @brief Fill this node with a registration's identity: set the role, QoS settings, and
     *        handlers, and mark it @ref registered.
     *
     * Called under the graph's UNIQUE map lock — either on a freshly constructed node or on
     * a placeholder being registered in place (the allocation never moves, ADR-0057).
     */
    void fill(role_t role, settings_t settings, handlers_t handlers) {
        role_ = role;
        adopt_identity(role, settings, std::move(handlers));
        registered_ = true;
    }

    /**
     * @brief Adopt @p child into this node's children container and link its parent pointer.
     *
     * Small-vector-style storage: the first @ref kInlineChildren land in inline slots
     * (no heap block for narrow composites); the first spill moves ALL children into a heap
     * vector kept sorted by name record, so a wide composite resolves a child in
     * O(log children). The `vertex_t` itself never moves (only owning pointers do).
     * @note Called under the graph's UNIQUE map lock.
     * @return The adopted child (its stable address).
     */
    vertex_t* add_child(std::unique_ptr<vertex_t> child) {
        child->parent_ = this;
        vertex_t* raw = child.get();
        if (child_spill_.empty() && child_count_ < kInlineChildren) {
            child_inline_[child_count_] = std::move(child);
        } else {
            if (child_spill_.empty()) {  // first spill: migrate the inline slots, then sort
                child_spill_.reserve(child_count_ + 1);
                for (std::unique_ptr<vertex_t>& c : child_inline_)
                    if (c) child_spill_.push_back(std::move(c));
                std::sort(
                    child_spill_.begin(), child_spill_.end(),
                    [](const std::unique_ptr<vertex_t>& a, const std::unique_ptr<vertex_t>& b) {
                        return std::ranges::lexicographical_compare(a->name().bytes,
                                                                    b->name().bytes);
                    });
            }
            const auto pos = std::lower_bound(
                child_spill_.begin(), child_spill_.end(), child->name().bytes,
                [](const std::unique_ptr<vertex_t>& c, const std::vector<std::byte>& n) {
                    return std::ranges::lexicographical_compare(c->name().bytes, n);
                });
            child_spill_.insert(pos, std::move(child));
        }
        ++child_count_;
        return raw;
    }

    /**
     * @brief The child whose own NAME record equals @p record byte-for-byte, or `nullptr` —
     *        one level of the O(segments) resolution walk (ADR-0057).
     * @note Called under the graph's map lock (shared suffices).
     */
    [[nodiscard]] vertex_t* child_by_record(std::span<const std::byte> record) const noexcept {
        const auto matches = [&](const std::unique_ptr<vertex_t>& c) {
            const std::vector<std::byte>& n = c->name().bytes;
            return n.size() == record.size() && std::equal(n.begin(), n.end(), record.begin());
        };
        if (child_spill_.empty()) {
            for (std::size_t i = 0; i < child_count_; ++i)
                if (matches(child_inline_[i])) return child_inline_[i].get();
            return nullptr;
        }
        const auto it =
            std::lower_bound(child_spill_.begin(), child_spill_.end(), record,
                             [](const std::unique_ptr<vertex_t>& c, std::span<const std::byte> r) {
                                 return std::ranges::lexicographical_compare(c->name().bytes, r);
                             });
        return (it != child_spill_.end() && matches(*it)) ? it->get() : nullptr;
    }

    /**
     * @brief Run @p f over every child (placeholders included), in container order —
     *        member enumeration and the RFC-0005 subtree-counter walks.
     * @note Called under the graph's map lock (shared suffices); @p f must not mutate
     *       the tree.
     */
    template <typename F>
    void for_each_child(F&& f) const {
        if (child_spill_.empty()) {
            for (std::size_t i = 0; i < child_count_; ++i) f(*child_inline_[i]);
            return;
        }
        for (const std::unique_ptr<vertex_t>& c : child_spill_) f(*c);
    }

    // -- storage & readiness ----------------------------------------------------------

    /**
     * @brief Store @p value as this vertex's state: publish the last-known-value
     *        (lock-free), append the STREAM ring (keep-last trim), bump the write
     *        sequence, and wake awaiters.
     *
     * One allocation (`make_shared`): the rope's inline small-buffer holds the
     * single-link trivial case, so a scalar write costs exactly what the `view_t`
     * slot cost (ADR-0053 §6). The LKV publish happens BEFORE the lock; only the
     * ring trim + seq bump + notify run under it. Not for Handler-role writes —
     * the graph runs `handlers().on_write` and calls @ref note_write instead.
     * @return The published LKV pointer — exactly what a concurrent @ref read_stored
     *         observes — so the write path can deliver the stored value (RFC-0008 §D
     *         "deliver exactly what was stored") without recloning the rope.
     */
    std::shared_ptr<const rope_t> store(rope_t value) {
        auto sp = std::make_shared<const rope_t>(std::move(value));
        lkv_.store(sp);  // lock-free publish of the new last-known-value
        {
            const std::lock_guard lock(m_);
            vertex_ext_t* e = ext_.load(std::memory_order_acquire);
            if (role_ == role_t::STREAM && e != nullptr) {  // STREAM identity always has ext
                e->history.push_back(sp);  // refcount bump — the caller keeps the returned sp
                const std::size_t keep =
                    e->settings.history_keep_last ? e->settings.history_keep_last : 1;
                while (e->history.size() > keep) e->history.pop_front();
            }
            ++write_seq_;
            cv_.notify_all();
        }
        return sp;
    }

    /**
     * @brief Record a Handler-role write: bump the write sequence and wake awaiters
     *        (the vertex stores no value — the user handler consumed it).
     */
    void note_write() {
        const std::lock_guard lock(m_);
        ++write_seq_;
        cv_.notify_all();
    }

    /** @brief The stored last-known-value (lock-free; null ⇒ never assigned / Handler role). */
    [[nodiscard]] std::shared_ptr<const rope_t> read_stored() const { return lkv_.load(); }

    /**
     * @brief Block until the write sequence moves past @p seq0 or @p timeout elapses.
     * @param seq0    The @ref current_seq snapshot the caller waits to see surpassed.
     * @param timeout The maximum wait.
     * @return true iff a change was observed (`write_seq_ != seq0`); false on timeout.
     */
    [[nodiscard]] bool wait_for_change(std::uint64_t seq0, std::chrono::nanoseconds timeout) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, timeout, [&] { return write_seq_ != seq0; });
    }

    /** @brief The current write sequence (bumped per assign — the await predicate base). */
    [[nodiscard]] std::uint64_t current_seq() {
        const std::lock_guard lock(m_);
        return write_seq_;
    }

    /**
     * @brief Advance the STREAM drain cursor to "now" WITHOUT draining (RFC-0008 §E):
     *        an eager delivery already flushed the ring, so a later sweep must not
     *        re-deliver.
     */
    void mark_flushed() {
        const std::lock_guard lock(m_);
        if (vertex_ext_t* e = ext_.load(std::memory_order_acquire))
            e->last_flushed_seq = write_seq_;
    }

    /**
     * @brief Drain the STREAM ring entries appended since the last flush, in order —
     *        a queue, not a coalesce (RFC-0008 §E) — and advance the drain cursor.
     *
     * Snapshots under the lock into @p out (caller storage; overwritten); the caller
     * delivers OUTSIDE the lock. Entries trimmed out of the keep-last ring before this
     * drain are lost (bounded history).
     * @return The number of entries drained (0 ⇒ nothing appended since the last flush).
     */
    std::size_t drain_unflushed(std::vector<std::shared_ptr<const rope_t>>& out) {
        const std::lock_guard lock(m_);
        vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr) return 0;  // no ring — nothing was ever appended
        const std::uint64_t now = write_seq_;
        if (now == e->last_flushed_seq) return 0;
        const std::uint64_t n_new = now - e->last_flushed_seq;
        e->last_flushed_seq = now;
        const auto take =
            static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(n_new, e->history.size()));
        out.assign(e->history.end() - take, e->history.end());
        return out.size();
    }

    /** @brief The STREAM ring contents, oldest first — each entry a rope clone (refcount
     *         bumps, no byte copy). */
    [[nodiscard]] std::vector<rope_t> history_snapshot() {
        const std::lock_guard lock(m_);
        std::vector<rope_t> out;
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e == nullptr) return out;
        out.reserve(e->history.size());
        for (const auto& sp : e->history) out.push_back(*sp);
        return out;
    }

    // -- subscription edges -----------------------------------------------------------

    /**
     * @brief Append a subscription edge; atomically snapshot the transient-local
     *        durability latch when @p latch is non-null.
     *
     * Under ONE lock hold: the slot is appended, and — iff this vertex is
     * transient-local (`settings.durability == 1`) and already holds an LKV — the
     * value plus the new edge's dispatch view are snapshotted into @p latch, so a
     * concurrent `clear_edge` can never slip between append and latch. The caller
     * dispatches the latch OUTSIDE the lock (RFC-0004 §D / ADR-0049).
     * @return The appended slot's index (the `:subscribers[N]` slot number).
     */
    std::size_t add_edge(subscriber_t s, edge_latch_t* latch = nullptr) {
        const std::lock_guard lock(m_);
        subs_.push_back(std::move(s));
        const std::size_t idx = subs_.size() - 1;
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (latch != nullptr && e != nullptr && e->settings.durability == 1) {
            if (std::shared_ptr<const rope_t> lkv = lkv_.load()) {
                latch->value = std::move(lkv);
                latch->edge = edge_view_of(subs_.back());
            }
        }
        return idx;
    }

    /**
     * @brief Deactivate the edge slot @p idx (unsubscribe — a cleared `:subscribers[N]`).
     * @return true iff the slot existed and was active (the caller then adjusts the
     *         RFC-0005 listener bookkeeping).
     */
    bool clear_edge(std::size_t idx) {
        const std::lock_guard lock(m_);
        if (idx >= subs_.size() || !subs_[idx].active) return false;
        subs_[idx].active = false;
        return true;
    }

    /**
     * @brief Snapshot every ACTIVE edge's dispatch view into caller storage — the
     *        snapshot-under-lock half of the snapshot/dispatch-outside discipline.
     *
     * Small fan-out (the common case, ≤ `kInlineFanout`) placement-constructs into
     * @p inline_buf — no heap allocation AND no dead stack zeroing per publish; a
     * larger subscriber list reserves @p overflow once and fills it instead (then
     * @p overflow is non-empty and holds ALL views).
     * @param inline_buf The caller's raw stack buffer (cleared on entry).
     * @param overflow   The heap fallback for large fan-out (cleared on entry).
     * @return The number of views snapshotted (into whichever buffer was used).
     */
    std::size_t snapshot_edges(edge_snapshot_t& inline_buf, std::vector<edge_view_t>& overflow) {
        inline_buf.clear();
        overflow.clear();
        const std::lock_guard lock(m_);
        const bool use_heap = subs_.size() > edge_snapshot_t::kCapacity;
        if (use_heap) overflow.reserve(subs_.size());
        std::size_t n = 0;
        for (const subscriber_t& s : subs_) {
            if (!s.active) continue;
            if (use_heap)
                overflow.push_back(edge_view_of(s));
            else
                inline_buf.push_back(edge_view_of(s));
            ++n;
        }
        return n;
    }

    /**
     * @brief The stored SUBSCRIBER TLV view of the active slot @p idx (a `:subscribers[N]`
     *        read) — a refcount clone, no byte copy; `nullopt` for a missing / inactive /
     *        TLV-less (in-process sugar) slot.
     */
    [[nodiscard]] std::optional<view_t> edge_source(std::size_t idx) {
        const std::lock_guard lock(m_);
        if (idx < subs_.size() && subs_[idx].active && subs_[idx].source_view.owner)
            return subs_[idx].source_view;  // clone (refcount bump)
        return std::nullopt;
    }

    /** @brief Every active slot's stored SUBSCRIBER view, in slot order (the
     *         `:subscribers[]` array read) — each a refcount clone. */
    [[nodiscard]] std::vector<view_t> edge_sources() {
        const std::lock_guard lock(m_);
        std::vector<view_t> out;
        out.reserve(subs_.size());
        for (const subscriber_t& s : subs_)
            if (s.active && s.source_view.owner) out.push_back(s.source_view);
        return out;
    }

    // -- ACL state (#81, ADR-0018/0020) -------------------------------------------------

    /**
     * @brief Store this vertex's `:acl`: the raw TLV bytes (served back verbatim by an
     *        `:acl` read) plus the same bytes parsed into typed ACEs (what evaluation
     *        walks). Storing replaces; empty ⇒ no restrictions.
     */
    void set_acl(std::span<const std::byte> raw, std::vector<ace_t> aces) {
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(m_);
        e.acl.assign(raw.begin(), raw.end());
        e.aces = std::move(aces);
        // Publish-then-mark (ADR-0050 cache protocol): the new ACEs are visible
        // under m_ BEFORE the dirty flag is raised, so a rebuild that observes the
        // flag always reads the new list (or leaves the flag set for the next one).
        e.acl_cache_dirty.store(true, std::memory_order_release);
    }

    /** @brief A copy of the stored raw `:acl` TLV bytes (empty ⇒ no `:acl` set). */
    [[nodiscard]] std::vector<std::byte> acl_bytes() {
        const std::lock_guard lock(m_);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr ? e->acl : std::vector<std::byte>{};
    }

    /**
     * @brief Run @p f over this vertex's parsed ACE list under the vertex lock — the
     *        zero-copy evaluation accessor (`graph_t::acl_allows` hands the list to the
     *        pure ADR-0050 policy without snapshotting subject bytes per gated op).
     *
     * @p f must not re-enter this vertex (the lock is held) — it is a pure evaluation
     * over the list, per the ADR-0050 policy contract (no locks/clock/graph inside).
     * @return Whatever @p f returns.
     */
    template <typename F>
    auto with_aces(F&& f) -> decltype(f(std::declval<const std::vector<ace_t>&>())) {
        static const std::vector<ace_t> kNoAces{};
        const std::lock_guard lock(m_);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return f(e != nullptr ? e->aces : kNoAces);
    }

    /**
     * @brief Mark this vertex's cached effective-ACE merge stale (ADR-0050).
     *
     * Raised by the graph on every `:acl` write for the WRITTEN vertex's whole
     * subtree (subtree-precise invalidation via the ADR-0057 child links —
     * wiring-frequency); @ref set_acl raises it for the written vertex itself.
     * The next @ref with_effective_aces on a marked vertex rebuilds lazily.
     * @note Lock-free (a release store) — callable under the graph's map lock
     *       during the subtree walk without touching any vertex mutex.
     */
    void mark_acl_cache_dirty() noexcept {
        // No extension block ⇒ no cached merge exists to invalidate; a block created
        // later starts dirty, so a concurrent first-gated-op cannot miss this mark
        // (its rebuild reads ancestor ACEs already published before this walk).
        if (vertex_ext_t* e = ext_.load(std::memory_order_acquire))
            e->acl_cache_dirty.store(true, std::memory_order_release);
    }

    /**
     * @brief Evaluate against this vertex's cached effective-ACE merge, rebuilding
     *        it first iff it is stale — the ADR-0050 cached-merge verb.
     *
     * Under ONE hold of the vertex mutex: when the dirty flag is raised it is
     * lowered (an acquire-release exchange) and @p rebuild produces a fresh merged
     * list from this vertex's own parsed ACEs (passed in) — the graph's rebuild
     * walks the immutable parent chain and takes each ancestor's @ref with_aces,
     * nesting locks strictly LEAF-TO-ROOT along one parent chain (every other
     * holder takes a single vertex mutex, so the ordering is acyclic — no
     * deadlock). Then @p eval runs over the (now-current) cached list and its
     * result is returned.
     *
     * Race resolution (rebuild vs concurrent `:acl` write): the writer publishes
     * ACEs BEFORE raising the flag (@ref set_acl / graph subtree mark), and this
     * verb lowers the flag BEFORE @p rebuild reads — so a write landing after the
     * exchange leaves the flag raised and the possibly-stale cache is rebuilt on
     * the NEXT check; a stale-forever cache is impossible. Concurrent rebuilds are
     * serialized by the vertex mutex.
     *
     * @param rebuild `std::vector<ace_t>(const std::vector<ace_t>& own)` — the
     *                fresh merge; must not re-enter THIS vertex (its lock is held).
     * @param eval    Pure evaluation over the cached merged list (the ADR-0050
     *                policy contract: no locks/clock/graph inside).
     * @return Whatever @p eval returns.
     */
    template <typename Rebuild, typename Eval>
    auto with_effective_aces(Rebuild&& rebuild, Eval&& eval)
        -> decltype(eval(std::declval<const std::vector<ace_t>&>())) {
        vertex_ext_t& e = ensure_ext();  // gated eval caches its merge here (fresh ⇒ dirty)
        const std::lock_guard lock(m_);
        if (e.acl_cache_dirty.exchange(false, std::memory_order_acq_rel))
            e.eff_aces = rebuild(static_cast<const std::vector<ace_t>&>(e.aces));
        return eval(static_cast<const std::vector<ace_t>&>(e.eff_aces));
    }

    // -- settings & propagation policy --------------------------------------------------

    /** @brief A consistent copy of the QoS settings (taken under the vertex lock;
     *         `kDefaultSettings` when no extension block). */
    [[nodiscard]] settings_t settings_snapshot() {
        const std::lock_guard lock(m_);
        const vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        return e != nullptr ? e->settings : kDefaultSettings;
    }

    /**
     * @brief Mutate the QoS settings under the vertex lock: @p f receives `settings_t&`;
     *        its return value is passed through (the `:settings.<field>` write surface —
     *        concurrent single-field writes both land, no read-modify-write clobber).
     *        Allocates the extension block on a default-QoS vertex's first write.
     */
    template <typename F>
    auto update_settings(F&& f) -> decltype(f(std::declval<settings_t&>())) {
        vertex_ext_t& e = ensure_ext();
        const std::lock_guard lock(m_);
        return f(e.settings);
    }

    /** @brief How this vertex participates in an ANCESTOR's propagate sweep (RFC-0008 §C). */
    [[nodiscard]] delivery_mode_t delivery_mode() const noexcept { return delivery_mode_; }
    /** @brief Set the propagation policy — wiring-time, via `graph_t::set_delivery_mode`
     *         (which also maintains the sweep's UNCONDITIONAL membership). */
    void set_delivery_mode(delivery_mode_t mode) noexcept { delivery_mode_ = mode; }

    // -- RFC-0005 listener bookkeeping (lock-free counters) ------------------------------

    /** @brief This vertex's own active-slot count (what a subtree walk sums). */
    [[nodiscard]] std::uint32_t own_subs() const noexcept {
        return own_subs_.load(std::memory_order_relaxed);
    }
    /** @brief Adjust the own active-slot count by @p delta (subscribe/unsubscribe). */
    void bump_own_subs(std::int32_t delta) noexcept {
        own_subs_.fetch_add(static_cast<std::uint32_t>(delta), std::memory_order_relaxed);
    }
    /** @brief The active subscriber slots on strict ancestors — the one relaxed load the
     *         write hot path pays before deciding whether to walk ancestors at all. */
    [[nodiscard]] std::uint32_t listeners_above() const noexcept {
        return listeners_above_.load(std::memory_order_relaxed);
    }
    /** @brief Adjust the ancestor-listener count by @p delta (an ancestor's edge came/went). */
    void bump_listeners_above(std::int32_t delta) noexcept {
        listeners_above_.fetch_add(static_cast<std::uint32_t>(delta), std::memory_order_relaxed);
    }
    /** @brief Seed the ancestor-listener count at creation (the newborn's O(depth) sum). */
    void init_listeners_above(std::uint32_t count) noexcept {
        listeners_above_.store(count, std::memory_order_relaxed);
    }

   private:
    // The dispatch view of one slot; call with m_ held. Owning copies of the byte/string
    // fields (the slot may be cleared while dispatch runs outside the lock); the route
    // copy is a refcount clone (ADR-0041 §2 — keeps it alive across an unsubscribe).
    [[nodiscard]] edge_view_t edge_view_of(const subscriber_t& s) const {
        return edge_view_t{s.callback,     s.callback_ctx,     s.target_key, s.link,
                           s.return_route, s.delivery_compact, s.caller};
    }

    /**
     * @brief The extension block, creating it on first need (race-free CAS publish).
     *
     * Callable under any lock regime: allocation races between the registration path
     * (graph map lock) and the field-write verbs (vertex mutex) resolve by
     * compare-exchange — the loser frees its candidate and adopts the winner's block.
     * The pointer is never cleared once published (ADR-0057 insert-only lifetime), so
     * lock-free readers (@ref settings / @ref handlers) stay valid forever.
     */
    vertex_ext_t& ensure_ext() {
        vertex_ext_t* e = ext_.load(std::memory_order_acquire);
        if (e != nullptr) return *e;
        auto fresh = std::make_unique<vertex_ext_t>();
        vertex_ext_t* expected = nullptr;
        if (ext_.compare_exchange_strong(expected, fresh.get(), std::memory_order_acq_rel,
                                         std::memory_order_acquire))
            return *fresh.release();
        return *expected;  // another thread won the publish; fresh is freed here
    }

    /**
     * @brief Install a registration's identity (constructor + @ref fill): allocate the
     *        extension block iff this identity needs one — STREAM role (history ring),
     *        any user handler, or non-default QoS — and store the cold members there.
     *        A plain default leaf allocates nothing (#361 §1).
     */
    void adopt_identity(role_t role, const settings_t& settings, handlers_t handlers) {
        const bool has_handlers = handlers.on_read || handlers.on_write || handlers.on_children;
        if (role != role_t::STREAM && !has_handlers && settings == kDefaultSettings &&
            ext_.load(std::memory_order_acquire) == nullptr)
            return;
        vertex_ext_t& e = ensure_ext();
        e.settings = settings;
        e.handlers = std::move(handlers);
    }

    role_t role_;
    path_key_t name_;  // own canonical NAME record (one segment; empty at the root) — the
                       // full key is rendered on demand by walking parent_ (ADR-0057)

    // The stored value is a rope (ADR-0053 §6): a contiguous scalar is a single-link
    // rope (small-buffer inline, no extra alloc), a chunked stream keeps its links.
    std::atomic<std::shared_ptr<const rope_t>> lkv_{};  // lock-free read/write hot path
    std::vector<subscriber_t> subs_;                    // fan-out edges; guarded by m_
    // The lazily-allocated cold half (#361 §1): handlers, STREAM ring, the ACL state +
    // ADR-0050 effective-merge cache, non-default settings, and the stream drain cursor.
    // Null for the common default leaf. Published once by ensure_ext (CAS), never
    // cleared; freed by the destructor.
    std::atomic<vertex_ext_t*> ext_{nullptr};
    std::mutex m_;
    std::condition_variable cv_;
    std::uint64_t write_seq_ = 0;  // bumped per assign; await waits for an increment, and it is
                                   // the value-agnostic "newer" signal a sweep reads (RFC-0008 §B).
                                   // Guarded by m_.
    // How this vertex participates in an ANCESTOR's propagate sweep (RFC-0008 §C).
    // Set at wiring time via graph_t::set_delivery_mode (the "configure before frames
    // flow" contract, like the QoS settings); read on the assign path. Default IF_NEWER.
    delivery_mode_t delivery_mode_ = delivery_mode_t::IF_NEWER;

    // Subtree-subscription bookkeeping (RFC-0005): every subscription observes its
    // vertex AND all descendants, so a write must fan out to ancestor subscribers
    // too ("vertical bubbling"). These lock-free counters keep the idle write path
    // near-free: `listeners_above_` counts ACTIVE subscriber slots on strict
    // ancestors (maintained by graph_t at subscribe/unsubscribe — a subtree walk at
    // control-plane frequency — and summed from ancestors at vertex creation), so
    // the write hot path pays exactly one relaxed load before deciding whether to
    // walk ancestors at all. `own_subs_` is this vertex's own active-slot count —
    // what the subtree walk and the creation-time sum read.
    std::atomic<std::uint32_t> own_subs_{0};
    std::atomic<std::uint32_t> listeners_above_{0};

    // Composite tree links (ADR-0057), kept at the COLD tail so the write hot path's
    // members (LKV slot, mutex, seq, counters) stay on the front cache lines.
    // parent_/name_ are immutable once the node is linked (lock-free parent walks);
    // children/registered_ are guarded by graph_t's map lock. Children are owned via
    // non-moving unique_ptr allocations — vertex_t* stay stable for the graph's lifetime
    // (the insert-only invariant vertex_handle_t relies on): inline slots first, then one
    // sorted heap vector (O(log children) resolution under wide composites). Vertices are
    // never erased (retire-LIST deferred; see ADR-0057 lifetime).
    vertex_t* parent_ = nullptr;
    std::array<std::unique_ptr<vertex_t>, kInlineChildren> child_inline_{};
    std::vector<std::unique_ptr<vertex_t>> child_spill_;
    std::uint32_t child_count_ = 0;
    bool registered_ = false;  // false => placeholder intermediate (invisible to find)
};

}  // namespace tr::graph
