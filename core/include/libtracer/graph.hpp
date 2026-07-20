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

// There is no in-process dispatch-depth cap: a SUBSCRIBER delivery TERMINATES at its
// target (ADR-0051 / RFC-0007) — store + notify, never a re-dispatch to the target's
// own :subscribers[] — so a dispatch-level cycle cannot form and there is nothing to
// bound. Propagation past a target is exclusively the target's own logic (a controller
// re-emitting on its execution). The former kMaxDispatchDepth (ADR-0014/0015) is deleted
// with nothing replacing it (the no-synthetic-limits principle, RFC-0006).

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
     * @brief Retire a vertex and its whole subtree — the owner-facing mirror of
     *        @ref register_vertex
     * (RFC-0009 §A.1 / §B).
     *
     * Marks @p vh (and, per §B.3, every descendant) **logically absent**: invisible to
     * `find` / `read` / `:children[]`, reading `tr::path::not_found` exactly like a
     * never-built path (§C). The allocation is NOT freed and the handle stays
     * dereferenceable forever (ADR-0057 insert-only) — the vertex is *emptied*, not
     * erased. Retirement **re-virginizes** each vertex (§B.6): it clears the previous
     * owner's `:acl`, value seam, stored value, history, app-field table, subscribers,
     * settings, and delivery mode, so a later write-creates revive of the same address
     * inherits **nothing** of the retired owner — in particular the revived path inherits
     * its live ancestor's ACL policy, never the retired one's (the §Discussion-7 ruling:
     * an ACL does not survive churn). `write_seq_` survives (monotonic per address).
     *
     * Delivers nothing and wakes no `await` (§B.5). Idempotent (§B.4): retiring an
     * already-retired or unregistered vertex succeeds and does nothing. The root cannot be
     * retired. There is **no wire operation** that reaches here — a peer goes through the
     * device's own logic (§A.1 / §A.1.1), which is what calls this.
     */
    result_t<void> retire(vertex_handle_t vh);

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
     *
     * A vertex with ≥ 1 registered child serves the composed SUBTREE SNAPSHOT instead —
     * the folded POINT tree of @ref read_snapshot_folded (per-node stored TLVs verbatim,
     * READ-denied subtrees pruned). Leaf reads are byte-identical to the pre-snapshot
     * behavior, and a HANDLER target's `on_read` seam keeps precedence over the snapshot.
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
     * @brief FOLDED projection of the `:children` listing (L4 fold, Slice 0) — the SAME
     *        `POINT{ POINT{NAME}… }` that the materialized `read_children` serializes, but
     *        produced as a scatter-gather **rope** (an outer POINT header link plus one
     *        link per registered child) instead of one flat buffer.
     *
     * A read-only projection over the materialized tree — the tree stays the source of
     * truth; this walks it and gathers rather than copying the whole listing into a
     * single allocation. `read_children_folded(v).flatten()` is **byte-identical** to the
     * materialized `read_children` serialize, which `folded_children_test` gates over many
     * graph shapes. The rope is valid while the graph (and its insert-only, pointer-stable
     * vertices) outlive it. The synthesized-listing case (ADR-0044) has nothing to gather
     * and crosses as a single-link rope. Each member's NAME bytes are borrowed IN PLACE
     * (zero copy, @ref view::borrow_const) over the pinned child vertex — only the tiny
     * POINT headers are emitted — so the listing is never copied whole.
     */
    [[nodiscard]] result_t<rope_t> read_children_folded(vertex_handle_t v) const;

    /**
     * @brief MATERIALIZED `:children` listing — the flat single-link serialize of the same
     *        `POINT{ POINT{NAME}… }` the fold gathers.
     *
     * The production field read serves the FOLDED rope; this flat form exists as the
     * independent oracle `folded_children_test` diffs the fold against (byte identity on
     * flatten() over many graph shapes) — without it the differential would be
     * tautological.
     */
    [[nodiscard]] result_t<rope_t> read_children_materialized(vertex_handle_t v) const;

    /**
     * @brief Composed SUBTREE-SNAPSHOT read (RFC-0005 §C follow-on): the POINT tree of
     *        @p v's registered subtree, folded as a scatter-gather **rope** (zero flatten).
     *
     * `snapshot(target) = POINT{ [stored TLV of target]?, child_node* }` and
     * `child_node(c) = POINT{ NAME(c), [stored TLV of c]?, child_node(grandchild)* }` —
     * each node's value is that vertex's stored TLV **verbatim** (the landed LKV bytes,
     * opaque: a non-VALUE TLV such as a STATUS composes as-is; descendant HANDLER `on_read`
     * seams are **not** invoked). Unregistered placeholders are skipped exactly as
     * `read_children` skips them; synthesized `on_children` transport listings are not
     * graph children and are absent. A vertex the @p caller may not READ **prunes** its
     * whole subtree (siblings unaffected). A branch with no descendant values folds to a
     * names-only (topology) POINT tree.
     *
     * This is what a plain @ref read serves when the target has ≥ 1 registered child; it
     * is public for the same oracle reason as @ref read_children_materialized's split.
     * Per node: one atomic `read_stored()` load, LKV links refcount-**cloned** (no byte
     * copy), the child's NAME record borrowed **in place** over the pinned vertex, and an
     * owned per-level POINT header (`opt.ll` auto-widened at the same 0xFFFF boundary as
     * `wire::emit_tlv`). The walk is an ITERATIVE stack machine (graph depth is
     * `kMaxSegments`-bounded structurally — no synthetic cap); allocation failure is
     * `BACKPRESSURE`.
     */
    [[nodiscard]] result_t<rope_t> read_snapshot_folded(vertex_handle_t v,
                                                        std::string_view caller = {}) const;

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
     * @brief Install (or replace) @p v's field descriptor table — the OWNER declaring its
     *        application property fields under `:settings.app.` (RFC-0010 §A).
     *
     * A local, owner-facing host API, the mirror of @ref register_vertex (the RFC-0009
     * §A.1 doctrine: the field catalog is device state, so there is no wire operation
     * that declares a field) — remote peers write DECLARED fields, per their declared
     * `app_access_t` and under the vertex WRITE right, never invent them; every
     * undeclared name keeps `SCHEMA_NOT_FOUND` (the `ENOTTY` default). Entries may carry
     * an initial value and the §B.1 descriptor bytes `read :schema` serves verbatim
     * (after the runtime-projected `access` member). Replacing the table is atomic with
     * respect to concurrent field operations on @p v; an empty table uninstalls (back to
     * the closed pre-RFC surface). Callable at any time — declaration is not one-shot.
     * App-field writes never wake `await` and never propagate (§C): a change consumers
     * should notice is followed by the owner's ordinary announce write.
     */
    void set_app_fields(vertex_handle_t v, std::vector<app_field_t> table);

    /**
     * @brief Install this NODE's identity — the key `read <vertex>:identity` serves
     *        (#406, RFC-0011; ADR-0045 decision 3 "the public key *is* the identity").
     *
     * NODE-scoped, not per-vertex: a node is one path tree, so EVERY vertex of this graph
     * answers `:identity` with the same byte-identical record. That invariant is the whole
     * point — it is what makes the record a valid CROSS-PATH key, so a client walking
     * `/b` and `/c/a/b` can prove they are one device (ADR-0044 point 3: the core never
     * dedups; the client does, keyed by an identity it chooses — this is that key).
     *
     * NO CRYPTO IS INVOLVED HERE, deliberately. The record is a **claim**: this seam
     * stores and serves bytes the owner supplies and verifies nothing. Proving a node
     * HOLDS the key is authentication (the ADR-0045 challenge/Noise handshake) and lives
     * elsewhere; a claim is nevertheless exactly what a TOFU peer needs to pin, and what
     * a topology walk needs to dedup. Treat an unpinned identity accordingly.
     *
     * Idempotent and re-callable; the last install wins. Call before frames flow (the
     * `register_child_type` thread contract).
     *
     * @param kind The RFC-0011 §B identity-kind (`0x01` = ed25519 raw public key).
     * @param key  The raw public key. Length MUST match @p kind (ed25519 ⇒ exactly 32).
     * @retval TYPE_MISMATCH `kind` is outside the registry (`0x00` is reserved-invalid),
     *         or `key`'s length contradicts `kind`.
     */
    [[nodiscard]] result_t<void> set_identity(std::uint8_t kind, std::span<const std::byte> key);

    /**
     * @brief Drop this node's identity — `:identity` reverts to `SCHEMA_NOT_FOUND`.
     *
     * The keyless state is the surface being ABSENT, not empty (RFC-0011 §C.3): a node
     * without a keypair genuinely has no identity facet, which is the `ENOTTY` of an
     * unsupported field, byte-for-byte the pre-RFC behaviour.
     */
    void clear_identity();

    /**
     * @brief Install (or replace) @p v's field descriptor table from BORROWED, static-storage
     *        declarations (ADR-0058) — the same owner-facing semantics as @ref set_app_fields,
     *        but the `name`/`descriptor` bytes are VIEWED, never copied.
     *
     * For an MCU owner whose field table is `constexpr` in flash, this costs **zero
     * declaration RAM**: the runtime stores views into @p table's `name`/`descriptor`
     * storage, so the caller MUST keep that storage alive for the vertex's lifetime (pass
     * pointers into flash / `.rodata`, never into stack or a soon-freed heap). Declaration
     * only — no initial value; write values later through the field-write surface. Empty
     * @p table uninstalls, exactly as @ref set_app_fields. Wire-invariant: `:schema` serves
     * the same verbatim bytes as the owning overload.
     */
    void set_app_fields_static(vertex_handle_t v, std::span<const app_field_static_t> table);

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
     * @brief Does the root have a first-level child whose NAME record equals @p record?
     *
     * The placeholder-inclusive existence test `find` cannot give: it matches a top-level
     * vertex whether it is `registered()` or a structural placeholder (an intermediate whose
     * only registered members are deeper, e.g. `/system/mode` with `/system` unfilled).
     * Used by the transport plane to reject a child-link name that would shadow a first-level
     * subtree, since a FWD's first `dst` segment resolves against the child-link registry
     * before the local graph (a link named `system` otherwise black-holes every `/system/...`
     * read onto the transport). @p record is a single canonical NAME record (`wire::emit_name`).
     */
    [[nodiscard]] bool has_first_level_child(std::span<const std::byte> record) const;

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
    // `caller` is the ACL caller context gating the WRITE right (the API caller's
    // for a direct write; a delivered subscription's stored context terminates at
    // its target instead — see dispatch_edge_target, ADR-0051).
    result_t<void> write_impl(vertex_t* v, rope_t value, std::string_view caller);
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
    result_t<void> write_branch(vertex_t* v, const rope_t& value, std::string_view caller,
                                bool notify);
    void fan_out(vertex_t* v, const rope_t& value);
    // The ONE dispatch of a subscription edge's three legs (in-process callback, local
    // target re-dispatch, remote sink) — shared by fan_out and the admission durability
    // latch (ADR-0049), always called OUTSIDE the vertex lock. The target/remote legs
    // are split out so the per-edge body stays small enough to inline into the fan-out
    // loop (the wide-fan-out hot loop; the callback leg is the in-process hot case).
    void dispatch_edge(const edge_view_t& e, const rope_t& value);
    // A SUBSCRIBER delivery TERMINATES at its target (ADR-0051 / RFC-0007): apply the
    // target-local effects of a write — store (LKV/history per role) + await wake + the
    // target's own handler reaction — gated by the TARGET's WRITE :acl, and NEVER
    // re-dispatch to the target's own :subscribers[]. Propagation past a target is the
    // target's own logic; a dispatch cycle is impossible by construction (no depth cap).
    void dispatch_edge_target(const edge_view_t& e, const rope_t& value);
    void dispatch_edge_remote(const edge_view_t& e, const rope_t& value);
    // Vertical bubbling (RFC-0005): fan `value` out to every registered ancestor's
    // subscribers. Called only when v->listeners_above_ says someone is listening.
    void bubble_up(vertex_t* v, const rope_t& value);
    // Deliver `value` as `v`'s value to v's full observer set: v's own edges (fan_out)
    // + every ancestor subtree subscriber (bubble_up, gated on listeners_above_). The
    // per-vertex delivery unit both `write` (eager) and `propagate` (sweep) build on.
    void deliver_vertex(vertex_t* v, const rope_t& value);
    // Deliver v's CURRENT stored value (propagate reads the LKV — no value argument).
    // STORED_VALUE: the last-known-value once; STREAM: drains the ring entries appended
    // since the last flush, in order (RFC-0008 §E — a queue, not a coalesce); HANDLER /
    // never-assigned (null LKV): nothing.
    void deliver_current(vertex_t* v);
    // The propagate(v) sweep body: delivers v then its qualifying descendants
    // (RFC-0008 §B/§C). Loop-free by construction (each delivery terminates at its
    // target — ADR-0051), so no recursion depth to thread.
    void propagate_impl(vertex_t* v);
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
    // Field surface: ":settings.<f>", ":settings.app.<name…>" (RFC-0010),
    // ":subscribers[]" / "[N]", ":children[]".
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
    /** @brief True iff `v` has at least one REGISTERED child — the branch/leaf fork of the
     *         plain read surface (a branch serves the composed subtree snapshot; a leaf
     *         serves its LKV byte-identically to before the snapshot read existed). Takes
     *         map_mutex_ shared. */
    [[nodiscard]] bool has_registered_child(vertex_t* v) const;
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
    // ":identity" read => the node-scoped SETTINGS{kind,key} record (RFC-0011 §B), or
    // SCHEMA_NOT_FOUND when no keypair is installed. Takes no vertex: the identity is
    // the NODE's, and every vertex serves the identical bytes.
    [[nodiscard]] result_t<view_t> read_identity() const;
    // ":children[]" read => member enumeration (write-spec / read-members asymmetry,
    // reference 05 §SPEC): a POINT whose children are POINT{NAME} member descriptors.
    // A vertex carrying handlers.on_children serves that synthesized listing instead
    // (ADR-0044 — a transport vertex lists live bus peers, no vertices created);
    // otherwise the direct child vertices registered under v's key are enumerated.
    [[nodiscard]] result_t<view_t> read_children(vertex_t* v) const;
    // ":acl" read => the raw stored ACL TLV bytes verbatim (#81-A, ADR-0018/0020). The
    // caller-facing gate (READ_ACL) runs in read(v, field, caller) before reaching here.
    [[nodiscard]] result_t<view_t> read_acl(vertex_t* v) const;
    // Bare ":settings" read (RFC-0010 §A.4) => the full settings container: the
    // implemented protocol QoS knobs, plus the nested `app` record iff a descriptor
    // table is installed — the one-traversal property tree a generic renderer walks.
    [[nodiscard]] result_t<view_t> read_settings(vertex_t* v) const;
    // ":settings.app" read (RFC-0010 §A.4) => the app container alone: declared,
    // non-`wo` fields that hold a value, in table order, values verbatim.
    // SCHEMA_NOT_FOUND when no table is installed (the closed default).
    [[nodiscard]] result_t<view_t> read_settings_app(vertex_t* v) const;

    // The full canonical key of `v` — its ancestors' NAME records concatenated root-down
    // (ADR-0057 render-on-demand: vertices store one segment, not the full key). Walks
    // immutable parent links, so no lock. Used only at sweep/observed-write/wiring
    // frequency (the RFC-0008 byte-keyed sweep sets, `create_child` key composition).
    [[nodiscard]] static std::vector<std::byte> build_key(const vertex_t* v);
    // Bump every strict descendant's listeners_above_ by `delta` (RFC-0005 bookkeeping) —
    // a child-link subtree walk (placeholders included, so a later fill inherits a
    // correct count). Call with map_mutex_ held (shared suffices; counters are atomics).
    static void bump_subtree_listeners(vertex_t* v, std::int32_t delta);

    // RFC-0009 §B.6: pre-order re-virginize of @p v's subtree — unwind each vertex's
    // subscriber contribution to its descendants' listeners_above_, revert it to a
    // placeholder, and flip it unregistered. Collects each retired vertex's key into
    // @p keys for the caller's sweep-set cleanup, and parks each detached value-seam block
    // into @ref retired_seams_. Call with map_mutex_ held UNIQUE (it flips registered_ and
    // appends to retired_seams_, both map-lock-guarded).
    void retire_subtree(vertex_t* v, std::vector<std::vector<std::byte>>& keys);

    // Value-seam blocks detached by retirement (RFC-0009 §B.6). A seam is read lock-free,
    // so a swapped-out block cannot be freed while a reader might still hold the old
    // pointer — it is parked here and reclaimed only at graph teardown (ADR-0057 insert-
    // only, applied to the seam). Kept on the GRAPH, not per-vertex, so an app-field / leaf
    // vertex pays zero extra bytes. Appended only under map_mutex_ (unique); bounded by the
    // node's total retire count — the "memory is not reclaimed under churn" trade §B books.
    std::vector<std::unique_ptr<value_handlers_t>> retired_seams_;

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
    // The NODE's identity record, pre-serialized (#406, RFC-0011 §B): the complete
    // SETTINGS{kind,key} TLV, built once at install so every `:identity` read is a copy
    // of settled bytes rather than a re-emit — the "all vertices return byte-identical
    // records" invariant (§C.1) then holds by construction, not by discipline. Empty =
    // no keypair installed => SCHEMA_NOT_FOUND (§C.3). Same set-before-frames-flow
    // contract as subject_resolver_.
    std::vector<std::byte> identity_record_;
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
