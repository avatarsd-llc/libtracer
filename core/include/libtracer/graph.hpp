/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The L4 in-process graph runtime. Holds the vertex map (keyed on canonical
 * PATH-TLV payload bytes, docs/reference/02 §dispatch) and exposes the entire
 * data API: read / write / await (ADR-0006). The hot path resolves a vertex_t*
 * once (at registration or via one guarded lookup), then read/write/await on
 * that handle are lock-free in the LKV slot. subscriber_t fan-out + field-write
 * land in M3b; M3a delivers values via the LKV and the blocking await.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libtracer/path.hpp"
#include "libtracer/status.hpp"
#include "libtracer/vertex.hpp"
#include "libtracer/view.hpp"

namespace tr::wire {
struct tlv_t;  // fwd-decl: the child factory takes a `const tlv_t*` config (no L2 pull-in).
}

namespace tr::graph {

// In-process fan-out cycle bound: a re-dispatch chain deeper than this is dropped
// (Backpressure). This is the in-process analogue of the wire hop_count/MAX_HOPS
// (ADR-0014); an A->B->A subscriber loop terminates here instead of recursing
// forever. See ADR-0015.
inline constexpr int kMaxDispatchDepth = 32;

/**
 * @brief What the producer fan-out hands a remote subscriber's delivery sink (#136).
 *
 * A pure description of one remote subscription edge: the consumer's accumulated
 * return route and this node's NAME for the link it arrived on, both opaque to L4,
 * plus the @ref vertex_t::subscriber_t delivery_compact opt-in. The injected sink
 * (a `tr::net` concern — @ref graph_t::set_remote_delivery_sink) interprets these:
 * it maps @ref link to a transport child and emits a full-route `FWD{WRITE}` or,
 * when @ref delivery_compact, an auto-promoted label `COMPACT` (RFC-0004 §D/§E.1).
 * @ref link is borrowed for the sink call only; @ref return_route is a refcount
 * clone of the stored route segment (ADR-0041 §2) — the sink may rope it into an
 * egress frame, and it stays alive across a concurrent unsubscribe.
 */
struct remote_delivery_t {
    std::string_view link; /**< @brief This node's NAME for the consumer link. */
    view_t return_route;   /**< @brief Consumer return route (PATH TLV view, refcount clone). */
    bool delivery_compact = false; /**< @brief Opt-in to label-compacted delivery. */
};

/**
 * @brief An operation's subject token — opaque bytes matched against ACE subjects (ADR-0018).
 */
using subject_token_t = std::vector<std::byte>;

/**
 * @brief The pluggable subject resolver (ADR-0018, #81): caller context → subject token.
 *
 * Maps an operation's caller context — this node's NAME for the inbound link a
 * remote FWD arrived on, or empty for a local API call — to the subject token ACL
 * evaluation matches against ACE subjects. Returning `std::nullopt` marks the
 * caller trusted (no subject — the operation is allowed unchecked), which is the
 * natural mapping for empty (local) contexts. The token is identity-provenance:
 * v1 typically returns the transport-authenticated peer id for a link; a stronger
 * (PKI) token slots in later without changing the ACL model.
 */
using subject_resolver_t = std::function<std::optional<subject_token_t>(std::string_view caller)>;

class graph_t {
   public:
    graph_t();
    graph_t(const graph_t&) = delete;
    graph_t& operator=(const graph_t&) = delete;

    // Register a vertex at `path` (any field tail is ignored). Returns the pinned
    // handle, or PathInUse if the path is already registered.
    [[nodiscard]] result_t<vertex_t*> register_vertex(const path_t& path, role_t role,
                                                      handlers_t handlers = {},
                                                      settings_t settings = {});

    // Register a vertex by its canonical PATH-payload key directly (the in-band
    // `:children[]` path — the key is composed parent-key + NAME(child), not parsed
    // from a string). PathInUse if the key is already registered.
    [[nodiscard]] result_t<vertex_t*> register_vertex_key(std::vector<std::byte> key, role_t role,
                                                          handlers_t handlers = {},
                                                          settings_t settings = {});

    // A child-vertex factory: the device-catalog entry ADR-0017 makes concrete. Given
    // the composed child key (parent key + the SPEC's `name` NAME) and the optional
    // SPEC `config` SETTINGS, it registers the child vertex(es) and returns the primary
    // handle (or a status — e.g. PATH_IN_USE). The graph owns the *addressing* (the key
    // is composed for it); the factory owns the *catalog* (what a `type` instantiates).
    using child_factory_t = std::function<result_t<vertex_t*>(
        graph_t&, std::vector<std::byte> child_key, const wire::tlv_t* config)>;

    // Populate the device's creation catalog (ADR-0017): map a SPEC `type` selector to a
    // factory. A `:children[]` SPEC write whose `type` is unregistered returns
    // SCHEMA_NOT_FOUND (the ENOTTY of an unsupported creation). Not thread-safe against
    // concurrent creation — call at setup, before frames flow (mirrors the delivery sink).
    // The built-in `stored_value` type is registered by the constructor.
    void register_child_type(std::string type, child_factory_t factory);

    // Hot path — operate on a resolved handle; lock-free in the LKV slot. The trailing
    // `caller` on each op is the ACL caller context (#81): empty for a local API call
    // (the default — zero churn), the inbound link NAME when the FWD resolver drives
    // the op. With no subject resolver installed it costs one null check.
    [[nodiscard]] result_t<view_t> read(vertex_t* v, std::string_view caller = {}) const;
    [[nodiscard]] result_t<void> write(vertex_t* v, view_t value, std::string_view caller = {});
    // Field-write by handle: resolve the vertex_t* and field_path_t once (e.g. from a
    // path_t::parse("/x:settings.reliability") kept around), then reuse them on the
    // hot path — no string parse, no map lookup per call. An empty `field` is an
    // ordinary value write. Pass `path.field()` for the field selector.
    [[nodiscard]] result_t<void> write(vertex_t* v, const field_path_t& field, view_t value,
                                       std::string_view caller = {});
    [[nodiscard]] result_t<view_t> await(vertex_t* v, std::chrono::nanoseconds timeout,
                                         std::string_view caller = {});
    // Field-read by handle (the read dual of the field-write overload): an empty `field`
    // is an ordinary value read; otherwise serve ":schema", ":acl", or a single
    // ":subscribers[N]" slot (the slot's stored SUBSCRIBER view, zero-copy). For the
    // whole-array ":subscribers[]" read use read_subscribers(). Used by the FWD resolver.
    [[nodiscard]] result_t<view_t> read(vertex_t* v, const field_path_t& field,
                                        std::string_view caller = {}) const;
    // Read the ":subscribers[]" array: the populated slot SUBSCRIBER views in slot order
    // (each a zero-copy refcount clone of the stored source view). The FWD resolver ropes
    // these under a fresh PL=1 wrapper into the REPLY (RFC-0004 §D, no byte copy).
    [[nodiscard]] result_t<std::vector<view_t>> read_subscribers(
        vertex_t* v, std::string_view caller = {}) const;
    // Stream history, newest last (Stream role only).
    [[nodiscard]] result_t<std::vector<view_t>> history(vertex_t* v) const;

    // Subscribe `src` to a target vertex (spec-faithful: a write to src re-dispatches
    // the cloned value to `target`). NotFound if src is unknown.
    //
    // These subscribe(...) overloads are *host SDK sugar*, not new wire primitives:
    // the wire data API stays read/write/await (ADR-0006). On the wire, subscription
    // is a consumer-initiated SUBSCRIBER write into the producer's `:subscribers[]`
    // field (ADR-0026), exactly as connect() is sugar over that field-write. Routing
    // these helpers through the `:subscribers[]` field-write surface (rather than the
    // current direct path) is tracked in #59.
    [[nodiscard]] result_t<void> subscribe(const path_t& src, const path_t& target,
                                           delivery_mode_t mode = delivery_mode_t::EVERY);
    // Subscribe `src` to an in-process callback (sugar; the callback fires inline on
    // each write to src with a cloned view). `mode` gates delivery producer-side.
    [[nodiscard]] result_t<void> subscribe(const path_t& src,
                                           std::function<void(const view_t&)> callback,
                                           delivery_mode_t mode = delivery_mode_t::EVERY);

    // Install the sink the producer fan-out hands each REMOTE subscriber's delivery to
    // (#136, RFC-0004 §D/§E.1). Set once at wiring time by the transport plane
    // (tr::net::fwd_router_t) before frames flow; the sink fires on whatever thread
    // calls write() (outside the vertex lock), and on subscribe for a transient-local
    // latch. L4 keeps it as an opaque std::function, so the graph never depends on a
    // transport. A null sink (the default) ⇒ remote slots are stored but never deliver.
    void set_remote_delivery_sink(
        std::function<void(const remote_delivery_t&, const view_t&)> sink);

    /**
     * @brief Install the pluggable subject resolver (ADR-0018) — the ACL enforcement switch.
     *
     * No resolver (the default) ⇒ enforcement is DISABLED: every operation is allowed,
     * exactly today's behavior, and the hot path pays one null check. With a resolver
     * installed, each gated operation maps its caller context through it and — when a
     * subject token comes back — evaluates the target vertex's *effective* ACL (own
     * ACEs + ancestor ACEs carrying INHERIT, ADR-0020): allowed iff some non-expired
     * ACE with a matching subject (or `"EVERYONE@"`) grants the operation's right bit;
     * a vertex whose effective ACL is empty stays open (enforcement is opt-in per
     * vertex via ACL presence). Denial returns status_t::PERMISSION_DENIED
     * (`tr::access::denied` on the wire, RFC-0002).
     *
     * Set once at wiring time, before frames flow — read-only afterwards on the op
     * paths, so no lock (the remote-sink / child-catalog contract).
     */
    void set_subject_resolver(subject_resolver_t resolver);

    // Store a REMOTE subscriber on `v`: a SUBSCRIBER slot carrying the consumer's
    // `return_route` (a view over a refcounted segment — the ONE copy of the route,
    // made by the caller at subscribe; every later delivery clones the refcount,
    // ADR-0041 §2) + this node's NAME for its `link` + the `delivery_compact` opt-in,
    // so a later write fans out a FWD{WRITE}/COMPACT delivery via the remote sink. The
    // wire dual of the local subscribe(...) sugar, driven by the FWD resolver on an
    // inbound `:subscribers[]` WRITE (#59/#136). If `v` is transient-local
    // (settings.durability == 1) and already holds a value, the current LKV is latched
    // to this subscriber immediately (one synchronous sink call, RFC-0004 §D). The
    // `source_view` (the SUBSCRIBER TLV) is retained zero-copy so a `:subscribers[]`
    // read serves it back. NotFound is impossible (the caller holds `v`). The producer
    // fan-out gate (#81, ADR-0026): `link` is the caller context — the append requires
    // the SUBSCRIBE right on `v`'s :acl, else PERMISSION_DENIED.
    [[nodiscard]] result_t<void> add_remote_subscriber(
        vertex_t* v, view_t source_view, view_t return_route, std::string link,
        bool delivery_compact, delivery_mode_t mode = delivery_mode_t::EVERY);

    // Convenience — resolve the path key once (guarded map lookup), then hot path.
    // A write/read whose path has a field tail (e.g. ":settings.deadline_ns",
    // ":subscribers[]", ":schema") is routed to the field surface.
    [[nodiscard]] result_t<view_t> read(const path_t& path) const;
    [[nodiscard]] result_t<void> write(const path_t& path, view_t value);
    [[nodiscard]] result_t<view_t> await(const path_t& path, std::chrono::nanoseconds timeout);

    // Resolve a canonical PATH-payload key to its vertex (nullptr if unknown).
    [[nodiscard]] vertex_t* find(std::span<const std::byte> key) const;

    /**
     * @brief Find-or-create the vertex at @p key (write-creates, RFC-0005).
     *
     * Resolves @p key; when absent, creates the vertex — and every missing
     * intermediate level, `mkdir -p` style, each a STORED_VALUE vertex — gated by
     * the CREATE right on the nearest EXISTING ancestor's effective ACL under
     * @p caller (PERMISSION_DENIED when denied; a graph holding no ancestor at all
     * is open, matching ACL-presence opt-in). A creation race lost to a concurrent
     * caller is benign (the winner's vertex is returned). @p key must be a
     * well-formed, non-empty canonical PATH-payload (else INVALID_PATH).
     */
    [[nodiscard]] result_t<vertex_t*> ensure_vertex(std::span<const std::byte> key,
                                                    std::string_view caller = {});

    /**
     * @brief How many writes performed the ancestor (bubbling) walk — instrumentation.
     *
     * The near-free-when-idle observable (RFC-0005): stays 0 while no subscriber
     * exists above any written vertex, so tests and benches can assert a write
     * never walks ancestors unless someone is listening. Relaxed monotonic counter.
     */
    [[nodiscard]] std::uint64_t ancestor_walks() const noexcept;

   private:
    // Update the vertex value (LKV/history/handler), then fan out to subscribers.
    // `depth` bounds in-process re-dispatch cycles (kMaxDispatchDepth). `caller` is
    // the ACL caller context gating the WRITE right: the API caller's at depth 0,
    // the subscription edge's stored context on a fan-out re-dispatch (fan-in gate).
    result_t<void> write_impl(vertex_t* v, view_t value, int depth, std::string_view caller);
    // The store half of a write (LKV/history/handler + seq bump + await wake),
    // WITHOUT fan-out — shared by write_impl and the branch-write apply (RFC-0005).
    result_t<void> store_value(vertex_t* v, view_t value);
    // Branch-write decomposition (RFC-0005): a POINT payload written to `v` lands
    // each value-carrying node at the corresponding descendant vertex as a
    // refcount SUBVIEW of the written frame (creating missing vertices, CREATE-
    // gated), then notifies each covered subscription point once with its slice.
    result_t<void> write_branch(vertex_t* v, const view_t& value, int depth,
                                std::string_view caller);
    void fan_out(vertex_t* v, const view_t& value, int depth);
    // Vertical bubbling (RFC-0005): fan `value` out to every registered ancestor's
    // subscribers. Called only when v->listeners_above_ says someone is listening.
    void bubble_up(vertex_t* v, const view_t& value, int depth);
    // Subscribe/unsubscribe bookkeeping (RFC-0005): bump v's own active-slot count
    // and every descendant's listeners_above_, under the map lock (shared — the
    // counters are atomics; the lock only excludes concurrent vertex creation so
    // a newborn's creation-time sum and this walk never double-count).
    void note_subscriber_added(vertex_t* v);
    void note_subscriber_removed(vertex_t* v);
    // Field surface: ":settings.<f>", ":subscribers[]" / "[N]", ":children[]".
    result_t<void> field_write(vertex_t* v, const field_path_t& field, const view_t& value,
                               std::string_view caller);
    // The ACL gate (#81, ADR-0018/0020 core subset): true iff `caller` may exercise
    // `right` on `v`. True with no resolver installed (one null check — enforcement
    // off), for a trusted caller (resolver returns nullopt), or when the effective
    // ACL (own ACEs + INHERIT-flagged ancestor ACEs, walked at check time) is empty;
    // otherwise true iff some non-expired matching ACE grants the bit. Takes each
    // vertex's mutex one at a time (never nested) — control-plane frequency only.
    [[nodiscard]] bool acl_allows(vertex_t* v, std::string_view caller, acl_right_t right) const;
    // ":children[]" append: instantiate a child from a SPEC via the type catalog (#82,
    // ADR-0017). Composes the child key (parent key + the SPEC `name` NAME), dispatches
    // on the SPEC `type`. Unknown type => SCHEMA_NOT_FOUND; duplicate name => PATH_IN_USE.
    result_t<void> create_child(vertex_t* parent, const view_t& spec_value);
    // ":schema" read => a POINT descriptor (name + settings).
    [[nodiscard]] result_t<view_t> read_schema(vertex_t* v) const;
    // ":children[]" read => member enumeration (write-spec / read-members asymmetry,
    // reference 05 §SPEC): a POINT whose children are POINT{NAME} member descriptors.
    // A vertex carrying handlers.on_children serves that synthesized listing instead
    // (ADR-0044 — a transport vertex lists live bus peers, no vertices created);
    // otherwise the direct child vertices registered under v's key are enumerated.
    [[nodiscard]] result_t<view_t> read_children(vertex_t* v) const;
    // ":acl" read => the raw stored ACL TLV bytes verbatim (#81-A, ADR-0018/0020). The
    // caller-facing gate (READ_ACL) runs in read(v, field, caller) before reaching here.
    [[nodiscard]] result_t<view_t> read_acl(vertex_t* v) const;

    mutable std::shared_mutex map_mutex_;
    std::unordered_map<path_key_t, std::unique_ptr<vertex_t>, path_key_hash_t> vertices_;
    // The device creation catalog (#82, ADR-0017): SPEC `type` -> factory. Populated at
    // setup (register_child_type), read-only once frames flow, so no lock (same contract
    // as remote_sink_). `std::less<>` enables heterogeneous string_view lookup.
    std::map<std::string, child_factory_t, std::less<>> child_types_;
    // The remote-delivery sink (#136). Set once before frames flow, then read-only on
    // the write hot path — no lock needed (a benign data race with a late setup write
    // is excluded by the "configure before frames flow" contract, mirroring fwd_router).
    std::function<void(const remote_delivery_t&, const view_t&)> remote_sink_;
    // The pluggable subject resolver (#81, ADR-0018). Null (default) => ACL enforcement
    // disabled. Same set-once-before-frames-flow contract as remote_sink_.
    subject_resolver_t subject_resolver_;
    // Bubbling-walk instrumentation (RFC-0005) — see ancestor_walks().
    mutable std::atomic<std::uint64_t> ancestor_walks_{0};
};

}  // namespace tr::graph
