/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The L4 in-process graph runtime. Holds the Composite vertex tree (ADR-0057:
 * parent/children links, one NAME segment per node; a canonical PATH-payload
 * key resolves by an O(segments) child walk, docs/reference/02 §dispatch) and
 * exposes the entire data API: read / write / await (ADR-0006). The hot path
 * resolves a vertex_t* once (at registration or via one guarded lookup), then
 * read/write/await on that handle are lock-free in the LKV slot. subscriber_t fan-out + field-write
 * land in M3b; M3a delivers values via the LKV and the blocking await.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "libtracer/path.hpp"
#include "libtracer/status.hpp"
#include "libtracer/vertex.hpp"
#include "libtracer/view.hpp"

namespace tr::wire {
struct tlv_t;  // fwd-decl: the child factory takes a `const tlv_t*` config (no L2 pull-in).
}

namespace tr::graph {

class graph_t;  // fwd-decl: vertex_handle_t names it as its sole constructing friend.

/**
 * @brief A non-owning, non-null, opaque handle to a graph vertex (ADR-0056).
 *
 * The caller-held result of @ref graph_t::register_vertex / @ref graph_t::find and the
 * token handed back into every `graph_t` data op (read / write / await / assign /
 * propagate / subscribe / history / field-write). Pointer-sized and trivially copyable,
 * so it loads and passes exactly like the `vertex_t*` it replaces — identical codegen —
 * but it exposes no `operator*` or raw-pointer accessor: a `vertex_t` is opaque L4 state,
 * never dereferenced by callers. Constructed ONLY by @ref graph_t (the `friend`), which
 * owns the pinned, pointer-stable, insert-only vertex map — so a handle always names a
 * live vertex for the graph's lifetime. There is no invalid/null state; "no such vertex"
 * is modelled by the `std::optional<vertex_handle_t>` @ref graph_t::find returns.
 */
class vertex_handle_t {
   public:
    /** @brief Two handles compare equal iff they name the same vertex. (`!=` is synthesized.) */
    [[nodiscard]] friend bool operator==(vertex_handle_t a, vertex_handle_t b) noexcept {
        return a.ptr_ == b.ptr_;
    }

   private:
    friend class graph_t;  // sole constructor + the only code that unwraps to `vertex_t*`.
    explicit vertex_handle_t(vertex_t* ptr) noexcept : ptr_(ptr) {}
    [[nodiscard]] vertex_t* get() const noexcept { return ptr_; }
    vertex_t* ptr_;
};

// The ADR-0056 zero-overhead claim, enforced: a handle is exactly a pointer.
static_assert(std::is_trivially_copyable_v<vertex_handle_t>);
static_assert(sizeof(vertex_handle_t) == sizeof(vertex_t*));

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
 * plus the `vertex_t::subscriber_t` delivery_compact opt-in. The injected sink
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

/**
 * @brief The L4 in-process graph runtime: the Composite vertex tree plus the whole data
 *        API (register / read / write / await / subscribe, ADR-0006).
 *
 * Vertices form a Composite tree (ADR-0057): each node stores its own NAME segment and
 * its children; a canonical PATH-TLV payload key (docs/reference/02 §dispatch) resolves
 * by an O(segments) child walk at wiring frequency. The hot path resolves a `vertex_t*`
 * once — at registration or via one guarded @ref find — then read/write/await on that
 * handle are lock-free in the vertex's last-known-value slot. Non-copyable; a graph is a
 * fixed runtime root.
 */
class graph_t {
   public:
    /** @brief Construct an empty graph (registers the built-in `stored_value` child type). */
    /**
     * @brief Construct a graph drawing its per-write allocations from @p mr —
     *        the ADR-0039 §1 injection seam (#361 §5). A 16 KB node installs a
     *        pool/monotonic resource over a static arena; a host passes nothing
     *        and gets the standard heap (zero churn). The resource must outlive
     *        the graph and every value handle obtained from it.
     */
    explicit graph_t(std::pmr::memory_resource* mr = std::pmr::get_default_resource());
    graph_t(const graph_t&) = delete;
    graph_t& operator=(const graph_t&) = delete;

    /**
     * @brief Register a vertex at a known-good @p path LITERAL, parsing nothing further (any
     *        `:field` tail is ignored) — INFALLIBLE (ADR-0056).
     *
     * The init-time registration form: a `PATH_IN_USE` collision on a compile-site literal is
     * a source bug, not a runtime condition, so this **hard-aborts** (like
     * `path_t(std::string_view)`, ADR-0054) rather than yielding a `result_t` the caller would
     * only `*`-deref unchecked. Returns the pinned @ref vertex_handle_t directly — no `*`.
     * For a genuine runtime path whose collision is a real outcome, use @ref try_register_vertex.
     */
    [[nodiscard]] vertex_handle_t register_vertex(const path_t& path, role_t role,
                                                  handlers_t handlers = {},
                                                  settings_t settings = {});

    /**
     * @brief Register a vertex at @p path — FALLIBLE (the runtime-path form of
     *        @ref register_vertex).
     * @return The pinned @ref vertex_handle_t, or `PATH_IN_USE` if the path is already
     *         registered.
     */
    [[nodiscard]] result_t<vertex_handle_t> try_register_vertex(const path_t& path, role_t role,
                                                                handlers_t handlers = {},
                                                                settings_t settings = {});

    /**
     * @brief Register a vertex by its canonical PATH-payload @p key directly (the in-band
     *        `:children[]` path) — FALLIBLE.
     *
     * The key is a composed parent-key + `NAME(child)`, not parsed from a string. This is the
     * genuine runtime path (a `:children[]` write can race a duplicate name), so it stays
     * fallible.
     * @return The pinned @ref vertex_handle_t, or `PATH_IN_USE` if the key is already registered.
     */
    [[nodiscard]] result_t<vertex_handle_t> register_vertex_key(std::vector<std::byte> key,
                                                                role_t role,
                                                                handlers_t handlers = {},
                                                                settings_t settings = {});

    /**
     * @brief A child-vertex factory: the device-catalog entry ADR-0017 makes concrete.
     *
     * Given the composed child key (parent key + the SPEC's `name` NAME) and the optional
     * SPEC `config` SETTINGS, it registers the child vertex(es) and returns the primary
     * handle (or a status — e.g. `PATH_IN_USE`). The graph owns the *addressing* (the key
     * is composed for it); the factory owns the *catalog* (what a `type` instantiates).
     */
    using child_factory_t = std::function<result_t<vertex_handle_t>(
        graph_t&, std::vector<std::byte> child_key, const wire::tlv_t* config)>;

    /**
     * @brief Populate the device creation catalog (ADR-0017): map a SPEC `type` selector
     *        to a @ref child_factory_t.
     *
     * A `:children[]` SPEC write whose `type` is unregistered returns `SCHEMA_NOT_FOUND`
     * (the ENOTTY of an unsupported creation). Not thread-safe against concurrent creation
     * — call at setup, before frames flow (mirrors the delivery sink). The built-in
     * `stored_value` type is registered by the constructor.
     */
    void register_child_type(std::string type, child_factory_t factory);

    /**
     * @brief Read a resolved vertex's stored value (the hot path — lock-free in the LKV slot).
     *
     * Returns the last-known-value as a rope (ADR-0053 §6): a scalar is the single-link
     * case; a consumer needing contiguous bytes calls `rope_t::only()` (single-link, zero
     * copy) or `rope_t::materialize()`. The trailing @p caller is the ACL caller context
     * (#81): empty for a local API call (the default — zero churn), the inbound link NAME
     * when the FWD resolver drives the op. With no subject resolver installed it costs one
     * null check.
     */
    [[nodiscard]] result_t<rope_t> read(vertex_handle_t v, std::string_view caller = {}) const;
    /**
     * @brief Write a resolved vertex's value: `assign` then deliver (RFC-0008 §D).
     *
     * Takes a rope; an existing `view_t` caller compiles unchanged via the implicit
     * `view_t`→`rope_t`. @p caller is the ACL caller context (see @ref read).
     */
    [[nodiscard]] result_t<void> write(vertex_handle_t v, rope_t value,
                                       std::string_view caller = {});
    /**
     * @brief Field-write by handle: resolve the @ref vertex_handle_t and @ref field_path_t
     *        once, then reuse them on the hot path — no string parse, no map lookup per call.
     *
     * An empty @p field is an ordinary value write. Pass `path.field()` for the field
     * selector. A field write targets a contiguous control TLV, so a multi-link value is
     * materialized first.
     */
    [[nodiscard]] result_t<void> write(vertex_handle_t v, const field_path_t& field, rope_t value,
                                       std::string_view caller = {});
    /**
     * @brief Assign a vertex's value — the STATE transition only, sends NOTHING (RFC-0008).
     *
     * One of the two irreducible operations `write` composes: swap v's last-known-value
     * (atomic), append to the stream ring, bump the write sequence (waking await), and
     * mark v for the next covering @ref propagate sweep (unless v is EXPLICIT, or nobody
     * observes at/above it). WRITE-gated like @ref write; never gated by delivery_mode. A
     * branch POINT decomposes and assigns each descendant (no notify). Pair with
     * @ref propagate for the "update many, propagate once" workflow.
     */
    [[nodiscard]] result_t<void> assign(vertex_handle_t v, rope_t value,
                                        std::string_view caller = {});
    /**
     * @brief Propagate along subscription edges — the EDGE transition only (RFC-0008 §B/§C).
     *
     * Delivers v's current value (always — @p v is the explicit target, so a direct
     * propagate is never gated by v's delivery_mode) AND the qualifying descendants of v's
     * subtree per each descendant's delivery_mode: IF_NEWER descendants assigned since the
     * last covering sweep, and every UNCONDITIONAL descendant. Reads the last-known-value
     * — no value argument. Costs O((pending + unconditional)-in-subtree).
     */
    void propagate(vertex_handle_t v);
    /**
     * @brief Set v's per-vertex propagation policy (RFC-0008 §C).
     *
     * A wiring-time call (the "configure before frames flow" contract), like settings;
     * maintains the sweep's UNCONDITIONAL membership. Default (unset) is IF_NEWER.
     */
    void set_delivery_mode(vertex_handle_t v, delivery_mode_t mode);
    /**
     * @brief Block until the vertex's value changes or @p timeout elapses; return the value.
     * @return The stored value as a rope, or a `status_t` (e.g. `TIMEOUT`).
     */
    [[nodiscard]] result_t<rope_t> await(vertex_handle_t v, std::chrono::nanoseconds timeout,
                                         std::string_view caller = {});
    /**
     * @brief Field-read by handle (the read dual of the field-write overload).
     *
     * An empty @p field is an ordinary value read (the stored rope); otherwise serve
     * `:schema`, `:acl`, or a single `:subscribers[N]` slot (the slot's stored SUBSCRIBER
     * view, zero-copy) as a single-link rope. For the whole-array `:subscribers[]` read use
     * @ref read_subscribers. Used by the FWD resolver.
     */
    [[nodiscard]] result_t<rope_t> read(vertex_handle_t v, const field_path_t& field,
                                        std::string_view caller = {}) const;
    /**
     * @brief Read the `:subscribers[]` array — the populated slot SUBSCRIBER views in slot order.
     *
     * Each is a zero-copy refcount clone of the stored source view. The FWD resolver ropes
     * these under a fresh PL=1 wrapper into the REPLY (RFC-0004 §D, no byte copy).
     */
    [[nodiscard]] result_t<std::vector<view_t>> read_subscribers(
        vertex_handle_t v, std::string_view caller = {}) const;
    /** @brief Stream history, newest last (Stream role only) — each entry the stored rope value. */
    [[nodiscard]] result_t<std::vector<rope_t>> history(vertex_handle_t v) const;

    /**
     * @brief Subscribe @p src to a @p target vertex — a write to src re-dispatches the
     *        cloned value to target (spec-faithful). `NOT_FOUND` if src is unknown.
     *
     * These `subscribe(...)` overloads are *host SDK sugar*, not new wire primitives: the
     * wire data API stays read/write/await (ADR-0006). On the wire, subscription is a
     * consumer-initiated SUBSCRIBER write into the producer's `:subscribers[]` field
     * (ADR-0026), exactly as connect() is sugar over that field-write. Per ADR-0049 (#59)
     * this overload ENCODES a `SUBSCRIBER{PATH}` TLV and enters the same `:subscribers[]`
     * field-write admission door as a wire subscribe — one parse, one SUBSCRIBE gate, one
     * durability latch, and the edge's stored SUBSCRIBER view reads back byte-identically
     * from `:subscribers[]`.
     */
    [[nodiscard]] result_t<void> subscribe(const path_t& src, const path_t& target);
    /**
     * @brief Subscribe @p src to an in-process `{fn, ctx}` callback (sugar; fires inline
     *        on each delivery to src with the rope value).
     *
     * The per-edge sink is a plain function-pointer pair (ADR-0047 hot-path shape, like
     * `transport_t::set_receiver`), so the per-publish edge snapshot is a trivial copy —
     * no `std::function` clone. Delivery is value-agnostic (RFC-0008): WHICH vertices a
     * sweep propagates is the source vertex's delivery_mode, not a per-edge policy. A
     * callback cannot ride a TLV, so this overload skips the door's parse — but it enters
     * the SAME single admission step (SUBSCRIBE gate → append → durability latch,
     * ADR-0049) as every other door.
     * @param fn  The per-delivery sink; @p ctx is passed back as its first argument.
     * @param ctx Caller-owned context; must outlive every possible delivery (edges are
     *            never destroyed while the graph lives — an unsubscribe only deactivates
     *            the slot, but an in-flight delivery may still be running).
     */
    [[nodiscard]] result_t<void> subscribe(const path_t& src, subscriber_fn_t fn, void* ctx);

    /**
     * @brief Subscribe @p src to a caller-owned callable (sugar over the `{fn, ctx}` form).
     *
     * Zero-erasure sugar mirroring `transport_t::set_receiver`: @p callback is bound by
     * address (lvalues only — a temporary would dangle) and MUST outlive every delivery.
     */
    template <typename F>
        requires std::invocable<F&, const view::rope_t&>
    [[nodiscard]] result_t<void> subscribe(const path_t& src, F& callback) {
        return subscribe(
            src, [](void* c, const view::rope_t& v) { (*static_cast<F*>(c))(v); }, &callback);
    }

    /**
     * @brief Install the sink the producer fan-out hands each REMOTE subscriber's delivery
     *        to (#136, RFC-0004 §D/§E.1).
     *
     * Set once at wiring time by the transport plane (`tr::net::fwd_router_t`) before frames
     * flow; the sink fires on whatever thread calls @ref write (outside the vertex lock),
     * and on @ref subscribe for a transient-local latch. L4 keeps it as an opaque
     * `std::function`, so the graph never depends on a transport. A null sink (the default)
     * ⇒ remote slots are stored but never deliver. The value reaches the sink as a rope
     * (ADR-0053 §6): a single-link value materializes zero-copy, a multi-link value is
     * handed over as the rope it is.
     */
    void set_remote_delivery_sink(
        std::function<void(const remote_delivery_t&, const rope_t&)> sink);

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

    /**
     * @brief The wire `:subscribers[]` APPEND — the same admission door as the local
     *        sugars and field-writes (ADR-0049), plus the remote delivery binding.
     *
     * Called by the FWD resolver on an inbound `:subscribers[]` WRITE (#59/#136); it
     * replaces the retired `add_remote_subscriber` parallel API. @p source_view (the
     * SUBSCRIBER TLV, an owned copy) is parsed ONCE here — the `delivery_compact` opt-in
     * comes from this parse (the resolver no longer parses it in parallel) and the view is
     * retained zero-copy so a `:subscribers[]` read serves it back. A PATH child, if
     * present, names the consumer at ITS origin and is deliberately NOT bound as a local
     * re-dispatch target — remote delivery rides @p return_route (a view over a refcounted
     * segment — the ONE copy of the route; every later delivery clones the refcount,
     * ADR-0041 §2) over @p link via the remote sink. Admission is the single ADR-0049
     * step: SUBSCRIBE gate on @p v's `:acl` under @p link (#81, ADR-0026,
     * `PERMISSION_DENIED` on denial) → slot append → durability latch (if @p v is
     * transient-local, `settings.durability == 1`, and holds a value, the LKV is latched
     * to this subscriber — one synchronous sink call, RFC-0004 §D).
     */
    [[nodiscard]] result_t<void> subscribe_wire(vertex_handle_t v, view_t source_view,
                                                view_t return_route, std::string link);

    /**
     * @brief Read by path — resolve the path key once (guarded map lookup), then the hot path.
     *
     * A read whose path has a field tail (e.g. `:settings.deadline_ns`, `:subscribers[]`,
     * `:schema`) is routed to the field surface.
     */
    [[nodiscard]] result_t<rope_t> read(const path_t& path) const;
    /** @brief Write by path — resolve the key once, then @ref write(vertex_handle_t, rope_t,
     * std::string_view). */
    [[nodiscard]] result_t<void> write(const path_t& path, rope_t value);
    /** @brief Await by path — resolve the key once, then @ref await(vertex_handle_t,
     * std::chrono::nanoseconds, std::string_view). */
    [[nodiscard]] result_t<rope_t> await(const path_t& path, std::chrono::nanoseconds timeout);

    /** @brief Resolve a canonical PATH-payload @p key to its vertex handle (`nullopt` if
     *         unknown). */
    [[nodiscard]] std::optional<vertex_handle_t> find(std::span<const std::byte> key) const;

    /**
     * @brief The QoS settings of the vertex @p v names (ADR-0056).
     *
     * The read accessor the opaque handle does not expose directly: a resolver that needs a
     * per-vertex knob (e.g. `store_ref_min_bytes`, ADR-0042 §3) queries it here instead of
     * dereferencing the vertex. Wiring-stable — settings change only via `:settings` writes.
     */
    [[nodiscard]] const settings_t& settings(vertex_handle_t v) const noexcept;

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
    [[nodiscard]] result_t<vertex_handle_t> ensure_vertex(std::span<const std::byte> key,
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
    // Internal (raw `vertex_t*`) forms of the public handle-returning resolvers: the graph's
    // own machinery threads raw pointers (ADR-0056 — internal methods keep `vertex_t*`), and
    // the public @ref find / @ref ensure_vertex wrap these once at the boundary.
    [[nodiscard]] vertex_t* find_ptr(std::span<const std::byte> key) const;
    [[nodiscard]] result_t<vertex_t*> ensure_vertex_ptr(std::span<const std::byte> key,
                                                        std::string_view caller);
    // Update the vertex value (LKV/history/handler), then fan out to subscribers.
    // `depth` bounds in-process re-dispatch cycles (kMaxDispatchDepth). `caller` is
    // the ACL caller context gating the WRITE right: the API caller's at depth 0,
    // the subscription edge's stored context on a fan-out re-dispatch (fan-in gate).
    result_t<void> write_impl(vertex_t* v, rope_t value, int depth, std::string_view caller);
    // The store half of a write (LKV/history/handler + seq bump + await wake),
    // WITHOUT fan-out — shared by write_impl and the branch-write apply (RFC-0005).
    // Hands back the exact published LKV pointer (null for a Handler-role write —
    // the user handler consumed the value, nothing was stored), so the eager write
    // path delivers precisely what was stored (RFC-0008 §D) without a rope reclone.
    result_t<std::shared_ptr<const rope_t>> store_value(vertex_t* v, rope_t value);
    // Branch-write decomposition (RFC-0005): a POINT payload written to `v` lands
    // each value-carrying node at the corresponding descendant vertex as a
    // refcount SUBVIEW of the written frame (creating missing vertices, CREATE-
    // gated), then notifies each covered subscription point once with its slice. A
    // decomposable POINT is contiguous, so the walk reads the materialized head
    // (single-link: zero copy) and lands rope slices of it (ADR-0053 §6).
    // Branch-write decomposition (RFC-0005): a POINT payload written to `v` lands each
    // value-carrying node at the corresponding descendant vertex. `notify` picks the
    // half: true (the `write` path) delivers each covered site + bubbles; false (the
    // `assign` path) marks each landed vertex for the next sweep and delivers nothing.
    result_t<void> write_branch(vertex_t* v, const rope_t& value, int depth,
                                std::string_view caller, bool notify);
    void fan_out(vertex_t* v, const rope_t& value, int depth);
    // The ONE dispatch of a subscription edge's three legs (in-process callback, local
    // target re-dispatch, remote sink) — shared by fan_out and the admission durability
    // latch (ADR-0049), always called OUTSIDE the vertex lock. The target/remote legs
    // are split out so the per-edge body stays small enough to inline into the fan-out
    // loop (the wide-fan-out hot loop; the callback leg is the in-process hot case).
    void dispatch_edge(const edge_view_t& e, const rope_t& value, int depth);
    void dispatch_edge_target(const edge_view_t& e, const rope_t& value, int depth);
    void dispatch_edge_remote(const edge_view_t& e, const rope_t& value);
    // Vertical bubbling (RFC-0005): fan `value` out to every registered ancestor's
    // subscribers. Called only when v->listeners_above_ says someone is listening.
    void bubble_up(vertex_t* v, const rope_t& value, int depth);
    // Deliver `value` as `v`'s value to v's full observer set: v's own edges (fan_out)
    // + every ancestor subtree subscriber (bubble_up, gated on listeners_above_). The
    // per-vertex delivery unit both `write` (eager) and `propagate` (sweep) build on.
    void deliver_vertex(vertex_t* v, const rope_t& value, int depth);
    // Deliver v's CURRENT stored value (propagate reads the LKV — no value argument).
    // STORED_VALUE: the last-known-value once; STREAM: drains the ring entries appended
    // since the last flush, in order (RFC-0008 §E — a queue, not a coalesce); HANDLER /
    // never-assigned (null LKV): nothing.
    void deliver_current(vertex_t* v, int depth);
    // The propagate(v) sweep body at an explicit recursion depth (cycle-bounded like a
    // write re-dispatch). Delivers v then its qualifying descendants (RFC-0008 §B/§C).
    void propagate_impl(vertex_t* v, int depth);
    // Record v as assigned-since-last-sweep so a covering propagate flushes it (RFC-0008
    // §B). No-op for EXPLICIT (never ancestor-swept), for UNCONDITIONAL (already a
    // permanent sweep member), and — the idle-write fast path — when nothing observes at
    // or above v (a sweep would deliver it nowhere; RFC-0005 listeners gate).
    void mark_pending(vertex_t* v);
    // Drop v from the pending set (an eager `write` delivered it, so a later covering
    // sweep must not re-deliver). Gated on the same listeners fast path as mark_pending.
    void clear_pending(vertex_t* v);
    // Subscribe/unsubscribe bookkeeping (RFC-0005): bump v's own active-slot count
    // and every descendant's listeners_above_, under the map lock (shared — the
    // counters are atomics; the lock only excludes concurrent vertex creation so
    // a newborn's creation-time sum and this walk never double-count).
    void note_subscriber_added(vertex_t* v);
    void note_subscriber_removed(vertex_t* v);
    // The single SUBSCRIBER admission step (ADR-0049): SUBSCRIBE gate under `caller` →
    // slot append → transient-local durability latch (delivered outside the lock, per
    // the edge's kind) → RFC-0005 bookkeeping. Every door — the two subscribe() sugars,
    // the local `:subscribers[]` field-write, and the wire subscribe_wire — ends here,
    // so gate and latch semantics cannot diverge per entry point.
    result_t<void> admit_subscriber(vertex_t* v, subscriber_t s, std::string_view caller);
    // Field surface: ":settings.<f>", ":subscribers[]" / "[N]", ":children[]".
    result_t<void> field_write(vertex_t* v, const field_path_t& field, const view_t& value,
                               std::string_view caller);
    // The ACL gate (#81, ADR-0018/0020): true iff `caller` may exercise `right` on
    // `v`. True with no resolver installed (one null check — enforcement off), for a
    // trusted caller (resolver returns nullopt), or when the effective ACL (own ACEs
    // + INHERIT-flagged ancestor ACEs) is empty; otherwise the verdict of the pure
    // per-target policy over the CACHED effective-ACE merge (ADR-0050
    // effective_acl_t — own list before ancestors, pre-merged per vertex). Runs on
    // EVERY gated data op (read/write/await), and evaluates ONE list under one
    // vertex mutex — the ancestor mutex-walk happens only inside the lazy rebuild
    // of a dirty cache (after a :acl write marked the written vertex's subtree).
    [[nodiscard]] bool acl_allows(vertex_t* v, std::string_view caller, acl_right_t right) const;
    // Subtree-precise ADR-0050 cache invalidation: mark `v` and every descendant's
    // cached effective-ACE merge stale (release stores) after a :acl write on `v`,
    // via the ADR-0057 child links — wiring-frequency. Call with map_mutex_ held
    // (shared suffices; the walk only excludes concurrent vertex creation, and a
    // vertex created after the mark starts dirty anyway).
    static void mark_subtree_acl_dirty(vertex_t* v);
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

    // The full canonical key of `v` — its ancestors' NAME records concatenated root-down
    // (ADR-0057 render-on-demand: vertices store one segment, not the full key). Walks
    // immutable parent links, so no lock. Used only at sweep/observed-write/wiring
    // frequency (the RFC-0008 byte-keyed sweep sets, `create_child` key composition).
    [[nodiscard]] static std::vector<std::byte> build_key(const vertex_t* v);
    // Bump every strict descendant's listeners_above_ by `delta` (RFC-0005 bookkeeping) —
    // a child-link subtree walk (placeholders included, so a later fill inherits a
    // correct count). Call with map_mutex_ held (shared suffices; counters are atomics).
    static void bump_subtree_listeners(vertex_t* v, std::int32_t delta);

    mutable std::shared_mutex map_mutex_;
    // The Composite vertex tree's root (ADR-0057): an unregistered structural node whose
    // children container owns every top-level vertex (each child a non-moving unique_ptr
    // allocation, recursively). INSERT-ONLY (mutation under a unique map_mutex_ hold):
    // vertices are added, never erased. find() hands out a raw vertex_t* that callers
    // hold PAST the map lock; that is sound only because each vertex_t is pointer-stable
    // (owned by its parent's container via unique_ptr, never moved) AND never destroyed
    // while the graph lives. Implementing vertex retirement (the ADR "retire-LIST") must
    // NOT be a bare detach-from-parent — that would dangle every outstanding handle (the
    // route_handle clear_link dangling-ref class, fixed in #220); it needs a vertex
    // lifetime scheme (refcount / epoch reclamation, or a tombstone) first. Registering
    // the empty key fills this node in place (the "root vertex" the flat map allowed).
    /** @brief The ADR-0039 injected resource per-write allocations draw from (#361 §5):
     *         the LKV control block + rope of every `assign`. Host-owned; outlives the
     *         graph. Defaults to the standard heap. */
    std::pmr::memory_resource* mr_ = std::pmr::get_default_resource();
    std::unique_ptr<vertex_t> root_;
    // The device creation catalog (#82, ADR-0017): SPEC `type` -> factory. Populated at
    // setup (register_child_type), read-only once frames flow, so no lock (same contract
    // as remote_sink_). `std::less<>` enables heterogeneous string_view lookup.
    std::map<std::string, child_factory_t, std::less<>> child_types_;
    // The remote-delivery sink (#136). Set once before frames flow, then read-only on
    // the write hot path — no lock needed (a benign data race with a late setup write
    // is excluded by the "configure before frames flow" contract, mirroring fwd_router).
    std::function<void(const remote_delivery_t&, const rope_t&)> remote_sink_;
    // The pluggable subject resolver (#81, ADR-0018). Null (default) => ACL enforcement
    // disabled. Same set-once-before-frames-flow contract as remote_sink_.
    subject_resolver_t subject_resolver_;
    // Bubbling-walk instrumentation (RFC-0005) — see ancestor_walks().
    mutable std::atomic<std::uint64_t> ancestor_walks_{0};

    // The propagate-sweep selection sets (RFC-0008 §B), keyed on canonical PATH-payload
    // bytes and ORDERED so a subtree is a contiguous prefix range (a parent's key is a
    // byte-prefix of every descendant's — key_view_t::is_ancestor_of). `pending_` holds
    // the IF_NEWER vertices assigned since the last covering sweep (drained on sweep);
    // `unconditional_` holds every UNCONDITIONAL vertex (persistent membership, iterated
    // not drained). Both guarded by sweep_mutex_ — a distinct, coarse lock touched only
    // when a subscriber exists at/above a written vertex, so the idle write stays
    // lock-free. Snapshots are taken under it and delivered outside it (callbacks /
    // re-dispatch re-enter the graph), mirroring fan_out's discipline.
    std::set<std::vector<std::byte>> pending_;
    std::set<std::vector<std::byte>> unconditional_;
    std::mutex sweep_mutex_;
    // pending_.size() mirrored as a relaxed atomic: the observed-write fast path
    // (clear_pending on every eager delivery) skips the key render + sweep lock while no
    // assign has marked anything — losing a race with a concurrent mark_pending leaves the
    // mark for the next sweep, an ordering the locked erase already permitted (ADR-0057).
    std::atomic<std::size_t> pending_count_{0};
};

}  // namespace tr::graph
