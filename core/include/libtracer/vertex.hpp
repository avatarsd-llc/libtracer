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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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
};

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
 * @brief One subscription edge (M3b).
 *
 * A write to the owning vertex fans out to a target vertex (@ref target_key —
 * spec-faithful re-dispatch) and/or an in-process @ref callback (sugar), per
 * docs/reference/02 §dispatch + 04 §write fanout. An inactive slot models an unsubscribe
 * (a cleared `:subscribers[N]`).
 */
struct subscriber_t {
    std::vector<std::byte> target_key; /**< @brief Canonical PATH key (empty ⇒ callback-only). */
    std::function<void(const rope_t&)>
        callback; /**< @brief In-process sink; null ⇒ target-only (ADR-0053 §6 rope value). */
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
 * @brief An L4 graph vertex: a named, addressable position holding a value, a bounded
 *        history, or a user handler (docs/reference/11 §roles).
 *
 * Pinned in place (the atomic last-known-value slot + mutex + condvar are non-movable) and
 * always handled via a `vertex_handle_t` returned by `graph_t::register_vertex` (ADR-0056). The
 * read/write hot path is lock-free (an atomic shared_ptr swap); the mutex guards only the
 * history ring, the subscriber list, and the await waiter accounting. Non-copyable.
 */
class vertex_t {
   public:
    /** @brief Construct a vertex with its role, canonical key, QoS settings, and handlers. */
    vertex_t(role_t role, path_key_t key, settings_t settings, handlers_t handlers)
        : role_(role), key_(std::move(key)), settings_(settings), handlers_(std::move(handlers)) {}

    vertex_t(const vertex_t&) = delete;
    vertex_t& operator=(const vertex_t&) = delete;

    /** @brief This vertex's behavioral role. */
    [[nodiscard]] role_t role() const noexcept { return role_; }
    /** @brief This vertex's canonical PATH-payload key (the vertex-map key). */
    [[nodiscard]] const path_key_t& key() const noexcept { return key_; }
    /** @brief This vertex's QoS settings. */
    [[nodiscard]] const settings_t& settings() const noexcept { return settings_; }

   private:
    friend class graph_t;

    role_t role_;
    path_key_t key_;
    settings_t settings_;
    handlers_t handlers_;

    // The stored value is a rope (ADR-0053 §6): a contiguous scalar is a single-link
    // rope (small-buffer inline, no extra alloc), a chunked stream keeps its links.
    std::atomic<std::shared_ptr<const rope_t>> lkv_{};   // lock-free read/write hot path
    std::deque<std::shared_ptr<const rope_t>> history_;  // Stream ring; guarded by m_
    std::vector<subscriber_t> subs_;                     // fan-out edges; guarded by m_
    std::vector<std::byte> acl_;  // raw :acl TLV bytes, served back verbatim; guarded by m_
                                  // (#81-A, ADR-0018/0020). Empty => no :acl set.
    std::vector<ace_t> aces_;     // the :acl bytes parsed into core-subset ACEs at write time
                                  // (#81, ALLOW-only + INHERIT); guarded by m_. graph_t's
                                  // acl_allows evaluates these when a subject resolver is set.
    std::mutex m_;
    std::condition_variable cv_;
    std::uint64_t write_seq_ = 0;  // bumped per assign; await waits for an increment, and it is
                                   // the value-agnostic "newer" signal a sweep reads (RFC-0008 §B).
                                   // Guarded by m_.
    // How this vertex participates in an ANCESTOR's propagate sweep (RFC-0008 §C).
    // Set at wiring time via graph_t::set_delivery_mode (the "configure before frames
    // flow" contract, like settings_); read on the assign path. Default IF_NEWER.
    delivery_mode_t delivery_mode_ = delivery_mode_t::IF_NEWER;
    // STREAM drain cursor (RFC-0008 §E): write_seq_ at the last flush of this stream, so
    // a propagate drains the ring entries appended since — a queue, not a coalesce.
    // Guarded by m_.
    std::uint64_t last_flushed_seq_ = 0;

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
};

}  // namespace tr::graph
