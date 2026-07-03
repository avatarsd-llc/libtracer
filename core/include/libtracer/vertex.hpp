/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * An L4 graph vertex: a named, addressable position holding a value, a bounded
 * history, or a user handler (docs/reference/11 §roles). Pinned in place (the
 * atomic LKV slot + mutex + condvar are non-movable); always handled via a
 * vertex_t* returned by graph_t::register_vertex. The read/write LKV hot path is
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
#include "libtracer/status.hpp"
#include "libtracer/view.hpp"

namespace tr::graph {

// L1 types this layer consumes (upward dependency on tr::view, docs/adr/0016 §2).
using view::segment_ptr_t;
using view::view_t;

enum class role_t {
    STORED_VALUE,  // role 1: last-writer-wins; holds the last-written view_t
    STREAM,        // role 2: bounded history ring sized by settings.history_keep_last
    HANDLER,       // roles 3-7: user on_read / on_write supplies the behavior
};

// The mandatory core QoS fields (docs/reference/02 §core writable fields).
struct settings_t {
    std::uint8_t reliability = 0;         // 0=best-effort, 1=reliable
    std::uint8_t durability = 0;          // 0=volatile, 1=transient-local
    std::uint32_t history_keep_last = 1;  // Stream ring depth (>=1)
    std::uint64_t deadline_ns = 0;        // 0=off; max ns between writes before a liveness fault
    std::uint8_t priority = 0;            // 0=low .. 255=critical (transport hint, not a wire bit)
    std::uint32_t queue_max_bytes = 0;    // 0=unbounded; per-subscriber back-pressure cap
    // ADR-0042 §3: a view-delivered WRITE whose payload TLV is >= this many bytes (and
    // carries no trailer bits) is stored as a SUBVIEW of the inbound frame (refcount
    // pin, zero copy) instead of the one-copy trailer-sliced store. 0 (the default)
    // DISABLES referencing — pinning amplification is a per-vertex deployment call.
    std::uint32_t store_ref_min_bytes = 0;
};

// User behavior for a Handler-role vertex.
struct handlers_t {
    std::function<result_t<view_t>()> on_read;
    std::function<result_t<void>(const view_t&)> on_write;
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

/**
 * @brief One parsed ALLOW ACE of a vertex's `:acl` (core subset, ADR-0020 / #81).
 *
 * The core subset is ALLOW-only: a `:acl` write carrying a DENY ACE (or any flag
 * bit beyond @ref kAceInherit) is rejected at write time with TYPE_MISMATCH, so
 * stored ACEs never carry semantics this evaluator would silently weaken. Full
 * DENY / ordered first-match-per-bit evaluation is the `security_acl` host module.
 */
struct ace_t {
    std::uint8_t flags = 0;         /**< @brief ACE flags; only @ref kAceInherit is accepted. */
    std::vector<std::byte> subject; /**< @brief Opaque subject token (ADR-0018); the special
                                         subject `"EVERYONE@"` matches any resolved subject. */
    std::uint32_t access_mask = 0;  /**< @brief Granted rights (an OR of @ref acl_right_t bits). */
    std::uint64_t expires_ns = 0;   /**< @brief Absolute expiry, ns since the UNIX epoch;
                                         0 = never expires. An expired ACE grants nothing. */
};

// Per-subscriber delivery policy (byte-agnostic; SUBSCRIBER.qos_settings.delivery_mode
// in docs/reference/05). Numeric filtering (deadband) is NOT here — it is an
// application filter vertex (ADR-0021 sibling). Wire values match reference 05.
enum class delivery_mode_t : std::uint8_t {
    EVERY = 0,      // deliver every write
    THROTTLED = 1,  // reserved (min_interval_ns) — not yet enforced
    ON_CHANGE = 2,  // deliver only when the value bytes differ from the last delivered
};

// One subscription edge (M3b). A write to this vertex fans out to a target vertex
// (target_key — spec-faithful re-dispatch) and/or an in-process callback (sugar).
// docs/reference/02 §dispatch + 04 §write fanout. Inactive slots model an
// unsubscribe (a cleared :subscribers[N]).
struct subscriber_t {
    std::vector<std::byte> target_key;            // canonical PATH key (empty => callback-only)
    std::function<void(const view_t&)> callback;  // null => target-only
    delivery_mode_t mode = delivery_mode_t::EVERY;
    std::vector<std::byte> last_delivered;  // ON_CHANGE: bytes last sent (producer-side, under m_)
    bool active = true;
    // The route-handle opt-in (SUBSCRIBER.qos_settings.delivery_compact, RFC-0004
    // §E.1 / ADR-0035 slice 4). When true the consumer requests label-compacted
    // deliveries: the producer MAY advertise a per-link label aliasing this
    // subscriber's return route and thereafter stream lean COMPACT frames instead
    // of full-route FWD{WRITE} deliveries. Default false ⇒ stateless full-route
    // delivery (the slice-3 path), so a cold/one-shot flow allocates no label state.
    bool delivery_compact = false;
    // The consumer's accumulated return route (a complete PATH TLV's bytes — the FWD
    // `src` the subscribe arrived with) and this node's NAME for the link it arrived
    // on. Both empty ⇒ an in-process slot (callback/target sugar); the producer fan-out
    // ignores it for remote delivery. Both populated ⇒ a REMOTE subscriber: a write to
    // this vertex hands (link, return_route, delivery_compact, value) to the graph's
    // injected remote-delivery sink, which emits the FWD{WRITE} (or auto-promoted
    // COMPACT) back over the link (RFC-0004 §D/§E.1, ADR-0035 slice 4 / #136). Held as
    // a view over a REFCOUNTED segment (ADR-0041 §2): copied once at subscribe, then
    // every delivery snapshot is a refcount clone — O(1) copies over the subscription's
    // life, and an in-flight delivery keeps the route alive across a concurrent
    // unsubscribe. An opaque view + an opaque NAME, so L4 never depends on tr::net.
    view_t return_route{};
    std::string link;
    // The original SUBSCRIBER TLV view this slot was written from, retained zero-copy
    // (a refcount clone of the field-write payload). Empty for in-process callback/target
    // sugar that carries no TLV. A :subscribers[] read ropes these slot views into the
    // FWD{REPLY} with no byte copy (RFC-0004 §D / ADR-0035 slice 2 zero-copy reply rule).
    view_t source_view{};
    // The caller context this edge was created under (#81, ADR-0026 fan-in gate): the
    // inbound link NAME for a remote subscribe, empty for a locally-wired edge. A
    // fan-out re-dispatch into a LOCAL target vertex is gated by the TARGET's :acl
    // WRITE right under this context — the subscription's creator is the "writer"
    // the target authorizes ("who may write to me"). A REMOTE subscriber's fan-in
    // gate runs on the peer instead (its FWD{WRITE} terminus checks the same right).
    std::string caller;
};

class vertex_t {
   public:
    vertex_t(role_t role, path_key_t key, settings_t settings, handlers_t handlers)
        : role_(role), key_(std::move(key)), settings_(settings), handlers_(std::move(handlers)) {}

    vertex_t(const vertex_t&) = delete;
    vertex_t& operator=(const vertex_t&) = delete;

    [[nodiscard]] role_t role() const noexcept { return role_; }
    [[nodiscard]] const path_key_t& key() const noexcept { return key_; }
    [[nodiscard]] const settings_t& settings() const noexcept { return settings_; }

   private:
    friend class graph_t;

    role_t role_;
    path_key_t key_;
    settings_t settings_;
    handlers_t handlers_;

    std::atomic<std::shared_ptr<const view_t>> lkv_{};   // lock-free read/write hot path
    std::deque<std::shared_ptr<const view_t>> history_;  // Stream ring; guarded by m_
    std::vector<subscriber_t> subs_;                     // fan-out edges; guarded by m_
    std::vector<std::byte> acl_;  // raw :acl TLV bytes, served back verbatim; guarded by m_
                                  // (#81-A, ADR-0018/0020). Empty => no :acl set.
    std::vector<ace_t> aces_;     // the :acl bytes parsed into core-subset ACEs at write time
                                  // (#81, ALLOW-only + INHERIT); guarded by m_. graph_t's
                                  // acl_allows evaluates these when a subject resolver is set.
    std::mutex m_;
    std::condition_variable cv_;
    std::uint64_t write_seq_ = 0;  // bumped per write; await waits for an increment (guarded by m_)

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
